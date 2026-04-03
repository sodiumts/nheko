#pragma once

#ifdef GSTREAMER_AVAILABLE

#include <QObject>
#include <string>
#include <vector>

extern "C" {
#include "gst/gst.h"
#include "gst/sdp/sdp.h"
#define GST_USE_UNSTABLE_API
#include "gst/webrtc/webrtc.h"
}

class GStreamerSFUSession : public QObject
{
    Q_OBJECT

public:
    explicit GStreamerSFUSession(QObject *parent = nullptr);
    ~GStreamerSFUSession() override;

    bool initSubscriber(const std::string &sdp);
    void addSubscriberICECandidate(const std::string &candidate,
                                   const std::string &sdpMid,
                                   int sdpMLineIndex);

    void setTurnServers(const std::vector<std::string> &uris) { turnServers_ = uris; }
    void end();
    bool toggleMicMute();
    bool acceptRenegotiationOffer(const std::string &sdp);

signals:
    void subscriberAnswerCreated(const std::string &sdp);
    void subscriberICECandidate(const std::string &candidate,
                                const std::string &sdpMid,
                                int sdpMLineIndex);
    void stateChanged(const QString &state);
    void failed(const QString &reason);

private:
    static void onICECandidate(GstElement *webrtc,
                                guint mlineindex,
                                gchar *candidate,
                                gpointer user_data);
    static void onICEConnectionState(GstElement *webrtc,
                                      GParamSpec *,
                                      gpointer user_data);
    static void onICEGatheringState(GstElement *webrtc,
                                     GParamSpec *,
                                     gpointer user_data);
    static void onPadAdded(GstElement *webrtc,
                            GstPad *pad,
                            gpointer user_data);
    static void onDecodebinPadAdded(GstElement *decodebin,
                                     GstPad *pad,
                                     gpointer pipe);
    static gboolean onBusMessage(GstBus *bus,
                                  GstMessage *message,
                                  gpointer user_data);

    static void onAnswerCreated(GstPromise *promise, gpointer user_data);
    static void onConnectionState(GstElement *webrtc, GParamSpec *, gpointer user_data);


    void createAnswer();
    GstElement *createAudioSinkChain();
    void configureTurnServers();

    GstElement *pipe_    = nullptr;
    GstElement *webrtc_  = nullptr;
    guint busWatchId_    = 0;

    GstElement *audioMixer_ = nullptr;
    GstElement *audioSink_  = nullptr;

    std::vector<std::string> turnServers_;
    std::vector<std::pair<std::string, int>> pendingCandidates_;
};

#endif // GSTREAMER_AVAILABLE
