// src/voip/MatrixRTCSession.cpp
#include "voip/MatrixRTCSession.h"
#include <mtx/events/matrixrtc.hpp>
#include <mtx/responses/matrix_openid_token.hpp>
#include <mtx/responses/matrixrtc.hpp>
#include <mtx/responses/turn_server.hpp>
#include <qjsonobject.h>

#include "Logging.h"
#include "MatrixClient.h"
#include "TimelineModel.h"

MatrixRTCSession* MatrixRTCSession::instance_ = nullptr;

MatrixRTCSession* MatrixRTCSession::instance() {
    Q_ASSERT(instance_ != nullptr);
    return instance_;
}

MatrixRTCSession::MatrixRTCSession(QObject *parent)
  : QObject(parent)
{
    connect(&membershipRefreshTimer_, &QTimer::timeout,
            this, &MatrixRTCSession::refreshMembership);
    instance_ = this;
}

MatrixRTCSession::~MatrixRTCSession() {
    instance_ = nullptr;
}


void MatrixRTCSession::fetchOpenidToken() {
    http::client()->get_matrix_openid_token(
        [this](const mtx::responses::MatrixOpenidToken &token, mtx::http::RequestErr err) {
            if (err) {
                nhlog::net()->error("Failed to get OpenID token: status={}, error={}",
                    err->status_code, err->matrix_error.error);
                emit error("Failed to obtain OpenID token");
                leave();
                return;
            }
            nhlog::net()->info("OpenID token obtained, requesting LiveKit JWT");
            QMetaObject::invokeMethod(this, [this, token]() {
                requestLiveKitJWT(token);
            }, Qt::QueuedConnection);
        });
}
void MatrixRTCSession::requestLiveKitJWT(const mtx::responses::MatrixOpenidToken &openIDToken) {
    if (!nam_) {
        nam_ = new QNetworkAccessManager(this);
        connect(nam_, &QNetworkAccessManager::finished, this, &MatrixRTCSession::onCredentialsReceived);
    }
    // TODO: Change this to no longer be a static url as well as make it refresh the token each time
    // livekit requests to refresh
    const QUrl url("https://livekit.ernests.id.lv/get_token");
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QJsonObject payload;
    payload["room_id"] = QString::fromStdString(roomId_);
    payload["slot_id"] = "m.call#ROOM"; // No idea why, element sets it like this, and it does then connect to the same one as an element client

    QJsonObject openidObj;
    openidObj["matrix_server_name"] = QString::fromStdString(openIDToken.matrix_server_name);
    openidObj["access_token"] = QString::fromStdString(openIDToken.access_token);
    openidObj["token_type"] = QString::fromStdString(openIDToken.token_type);
    payload["openid_token"] = openidObj;

    QJsonObject memberObj;
    memberObj["id"] = "xyz";
    memberObj["claimed_device_id"] = QString::fromStdString(deviceId_);
    memberObj["claimed_user_id"] = QString::fromStdString(userId_);
    payload["member"] = memberObj;

    const QByteArray postData = QJsonDocument(payload).toJson();

    nam_->post(request, postData);
}

void MatrixRTCSession::join(const std::string &roomId,
                           const std::string &userId,
                           const std::string &deviceI)
{
    roomId_ = roomId;
    userId_ = userId;;
    deviceId_ = deviceI;
    stateKey_ = "_" + userId + "_" + deviceI + "_m.call";
    isActive_ = true;

    http::client()->get_turn_server([this](const mtx::responses::TurnServer &turn, mtx::http::RequestErr err) {
        if (!err && !turn.uris.empty()) {
            turnServers_ = turn;
        }
        sendMembershipEvent(false);
        fetchOpenidToken();
    });
}

