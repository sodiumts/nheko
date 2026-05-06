#ifdef GSTREAMER_AVAILABLE

#include "GStreamerSFUSession.h"
#include "Logging.h"
#include "ChatPage.h"
#include "UserSettingsPage.h"

#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>

#include <gst/rtp/gstrtpbuffer.h>
#include "gst/gl/gstgldisplay.h"

static constexpr auto STUN_SERVER = "stun://turn.matrix.org:3478";

// Salt used by livekit-client-sdk for HKDF key derivation.
// livekit-client-sdk-js FrameCryptor.ts
static constexpr auto LIVEKIT_HKDF_SALT = "LKFrameEncryptionKey";

GStreamerSFUSession::GStreamerSFUSession(QObject *parent)
  : QObject(parent),  devices_(CallDevices::instance())
{
}

GStreamerSFUSession::~GStreamerSFUSession()
{
    end();
}

void
GStreamerSFUSession::end()
{
    endPublisher();

    if (pipe_) {
        gst_element_set_state(pipe_, GST_STATE_NULL);
        gst_object_unref(pipe_);
        pipe_   = nullptr;
        webrtc_ = nullptr;
        audioMixer_ = nullptr;
        audioSink_ = nullptr;
    }
    if (busWatchId_) {
        g_source_remove(busWatchId_);
        busWatchId_ = 0;
    }
    pendingCandidates_.clear();
}

bool
GStreamerSFUSession::toggleMicMute()
{
    micMuted_ = !micMuted_;
    nhlog::ui()->info("SFU: mic {}", micMuted_ ? "muted" : "unmuted");
    return micMuted_;
}
void
GStreamerSFUSession::setMicMuted(bool muted) {
    micMuted_ = muted;
    nhlog::ui()->info("SFU: mic {}", micMuted_ ? "muted" : "unmuted");
}

void
GStreamerSFUSession::setDecryptionKey(uint8_t kid, const std::vector<uint8_t> &rawKey)
{
    std::vector<uint8_t> derived = deriveMediaKey(rawKey);
    if (derived.empty()) {
        nhlog::ui()->error("SFU: HKDF derivation failed for KID {}", kid);
        return;
    }

    std::unique_lock lock(keyMutex_);
    decryptionKeys_[kid] = std::move(derived);
    nhlog::ui()->info("SFU: stored derived decryption key for KID {}", kid);
}
std::vector<uint8_t>
GStreamerSFUSession::generateEncryptionKeyMaterial(uint8_t kid)
{
    std::vector<uint8_t> raw(16);
    if (RAND_bytes(raw.data(), 16) != 1) {
        nhlog::ui()->error("SFU: RAND_bytes failed — cannot generate encryption key");
        return {};
    }

    std::vector<uint8_t> derived = deriveMediaKey(raw);
    if (derived.empty()) {
        nhlog::ui()->error("SFU: HKDF derivation failed for outbound KID {}", kid);
        return {};
    }

    {
        std::unique_lock lock(encKeyMutex_);
        encryptionKeys_[kid] = std::move(derived);
        currentEncKid_       = kid;
    }

    nhlog::ui()->info("SFU: generated outbound encryption key for KID {}", kid);
    return raw;
}

std::string patchSdpWithSsrcAndMsid(const std::string &sdp,
                                           const std::string &trackCid,
                                           uint32_t ssrc)
{
    std::string patched = sdp;
    size_t audioPos = patched.find("\nm=audio");
    if (audioPos == std::string::npos)
        return patched;
    size_t nextMedia = patched.find("\nm=", audioPos + 1);
    size_t audioEnd = (nextMedia == std::string::npos) ? patched.size() : nextMedia;

    std::stringstream insert;
    insert << "a=ssrc:" << ssrc << " cname:{" << trackCid << "}\r\n";
    insert << "a=ssrc:" << ssrc << " msid:" << trackCid << " " << trackCid << "\r\n";
    insert << "a=msid:" << trackCid << "\r\n";

    patched.insert(audioEnd, insert.str());
    return patched;
}

void
GStreamerSFUSession::onPublisherOfferCreated(GstPromise *promise, gpointer user_data)
{
    auto *self = static_cast<GStreamerSFUSession *>(user_data);

    const GstStructure *reply = gst_promise_get_reply(promise);
    GstWebRTCSessionDescription *offer = nullptr;
    gst_structure_get(reply, "offer",
                      GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, nullptr);
    gst_promise_unref(promise);

    if (!offer) {
        nhlog::ui()->error("SFU: publisher offer creation failed");
        QMetaObject::invokeMethod(self, [self]() {
            emit self->failed(QStringLiteral("Failed to create publisher WebRTC offer"));
        }, Qt::QueuedConnection);
        return;
    }

    gchar *sdpStr = gst_sdp_message_as_text(offer->sdp);
    std::string sdp(sdpStr);
    g_free(sdpStr);

    GstPromise *localPromise = gst_promise_new();
    g_signal_emit_by_name(self->pubWebrtc_, "set-local-description", offer, localPromise);
    gst_promise_unref(localPromise);
    gst_webrtc_session_description_free(offer);


    nhlog::ui()->info("SFU: publisher offer SDP ({} bytes)", sdp.size());

    std::string patchedSdp = patchSdpWithSsrcAndMsid(sdp, self->trackCid_, self->publisherSsrc_);

    nhlog::ui()->info("SFU: publisher offer SDP:\n{}", patchedSdp);

    QMetaObject::invokeMethod(self, [self, patchedSdp]() {
        emit self->publisherPipelineReady();
        emit self->publisherOfferCreated(patchedSdp);
    }, Qt::QueuedConnection);
}
std::vector<uint8_t>
GStreamerSFUSession::getDecryptionKey(const uint8_t kid) const
{
    std::shared_lock lock(keyMutex_);
    const auto it = decryptionKeys_.find(kid);
    if (it == decryptionKeys_.end())
        return {};
    return it->second;
}

std::vector<uint8_t>
GStreamerSFUSession::getEncryptionKey(uint8_t &outKid) const
{
    std::shared_lock lock(encKeyMutex_);
    outKid    = currentEncKid_;
    auto it   = encryptionKeys_.find(currentEncKid_);
    if (it == encryptionKeys_.end())
        return {};
    return it->second;
}

std::vector<uint8_t>
GStreamerSFUSession::deriveMediaKey(const std::vector<uint8_t> &rawKey)
{
    std::vector<uint8_t> out(16);
    size_t outLen = out.size();

    // HKDF info must be 128 zero bytes from livekit-client-sdk-js
    static constexpr uint8_t hkdfInfo[128] = {};

    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
    if (!ctx)
        return {};

    const bool ok =
      EVP_PKEY_derive_init(ctx) > 0 && EVP_PKEY_CTX_set_hkdf_md(ctx, EVP_sha256()) > 0 &&
      EVP_PKEY_CTX_set1_hkdf_salt(ctx,
                                  reinterpret_cast<const unsigned char *>(LIVEKIT_HKDF_SALT),
                                  strlen(LIVEKIT_HKDF_SALT)) > 0 &&
      EVP_PKEY_CTX_set1_hkdf_key(ctx, rawKey.data(), rawKey.size()) > 0 &&
      EVP_PKEY_CTX_add1_hkdf_info(ctx, hkdfInfo, sizeof(hkdfInfo)) > 0 &&
      EVP_PKEY_derive(ctx, out.data(), &outLen) > 0;

    EVP_PKEY_CTX_free(ctx);

    if (!ok || outLen != 16)
        return {};

    return out;
}

