#ifdef GSTREAMER_AVAILABLE

#include "GStreamerSFUSession.h"
#include "Logging.h"
#include "ChatPage.h"
#include "UserSettingsPage.h"

static constexpr auto STUN_SERVER = "stun://turn.matrix.org:3478";

GStreamerSFUSession::GStreamerSFUSession(QObject *parent)
  : QObject(parent)
{
}

GStreamerSFUSession::~GStreamerSFUSession()
{
    end();
}

void
GStreamerSFUSession::end()
{
    audioSink_  = nullptr;
    if (pipe_) {
        gst_element_set_state(pipe_, GST_STATE_NULL);
        gst_object_unref(pipe_);
        pipe_   = nullptr;
        webrtc_ = nullptr;
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
    return false;
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

    GstClock *clock = gst_system_clock_obtain();
    gst_pipeline_use_clock(GST_PIPELINE(pipe_), clock);
    gst_object_unref(clock);

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
            GstWebRTCRTPTransceiver *trans = g_array_index(transceivers, GstWebRTCRTPTransceiver *, i);
            g_object_set(trans, "direction", GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_RECVONLY, nullptr);
        }
        g_array_unref(transceivers);
    } else {
        nhlog::ui()->warn("SFU: no transceivers found after setting remote offer");
    }

    for (const auto &[candidate, mlineindex] : pendingCandidates_) {
        g_signal_emit_by_name(webrtc_, "add-ice-candidate", static_cast<guint>(mlineindex), candidate.c_str());
    }
    pendingCandidates_.clear();

    GstPromise *answerPromise = gst_promise_new_with_change_func(onAnswerCreated, this, nullptr);
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
GStreamerSFUSession::onConnectionState(GstElement *webrtc,
                                        GParamSpec *,
                                        gpointer user_data)
{
    auto *self = static_cast<GStreamerSFUSession *>(user_data);
    GstWebRTCPeerConnectionState state;
    g_object_get(webrtc, "connection-state", &state, nullptr);

    const char *stateStr = "unknown";
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
GStreamerSFUSession::createAnswer()
{
    GstPromise *promise = gst_promise_new_with_change_func(onAnswerCreated, this, nullptr);
    g_signal_emit_by_name(webrtc_, "create-answer", nullptr, promise);
}
void
GStreamerSFUSession::onAnswerCreated(GstPromise *promise, gpointer user_data)
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
                                               const std::string &sdpMid,
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
GStreamerSFUSession::onICECandidate(GstElement *,
                                     guint mlineindex,
                                     gchar *candidate,
                                     gpointer user_data)
{
    auto *self = static_cast<GStreamerSFUSession *>(user_data);
    nhlog::ui()->debug("SFU: local ICE candidate: {}", candidate);

    emit self->subscriberICECandidate(
        std::string(candidate),
        std::to_string(mlineindex),
        static_cast<int>(mlineindex));
}

void
GStreamerSFUSession::onICEConnectionState(GstElement *webrtc,
                                           GParamSpec *,
                                           gpointer user_data)
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
                                          gpointer user_data)
{
    GstWebRTCICEGatheringState state;
    g_object_get(webrtc, "ice-gathering-state", &state, nullptr);

    const char *stateStr = "unknown";
    switch (state) {
        case GST_WEBRTC_ICE_GATHERING_STATE_NEW:       stateStr = "new"; break;
        case GST_WEBRTC_ICE_GATHERING_STATE_GATHERING: stateStr = "gathering"; break;
        case GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE:  stateStr = "complete"; break;
    }
    nhlog::ui()->info("SFU: ICE gathering state: {}", stateStr);
}

void GStreamerSFUSession::onPadAdded(GstElement *, GstPad *pad, gpointer user_data)
{
    auto *self = static_cast<GStreamerSFUSession *>(user_data);

    if (GST_PAD_DIRECTION(pad) != GST_PAD_SRC)
        return;

    GstCaps *caps = gst_pad_get_current_caps(pad);
    if (!caps) caps = gst_pad_query_caps(pad, nullptr);
    if (!caps || gst_caps_is_empty(caps) || gst_caps_is_any(caps)) {
        if (caps) gst_caps_unref(caps);
        return;
    }

    const GstStructure *s = gst_caps_get_structure(caps, 0);
    const char *media = gst_structure_get_string(s, "media");
    gst_caps_unref(caps);

    if (!media || g_strcmp0(media, "audio") != 0)
        return;

    GstElement *depay    = gst_element_factory_make("rtpopusdepay", nullptr);
    GstElement *dec      = gst_element_factory_make("opusdec", nullptr);
    GstElement *conv     = gst_element_factory_make("audioconvert", nullptr);
    GstElement *resample = gst_element_factory_make("audioresample", nullptr);
    GstElement *sink     = gst_element_factory_make("pulsesink", nullptr);
    if (!sink)
        sink = gst_element_factory_make("autoaudiosink", nullptr);

    if (!depay || !dec || !conv || !resample || !sink) {
        nhlog::ui()->error("SFU: failed to create audio elements");
        return;
    }

    g_object_set(dec, "use-inband-fec", TRUE, nullptr);

    gst_bin_add_many(GST_BIN(self->pipe_), depay, dec, conv, resample, sink, nullptr);

    if (!gst_element_link_many(depay, dec, conv, resample, sink, nullptr)) {
        nhlog::ui()->error("SFU: failed to link audio chain");
        return;
    }

    GstPad *depaySink = gst_element_get_static_pad(depay, "sink");
    if (!depaySink || gst_pad_link(pad, depaySink) != GST_PAD_LINK_OK)
        nhlog::ui()->error("SFU: failed to link webrtc pad to depayloader");
    if (depaySink) gst_object_unref(depaySink);

    gst_element_sync_state_with_parent(depay);
    gst_element_sync_state_with_parent(dec);
    gst_element_sync_state_with_parent(conv);
    gst_element_sync_state_with_parent(resample);
    gst_element_sync_state_with_parent(sink);

    nhlog::ui()->info("SFU: audio pipeline connected");
}