void MatrixRTCSession::onCredentialsReceived(QNetworkReply *reply)
{
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        nhlog::net()->error("Failed to fetch LiveKit token: {}", reply->errorString().toStdString());
        emit error("Failed to obtain LiveKit token");
        leave();
        return;
    }

    const QByteArray data = reply->readAll();
    const QJsonDocument doc = QJsonDocument::fromJson(data);
    if (doc.isNull() || !doc.isObject()) {
        nhlog::net()->error("Invalid JSON response from token service: {}", data.toStdString());
        emit error("Invalid response from token service");
        leave();
        return;
    }

    QJsonObject obj = doc.object();
    const QString wsUrl = obj["url"].toString();
    const QString jwt = obj["jwt"].toString();
    if (wsUrl.isEmpty() || jwt.isEmpty()) {
        nhlog::net()->error("Missing url or jwt in token response");
        emit error("Incomplete token response");
        leave();
        return;
    }

    // refresh memebership message
    membershipRefreshTimer_.start(
        std::chrono::milliseconds(3600000 * 8 / 10));

    livekitSession_ = new LiveKitSession(this);

    for (auto& [k, v]: pendingDecryptionKeys_) {
        livekitSession_->setDecryptionKey(k, v);
    }
    pendingDecryptionKeys_.clear();


    connect(livekitSession_, &LiveKitSession::connected,
            this, &MatrixRTCSession::onLiveKitConnected);
    connect(livekitSession_, &LiveKitSession::error,
            this, &MatrixRTCSession::onLiveKitError);
    connect(livekitSession_, &LiveKitSession::participantConnected,
            this, &MatrixRTCSession::participantJoined);
    connect(livekitSession_, &LiveKitSession::participantDisconnected,
            this, &MatrixRTCSession::participantLeft);

    if (!turnServers_.uris.empty()) {
        livekitSession_->setTurnServers(turnServers_.uris, turnServers_.username, turnServers_.password);
    }

    livekitSession_->connect(wsUrl, jwt);
}

void MatrixRTCSession::leave()
{
    isActive_ = false;
    membershipRefreshTimer_.stop();
    sendMembershipEvent(true);

    if (livekitSession_) {
        livekitSession_->disconnect();
        livekitSession_->deleteLater();
        livekitSession_ = nullptr;
    }

    emit left();
}

void MatrixRTCSession::refreshMembership()
{
    if (isActive_)
        sendMembershipEvent();
}

void MatrixRTCSession::onLiveKitConnected()
{
    nhlog::ui()->info("MatrixRTC: connected to LiveKit");
    emit joined();
}

void MatrixRTCSession::onLiveKitError(const QString &message)
{
    nhlog::ui()->error("MatrixRTC: LiveKit error: {}", message.toStdString());
    emit error(message);
    leave();
}

void MatrixRTCSession::sendMembershipEvent(bool leave)
{
    auto state_key = "_" + userId_ + "_" + deviceId_ + "_m.call";

    mtx::events::state::CallMember evt;

    if (!leave) {
        evt.application = "m.call";
        evt.call_id     = "";
        evt.scope       = "m.room";
        evt.device_id   = deviceId_;
        evt.expires     = 3600000;
        evt.intent      = "audio";
        evt.membership_id = "";
        evt.created_ts  = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();

        mtx::events::state::CallMemberFocus focus;
        focus.type                = "livekit";
        focus.livekit_service_url = "https://livekit.ernests.id.lv";
        focus.livekit_alias       = roomId_;
        evt.foci_preferred.push_back(focus);

        mtx::events::state::CallMemberActiveFocus activeFocus;
        activeFocus.type            = "livekit";
        activeFocus.focus_selection = "oldest_membership";
        evt.focus_active            = activeFocus;
    }

    http::client()->send_state_event<mtx::events::state::CallMember>(
        roomId_,
        state_key,
        evt,
        [this](const mtx::responses::EventId &, mtx::http::RequestErr err) {
            if (err) {
                nhlog::net()->error("Failed to send call member event: {}", err->matrix_error.error);
                emit error(QString::fromStdString(err->matrix_error.error));
            } else {
                nhlog::net()->info("Call member event sent");
            }
        });
}