GstPadProbeReturn
GStreamerSFUSession::sframeVideoDecryptProbe([[maybe_unused]] GstPad *pad,
                                              GstPadProbeInfo *info,
                                              const gpointer user_data)
{
    const auto *self = static_cast<GStreamerSFUSession *>(user_data);

    GstBuffer *buf = gst_buffer_make_writable(GST_PAD_PROBE_INFO_BUFFER(info));
    GST_PAD_PROBE_INFO_DATA(info) = buf;

    GstMapInfo map;
    if (!gst_buffer_map(buf, &map, GST_MAP_READWRITE))
        return GST_PAD_PROBE_OK;

    const size_t size = map.size;
    uint8_t *data = map.data;

    if (size < 2 + 12 + 16) {
        gst_buffer_unmap(buf, &map);
        return GST_PAD_PROBE_OK;
    }

    uint8_t kid      = data[size - 1];
    uint8_t ivLength = data[size - 2];

    if (ivLength != 12) {
        gst_buffer_unmap(buf, &map);
        return GST_PAD_PROBE_OK;
    }

    const bool isKeyframe = !(data[0] & 0x01);
    const size_t unencryptedBytes = isKeyframe ? 10 : 3;

    if (size < unencryptedBytes + 2 + 12 + 16) {
        gst_buffer_unmap(buf, &map);
        return GST_PAD_PROBE_OK;
    }

    const uint8_t *iv          = data + size - 2 - ivLength;
    const uint8_t *ciphertext  = data + unencryptedBytes;
    const size_t  ciphertextLen = size - unencryptedBytes - ivLength - 2;

    if (ciphertextLen < 16) {
        gst_buffer_unmap(buf, &map);
        return GST_PAD_PROBE_OK;
    }

    const std::vector<uint8_t> key = self->getDecryptionKey(kid);
    if (key.empty()) {
        nhlog::ui()->warn("SFU video: no decryption key for KID {} — dropping", kid);
        gst_buffer_unmap(buf, &map);
        return GST_PAD_PROBE_DROP;
    }

    const size_t tagOffset = ciphertextLen - 16;
    std::vector<uint8_t> plaintext(tagOffset);
    int decLen = 0, finalLen = 0;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    EVP_DecryptInit_ex(ctx, EVP_aes_128_gcm(), nullptr, nullptr, nullptr);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr);
    EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), iv);
    EVP_DecryptUpdate(ctx, nullptr, &decLen, data, static_cast<int>(unencryptedBytes));
    EVP_DecryptUpdate(ctx, plaintext.data(), &decLen, ciphertext, static_cast<int>(tagOffset));
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16,
                        const_cast<uint8_t *>(ciphertext + tagOffset));
    const int authOk = EVP_DecryptFinal_ex(ctx, plaintext.data() + decLen, &finalLen);
    EVP_CIPHER_CTX_free(ctx);

    if (authOk <= 0) {
        nhlog::ui()->warn("SFU video: AES-GCM auth failed KID={} isKeyframe={}", kid, isKeyframe);
        gst_buffer_unmap(buf, &map);
        return GST_PAD_PROBE_DROP;
    }

    const size_t plaintextLen = static_cast<size_t>(decLen + finalLen);
    memcpy(data + unencryptedBytes, plaintext.data(), plaintextLen);
    gst_buffer_unmap(buf, &map);
    gst_buffer_resize(buf, 0, static_cast<gssize>(unencryptedBytes + plaintextLen));

    return GST_PAD_PROBE_OK;
}

GstPadProbeReturn
GStreamerSFUSession::sframeDecryptProbe([[maybe_unused]] GstPad *pad,
                                        GstPadProbeInfo *info,
                                        const gpointer user_data)
{
    const auto *self = static_cast<GStreamerSFUSession *>(user_data);

    GstBuffer *buf = gst_buffer_make_writable(GST_PAD_PROBE_INFO_BUFFER(info));
    GST_PAD_PROBE_INFO_DATA(info) = buf;

    GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
    if (!gst_rtp_buffer_map(buf, GST_MAP_READWRITE, &rtp))
        return GST_PAD_PROBE_OK;

    const guint payloadLen = gst_rtp_buffer_get_payload_len(&rtp);
    const auto payload        = static_cast<guint8 *>(gst_rtp_buffer_get_payload(&rtp));
    const guint headerLen        = gst_rtp_buffer_get_header_len(&rtp);

    // need at least: 1 unencrypted + trailer (2 bytes)
    if (payloadLen < 3) { 
        gst_rtp_buffer_unmap(&rtp);
        return GST_PAD_PROBE_OK;
    }

    // LiveKit trailer format
    uint8_t keyIndex = payload[payloadLen - 1];
    uint8_t ivLength = payload[payloadLen - 2]; // always 12

    if (ivLength != 12 || payloadLen < static_cast<size_t>(1 + ivLength + 2)) {
        nhlog::ui()->warn("SFU: unexpected ivLength={}", ivLength);
        gst_rtp_buffer_unmap(&rtp);
        return GST_PAD_PROBE_DROP;
    }

    // Opus TOC
    constexpr size_t unencryptedBytes = 1;

    // IV: 12bytes before the 2byte trailer
    const uint8_t *iv = payload + payloadLen - 2 - ivLength;

    // Ciphertext starts after the unencrypted prefix
    const uint8_t *ciphertext = payload + unencryptedBytes;
    const size_t ciphertextLen = payloadLen - unencryptedBytes - ivLength - 2;

    if (ciphertextLen < 16) {
        nhlog::ui()->warn("SFU: ciphertext too short");
        gst_rtp_buffer_unmap(&rtp);
        return GST_PAD_PROBE_DROP;
    }

    // look up stored key for this kid
    const std::vector<uint8_t> key = self->getDecryptionKey(keyIndex);
    if (key.empty()) {
        nhlog::ui()->warn("SFU: no decryption key for KID {} — dropping", keyIndex);
        gst_rtp_buffer_unmap(&rtp);
        return GST_PAD_PROBE_DROP;
    }

    // AES-128-GCM decrypt
    const size_t tagOffset = ciphertextLen - 16;
    std::vector<uint8_t> plaintext(tagOffset);
    int decLen = 0, finalLen = 0;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    EVP_DecryptInit_ex(ctx, EVP_aes_128_gcm(), nullptr, nullptr, nullptr);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr);
    EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), iv);
    // rest is unencrypted header
    EVP_DecryptUpdate(ctx, nullptr, &decLen, payload, unencryptedBytes);
    EVP_DecryptUpdate(ctx, plaintext.data(), &decLen, ciphertext, static_cast<int>(tagOffset));
    EVP_CIPHER_CTX_ctrl(
      ctx, EVP_CTRL_GCM_SET_TAG, 16, const_cast<uint8_t *>(ciphertext + tagOffset));
    const int authOk = EVP_DecryptFinal_ex(ctx, plaintext.data() + decLen, &finalLen);
    EVP_CIPHER_CTX_free(ctx);

    if (authOk <= 0) {
        nhlog::ui()->warn("SFU: AES-GCM auth failed KID={}", keyIndex);
        gst_rtp_buffer_unmap(&rtp);
        return GST_PAD_PROBE_DROP;
    }

    const size_t plaintextLen = static_cast<size_t>(decLen + finalLen);

    // 1 unencrypted byte, plaintext, strip IV+trailer
    memcpy(payload + unencryptedBytes, plaintext.data(), plaintextLen);
    gst_rtp_buffer_unmap(&rtp);
    gst_buffer_resize(buf, 0, static_cast<gssize>(headerLen + unencryptedBytes + plaintextLen));

    return GST_PAD_PROBE_OK;
}

