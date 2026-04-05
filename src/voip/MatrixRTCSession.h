#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
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

public:
    explicit MatrixRTCSession(QObject *parent = nullptr);
    ~MatrixRTCSession() override;

    static MatrixRTCSession* instance();

    LiveKitSession* livekitSession() const { return livekitSession_; }

    void join(const std::string &roomId,
                               const std::string &userId,
                               const std::string &deviceI);
    void leave();

    bool isActive() const { return isActive_; }

    void storePendingDecryptionKey(const uint8_t kid, const std::vector<uint8_t>& rawKey)
    {
        pendingDecryptionKeys_[kid] = rawKey;
    }

signals:
    void joined();
    void left();
    void error(const QString &message);
    void participantJoined(const QString &userId);
    void participantLeft(const QString &userId);

private slots:
    void onCredentialsReceived(QNetworkReply *reply);
    void onLiveKitConnected();
    void onLiveKitError(const QString &message);
    void refreshMembership();

private:
    void sendMembershipEvent(bool leave = false);
    void fetchOpenidToken();
    void requestLiveKitJWT(const mtx::responses::MatrixOpenidToken &openIDToken);

    std::string roomId_;
    std::string userId_;
    std::string deviceId_;
    std::string stateKey_;
    QNetworkAccessManager *nam_ = nullptr;
    bool isActive_ = false;

    mtx::responses::TurnServer turnServers_;
    LiveKitSession *livekitSession_ = nullptr;
    QTimer membershipRefreshTimer_;

    std::map<uint8_t, std::vector<uint8_t>> pendingDecryptionKeys_;

    static MatrixRTCSession *instance_;
};
