#pragma once

#include "TimelineModel.h"
#include "WebRTCSession.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QUrlQuery>
#include <mtx/responses/common.hpp>
#include <mtx/responses/matrix_openid_token.hpp>
#include <mtx/responses/matrixrtc.hpp>
#include <mtx/responses/turn_server.hpp>
#include <qnetworkaccessmanager.h>
#include <qnetworkreply.h>

#include "voip/LiveKitSession.h"

class MatrixRTCSession : public QObject {
    Q_OBJECT

    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(webrtc::ScreenShareType screenShareType READ screenShareType NOTIFY screenShareChanged)
    Q_PROPERTY(bool screenShareReady READ screenShareReady NOTIFY screenShareChanged)
    Q_PROPERTY(bool isOnCall READ isOnCall NOTIFY isOnCallChanged)
    Q_PROPERTY(int callState READ callState NOTIFY callStateChanged)
    Q_PROPERTY(bool isMicMuted READ isMicMuted NOTIFY micMutedChanged)
    Q_PROPERTY(int callType READ callType CONSTANT)
    Q_PROPERTY(bool isStreaming READ isStreaming NOTIFY isStreamingChanged)
    Q_PROPERTY(QStringList participants READ participants NOTIFY participantsChanged)
    Q_PROPERTY(QStringList streamingParticipants READ streamingParticipants NOTIFY streamingParticipantsChanged)
public:
    static MatrixRTCSession *create(QQmlEngine *qmlEngine, QJSEngine *jsEngine);
    MatrixRTCSession(QObject *);

    LiveKitSession* livekitSession() const { return livekitSession_; }
    
    Q_INVOKABLE void setParticipantVolume(const QString &identity, double volume);
    Q_INVOKABLE double getSavedParticipantVolume(const QString &fullUserId);
    
    void publishMicrophone();

    void shareScreen();

    void join(const std::string &roomId,
                               const std::string &userId,
                               const std::string &deviceI,
                               TimelineModel* timelineModel);
    void leave();

    bool isActive() const { return isActive_; }

    bool isStreaming() const { return isStreaming_; }
    void storePendingDecryptionKey(const uint8_t kid, const std::vector<uint8_t>& rawKey)
    {
        pendingDecryptionKeys_[kid] = rawKey;
    }

    TimelineModel* getCurrentTimeline() const { return currentTimeline_; }

    void addUsedKID(uint8_t kid)
    {
        usedKIDs_.insert(kid);
    }
    webrtc::ScreenShareType screenShareType() const { return screenShareType_; }
    bool screenShareReady() const;

    bool isOnCall() const { return isActive_; }
    int callState() const { return callState_;};
    bool isMicMuted() const { return livekitSession_ ? livekitSession_->isMicMuted() : false; }
    int callType() const { return static_cast<int>(webrtc::CallType::VOICE); }
    QStringList participants() const { return participantsList_; }

    QStringList streamingParticipants() const { return streamingParticipantsList_; }

signals:
    void joined();
    void left();
    void error(const QString &message);
    void participantJoined(const QString &userId);
    void participantLeft(const QString &userId);

    void screenShareChanged();

    void isOnCallChanged();
    void callStateChanged();
    void micMutedChanged();
    void isStreamingChanged();

    void participantsChanged();

    void streamingParticipantsChanged();

public slots:
    void startScreenShare(const QString &roomid, unsigned int windowIndex = 0);
    void onCredentialsReceived(QNetworkReply *reply);
    void onLiveKitConnected();
    void onLiveKitError(const QString &message);
    void refreshMembership();
    void rotateEncryptionKey();
    void closeScreenShare();
    QStringList screenShareTypeList();
    void setScreenShareType(unsigned int index);

    void setupScreenShareXDP();
    void toggleMicMute();

private:
    void sendMembershipEvent(bool leave = false);
    void fetchOpenidToken();
    void requestLiveKitJWT(const mtx::responses::MatrixOpenidToken &openIDToken);
    void getHomeserverLivekitBackend();

    void sendEncryptionKeyToUser(const std::string &matrixUserId,
                                 uint8_t kid,
                                 const std::vector<uint8_t> &rawKeyMaterial);
    void sendEncryptionKeyToAllParticipants(uint8_t kid,
                                            const std::vector<uint8_t> &rawKeyMaterial);

    void setCallState(int newState);
    
    double getSavedParticipantVolume(const std::string &fullUserId);
    void saveParticipantVolume(const std::string &fullUserId, double volume);

    std::set<std::string> activeParticipantUserIds_;

    QStringList participantsList_;

    uint8_t currentEncKid_ = 0;
    std::vector<uint8_t> currentEncKeyMaterial_;

    int callState_ = static_cast<int>(webrtc::State::DISCONNECTED);

    QTimer keyRotationTimer_;

    bool isStreaming_ = false;

    std::vector<QString> streamingCurrently_;
    QStringList streamingParticipantsList_;

    std::string roomId_;
    std::string userId_;
    std::string deviceId_;
    std::string stateKey_;
    QNetworkAccessManager *nam_ = nullptr;
    bool isActive_ = false;
    bool sfuConnectionsMade_ = false;

    mtx::responses::TurnServer turnServers_;
    LiveKitSession *livekitSession_ = nullptr;
    QTimer membershipRefreshTimer_;

    TimelineModel* currentTimeline_;

    std::map<uint8_t, std::vector<uint8_t>> pendingDecryptionKeys_;
    std::set<uint8_t> usedKIDs_;
    webrtc::ScreenShareType screenShareType_;

    std::string livekitEndpoint_ = "";
    
    std::map<std::string, double> participantVolumes_;

};