GstPadProbeReturn
GStreamerSFUSession::sframeEncryptProbe(GstPad * /*pad*/,
                                        GstPadProbeInfo *info,
                                        gpointer user_data)
{
    auto *self = static_cast<GStreamerSFUSession *>(user_data);


    if (!self->iceConnected_ || self->micMuted_)
        return GST_PAD_PROBE_OK;

    GstBuffer *inBuf = GST_PAD_PROBE_INFO_BUFFER(info);

    GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
    if (!gst_rtp_buffer_map(inBuf, GST_MAP_READ, &rtp))
        return GST_PAD_PROBE_OK;

    const guint  payloadLen = gst_rtp_buffer_get_payload_len(&rtp);
    const guint  headerLen  = gst_rtp_buffer_get_header_len(&rtp);
    const auto  *payloadPtr = static_cast<const guint8 *>(
                                  gst_rtp_buffer_get_payload(&rtp));

    if (payloadLen < 2) {
        gst_rtp_buffer_unmap(&rtp);
        return GST_PAD_PROBE_OK;
    }

    std::vector<uint8_t> origPayload(payloadPtr, payloadPtr + payloadLen);
    gst_rtp_buffer_unmap(&rtp);

    GstMapInfo rawMap;
    gst_buffer_map(inBuf, &rawMap, GST_MAP_READ);
    std::vector<uint8_t> rawHeader(rawMap.data, rawMap.data + headerLen);
    gst_buffer_unmap(inBuf, &rawMap);

    uint8_t kid = 0;
    std::vector<uint8_t> key = self->getEncryptionKey(kid);
    if (key.empty()) {
        nhlog::ui()->warn("SFU: encrypt probe: no outbound key yet, passing unencrypted");
        return GST_PAD_PROBE_OK;
    }

    uint8_t iv[12] = {};
    const uint64_t counter = self->encFrameCounter_.fetch_add(1, std::memory_order_relaxed);
    for (int i = 0; i < 8; ++i)
        iv[4 + (7 - i)] = static_cast<uint8_t>((counter >> (8 * i)) & 0xFF);

    static constexpr size_t kUnencrypted = 1;
    const uint8_t *plain   = origPayload.data() + kUnencrypted;
    const size_t   plainLen = origPayload.size() - kUnencrypted;

    std::vector<uint8_t> ciphertext(plainLen);
    uint8_t tag[16];
    int encLen = 0, finalLen = 0;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    EVP_EncryptInit_ex(ctx, EVP_aes_128_gcm(), nullptr, nullptr, nullptr);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr);
    EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), iv);
    // Feed Opus TOC as additional authenticated data
    EVP_EncryptUpdate(ctx, nullptr, &encLen,
                      origPayload.data(), static_cast<int>(kUnencrypted));
    EVP_EncryptUpdate(ctx, ciphertext.data(), &encLen,
                      plain, static_cast<int>(plainLen));
    EVP_EncryptFinal_ex(ctx, ciphertext.data() + encLen, &finalLen);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag);
    EVP_CIPHER_CTX_free(ctx);

    const size_t ciphertextLen = static_cast<size_t>(encLen + finalLen);

    const size_t newPayloadLen = kUnencrypted + ciphertextLen + 16 + 12 + 2;

    GstBuffer *newBuf = gst_buffer_new_allocate(nullptr, headerLen + newPayloadLen, nullptr);
    gst_buffer_copy_into(newBuf, inBuf,
                         static_cast<GstBufferCopyFlags>(
                             GST_BUFFER_COPY_METADATA | GST_BUFFER_COPY_TIMESTAMPS),
                         0, 0);

    GstMapInfo writeMap;
    gst_buffer_map(newBuf, &writeMap, GST_MAP_WRITE);
    uint8_t *out = writeMap.data;

    memcpy(out, rawHeader.data(), headerLen);   out += headerLen;
    out[0] = origPayload[0];                    out += kUnencrypted;
    memcpy(out, ciphertext.data(), ciphertextLen); out += ciphertextLen;
    memcpy(out, tag, 16);                        out += 16;
    memcpy(out, iv, 12);                         out += 12;
    *out++ = 12;
    *out   = kid;

    gst_buffer_unmap(newBuf, &writeMap);

    gst_buffer_unref(GST_PAD_PROBE_INFO_BUFFER(info));
    GST_PAD_PROBE_INFO_DATA(info) = newBuf;


    return GST_PAD_PROBE_OK;
}

bool
GStreamerSFUSession::initSubscriber(const std::string &sdp)
{
    nhlog::ui()->info("SFU: initializing subscriber pipeline");

    if (pipe_) {
        nhlog::ui()->warn("SFU: subscriber already initialized");
        return false;
    }

    GstSDPMessage *sdpMsg = nullptr;
    if (gst_sdp_message_new_from_text(sdp.c_str(), &sdpMsg) != GST_SDP_OK) {
        nhlog::ui()->error("SFU: failed to parse subscriber offer SDP");
        return false;
    }

    GstWebRTCSessionDescription *offer =
        gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_OFFER, sdpMsg);
    if (!offer) {
        nhlog::ui()->error("SFU: failed to create session description from offer");
        gst_sdp_message_free(sdpMsg);
        return false;
    }
    gst_debug_set_threshold_for_name("webrtcbin", GST_LEVEL_INFO);
    gst_debug_set_threshold_for_name("dtlssrtpdec", GST_LEVEL_INFO);
    gst_debug_set_threshold_for_name("nicesink", GST_LEVEL_INFO);

    pipe_ = gst_pipeline_new("sfu-subscriber");
    if (!pipe_) {
        nhlog::ui()->error("SFU: failed to create pipeline");
        gst_webrtc_session_description_free(offer);
        return false;
    }

    webrtc_ = gst_element_factory_make("webrtcbin", "webrtcbin");
    if (!webrtc_) {
        nhlog::ui()->error("SFU: failed to create webrtcbin");
        gst_object_unref(pipe_);
        pipe_ = nullptr;
        gst_webrtc_session_description_free(offer);
        return false;
    }

    g_object_set(webrtc_,
                 "bundle-policy", GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE,
                 nullptr);

    g_object_set(webrtc_,
                 "stun-server", STUN_SERVER,
                 nullptr);

    gst_bin_add(GST_BIN(pipe_), webrtc_);

    guint actualLatency = 0;
    g_object_get(webrtc_, "latency", &actualLatency, nullptr);
    nhlog::ui()->info("SFU: webrtcbin jitter buffer latency = {}ms", actualLatency);

    audioMixer_ = gst_element_factory_make("audiomixer", "audiomixer");
    audioSink_ = gst_element_factory_make("autoaudiosink", "audioout");

    if(!audioMixer_ || !audioSink_) {
        nhlog::ui()->error("SFU: failed to create audiomixer or autoaudiosink");
        gst_object_unref(pipe_);
        pipe_ = nullptr;
        gst_webrtc_session_description_free(offer);
        return false;
    }

    gst_bin_add_many(GST_BIN(pipe_), audioMixer_, audioSink_, nullptr);
    if (!gst_element_link(audioMixer_, audioSink_)) {
        nhlog::ui()->error("SFU: failed to link audiomixer -> autoaudiosink");
        end();
        return false;
    }

    configureTurnServers();

    g_signal_connect(webrtc_, "on-ice-candidate",
                     G_CALLBACK(onICECandidate), this);
    g_signal_connect(webrtc_, "notify::ice-connection-state",
                     G_CALLBACK(onICEConnectionState), this);
    g_signal_connect(webrtc_, "notify::ice-gathering-state",
                     G_CALLBACK(onICEGatheringState), this);
    g_signal_connect(webrtc_, "pad-added",
                     G_CALLBACK(onPadAdded), this);
    g_signal_connect(webrtc_, "notify::connection-state",
                     G_CALLBACK(onConnectionState), this);

    gst_element_set_state(pipe_, GST_STATE_READY);

    nhlog::ui()->info("SFU: offer SDP:\n{}", sdp);

    GstPromise *promise = gst_promise_new();
    g_signal_emit_by_name(webrtc_, "set-remote-description", offer, promise);
    gst_promise_wait(promise);
    gst_promise_unref(promise);
    gst_webrtc_session_description_free(offer);

    GArray *transceivers = nullptr;
    g_signal_emit_by_name(webrtc_, "get-transceivers", &transceivers);
    if (transceivers) {
        nhlog::ui()->info("SFU: found {} transceivers after remote offer", transceivers->len);
        for (guint i = 0; i < transceivers->len; i++) {
            GstWebRTCRTPTransceiver *trans =
              g_array_index(transceivers, GstWebRTCRTPTransceiver *, i);
            g_object_set(
              trans, "direction", GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_RECVONLY, nullptr);
        }
        g_array_unref(transceivers);
    } else {
        nhlog::ui()->warn("SFU: no transceivers found after setting remote offer");
    }

    for (const auto &[candidate, mlineindex] : pendingCandidates_) {
        g_signal_emit_by_name(
          webrtc_, "add-ice-candidate", static_cast<guint>(mlineindex), candidate.c_str());
    }
    pendingCandidates_.clear();

    GstPromise *answerPromise =
        gst_promise_new_with_change_func(onAnswerCreated, this, nullptr);
    g_signal_emit_by_name(webrtc_, "create-answer", nullptr, answerPromise);

    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(pipe_));
    busWatchId_ = gst_bus_add_watch(bus, onBusMessage, this);
    gst_object_unref(bus);

    GstStateChangeReturn ret = gst_element_set_state(pipe_, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_ASYNC) {
        nhlog::ui()->info("SFU: pipeline state change async, answer will be generated");
    } else if (ret != GST_STATE_CHANGE_SUCCESS) {
        nhlog::ui()->error("SFU: failed to start subscriber pipeline");
        end();
        return false;
    }

    nhlog::ui()->info("SFU: subscriber pipeline started");
    return true;
}

