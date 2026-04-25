#pragma once

#ifdef GSTREAMER_AVAILABLE

#include <QObject>
#include <QTimer>
#include <QWebSocket>
#include <string>
#include <vector>

#include "GStreamerSFUSession.h"
#include "livekit_rtc.pb.h"
#include "livekit_models.pb.h"

class LiveKitSession : public QObject
{
    Q_OBJECT
public:
    enum class State {
        Disconnected,
        Connecting,
        Connected,
        Publishing,
        Failed
    };

    explicit LiveKitSession(QObject *parent = nullptr);
    ~LiveKitSession() override;

    void connect(const QString &url, const QString &jwt);
    void disconnect();
    void publishMicrophone();
    void toggleMicMute();
    void setMicMuted(bool mute);
    void setTurnServers(const std::vector<std::string> &uris,
                                         const std::string &username,
                                         const std::string &credential);

    State state() const { return state_; }

    std::vector<std::string> buildTurnUris() const;

    void setDecryptionKey(uint8_t kid, const std::vector<uint8_t> &rawKey);

    GStreamerSFUSession *sfu_session() const { return sfuSession_; }
    bool isMicMuted() const { return micMuted_; }

signals:
    void connected();
    void disconnected();
    void stateChanged(State state);
    void participantConnected(const QString &identity);
    void participantDisconnected(const QString &identity);
    void error(const QString &message);
    void publisherPipelineReady();

private slots:
    void onWebSocketConnected();
    void onWebSocketDisconnected();
    void onBinaryMessageReceived(const QByteArray &message);
    void onWebSocketError(QAbstractSocket::SocketError error);
    void onPingTimer();

private:
    void setState(State state);
    void sendSignalRequest(const livekit::SignalRequest &request);
    void handleJoin(const livekit::JoinResponse &join);
    void handleOffer(const livekit::SessionDescription &offer);
    void handleAnswer(const livekit::SessionDescription &answer);
    void handleTrickle(const livekit::TrickleRequest &trickle);
    void handleParticipantUpdate(const livekit::ParticipantUpdate &update);
    void handleTrackPublished(const livekit::TrackPublishedResponse &published);
    void handleLeave(const livekit::LeaveRequest &leave);
    void sendAnswer(const std::string &sdp);
    void sendICECandidate(const std::string &candidate,
                          const std::string &sdpMid,
                          int sdpMLineIndex,
                          livekit::SignalTarget target);
    void sendAddTrack(const std::string &cid,
                      const std::string &name,
                      livekit::TrackType type,
                      livekit::Encryption_Type encryption = livekit::Encryption_Type_GCM);
    void sendMuteTrack(const std::string &sid, bool muted);
    void sendPing();

    void flushPendingDecryptionKeys();

    QWebSocket webSocket_;

    State state_ = State::Disconnected;
    QString serverUrl_;
    QString jwt_;

    std::string localParticipantIdentity_;
    std::string localParticipantSid_;
    std::string roomName_;
    std::string micTrackCid_;
    std::string micTrackSid_;

    std::vector<std::string> turnUris_;
    std::string turnUsername_;
    std::string turnCredential_;

    bool micMuted_ = false;
    QTimer pingTimer_;
    int pingInterval_ = 10000;

    bool publisherSignalsConnected_ = false;
    bool wantToPublish_ = false;

    GStreamerSFUSession *sfuSession_ = nullptr;
    std::map<uint8_t, std::vector<uint8_t>> pendingDecryptionKeys_;
};

#endif // GSTREAMER_AVAILABLE