void
GStreamerSFUSession::onDecodebinPadAdded(GstElement *,
                                          GstPad *pad,
                                          gpointer user_data)
{
    auto *pipe = static_cast<GstElement *>(user_data);

    GstCaps *caps = gst_pad_get_current_caps(pad);
    if (!caps)
        caps = gst_pad_query_caps(pad, nullptr);

    gchar *caps_str = gst_caps_to_string(caps);
    nhlog::ui()->info("Incoming audio caps: {}", caps_str);
    g_free(caps_str);
    gst_caps_unref(caps);

    const gchar *name = gst_structure_get_name(gst_caps_get_structure(caps, 0));
    nhlog::ui()->info("SFU: decoded pad type: {}", name);

    GstElement *sink = nullptr;

    if (g_str_has_prefix(name, "audio/")) {
        GstElement *queue    = gst_element_factory_make("queue", nullptr);
        GstElement *convert  = gst_element_factory_make("audioconvert", nullptr);
        GstElement *resample = gst_element_factory_make("audioresample", nullptr);
        sink                 = gst_element_factory_make("pulsesink", nullptr);

        if (!queue || !convert || !resample || !sink) {
            nhlog::ui()->error("SFU: failed to create audio sink elements");
            gst_caps_unref(caps);
            return;
        }

        gst_bin_add_many(GST_BIN(pipe), queue, convert, resample, sink, nullptr);
        gst_element_link_many(queue, convert, resample, sink, nullptr);
        gst_element_sync_state_with_parent(sink);
        gst_element_sync_state_with_parent(resample);
        gst_element_sync_state_with_parent(convert);
        gst_element_sync_state_with_parent(queue);

        GstPad *sinkpad = gst_element_get_static_pad(queue, "sink");
        if (GST_PAD_LINK_FAILED(gst_pad_link(pad, sinkpad)))
            nhlog::ui()->error("SFU: failed to link audio to sink");
        gst_object_unref(sinkpad);

    } else if (g_str_has_prefix(name, "video/")) {
        // TODO: Implement video stream receiving
        nhlog::ui()->info("SFU: video track received (not rendering yet)");
        sink = gst_element_factory_make("fakesink", nullptr);
        if (sink) {
            gst_bin_add(GST_BIN(pipe), sink);
            gst_element_sync_state_with_parent(sink);
            GstPad *sinkpad = gst_element_get_static_pad(sink, "sink");
            gst_pad_link(pad, sinkpad);
            gst_object_unref(sinkpad);
        }
    }

    gst_caps_unref(caps);
}

gboolean
GStreamerSFUSession::onBusMessage(GstBus *,
                                   GstMessage *message,
                                   gpointer user_data)
{
    auto *self = static_cast<GStreamerSFUSession *>(user_data);

    switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_ERROR: {
        GError *err     = nullptr;
        gchar *dbg_info = nullptr;
        gst_message_parse_error(message, &err, &dbg_info);
        nhlog::ui()->error("SFU: GStreamer error: {} ({})",
                           err->message, dbg_info ? dbg_info : "none");
        emit self->failed(QString::fromUtf8(err->message));
        g_error_free(err);
        g_free(dbg_info);
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

    GstWebRTCSessionDescription *offer =
        gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_OFFER, sdpMsg);

    GstPromise *promise = gst_promise_new();
    g_signal_emit_by_name(webrtc_, "set-remote-description", offer, promise);
    gst_promise_wait(promise);
    gst_promise_unref(promise);
    gst_webrtc_session_description_free(offer);

    GArray *transceivers = nullptr;
    g_signal_emit_by_name(webrtc_, "get-transceivers", &transceivers);
    if (transceivers) {
        nhlog::ui()->info("SFU: {} transceivers after renegotiation offer",
                          transceivers->len);
        for (guint i = 0; i < transceivers->len; i++) {
            GstWebRTCRTPTransceiver *trans =
                g_array_index(transceivers, GstWebRTCRTPTransceiver *, i);
            GstWebRTCRTPTransceiverDirection dir;
            g_object_get(trans, "direction", &dir, nullptr);
            nhlog::ui()->info("SFU: transceiver {} direction: {}", i,
                              static_cast<int>(dir));
            g_object_set(trans, "direction",
                         GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_RECVONLY, nullptr);
        }
        g_array_unref(transceivers);
    }

    GstPromise *answerPromise =
        gst_promise_new_with_change_func(onAnswerCreated, this, nullptr);
    g_signal_emit_by_name(webrtc_, "create-answer", nullptr, answerPromise);

    return true;
}
void
GStreamerSFUSession::configureTurnServers()
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

#endif // GSTREAMER_AVAILABLE
