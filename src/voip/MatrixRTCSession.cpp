// src/voip/MatrixRTCSession.cpp
#include "voip/MatrixRTCSession.h"
#include <mtx/events/matrixrtc.hpp>
#include <mtx/responses/matrix_openid_token.hpp>
#include <mtx/responses/matrixrtc.hpp>
#include <mtx/responses/turn_server.hpp>
#include <qjsonobject.h>

#include "Logging.h"
#include "ChatPage.h"
#include "MatrixClient.h"
#include "ScreenCastPortal.h"
#include "TimelineModel.h"
#include "UserSettingsPage.h"

#include <QRandomGenerator>
#include <mtx/responses/well-known.hpp>

MatrixRTCSession *MatrixRTCSession::create(QQmlEngine *qmlEngine, QJSEngine *)
{
    auto instance = ChatPage::instance()->matrixRTC();
    Q_ASSERT(instance);
    nhlog::net()->error("MatrixRTC create() instance: {:p}", (void *)instance);
    Q_ASSERT(instance);

    Q_ASSERT(qmlEngine->thread() == instance->thread());

    static QJSEngine *s_engine = nullptr;
    if (s_engine)
        Q_ASSERT(qmlEngine == s_engine);
    else
        s_engine = qmlEngine;

    QJSEngine::setObjectOwnership(instance, QJSEngine::CppOwnership);
    return instance;
}