void
GStreamerSFUSession::onConnectionState(GstElement *webrtc, GParamSpec *, const gpointer user_data)
{
    auto *self = static_cast<GStreamerSFUSession *>(user_data);
    GstWebRTCPeerConnectionState state;
    g_object_get(webrtc, "connection-state", &state, nullptr);

    auto stateStr = "unknown";
    switch (state) {
    case GST_WEBRTC_PEER_CONNECTION_STATE_NEW:          stateStr = "new"; break;
    case GST_WEBRTC_PEER_CONNECTION_STATE_CONNECTING:   stateStr = "connecting"; break;
    case GST_WEBRTC_PEER_CONNECTION_STATE_CONNECTED:    stateStr = "connected"; break;
    case GST_WEBRTC_PEER_CONNECTION_STATE_DISCONNECTED: stateStr = "disconnected"; break;
    case GST_WEBRTC_PEER_CONNECTION_STATE_FAILED:       stateStr = "failed"; break;
    case GST_WEBRTC_PEER_CONNECTION_STATE_CLOSED:       stateStr = "closed"; break;
    }
    nhlog::ui()->info("SFU: peer connection state: {}", stateStr);

    if (state == GST_WEBRTC_PEER_CONNECTION_STATE_FAILED)
        emit self->failed(QStringLiteral("Peer connection failed"));
}
void
GStreamerSFUSession::onPubICECandidate(GstElement *, guint mlineindex, gchar *candidate, gpointer user_data)
{
    auto *self = static_cast<GStreamerSFUSession *>(user_data);
    nhlog::ui()->debug("SFU: publisher local ICE: {}", candidate);
    emit self->publisherICECandidate(std::string(candidate),
                                     std::to_string(mlineindex),
                                     static_cast<int>(mlineindex));
}
void
GStreamerSFUSession::onPubICEConnectionState(GstElement *webrtc, GParamSpec *, gpointer user_data)
{
    auto *self = static_cast<GStreamerSFUSession *>(user_data);
    GstWebRTCICEConnectionState state;
    g_object_get(webrtc, "ice-connection-state", &state, nullptr);

    switch (state) {
    case GST_WEBRTC_ICE_CONNECTION_STATE_CONNECTED:
        nhlog::ui()->info("SFU: publisher ICE connected (waiting for DTLS)");
        break;
    case GST_WEBRTC_ICE_CONNECTION_STATE_FAILED:
        nhlog::ui()->error("SFU: publisher ICE failed");
        emit self->failed(QStringLiteral("Publisher ICE connection failed"));
        break;
    default:
        break;
    }
}
gboolean
GStreamerSFUSession::onPubBusMessage(GstBus *, GstMessage *message, gpointer user_data)
{
    auto *self = static_cast<GStreamerSFUSession *>(user_data);
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
        GError *err     = nullptr;
        gchar  *dbgInfo = nullptr;
        gst_message_parse_error(message, &err, &dbgInfo);
        nhlog::ui()->error("SFU: publisher pipeline error: {} ({})",
                           err->message, dbgInfo ? dbgInfo : "none");
        emit self->failed(QString::fromUtf8(err->message));
        g_error_free(err);
        g_free(dbgInfo);
    }
    return TRUE;
}

void
GStreamerSFUSession::createAnswer()
{
    GstPromise *promise =
        gst_promise_new_with_change_func(onAnswerCreated, this, nullptr);
    g_signal_emit_by_name(webrtc_, "create-answer", nullptr, promise);
}

void
GStreamerSFUSession::onAnswerCreated(GstPromise *promise, const gpointer user_data)
{
    auto *self = static_cast<GStreamerSFUSession *>(user_data);

    nhlog::ui()->info("SFU: onAnswerCreated callback fired");

    const GstStructure *reply = gst_promise_get_reply(promise);
    if (!reply) {
        nhlog::ui()->error("SFU: onAnswerCreated - null reply from promise");
        gst_promise_unref(promise);
        return;
    }

    GstWebRTCSessionDescription *answer = nullptr;
    gst_structure_get(reply, "answer",
                      GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &answer, nullptr);
    gst_promise_unref(promise);

    if (!answer) {
        nhlog::ui()->error("SFU: failed to get answer from promise reply");
        QMetaObject::invokeMethod(self, [self]() {
            emit self->failed(QStringLiteral("Failed to create WebRTC answer"));
        }, Qt::QueuedConnection);
        return;
    }

    gchar *sdpStr = gst_sdp_message_as_text(answer->sdp);
    std::string sdp(sdpStr);
    g_free(sdpStr);

    nhlog::ui()->info("SFU: answer SDP length: {} bytes", sdp.size());

    GstPromise *localPromise = gst_promise_new();
    g_signal_emit_by_name(self->webrtc_, "set-local-description", answer, localPromise);
    gst_promise_unref(localPromise);
    gst_webrtc_session_description_free(answer);

    QMetaObject::invokeMethod(self, [self, sdp]() {
        nhlog::ui()->info("SFU: emitting subscriberAnswerCreated");
        nhlog::ui()->info("SFU: answer SDP:\n{}", sdp);
        emit self->subscriberAnswerCreated(sdp);
    }, Qt::QueuedConnection);
}

