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

public:
    explicit MatrixRTCSession(QObject *parent = nullptr);
    ~MatrixRTCSession() override;

    static MatrixRTCSession* instance();

    LiveKitSession* livekitSession() const { return livekitSession_; }

    void publishMicrophone();

    void shareScreen();

    void join(const std::string &roomId,
                               const std::string &userId,
                               const std::string &deviceI,
                               TimelineModel* timelineModel);
    void leave();

    bool isActive() const { return isActive_; }

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

    //void onCallMemberEvent(const std::string& userId, const std::string& deviceId);

signals:
    void joined();
    void left();
    void error(const QString &message);
    void participantJoined(const QString &userId);
    void participantLeft(const QString &userId);

    void screenShareChanged();

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

    std::optional<mtx::events::state::CallMember> findActiveCallMember(TimelineModel *timelineModel);

    std::set<std::string> activeParticipantUserIds_;
    uint8_t currentEncKid_ = 0;
    std::vector<uint8_t> currentEncKeyMaterial_;

    QTimer keyRotationTimer_;

    std::string roomId_;
    std::string userId_;
    std::string deviceId_;
    std::string stateKey_;
    QNetworkAccessManager *nam_ = nullptr;
    bool isActive_ = false;

    mtx::responses::TurnServer turnServers_;
    LiveKitSession *livekitSession_ = nullptr;
    QTimer membershipRefreshTimer_;

    TimelineModel* currentTimeline_;

    std::map<uint8_t, std::vector<uint8_t>> pendingDecryptionKeys_;
    std::set<uint8_t> usedKIDs_;
    //std::unordered_map<std::string, std::string> participantDevice_;
    webrtc::ScreenShareType screenShareType_;

    std::string livekitEndpoint_ = "";

    static MatrixRTCSession *instance_;
};
