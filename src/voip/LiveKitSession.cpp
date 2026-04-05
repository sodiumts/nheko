#include "livekit_rtc.pb.h"
#ifdef GSTREAMER_AVAILABLE

#include "LiveKitSession.h"
#include <QJsonObject>
#include <QNetworkRequest>
#include <QUuid>
#include "Logging.h"

LiveKitSession::LiveKitSession(QObject *parent)
  : QObject(parent)
{
    QObject::connect(&webSocket_, &QWebSocket::connected,
                     this, &LiveKitSession::onWebSocketConnected);
    QObject::connect(&webSocket_, &QWebSocket::disconnected,
                     this, &LiveKitSession::onWebSocketDisconnected);
    QObject::connect(&webSocket_, &QWebSocket::binaryMessageReceived,
                     this, &LiveKitSession::onBinaryMessageReceived);
    QObject::connect(&webSocket_, &QWebSocket::errorOccurred,
                     this, &LiveKitSession::onWebSocketError);
    QObject::connect(&pingTimer_, &QTimer::timeout,
                     this, &LiveKitSession::onPingTimer);
}

LiveKitSession::~LiveKitSession()
{
    disconnect();
}

void
LiveKitSession::connect(const QString &url, const QString &jwt)
{
    if (state_ != State::Disconnected) {
        nhlog::net()->warn("LiveKitSession: already connected or connecting");
        return;
    }

    serverUrl_ = url;
    jwt_       = jwt;
    setState(State::Connecting);

    QString wsUrl = url;
    wsUrl.replace(QStringLiteral("https://"), QStringLiteral("wss://"));
    wsUrl.replace(QStringLiteral("http://"), QStringLiteral("ws://"));
    wsUrl += QStringLiteral("/rtc?access_token=") + jwt +
             QStringLiteral("&protocol=9&auto_subscribe=1");

    nhlog::net()->info("LiveKit: connecting to {}", wsUrl.toStdString());

    QNetworkRequest request{QUrl(wsUrl)};
    request.setRawHeader("User-Agent", "nheko-matrixrtc/0.1");
    webSocket_.open(request);
}

void
LiveKitSession::disconnect()
{
    pingTimer_.stop();

    if (webSocket_.state() != QAbstractSocket::UnconnectedState) {
        livekit::SignalRequest request;
        request.mutable_leave()->set_can_reconnect(false);
        sendSignalRequest(request);
        webSocket_.close();
    }

    if (sfuSession_) {
        sfuSession_->end();
        sfuSession_->deleteLater();
        sfuSession_ = nullptr;
    }

    setState(State::Disconnected);
}

void
LiveKitSession::setState(const State state)
{
    if (state_ != state) {
        state_ = state;
        emit stateChanged(state);
        if (state == State::Connected)
            emit connected();
        else if (state == State::Disconnected)
            emit disconnected();
    }
}

void
LiveKitSession::sendSignalRequest(const livekit::SignalRequest &request)
{
    if (webSocket_.state() != QAbstractSocket::ConnectedState) {
        nhlog::net()->warn("LiveKit: tried to send while not connected");
        return;
    }
    std::string serialized;
    if (!request.SerializeToString(&serialized)) {
        nhlog::net()->error("LiveKit: failed to serialize SignalRequest");
        return;
    }
    webSocket_.sendBinaryMessage(
        QByteArray(serialized.data(), static_cast<int>(serialized.size())));
}

void
LiveKitSession::onWebSocketConnected()
{
    nhlog::net()->info("LiveKit: WebSocket connected, waiting for JoinResponse");
}

void
LiveKitSession::onWebSocketDisconnected()
{
    nhlog::net()->info("LiveKit: WebSocket disconnected");
    pingTimer_.stop();
    setState(State::Disconnected);
}

void
LiveKitSession::onWebSocketError(const QAbstractSocket::SocketError socketError)
{
    nhlog::net()->error("LiveKit: WebSocket error {}: {}",
                        static_cast<int>(socketError),
                        webSocket_.errorString().toStdString());
    emit error(webSocket_.errorString());
    setState(State::Failed);
}