void
GStreamerSFUSession::addSubscriberICECandidate(const std::string &candidate,
                                               [[maybe_unused]] const std::string &sdpMid,
                                               int sdpMLineIndex)
{
    if (!webrtc_) {
        pendingCandidates_.push_back({candidate, sdpMLineIndex});
        return;
    }

    g_signal_emit_by_name(webrtc_, "add-ice-candidate",
                          static_cast<guint>(sdpMLineIndex),
                          candidate.c_str());
}

void
GStreamerSFUSession::onICECandidate([[maybe_unused]] GstElement *webrtc,
                                    const guint mLineIndex,
                                    gchar *candidate,
                                    const gpointer user_data)
{
    auto *self = static_cast<GStreamerSFUSession *>(user_data);
    nhlog::ui()->debug("SFU: local ICE candidate: {}", candidate);

    emit self->subscriberICECandidate(
        std::string(candidate),
        std::to_string(mLineIndex),
        static_cast<int>(mLineIndex));
}

void
GStreamerSFUSession::onICEConnectionState(GstElement *webrtc,
                                          GParamSpec *,
                                          const gpointer user_data)
{
    auto *self = static_cast<GStreamerSFUSession *>(user_data);

    GstWebRTCICEConnectionState state;
    g_object_get(webrtc, "ice-connection-state", &state, nullptr);

    switch (state) {
    case GST_WEBRTC_ICE_CONNECTION_STATE_CONNECTED:
        nhlog::ui()->info("SFU: subscriber ICE connected");
        emit self->stateChanged(QStringLiteral("connected"));
        break;
    case GST_WEBRTC_ICE_CONNECTION_STATE_FAILED:
        nhlog::ui()->error("SFU: subscriber ICE failed");
        emit self->failed(QStringLiteral("ICE connection failed"));
        break;
    case GST_WEBRTC_ICE_CONNECTION_STATE_DISCONNECTED:
        nhlog::ui()->warn("SFU: subscriber ICE disconnected");
        break;
    default:
        break;
    }
}

void
GStreamerSFUSession::onICEGatheringState(GstElement *webrtc,
                                         GParamSpec *,
                                         [[maybe_unused]] gpointer user_data)
{
    GstWebRTCICEGatheringState state;
    g_object_get(webrtc, "ice-gathering-state", &state, nullptr);

    auto stateStr = "unknown";
    switch (state) {
    case GST_WEBRTC_ICE_GATHERING_STATE_NEW:       stateStr = "new";       break;
    case GST_WEBRTC_ICE_GATHERING_STATE_GATHERING: stateStr = "gathering"; break;
    case GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE:  stateStr = "complete";  break;
    }
    nhlog::ui()->info("SFU: ICE gathering state: {}", stateStr);
}

