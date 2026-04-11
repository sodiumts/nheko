#pragma once

#include <glib.h>
#ifdef GSTREAMER_AVAILABLE

#include "CallDevices.h"

#include <QObject>
#include <map>
#include <shared_mutex>
#include <string>
#include <vector>

extern "C" {
#include "gst/gst.h"
#include "gst/sdp/sdp.h"
#define GST_USE_UNSTABLE_API
#include "gst/webrtc/webrtc.h"
}

class GStreamerSFUSession : public QObject {
    Q_OBJECT

public:
    explicit GStreamerSFUSession(QObject* parent = nullptr);
    ~GStreamerSFUSession() override;

    bool initSubscriber(const std::string& sdp);
    void addSubscriberICECandidate(const std::string& candidate,
        const std::string& sdpMid,
        int sdpMLineIndex);
    bool acceptRenegotiationOffer(const std::string& sdp);

    bool initPublisher();
    void endPublisher();
    void acceptPublisherAnswer(const std::string &sdp);
    void addPublisherICECandidate(const std::string& candidate,
        const std::string& sdpMid,
        int sdpMLineIndex);

    void setTurnServers(const std::vector<std::string>& uris) { turnServers_ = uris; }
    void end();
    bool toggleMicMute();

    void setDecryptionKey(uint8_t kid, const std::vector<uint8_t>& rawKey);

    void setTrackCid(const std::string &cid) { trackCid_ = cid; }

    std::vector<uint8_t> generateEncryptionKeyMaterial(uint8_t kid = 0);


signals:
    void subscriberAnswerCreated(const std::string& sdp);
    void subscriberICECandidate(const std::string& candidate,
        const std::string& sdpMid,
        int sdpMLineIndex);

    void publisherOfferCreated(const std::string &sdp);
    void publisherICECandidate(const std::string& candidate,
        const std::string& sdpMid,
        int sdpMLineIndex);

    void stateChanged(const QString& state);
    void failed(const QString& reason);
    void publisherPipelineReady();

private:
    static void onICECandidate(GstElement* webrtc,
        guint mLineIndex,
        gchar* candidate,
        gpointer user_data);
    static void onICEConnectionState(GstElement* webrtc,
        GParamSpec*,
        gpointer user_data);
    static void onICEGatheringState(GstElement* webrtc,
        GParamSpec*,
        gpointer user_data);
    static void onPadAdded(GstElement* webrtc,
        GstPad* pad,
        gpointer user_data);
    static gboolean onBusMessage(GstBus* bus,
        GstMessage* message,
        gpointer user_data);

    static void onAnswerCreated(GstPromise* promise, gpointer user_data);
    static void onConnectionState(GstElement* webrtc, GParamSpec*, gpointer user_data);

    static void onPubICECandidate(GstElement *, guint, gchar *, gpointer);
    static void onPubICEConnectionState(GstElement *, GParamSpec *, gpointer);
    static gboolean onPubBusMessage(GstBus *, GstMessage *, gpointer);
    static void onPublisherOfferCreated(GstPromise *, gpointer);
    static void onPubConnectionState(GstElement *webrtc, GParamSpec *, gpointer user_data);

    std::vector<uint8_t> getDecryptionKey(uint8_t kid) const;
    std::vector<uint8_t> getEncryptionKey(uint8_t &outKid) const;

    static std::vector<uint8_t> deriveMediaKey(const std::vector<uint8_t>& rawKey);
    static GstPadProbeReturn sframeDecryptProbe(GstPad* pad, GstPadProbeInfo* info, gpointer user_data);
    static GstPadProbeReturn sframeEncryptProbe(GstPad *, GstPadProbeInfo *, gpointer);

    void createAnswer();
    void configureTurnServers() const;
    void configurePubTurnServers();

    GstElement* pipe_ = nullptr;
    GstElement* webrtc_ = nullptr;
    GstElement* audioMixer_ { nullptr };
    GstElement* audioMixerConvert_ { nullptr };
    GstElement* audioMixerResample_ { nullptr };
    GstElement* audioSink_ { nullptr };
    guint busWatchId_ = 0;

    std::vector<std::pair<std::string, int>> pendingCandidates_;

    GstElement *pubPipe_ = nullptr;
    GstElement *pubWebrtc_ = nullptr;
    //GstElement *pubValve_ = nullptr;
    guint pubBusWatchId_ = 0;
    bool micMuted_ = false;
    bool iceConnected_ = false;

    std::vector<std::pair<std::string, int>> pendingPubCandidates_;

    std::vector<std::string> turnServers_;

    mutable std::shared_mutex keyMutex_;
    std::map<uint8_t, std::vector<uint8_t>> decryptionKeys_;

    std::string trackCid_;
    guint publisherSsrc_;

    mutable std::shared_mutex encKeyMutex_;
    std::map<uint8_t, std::vector<uint8_t>> encryptionKeys_;
    uint8_t currentEncKid_ = 0;
    std::atomic<uint64_t> encFrameCounter_{0};



    CallDevices &devices_;
};

#endif // GSTREAMER_AVAILABLE