void
LiveKitSession::onBinaryMessageReceived(const QByteArray &message)
{
    livekit::SignalResponse response;
    if (!response.ParseFromArray(message.data(), message.size())) {
        nhlog::net()->error("LiveKit: failed to parse SignalResponse");
        return;
    }

    switch (response.message_case()) {
    case livekit::SignalResponse::kJoin:
        handleJoin(response.join());
        break;
    case livekit::SignalResponse::kOffer:
        handleOffer(response.offer());
        break;
    case livekit::SignalResponse::kAnswer:
        handleAnswer(response.answer());
        break;
    case livekit::SignalResponse::kTrickle:
        handleTrickle(response.trickle());
        break;
    case livekit::SignalResponse::kUpdate:
        handleParticipantUpdate(response.update());
        break;
    case livekit::SignalResponse::kRoomUpdate: {
        nhlog::net()->debug("Received room update");
    }
    break;
    case livekit::SignalResponse::kTrackPublished:
        handleTrackPublished(response.track_published());
        break;
    case livekit::SignalResponse::kLeave:
        handleLeave(response.leave());
        break;
    case livekit::SignalResponse::kSpeakersChanged: {
        for (const auto &speakers = response.speakers_changed().speakers();
             const auto &speaker : speakers) {
            nhlog::net()->debug("Speaker changed: sid={}, level={}, active={}",
                                speaker.sid(),
                                speaker.level(),
                                speaker.active());
        }

        break;
    }
    case livekit::SignalResponse::kSubscriptionPermissionUpdate: {
        nhlog::net()->debug("Subscription permission update received");
    }
    break;
    case livekit::SignalResponse::kPong:
        nhlog::net()->debug("LiveKit: pong");
        break;
    case livekit::SignalResponse::kPongResp:
        nhlog::net()->debug("LiveKit: pong response");
        break;
    default:
        nhlog::net()->debug("LiveKit: unhandled message type {}",
                            static_cast<int>(response.message_case()));
        break;
    }
}

void
LiveKitSession::handleJoin(const livekit::JoinResponse &join)
{
    nhlog::net()->info("LiveKit: joined room '{}' as '{}'",
                       join.room().name(),
                       join.participant().identity());

    roomName_                 = join.room().name();
    localParticipantIdentity_ = join.participant().identity();
    localParticipantSid_      = join.participant().sid();

    if (join.ping_interval() > 0)
        pingInterval_ = join.ping_interval() * 1000;
    pingTimer_.start(pingInterval_);

    for (const auto &p : join.other_participants()) {
        nhlog::net()->info("LiveKit: existing participant: {}", p.identity());
        emit participantConnected(QString::fromStdString(p.identity()));
    }

    setState(State::Connected);
}

std::vector<std::string>
LiveKitSession::buildTurnUris() const
{
    std::vector<std::string> fullUris;
    for (const auto &uri : turnUris_) {
        std::string hostAndQuery = uri.substr(5); // dont need turn://
        QString userEnc = QUrl::toPercentEncoding(QString::fromStdString(turnUsername_));
        QString passEnc = QUrl::toPercentEncoding(QString::fromStdString(turnCredential_));
        QString fullUri = QStringLiteral("turn://%1:%2@%3")
                            .arg(userEnc, passEnc,
                                 QString::fromStdString(hostAndQuery));
        fullUris.push_back(fullUri.toStdString());
    }
    return fullUris;
}

void
LiveKitSession::setDecryptionKey(uint8_t kid, const std::vector<uint8_t> &rawKey)
{
    if (sfuSession_) {
        sfuSession_->setDecryptionKey(kid, rawKey);
    } else {
        nhlog::net()->info("LiveKit: buffering decryption key for KID {} (sfuSession not ready)", kid);
        pendingDecryptionKeys_[kid] = rawKey;
    }
}

void
LiveKitSession::handleOffer(const livekit::SessionDescription &offer)
{
    nhlog::net()->info("LiveKit: received offer (type={})", offer.type());
    nhlog::net()->info("LiveKit: offer SDP preview: {}",
                       offer.sdp().substr(0, 200));

    if (!sfuSession_) {
        sfuSession_ = new GStreamerSFUSession(this);

        QObject::connect(sfuSession_,
                         &GStreamerSFUSession::subscriberAnswerCreated,
                         this,
                         &LiveKitSession::sendAnswer);

        QObject::connect(
          sfuSession_,
          &GStreamerSFUSession::subscriberICECandidate,
          this,
          [this](const std::string &candidate, const std::string &sdpMid, const int sdpMLineIndex) {
              sendICECandidate(candidate, sdpMid, sdpMLineIndex, livekit::SignalTarget::SUBSCRIBER);
          });

        QObject::connect(sfuSession_,
                         &GStreamerSFUSession::failed,
                         this,
                         [this](const QString &reason) { emit error(reason); });

        sfuSession_->setTurnServers(buildTurnUris());

        flushPendingDecryptionKeys();

        if (!sfuSession_->initSubscriber(offer.sdp()))
            emit error(QStringLiteral("Failed to initialize subscriber pipeline"));
    } else {
        nhlog::net()->info("LiveKit: renegotiation offer received");
        if (!sfuSession_->acceptRenegotiationOffer(offer.sdp()))
            emit error(QStringLiteral("Failed to accept renegotiation offer"));
    }
}

void
LiveKitSession::handleAnswer([[maybe_unused]] const livekit::SessionDescription &answer)
{
    // TODO: Actually implement this
    nhlog::net()->info("LiveKit: received publisher answer (not handled yet)");
}
void
LiveKitSession::setTurnServers(const std::vector<std::string> &uris,
                                     const std::string &username,
                                     const std::string &credential) {
    turnUris_ = uris;
    turnUsername_ = username;
    turnCredential_ = credential;
}