void
GStreamerSFUSession::onPadAdded(GstElement *, GstPad *pad, const gpointer user_data)
{
    auto *self = static_cast<GStreamerSFUSession *>(user_data);

    if (GST_PAD_DIRECTION(pad) != GST_PAD_SRC)
        return;

    GstCaps *caps = gst_pad_get_current_caps(pad);
    if (!caps)
        caps = gst_pad_query_caps(pad, nullptr);
    if (!caps)
        return;

    const GstStructure *s = gst_caps_get_structure(caps, 0);
    const char *media     = gst_structure_get_string(s, "media");
    const char *encoding  = gst_structure_get_string(s, "encoding-name");
    gst_caps_unref(caps);

    // for audio subscribing
    if (media && g_strcmp0(media, "audio") == 0 &&
        (!encoding || g_ascii_strcasecmp(encoding, "OPUS") == 0))
    {
        if (!self->audioMixer_) {
            nhlog::ui()->error("SFU: audio pad arrived but audiomixer not initialized");
            return;
        }

        GstElement *queue     = gst_element_factory_make("queue",         nullptr);
        GstElement *depay     = gst_element_factory_make("rtpopusdepay",  nullptr);
        GstElement *dec       = gst_element_factory_make("opusdec",       nullptr);
        GstElement *vol       = gst_element_factory_make("volume", nullptr);
        GstElement *audiorate = gst_element_factory_make("audiorate",     nullptr);
        GstElement *convert   = gst_element_factory_make("audioconvert",  nullptr);
        GstElement *resample  = gst_element_factory_make("audioresample", nullptr);

        if (!queue || !depay || !dec || !vol || !audiorate || !convert || !resample) {
            nhlog::ui()->error("SFU: failed to create audio branch elements");
            return;
        }

        std::string padName = gst_pad_get_name(pad);
        self->audioVolumeElements_[padName] = vol;

        g_object_set(dec, "use-inband-fec", TRUE, "plc", TRUE, nullptr);

        gst_bin_add_many(GST_BIN(self->pipe_),
                         queue, depay, dec, vol, audiorate, convert, resample, nullptr);

        if (!gst_element_link_many(queue, depay, dec, vol, audiorate, convert, resample, nullptr)) {
            nhlog::ui()->error("SFU: failed to link audio branch");
            return;
        }

        GstPad *depaySinkPad = gst_element_get_static_pad(depay, "sink");
        if (depaySinkPad) {
            gst_pad_add_probe(depaySinkPad, GST_PAD_PROBE_TYPE_BUFFER,
                              sframeDecryptProbe, self, nullptr);
            gst_object_unref(depaySinkPad);
        } else {
            nhlog::ui()->error("SFU: could not get depay sink pad for decrypt probe");
        } 

        GstPad *mixerSinkPad = gst_element_request_pad_simple(self->audioMixer_, "sink_%u");
        if (!mixerSinkPad) {
            nhlog::ui()->error("SFU: failed to request audiomixer sink pad");
            return;
        }

        GstPad *resampleSrc = gst_element_get_static_pad(resample, "src");
        if (!resampleSrc || gst_pad_link(resampleSrc, mixerSinkPad) != GST_PAD_LINK_OK) {
            nhlog::ui()->error("SFU: failed to link resample -> audiomixer");
            if (resampleSrc) gst_object_unref(resampleSrc);
            gst_object_unref(mixerSinkPad);
            return;
        }
        gst_object_unref(resampleSrc);
        gst_object_unref(mixerSinkPad);

        GstPad *queueSink = gst_element_get_static_pad(queue, "sink");
        if (!queueSink || gst_pad_link(pad, queueSink) != GST_PAD_LINK_OK) {
            nhlog::ui()->error("SFU: failed to link webrtc pad to queue");
            if (queueSink) gst_object_unref(queueSink);
            return;
        }
        gst_object_unref(queueSink);

        gst_element_sync_state_with_parent(queue);
        gst_element_sync_state_with_parent(depay);
        gst_element_sync_state_with_parent(dec);
        gst_element_sync_state_with_parent(vol);
        gst_element_sync_state_with_parent(audiorate);
        gst_element_sync_state_with_parent(convert);
        gst_element_sync_state_with_parent(resample);

        nhlog::ui()->info("SFU: routed audio pad to audiomixer (with SFrame decryption)");

        QMetaObject::invokeMethod(self, [self, padName]() {
            emit self->audioStreamAdded(padName);
        }, Qt::QueuedConnection);

        return;
    }

    // TODO: implement multi stream watching
    if (media && g_strcmp0(media, "video") == 0) {
        if (self->videoItem_) {
            if (self->videoSink_) {
                self->clearVideoDisplay();
            }

            const char *depay_name = nullptr;
            const char *dec_name   = nullptr;

            if (encoding && g_ascii_strcasecmp(encoding, "VP8") == 0) {
                depay_name = "rtpvp8depay";
                dec_name   = "vp8dec";
            } else if (encoding && g_ascii_strcasecmp(encoding, "VP9") == 0) {
                depay_name = "rtpvp9depay";
                dec_name   = "vp9dec";
            } else if (encoding && g_ascii_strcasecmp(encoding, "H264") == 0) {
                depay_name = "rtph264depay";
                dec_name   = "avdec_h264";
            } else if (encoding && g_ascii_strcasecmp(encoding, "AV1") == 0) {
                depay_name = "rtpav1depay";
                dec_name   = "av1dec";
            } else {
                // Default to VP8
                depay_name = "rtpvp8depay";
                dec_name   = "vp8dec";
                nhlog::ui()->warn("SFU: unknown video encoding '{}', defaulting to VP8",
                                  encoding ? encoding : "unknown");
            }

            GstElement *queue     = gst_element_factory_make("queue", nullptr);
            GstElement *depay     = gst_element_factory_make(depay_name, nullptr);
            GstElement *dec       = gst_element_factory_make(dec_name, nullptr);
            GstElement *convert   = gst_element_factory_make("videoconvert", nullptr);
            GstElement *capsf     = gst_element_factory_make("capsfilter", nullptr);
            GstElement *glsinkbin = gst_element_factory_make("glsinkbin", nullptr);
            GstElement *sink      = gst_element_factory_make("qml6glsink", nullptr);

            if (queue && depay && dec && convert && capsf && glsinkbin && sink) {
                GstCaps *video_caps =
                  gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "I420", nullptr);
                g_object_set(capsf, "caps", video_caps, nullptr);
                gst_caps_unref(video_caps);

                g_object_set(sink, "widget", self->videoItem_, nullptr);
                g_object_set(glsinkbin, "sink", sink, nullptr);

                self->videoSink_    = glsinkbin;
                self->videoQueue_   = queue;
                self->videoDepay_   = depay;
                self->videoDec_     = dec;
                self->videoConvert_ = convert;

                gst_bin_add_many(
                  GST_BIN(self->pipe_), queue, depay, dec, convert, capsf, glsinkbin, nullptr);

                if (gst_element_link_many(queue, depay, dec, convert, capsf, glsinkbin, nullptr)) {
                    gst_element_set_state(sink, GST_STATE_READY);

                    if (QGuiApplication::platformName() == QStringLiteral("wayland")) {
                        auto context = gst_element_get_context(sink, "gst.gl.GLDisplay");
                        if (context) {
                            GstGLDisplay *display;
                            gst_context_get_gl_display(context, &display);
                        }
                    }

                    GstPad *decSinkPad = gst_element_get_static_pad(dec, "sink");
                    if (decSinkPad) {
                        gst_pad_add_probe(decSinkPad,
                                          GST_PAD_PROBE_TYPE_BUFFER,
                                          sframeVideoDecryptProbe,
                                          self,
                                          nullptr);
                        gst_object_unref(decSinkPad);
                    }

                    GstPad *queueSink = gst_element_get_static_pad(queue, "sink");
                    if (queueSink && gst_pad_link(pad, queueSink) == GST_PAD_LINK_OK) {
                        gst_object_unref(queueSink);
                        gst_element_sync_state_with_parent(queue);
                        gst_element_sync_state_with_parent(depay);
                        gst_element_sync_state_with_parent(dec);
                        gst_element_sync_state_with_parent(convert);
                        gst_element_sync_state_with_parent(capsf);
                        gst_element_sync_state_with_parent(glsinkbin);
                        nhlog::ui()->info("SFU: routed video pad to qml6glsink via glsinkbin");
                        return;
                    }
                    if (queueSink)
                        gst_object_unref(queueSink);
                }

                nhlog::ui()->error("SFU: failed to link video branch, falling back to fakesink");
                for (auto *el : {glsinkbin, capsf, convert, dec, depay, queue}) {
                    gst_element_set_state(el, GST_STATE_NULL);
                    gst_bin_remove(GST_BIN(self->pipe_), el);
                }
            } else {
                nhlog::ui()->error(
                  "SFU: failed to create video branch elements, falling back to fakesink");
                if (glsinkbin && sink) {
                    g_object_set(glsinkbin, "sink", nullptr, nullptr);
                }
                for (auto *el : {queue, depay, dec, convert, capsf, glsinkbin, sink}) {
                    if (el)
                        gst_object_unref(el);
                }
            }
        } else {
            nhlog::ui()->warn("SFU: video pad arrived but no videoItem set, using fakesink");
        }
    }

    nhlog::ui()->info("SFU: discarding non-audio pad (media={})", media ? media : "unknown");

    GstElement *fakesink = gst_element_factory_make("fakesink", nullptr);
    if (!fakesink) {
        nhlog::ui()->error("SFU: failed to create fakesink for non-audio pad");
        return;
    }
    g_object_set(fakesink, "sync", FALSE, "async", FALSE, nullptr);
    gst_bin_add(GST_BIN(self->pipe_), fakesink);

    GstPad *sinkPad = gst_element_get_static_pad(fakesink, "sink");
    if (!sinkPad || gst_pad_link(pad, sinkPad) != GST_PAD_LINK_OK) {
        nhlog::ui()->error("SFU: failed to link non-audio pad to fakesink");
        if (sinkPad) gst_object_unref(sinkPad);
        gst_bin_remove(GST_BIN(self->pipe_), fakesink);
        return;
    }
    gst_object_unref(sinkPad);
    gst_element_sync_state_with_parent(fakesink);
}

gboolean
GStreamerSFUSession::onBusMessage([[maybe_unused]] GstBus *bus,
                                  GstMessage *message,
                                  const gpointer user_data)
{
    auto *self = static_cast<GStreamerSFUSession *>(user_data);

    switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_ERROR: {
        GError *err     = nullptr;
        gchar  *dbgInfo = nullptr;
        gst_message_parse_error(message, &err, &dbgInfo);
        nhlog::ui()->error("SFU: GStreamer error: {} ({})",
                           err->message, dbgInfo ? dbgInfo : "none");
        
        GstObject *source = GST_MESSAGE_SRC(message);
        if (source && GST_IS_ELEMENT(source)) {
            const gchar *element_name = GST_ELEMENT_NAME(source);
            if (element_name && g_str_has_prefix(element_name, "qml6glsink")) {
                nhlog::ui()->warn("SFU: qml6glsink initialization failed, video display unavailable but continuing session");
                g_error_free(err);
                g_free(dbgInfo);
                return TRUE;
            }
        }
        
        // For all other errors, fail the session
        emit self->failed(QString::fromUtf8(err->message));
        g_error_free(err);
        g_free(dbgInfo);
        break;
    }
    case GST_MESSAGE_EOS:
        nhlog::ui()->info("SFU: pipeline EOS");
        break;
    default:
        break;
    }
    return TRUE;
}

