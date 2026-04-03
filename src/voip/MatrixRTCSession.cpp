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

MatrixRTCSession::MatrixRTCSession(const std::string &roomId,
                                    const std::string &userId,
                                    const std::string &deviceId,
                                    QObject *parent)
  : QObject(parent)
  , roomId_(roomId)
  , userId_(userId)
  , deviceId_(deviceId)
  , stateKey_("_" + userId + "_" + deviceId + "_m.call")
{
    connect(&membershipRefreshTimer_, &QTimer::timeout,
            this, &MatrixRTCSession::refreshMembership);
}

MatrixRTCSession::~MatrixRTCSession() {
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
void MatrixRTCSession::requestLiveKitJWT(const mtx::responses::MatrixOpenidToken &openidtoken) {
    if (!nam_) {
        nam_ = new QNetworkAccessManager(this);
        connect(nam_, &QNetworkAccessManager::finished, this, &MatrixRTCSession::onCredentialsReceived);
    }
    QUrl url("https://livekit.ernests.id.lv/get_token");
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QJsonObject payload;
    payload["room_id"] = QString::fromStdString(roomId_);
    payload["slot_id"] = "m.call#ROOM"; // No idea why, element sets it like this, and it does then connect to the same one as an element client

    QJsonObject openidObj;
    openidObj["matrix_server_name"] = QString::fromStdString(openidtoken.matrix_server_name);
    openidObj["access_token"] = QString::fromStdString(openidtoken.access_token);
    openidObj["token_type"] = QString::fromStdString(openidtoken.token_type);
    payload["openid_token"] = openidObj;

    QJsonObject memberObj;
    memberObj["id"] = "";
    memberObj["claimed_device_id"] = QString::fromStdString(deviceId_);
    memberObj["claimed_user_id"] = QString::fromStdString(userId_);
    payload["member"] = memberObj;

    QByteArray postData = QJsonDocument(payload).toJson();

    nam_->post(request, postData);
}

void MatrixRTCSession::join()
{
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

    QByteArray data = reply->readAll();
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (doc.isNull() || !doc.isObject()) {
        nhlog::net()->error("Invalid JSON response from token service: {}", data.toStdString());
        emit error("Invalid response from token service");
        leave();
        return;
    }

    QJsonObject obj = doc.object();
    QString wsUrl = obj["url"].toString();
    QString jwt = obj["jwt"].toString();
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
    // per-device state key: _{userId}_{deviceId}_{callId}
    auto state_key = "_" + userId_ + "_" + deviceId_ + "_m.call";

    mtx::events::state::CallMember evt;

    evt.empty = false;
    if (!leave) {
        mtx::events::state::CallMemberMembership membership;
        membership.application = "m.call";
        membership.call_id     = "";
        membership.scope       = "m.room";
        membership.device_id   = deviceId_;
        membership.expires     = 3600000; // 1 hour
        membership.intent      = "audio";

        mtx::events::state::CallMemberFocus focus;
        focus.type                = "livekit";
        focus.livekit_service_url = "https://livekit.ernests.id.lv";
        focus.livekit_alias       = roomId_;
        membership.foci_preferred.push_back(focus);

        mtx::events::state::CallMemberActiveFocus activeFocus;
        activeFocus.type            = "livekit";
        activeFocus.focus_selection = "oldest_membership";
        membership.focus_active     = activeFocus;

        evt.memberships.push_back(membership);
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