MatrixRTCSession::MatrixRTCSession(QObject *parent)
  : QObject(parent)
{
    connect(&membershipRefreshTimer_, &QTimer::timeout, this, &MatrixRTCSession::refreshMembership);
    connect(&keyRotationTimer_, &QTimer::timeout, this, &MatrixRTCSession::rotateEncryptionKey);

    keyRotationTimer_.setInterval(30'000);

    callState_ = static_cast<int>(webrtc::State::DISCONNECTED);
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

    std::string livekitUrlString = livekitEndpoint_ + "/sfu/get";

    const QUrl url(livekitUrlString.c_str());
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QJsonObject payload;
    payload["room"] = QString::fromStdString(roomId_);
    payload["device_id"] = QString::fromStdString(deviceId_);
    QJsonObject openidObj;
    openidObj["matrix_server_name"] = QString::fromStdString(openIDToken.matrix_server_name);
    openidObj["access_token"] = QString::fromStdString(openIDToken.access_token);
    openidObj["token_type"] = QString::fromStdString(openIDToken.token_type);
    payload["openid_token"] = openidObj;

    const QByteArray postData = QJsonDocument(payload).toJson();


    nhlog::net()->info("THING THANG {}", postData.toStdString());

    nam_->post(request, postData);
}

void
MatrixRTCSession::getHomeserverLivekitBackend()
{
    http::client()->well_known(
      [this](const mtx::responses::WellKnown &res, mtx::http::RequestErr err) {
          if (err) {
              if (err->status_code == 404) {
                  nhlog::net()->info("Autodiscovery: No .well-known.");
                  return;
              }

              if (!err->parse_error.empty()) {
                  nhlog::net()->error("Autodiscovery failed. Received malformed response. {}",
                                      err->parse_error);
                  return;
              }
              nhlog::net()->error("Autodiscovery failed. Unknown error when "
                                  "requesting .well-known. {}",
                                  *err);
              return;
          }

          if (res.rtc_transports.has_value()) {
              for (const auto &foci : *res.rtc_transports) {
                  if (foci.type == "livekit") {
                      this->livekitEndpoint_ = foci.data.value("livekit_service_url", "");
                  }
              }
          }
          if (this->livekitEndpoint_ == "") {
              nhlog::net()->error("Livekit service URL not found in .well_known/matrix/client");
              emit error("Failed to obtain LiveKit service url");
              return;
          }

          nhlog::net()->info("Found livekit backend url: {}", livekitEndpoint_);
      });
}
void MatrixRTCSession::join(const std::string &roomId,
                           const std::string &userId,
                           const std::string &deviceI,
                           TimelineModel* timelineModel)
{
    nhlog::net()->error("join() this={}, isActive now={}", (void*)this, isActive_);
    currentTimeline_ = timelineModel;
    roomId_ = roomId;
    userId_ = userId;;
    deviceId_ = deviceI;
    stateKey_ = "_" + userId + "_" + deviceI + "_m.call";
    isActive_ = true;
    nhlog::net()->error("SET TO TRUE");
    emit isOnCallChanged();
    setCallState(static_cast<int>(webrtc::State::CONNECTING));

    if (timelineModel->getLastFociPreferred().empty()) {
        getHomeserverLivekitBackend();
    } else {
        livekitEndpoint_ = timelineModel->getLastFociPreferred();
    }

    http::client()->get_turn_server([this](const mtx::responses::TurnServer &turn, mtx::http::RequestErr err) {
        if (!err && !turn.uris.empty()) {
            turnServers_ = turn;
        }
        sendMembershipEvent(false);
        fetchOpenidToken();
    });
}

void
MatrixRTCSession::setCallState(int newState) {
    if (callState_ == newState) 
        return;

    callState_ = newState;
    emit callStateChanged();
}

void
MatrixRTCSession::startScreenShare(const QString &roomid, unsigned int windowIndex)
{
    nhlog::net()->info("MatrixRTC NOT IMPLEMENTED: start screenshare window index: {}", windowIndex);
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

    membershipRefreshTimer_.start(
        std::chrono::milliseconds(3600000 * 8 / 10));

    livekitSession_ = new LiveKitSession(this);

    for (auto& [k, v]: pendingDecryptionKeys_) {
        livekitSession_->setDecryptionKey(k, v);
    }
    pendingDecryptionKeys_.clear();

    connect(
      livekitSession_, &LiveKitSession::connected, this, &MatrixRTCSession::onLiveKitConnected);
    connect(livekitSession_, &LiveKitSession::error, this, &MatrixRTCSession::onLiveKitError);
    connect(livekitSession_,
            &LiveKitSession::participantConnected,
            this,
            [this](const QString &identity) {
                const std::string userId = identity.toStdString();
                activeParticipantUserIds_.insert(userId);

                if (!currentEncKeyMaterial_.empty())
                    sendEncryptionKeyToUser(userId, currentEncKid_, currentEncKeyMaterial_);

                emit participantJoined(identity);
            });
    connect(livekitSession_,
            &LiveKitSession::participantDisconnected,
            this,
            [this](const QString &identity) {
                activeParticipantUserIds_.erase(identity.toStdString());
                emit participantLeft(identity);
            });


    if (!turnServers_.uris.empty()) {
        livekitSession_->setTurnServers(turnServers_.uris, turnServers_.username, turnServers_.password);
    }

    livekitSession_->connect(wsUrl, jwt);
}

void MatrixRTCSession::leave()
{
    isActive_ = false;
    emit isOnCallChanged();
    setCallState(static_cast<int>(webrtc::State::DISCONNECTED));
    membershipRefreshTimer_.stop();
    //keyRotationTimer_.stop();
    activeParticipantUserIds_.clear();
    currentEncKeyMaterial_.clear();
    sendMembershipEvent(true);

    if (livekitSession_) {
        livekitSession_->disconnect();
        livekitSession_->deleteLater();
        livekitSession_ = nullptr;
    }

    emit left();
}
bool
MatrixRTCSession::screenShareReady() const
{
    return ScreenCastPortal::instance().ready();
}

void MatrixRTCSession::refreshMembership()
{
    if (isActive_)
        sendMembershipEvent();
}

void MatrixRTCSession::onLiveKitConnected()
{
    nhlog::ui()->info("MatrixRTC: connected to LiveKit");
    setCallState(static_cast<int>(webrtc::State::CONNECTED));
    emit joined();
    publishMicrophone();
}

void MatrixRTCSession::onLiveKitError(const QString &message)
{
    nhlog::ui()->error("MatrixRTC: LiveKit error: {}", message.toStdString());
    setCallState(static_cast<int>(webrtc::State::DISCONNECTED));
    emit error(message);
    leave();
}

uint8_t findFreeKID(const std::set<uint8_t>& usedKIDs) {
    for (uint16_t i = 0; i <= 255; ++i) {
        uint8_t candidate = static_cast<uint8_t>(i);
        if (usedKIDs.find(candidate) == usedKIDs.end()) {
            return candidate;
        }
    }
    return static_cast<uint8_t>(QRandomGenerator::global()->bounded(256));
}

void MatrixRTCSession::publishMicrophone()
{
    if (!livekitSession_) {
        nhlog::ui()->warn("MatrixRTC: publishMicrophone called but not connected yet");
        return;
    }

    livekitSession_->publishMicrophone();

    auto *lk = livekitSession_;
    connect(
      lk,
      &LiveKitSession::publisherPipelineReady,
      this,
      [this]() {
          auto *sfu = livekitSession_ ? livekitSession_->sfu_session() : nullptr;
          if (!sfu) {
              nhlog::ui()->error("MatrixRTC: publisherPipelineReady but sfuSession is null");
              return;
          }
          currentEncKid_         = findFreeKID(usedKIDs_);
          currentEncKeyMaterial_ = sfu->generateEncryptionKeyMaterial(currentEncKid_);
          if (currentEncKeyMaterial_.empty()) {
              nhlog::ui()->error("MatrixRTC: failed to generate encryption key");
              return;
          }
          nhlog::ui()->info("MatrixRTC: publisher ready, distributing KID {}", currentEncKid_);
          sendEncryptionKeyToAllParticipants(currentEncKid_, currentEncKeyMaterial_);
          //keyRotationTimer_.start();
      },
      Qt::SingleShotConnection);
}
void
MatrixRTCSession::shareScreen()
{
    //ScreenCastPortal::instance().init();
}

void MatrixRTCSession::rotateEncryptionKey()
{
    if (!livekitSession_ || !livekitSession_->sfu_session())
        return;

    currentEncKid_ = (currentEncKid_ + 1) % 256;
    currentEncKeyMaterial_ = livekitSession_->sfu_session()
                                 ->generateEncryptionKeyMaterial(currentEncKid_);
    if (currentEncKeyMaterial_.empty()) {
        nhlog::ui()->error("MatrixRTC: key rotation failed");
        return;
    }

    nhlog::ui()->info("MatrixRTC: rotated to KID {}", currentEncKid_);
    sendEncryptionKeyToAllParticipants(currentEncKid_, currentEncKeyMaterial_);
}
void
MatrixRTCSession::closeScreenShare()
{
    nhlog::ui()->info("MatrixRTC NOT IMPLEMENTED: closing screen share");
}
QStringList
MatrixRTCSession::screenShareTypeList()
{
    return QStringList(QString("PipeWire"));
}
void
MatrixRTCSession::setScreenShareType(unsigned int index)
{
    nhlog::ui()->info("MatrixRTC NOT IMPLEMENTED: Set screen share type to index {}", index);
    screenShareType_ = webrtc::ScreenShareType::XDP;
    emit screenShareChanged();
}
void
MatrixRTCSession::setupScreenShareXDP()
{
    ScreenCastPortal &sc_portal = ScreenCastPortal::instance();
    sc_portal.init();
}

void MatrixRTCSession::sendEncryptionKeyToUser(const std::string &matrixUserId,
                                               uint8_t kid,
                                               const std::vector<uint8_t> &rawKeyMaterial)
{
    std::string userId;
    std::string deviceId;

    size_t colonPos = matrixUserId.rfind(':');
    if (colonPos == std::string::npos) {
        nhlog::net()->warn("MatrixRTC: invalid user/device format: {}, cannot send key", matrixUserId);
        return;
    }

    userId = matrixUserId.substr(0, colonPos);
    deviceId = matrixUserId.substr(colonPos + 1);

    if (userId.empty() || deviceId.empty()) {
        nhlog::net()->warn("MatrixRTC: empty user or device ID from: {}", matrixUserId);
        return;
    }

    mtx::events::msg::CallEncryptionKeys content;
    content.room_id  = roomId_;
    content.sent_ts  = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch()).count();
    content.keys.index = kid;
    content.keys.key   = QByteArray(
                             reinterpret_cast<const char *>(rawKeyMaterial.data()),
                             static_cast<int>(rawKeyMaterial.size()))
                             .toBase64().toStdString();
    content.member.id                 = "xyz";
    content.member.claimed_device_id  = deviceId_;
    content.session.call_id           = "";
    content.session.application       = "m.call";
    content.session.scope             = "m.room";

    std::map<mtx::identifiers::User,
             std::map<std::string, mtx::events::msg::CallEncryptionKeys>> messages;
    messages[mtx::identifiers::parse<mtx::identifiers::User>(userId)][deviceId] = content;

    const std::string txid =
        QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();

    http::client()->send_to_device<mtx::events::msg::CallEncryptionKeys>(
        txid, messages,
        [matrixUserId, kid](mtx::http::RequestErr err) {
            if (err)
                nhlog::net()->error("MatrixRTC: key send to {} failed: {}",
                                    matrixUserId, err->matrix_error.error);
            else
                nhlog::net()->info("MatrixRTC: KID {} sent to {}", kid, matrixUserId);
        });
}

void MatrixRTCSession::sendEncryptionKeyToAllParticipants(
    uint8_t kid, const std::vector<uint8_t> &rawKeyMaterial)
{
    if (activeParticipantUserIds_.empty()) {
        nhlog::ui()->info("MatrixRTC: no active participants to send key to yet");
        return;
    }
    for (const auto &userId : activeParticipantUserIds_)
        sendEncryptionKeyToUser(userId, kid, rawKeyMaterial);
}
void MatrixRTCSession::toggleMicMute() {
    nhlog::net()->error("Toggled mic");
    if (livekitSession_) {
        bool newMute = !livekitSession_->isMicMuted();
        livekitSession_->setMicMuted(newMute);
        emit micMutedChanged();
    }
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
        evt.intent      = "video";
        evt.membership_id = userId_ + ":" + deviceId_;
        evt.created_ts  = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();

        mtx::events::state::CallMemberFocus focus;
        focus.type                = "livekit";
        focus.livekit_service_url = livekitEndpoint_;
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

#include "moc_MatrixRTCSession.cpp"