bool
GStreamerSFUSession::acceptRenegotiationOffer(const std::string &sdp)
{
    if (!webrtc_) {
        nhlog::ui()->error("SFU: acceptRenegotiationOffer called but no pipeline");
        return false;
    }

    nhlog::ui()->info("SFU: accepting renegotiation offer SDP:\n{}", sdp);

    GstSDPMessage *sdpMsg = nullptr;
    if (gst_sdp_message_new_from_text(sdp.c_str(), &sdpMsg) != GST_SDP_OK) {
        nhlog::ui()->error("SFU: failed to parse renegotiation offer");
        return false;
    }

    std::unordered_map<std::string, std::string> midToMedia;
    const guint mlineCount = gst_sdp_message_medias_len(sdpMsg);
    for (guint m = 0; m < mlineCount; m++) {
        const GstSDPMedia *media = gst_sdp_message_get_media(sdpMsg, m);
        const char *mid          = gst_sdp_media_get_attribute_val(media, "mid");
        const char *mediaType    = gst_sdp_media_get_media(media);
        if (mid && mediaType)
            midToMedia[mid] = mediaType;
    }

    bool videoActive = false;
    for (guint m = 0; m < mlineCount; m++) {
        const GstSDPMedia *media = gst_sdp_message_get_media(sdpMsg, m);
        const char *mediaType = gst_sdp_media_get_media(media);
        if (g_strcmp0(mediaType, "video") == 0) {
            const char *inactive = gst_sdp_media_get_attribute_val(media, "inactive");
            if (!inactive) {
                for (guint a = 0; a < gst_sdp_media_attributes_len(media); a++) {
                    const GstSDPAttribute *attr = gst_sdp_media_get_attribute(media, a);
                    if (g_strcmp0(attr->key, "ssrc") == 0) {
                        videoActive = true;
                        break;
                    }
                }
            }
            break;
        }
    }

    if (!videoActive && videoSink_) {
        nhlog::ui()->info("SFU: video became inactive, cleaning up video pipeline");
        clearVideoDisplay();
        emit videoBecameInactive();
    }

    GstWebRTCSessionDescription *offer =
        gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_OFFER, sdpMsg);

    GstPromise *promise = gst_promise_new();
    g_signal_emit_by_name(webrtc_, "set-remote-description", offer, promise);
    gst_promise_wait(promise);
    gst_promise_unref(promise);
    gst_webrtc_session_description_free(offer);

    GstCaps *audioCaps =
      gst_caps_from_string("application/x-rtp,media=audio,encoding-name=OPUS,payload=111;"
                           "application/x-rtp,media=audio,encoding-name=RED,payload=63");

    GArray *transceivers = nullptr;
    g_signal_emit_by_name(webrtc_, "get-transceivers", &transceivers);
    if (transceivers) {
        nhlog::ui()->info("SFU: {} transceivers after renegotiation offer", transceivers->len);
        for (guint i = 0; i < transceivers->len; i++) {
            GstWebRTCRTPTransceiver *trans =
              g_array_index(transceivers, GstWebRTCRTPTransceiver *, i);

            gchar *mid = nullptr;
            g_object_get(trans, "mid", &mid, nullptr);

            std::string mediaType;
            if (mid) {
                auto it = midToMedia.find(mid);
                if (it != midToMedia.end())
                    mediaType = it->second;
                g_free(mid);
            }

            GstWebRTCRTPTransceiverDirection dir;
            g_object_get(trans, "direction", &dir, nullptr);
            nhlog::ui()->info("SFU: transceiver {} direction: {} media: {}",
                              i, static_cast<int>(dir), mediaType.empty() ? "unknown" : mediaType);

            if (mediaType == "video" && !videoActive) {
                g_object_set(trans, "direction",
                             GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_INACTIVE, nullptr);
                nhlog::ui()->info("SFU: set video transceiver to INACTIVE");
            } else {
                g_object_set(trans, "direction",
                             GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_RECVONLY, nullptr);
            }

            if (mediaType == "audio")
                g_object_set(trans, "codec-preferences", audioCaps, nullptr);
        }
        g_array_unref(transceivers);
    }
    gst_caps_unref(audioCaps);

    GstPromise *answerPromise = gst_promise_new_with_change_func(onAnswerCreated, this, nullptr);
    g_signal_emit_by_name(webrtc_, "create-answer", nullptr, answerPromise);

    return true;
}

bool
GStreamerSFUSession::initPublisher()
{
    nhlog::ui()->info("SFU: initializing publisher pipeline");

    if (pubPipe_) {
        nhlog::ui()->warn("SFU: publisher already initialized");
        return false;
    }

    pubPipe_ = gst_pipeline_new("sfu-publisher");
    if (!pubPipe_) {
        nhlog::ui()->error("SFU: failed to create publisher pipeline");
        return false;
    }

    pubWebrtc_ = gst_element_factory_make("webrtcbin", "pub-webrtcbin");
    if (!pubWebrtc_) {
        nhlog::ui()->error("SFU: failed to create publisher webrtcbin");
        gst_object_unref(pubPipe_);
        pubPipe_ = nullptr;
        return false;
    }

    g_object_set(pubWebrtc_,
                 "bundle-policy", GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE,
                 "stun-server",   STUN_SERVER,
                 nullptr);


    GstDevice *device = devices_.audioDevice();
    if (!device)
        return false;

    GstElement *src      = gst_element_factory_make("autoaudiosrc",  "mic_src");
    GstElement *audioCaps = gst_element_factory_make("capsfilter", "audio_caps");
    GstElement *convert  = gst_element_factory_make("audioconvert",  "mic_convert");
    GstElement *resample = gst_element_factory_make("audioresample", "mic_resample");
    GstElement *enc      = gst_element_factory_make("opusenc",       "mic_enc");
    GstElement *pay      = gst_element_factory_make("rtpopuspay",    "mic_pay");

    GstCaps *rawCaps =
      gst_caps_from_string("audio/x-raw,format=S16LE,channels=1,rate=48000,layout=interleaved");
    g_object_set(audioCaps, "caps", rawCaps, nullptr);
    gst_caps_unref(rawCaps);

    if (!src || !audioCaps || !convert || !resample || !enc || !pay) {
        nhlog::ui()->error("SFU: failed to create publisher audio elements");
        endPublisher();
        return false;
    }

    g_object_set(enc, "bitrate", 32000, "inband-fec", TRUE, "dtx", TRUE, nullptr);
    g_object_set(pay, "pt", 111, "ssrc", g_random_int(), nullptr);

    gst_bin_add_many(GST_BIN(pubPipe_),
                     pubWebrtc_, src, audioCaps, convert, resample, enc, pay,
                     nullptr);

    if (!gst_element_link_many(src, audioCaps, convert, resample, enc, pay, nullptr)) {
        nhlog::ui()->error("SFU: failed to link publisher audio chain");
        endPublisher();
        return false;
    }

    GstPad *webrtcSink = gst_element_request_pad_simple(pubWebrtc_, "sink_%u");
    if (!webrtcSink) {
        nhlog::ui()->error("SFU: could not get webrtcbin sink pad for publisher");
        endPublisher();
        return false;
    }

    GstWebRTCRTPTransceiver *trans = nullptr;
    g_object_get(webrtcSink, "transceiver", &trans, nullptr);
    if (!trans) {
        nhlog::ui()->error("SFU: could not get transceiver from webrtcbin sink pad");
        gst_object_unref(webrtcSink);
        endPublisher();
        return false;
    }

    publisherSsrc_ = g_random_int();

    g_object_set(pay, "ssrc", publisherSsrc_, nullptr);

    g_object_set(trans, "direction",
                 GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY, nullptr);

    GstCaps *codecCaps = gst_caps_from_string(
        "application/x-rtp,"
        "media=audio,"
        "encoding-name=OPUS,"
        "payload=111,"
        "clock-rate=48000");
    g_object_set(trans, "codec-preferences", codecCaps, nullptr);
    gst_caps_unref(codecCaps);
    gst_object_unref(trans);

    nhlog::ui()->info("SFU: configured single SENDONLY transceiver with Opus codec prefs");

    GstPad *paySrc = gst_element_get_static_pad(pay, "src");
    gst_pad_add_probe(paySrc, GST_PAD_PROBE_TYPE_BUFFER,
                      sframeEncryptProbe, this, nullptr);

    if (gst_pad_link(paySrc, webrtcSink) != GST_PAD_LINK_OK) {
        nhlog::ui()->error("SFU: failed to link rtpopuspay → webrtcbin");
        gst_object_unref(paySrc);
        gst_object_unref(webrtcSink);
        endPublisher();
        return false;
    }
    gst_object_unref(paySrc);
    gst_object_unref(webrtcSink);
    nhlog::ui()->info("SFU: encrypt probe installed, full chain assembled");

    g_signal_connect(pubWebrtc_, "on-ice-candidate",
                     G_CALLBACK(onPubICECandidate), this);
    g_signal_connect(pubWebrtc_, "notify::ice-connection-state",
                     G_CALLBACK(onPubICEConnectionState), this);
    g_signal_connect(pubWebrtc_, "notify::connection-state",
                  G_CALLBACK(onPubConnectionState), this);

    configurePubTurnServers();

    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(pubPipe_));
    pubBusWatchId_ = gst_bus_add_watch(bus, onPubBusMessage, this);
    gst_object_unref(bus);

    GstStateChangeReturn ret = gst_element_set_state(pubPipe_, GST_STATE_READY);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        nhlog::ui()->error("SFU: failed to start publisher pipeline");
        endPublisher();
        return false;
    }

    GstPromise *offerPromise =
        gst_promise_new_with_change_func(onPublisherOfferCreated, this, nullptr);
    g_signal_emit_by_name(pubWebrtc_, "create-offer", nullptr, offerPromise);


    nhlog::ui()->info("SFU: publisher pipeline started (offer pending)");
    return true;
}