void
LiveKitSession::handleTrickle(const livekit::TrickleRequest &trickle)
{
    nhlog::net()->info("LiveKit: trickle received target={} candidate={}",
                       static_cast<int>(trickle.target()),
                       trickle.candidateinit().substr(0, 50));

    if (trickle.target() != livekit::SignalTarget::SUBSCRIBER)
        return;

    if (!sfuSession_) {
        nhlog::net()->warn("LiveKit: got trickle but sfuSession_ not ready");
        return;
    }

    auto candidateJson = QJsonDocument::fromJson(
        QByteArray::fromStdString(trickle.candidateinit())).object();

    sfuSession_->addSubscriberICECandidate(
        candidateJson[QStringLiteral("candidate")].toString().toStdString(),
        candidateJson[QStringLiteral("sdpMid")].toString().toStdString(),
        candidateJson[QStringLiteral("sdpMLineIndex")].toInt());
}

void
LiveKitSession::handleParticipantUpdate(const livekit::ParticipantUpdate &update)
{
    for (const auto &p : update.participants()) {
        if (p.identity() == localParticipantIdentity_)
            continue;

        if (p.state() == livekit::ParticipantInfo_State_ACTIVE)
            emit participantConnected(QString::fromStdString(p.identity()));
        else if (p.state() == livekit::ParticipantInfo_State_DISCONNECTED)
            emit participantDisconnected(QString::fromStdString(p.identity()));
    }
}

void
LiveKitSession::handleTrackPublished(const livekit::TrackPublishedResponse &published)
{
    nhlog::net()->info("LiveKit: track published cid={} sid={}",
                       published.cid(), published.track().sid());
    if (published.cid() == micTrackCid_) {
        micTrackSid_ = published.track().sid();
        setState(State::Publishing);
    }
}

void
LiveKitSession::handleLeave(const livekit::LeaveRequest &leave)
{
    nhlog::net()->info("LiveKit: server requested leave reason={}",
                       static_cast<int>(leave.reason()));
    emit error(QStringLiteral("Disconnected by server"));
    disconnect();
}

void
LiveKitSession::publishMicrophone()
{
    // TODO: implement microphone publishing
    nhlog::net()->info("LiveKit: publishMicrophone() not yet implemented");
}

void
LiveKitSession::toggleMicMute()
{
    micMuted_ = !micMuted_;
    if (sfuSession_)
        sfuSession_->toggleMicMute();
    if (!micTrackSid_.empty())
        sendMuteTrack(micTrackSid_, micMuted_);
}

void
LiveKitSession::sendAnswer(const std::string &sdp)
{
    livekit::SignalRequest request;
    auto *answer = request.mutable_answer();
    answer->set_type("answer");
    answer->set_sdp(sdp);
    sendSignalRequest(request);
    nhlog::net()->info("LiveKit: sent subscriber answer");
}

void
LiveKitSession::sendICECandidate(const std::string &candidate,
                                       const std::string &sdpMid,
                                       int sdpMLineIndex,
                                       livekit::SignalTarget target)
{
    QJsonObject obj;
    obj[QStringLiteral("candidate")]     = QString::fromStdString(candidate);
    obj[QStringLiteral("sdpMid")]        = QString::fromStdString(sdpMid);
    obj[QStringLiteral("sdpMLineIndex")] = sdpMLineIndex;

    livekit::SignalRequest request;
    auto *trickle = request.mutable_trickle();
    trickle->set_candidateinit(
        QJsonDocument(obj).toJson(QJsonDocument::Compact).toStdString());
    trickle->set_target(target);
    sendSignalRequest(request);
}

void
LiveKitSession::sendAddTrack(const std::string &cid,
                                   const std::string &name,
                                   livekit::TrackType type)
{
    livekit::SignalRequest request;
    auto *addTrack = request.mutable_add_track();
    addTrack->set_cid(cid);
    addTrack->set_name(name);
    addTrack->set_type(type);
    addTrack->set_source(type == livekit::TrackType::AUDIO
                             ? livekit::TrackSource::MICROPHONE
                             : livekit::TrackSource::CAMERA);
    sendSignalRequest(request);
}

void
LiveKitSession::sendMuteTrack(const std::string &sid, bool muted)
{
    livekit::SignalRequest request;
    auto *mute = request.mutable_mute();
    mute->set_sid(sid);
    mute->set_muted(muted);
    sendSignalRequest(request);
}

void
LiveKitSession::sendPing()
{
    livekit::SignalRequest request;
    request.mutable_ping_req()->set_timestamp(
        QDateTime::currentMSecsSinceEpoch());
    sendSignalRequest(request);
}

void
LiveKitSession::flushPendingDecryptionKeys()
{
    if (!sfuSession_ || pendingDecryptionKeys_.empty())
        return;

    nhlog::net()->info("LiveKit: flushing {} pending decryption key(s) into sfuSession",
                       pendingDecryptionKeys_.size());

    for (const auto &[kid, rawKey] : pendingDecryptionKeys_) {
        sfuSession_->setDecryptionKey(kid, rawKey);
    }
    pendingDecryptionKeys_.clear();
}

void
LiveKitSession::onPingTimer()
{
    sendPing();
}

#endif // GSTREAMER_AVAILABLE