void
GStreamerSFUSession::onPubConnectionState(GstElement *webrtc, GParamSpec *, gpointer user_data)
{
    auto *self = static_cast<GStreamerSFUSession *>(user_data);
    GstWebRTCPeerConnectionState state;
    g_object_get(webrtc, "connection-state", &state, nullptr);

    if (state == GST_WEBRTC_PEER_CONNECTION_STATE_CONNECTED) {
        self->iceConnected_ = true;
        nhlog::ui()->info("SFU: publisher peer connection fully established — opening mic valve");
    } else if (state == GST_WEBRTC_PEER_CONNECTION_STATE_FAILED) {
        nhlog::ui()->error("SFU: publisher peer connection failed");
        emit self->failed(QStringLiteral("Publisher peer connection failed"));
    }
}

void
GStreamerSFUSession::endPublisher()
{
    iceConnected_ = false;
    pubWebrtc_ = nullptr;

    if (pubPipe_) {
        gst_element_set_state(pubPipe_, GST_STATE_NULL);
        gst_object_unref(pubPipe_);
        pubPipe_ = nullptr;
    }
    if (pubBusWatchId_) {
        g_source_remove(pubBusWatchId_);
        pubBusWatchId_ = 0;
    }
    pendingPubCandidates_.clear();
}
void
GStreamerSFUSession::acceptPublisherAnswer(const std::string &sdp)
{
    if (!pubWebrtc_) {
        nhlog::ui()->error("SFU: acceptPublisherAnswer — no publisher pipeline");
        return;
    }

    nhlog::ui()->info("SFU: setting publisher remote answer");

    GstSDPMessage *sdpMsg = nullptr;
    if (gst_sdp_message_new_from_text(sdp.c_str(), &sdpMsg) != GST_SDP_OK) {
        nhlog::ui()->error("SFU: failed to parse publisher answer SDP");
        return;
    }

    GstWebRTCSessionDescription *answer =
        gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, sdpMsg);
    GstPromise *promise = gst_promise_new();
    g_signal_emit_by_name(pubWebrtc_, "set-remote-description", answer, promise);
    gst_promise_wait(promise);
    gst_promise_unref(promise);
    gst_webrtc_session_description_free(answer);

    GstStateChangeReturn ret = gst_element_set_state(pubPipe_, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        nhlog::ui()->error("SFU: failed to set publisher pipeline to PLAYING");
        emit failed(QStringLiteral("Publisher pipeline failed to start"));
    } else if (ret == GST_STATE_CHANGE_ASYNC) {
        nhlog::ui()->info("SFU: publisher pipeline starting asynchronously");
    } else {
        nhlog::ui()->info("SFU: publisher pipeline state set to PLAYING (sync)");
    }

    for (const auto &[candidate, mlineindex] : pendingPubCandidates_) {
        g_signal_emit_by_name(pubWebrtc_, "add-ice-candidate",
                              static_cast<guint>(mlineindex),
                              candidate.c_str());
    }
    pendingPubCandidates_.clear();

    nhlog::ui()->info("SFU: publisher answer applied");
}
void
GStreamerSFUSession::addPublisherICECandidate(const std::string &candidate,
                                              const std::string &sdpMid,
                                              int sdpMLineIndex)
{
    if (!pubWebrtc_) {
        pendingPubCandidates_.push_back({candidate, sdpMLineIndex});
        return;
    }
    g_signal_emit_by_name(pubWebrtc_, "add-ice-candidate",
                          static_cast<guint>(sdpMLineIndex),
                          candidate.c_str());
}

void
GStreamerSFUSession::configureTurnServers() const
{
    if (!webrtc_)
        return;

    for (const auto &uri : turnServers_) {
        nhlog::ui()->info("SFU: adding TURN server: {}", uri);
        gboolean result;
        g_signal_emit_by_name(webrtc_, "add-turn-server", uri.c_str(), &result);
    }

    if (turnServers_.empty())
        nhlog::ui()->warn("SFU: no TURN servers configured");
}
void
GStreamerSFUSession::configurePubTurnServers()
{
    if (!pubWebrtc_) return;
    for (const auto &uri : turnServers_) {
        gboolean ok;
        g_signal_emit_by_name(pubWebrtc_, "add-turn-server", uri.c_str(), &ok);
    }
}

void 
GStreamerSFUSession::setStreamVolume(const std::string &padName, double volume) {
    auto it = audioVolumeElements_.find(padName);
    if (it == audioVolumeElements_.end()) {
        nhlog::ui()->warn("SFU: no volume element found for pad '{}'", padName);
        return;
    }

    g_object_set(it->second, "volume", volume, nullptr);
    nhlog::ui()->info("SFU: set volume for pad '{}' to {:.2f}", padName, volume);
}

void
GStreamerSFUSession::clearVideoDisplay() {
    if (videoSink_ && pipe_) {
        nhlog::ui()->info("SFU: clearing video display and cleaning up video elements");
        
        if (videoSink_) gst_element_set_state(videoSink_, GST_STATE_NULL);
        if (videoUpload_) gst_element_set_state(videoUpload_, GST_STATE_NULL);
        if (videoConvert_) gst_element_set_state(videoConvert_, GST_STATE_NULL);
        if (videoDec_) gst_element_set_state(videoDec_, GST_STATE_NULL);
        if (videoDepay_) gst_element_set_state(videoDepay_, GST_STATE_NULL);
        if (videoQueue_) gst_element_set_state(videoQueue_, GST_STATE_NULL);
         
        if (videoSink_) gst_bin_remove(GST_BIN(pipe_), videoSink_);
        if (videoUpload_) gst_bin_remove(GST_BIN(pipe_), videoUpload_);
        if (videoConvert_) gst_bin_remove(GST_BIN(pipe_), videoConvert_);
        if (videoDec_) gst_bin_remove(GST_BIN(pipe_), videoDec_);
        if (videoDepay_) gst_bin_remove(GST_BIN(pipe_), videoDepay_);
        if (videoQueue_) gst_bin_remove(GST_BIN(pipe_), videoQueue_);
        
        videoSink_ = nullptr;
        videoQueue_ = nullptr;
        videoDepay_ = nullptr;
        videoDec_ = nullptr;
        videoConvert_ = nullptr;
        videoUpload_ = nullptr;
        
        nhlog::ui()->info("SFU: video display cleared");
    }
}
#endif // GSTREAMER_AVAILABLE

