#include "AgentWebSocketServer.h"
#include "WebSocketReadBuffer.h"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QNetworkInterface>
#include <QNetworkProxy>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <QWebSocket>
#include <QWebSocketCorsAuthenticator>
#include <QWebSocketServer>
#include <limits>
#include <protocol/ProtocolCodec.h>
#include <session/ISessionManager.h>

namespace rad
{
namespace
{
namespace wire = protocol;
wire::ErrorCode publicError(ErrorCode code)
{
    switch (code)
    {
    case ErrorCode::InvalidArgument:
        return wire::ErrorCode::InvalidPayload;
    case ErrorCode::NotFound:
        return wire::ErrorCode::SessionNotFound;
    case ErrorCode::PermissionDenied:
        return wire::ErrorCode::Forbidden;
    case ErrorCode::InvalidState:
    case ErrorCode::ControlUnavailable:
    case ErrorCode::ProviderUnavailable:
    case ErrorCode::QueueFull:
        return wire::ErrorCode::Busy;
    default:
        return wire::ErrorCode::InternalError;
    }
}
QString stateName(SessionStatus status)
{
    switch (status)
    {
    case SessionStatus::Creating:
        return QStringLiteral("creating");
    case SessionStatus::Running:
        return QStringLiteral("running");
    case SessionStatus::Exiting:
        return QStringLiteral("exiting");
    case SessionStatus::Exited:
        return QStringLiteral("exited");
    case SessionStatus::Failed:
        return QStringLiteral("failed");
    }
    return QStringLiteral("failed");
}
QJsonObject statusPayload(const SessionSnapshot &snapshot)
{
    return {{"process_state", stateName(snapshot.status)},
            {"attachable", snapshot.status != SessionStatus::Creating && !snapshot.metadataOnly},
            {"diagnostics", QJsonObject{{"created_at_utc_ms", QString::number(snapshot.createdAtUtcMs)},
                                        {"updated_at_utc_ms", QString::number(snapshot.updatedAtUtcMs)},
                                        {"exit_code_valid", snapshot.exitCodeValid},
                                        {"exit_code", snapshot.exitCode},
                                        {"failure_code", snapshot.failureCode},
                                        {"metadata_only", snapshot.metadataOnly}}}};
}
bool constantEqual(const QByteArray &left, const QByteArray &right)
{
    if (left.size() != right.size())
        return false;
    unsigned char difference = 0;
    for (int index = 0; index < left.size(); ++index)
        difference |= static_cast<unsigned char>(left[index] ^ right[index]);
    return difference == 0;
}
bool allowedAddress(const QHostAddress &address)
{
    if (address.protocol() != QAbstractSocket::IPv4Protocol)
        return false;
    if (address.isLoopback())
        return true;
    return address.isInSubnet(QHostAddress(QStringLiteral("100.64.0.0")), 10) &&
           QNetworkInterface::allAddresses().contains(address);
}
}

class AgentWebSocketServer::State
{
public:
    struct Subscription
    {
        quint64 generation = 0;
        quint64 inputSequence = 0;
        quint64 outputOffset = 0;
        bool outputPending = false;
    };
    struct Client
    {
        QPointer<QWebSocket> socket;
        QString id;
        bool ready = false;
        bool closed = false;
        qint64 connectedAt = 0;
        qint64 lastPong = 0;
        qint64 lastPing = 0;
        qint64 rateWindow = 0;
        int messages = 0;
        quint64 nextSubscriptionGeneration = 0;
        QSet<QString> requestIds;
        QHash<QString, Subscription> subscriptions;
    };
    using ClientPtr = std::shared_ptr<Client>;
    State(AgentWebSocketServer &owner, ISessionManager &manager, Options options)
        : m_owner(owner), m_manager(manager), m_options(std::move(options)),
          m_server(QStringLiteral("RADManager"), QWebSocketServer::NonSecureMode)
    {
        m_clock.start();
        m_server.setProxy(QNetworkProxy::NoProxy);
        QObject::connect(&m_server, &QWebSocketServer::newConnection, &m_owner,
                         [this] { acceptConnections(); });
        QObject::connect(&m_server, &QWebSocketServer::originAuthenticationRequired, &m_owner,
                         [](QWebSocketCorsAuthenticator *auth)
                         { auth->setAllowed(auth->origin().isEmpty()); });
        QObject::connect(&m_timer, &QTimer::timeout, &m_owner, [this] { tick(); });
        QObject::connect(&m_manager, &ISessionManager::outputAvailable, &m_owner,
                         [this](SessionId id, const QString &clientId)
                         {
                             for (const auto &client : m_clients.values())
                                 if (client->id == clientId)
                                     scheduleOutput(client, id.toString());
                         });
        QObject::connect(&m_manager, &ISessionManager::stateChanged, &m_owner,
                         [this](const SessionSnapshot &snapshot)
                         {
                             for (const auto &client : m_clients.values())
                                 if (client->ready && m_manager.get(snapshot.id, client->id).isSuccess())
                                     sendJson(client, {kProtocolVersion,
                                                       "session.status",
                                                       {},
                                                       snapshot.id.toString(),
                                                       statusPayload(snapshot)});
                         });
        QObject::connect(&m_manager, &ISessionManager::failed, &m_owner,
                         [this](SessionId id, const Error &error, bool)
                         {
                             for (const auto &client : m_clients.values())
                                 if (client->ready && m_manager.get(id, client->id).isSuccess())
                                     sendError(client, publicError(error.code), {}, id.toString());
                         });
    }
    OperationResult start()
    {
        if (QThread::currentThread() != m_owner.thread() || m_manager.thread() != m_owner.thread())
            return OperationResult::failure(
                {ErrorCode::InvalidState, QStringLiteral("服务器和会话管理器必须在同一线程")});
        if (m_server.isListening())
            return OperationResult::failure({ErrorCode::InvalidState, QStringLiteral("服务器已启动")});
        if (!allowedAddress(m_options.bindAddress) || m_options.credentials.isEmpty() ||
            m_options.credentials.size() > 128 || m_options.maxConnections < 1 ||
            m_options.maxConnections > 128 || m_options.handshakeTimeoutMs < 50 ||
            m_options.idleTimeoutMs < 150 || m_options.maxQueuedBytes < 256 ||
            m_options.maxQueuedBytes > 64 * 1024 * 1024 || m_options.maxMessagesPerSecond < 1 ||
            m_options.maxMessagesPerSecond > 65536 || m_options.maxRequestsPerConnection < 1 ||
            m_options.maxRequestsPerConnection > 65536)
            return OperationResult::failure({ErrorCode::InvalidArgument, QStringLiteral("服务器配置无效")});
        QSet<QString> identities;
        QSet<QByteArray> hashes;
        for (const auto &credential : m_options.credentials)
        {
            if (!QRegularExpression(QStringLiteral("\\A[A-Za-z0-9][A-Za-z0-9_.-]{0,63}\\z"))
                     .match(credential.clientId)
                     .hasMatch() ||
                credential.tokenSha256.size() != 32 || identities.contains(credential.clientId) ||
                hashes.contains(credential.tokenSha256))
                return OperationResult::failure({ErrorCode::InvalidArgument, QStringLiteral("授权配置无效")});
            identities.insert(credential.clientId);
            hashes.insert(credential.tokenSha256);
        }
        m_server.setMaxPendingConnections(m_options.maxConnections);
        m_server.setHandshakeTimeout(m_options.handshakeTimeoutMs);
        if (!m_server.listen(m_options.bindAddress, m_options.port))
            return OperationResult::failure(
                {ErrorCode::SystemError, QStringLiteral("无法监听指定地址和端口")});
        m_timer.start(qMax(10, qMin(m_options.handshakeTimeoutMs, m_options.idleTimeoutMs) / 3));
        return accepted();
    }
    void stop()
    {
        m_timer.stop();
        m_server.close();
        for (const auto &client : m_clients.values())
            disconnectClient(client);
    }
    void disconnectClient(const ClientPtr &client)
    {
        if (client->closed)
            return;
        client->closed = true;
        m_clients.remove(client->socket.data());
        const auto subscriptions = client->subscriptions.keys();
        client->subscriptions.clear();
        for (const auto &id : subscriptions)
        {
            const auto parsed = SessionId::parse(id);
            if (parsed.isSuccess())
                m_manager.detach(parsed.value(), client->id);
        }
        if (client->socket)
        {
            client->socket->disconnect(&m_owner);
            client->socket->abort();
            client->socket->deleteLater();
        }
        emit m_owner.auditEvent(QStringLiteral("disconnected"), client->id);
    }
    void acceptConnections()
    {
        while (m_server.hasPendingConnections())
        {
            auto *socket = m_server.nextPendingConnection();
            if (!socket)
                continue;
            socket->setParent(&m_owner);
            configureWebSocketReadBuffer(*socket);
            socket->setMaxAllowedIncomingFrameSize(wire::kMaxFrameBytes);
            socket->setMaxAllowedIncomingMessageSize(wire::kMaxFrameBytes);
            const QByteArray authorization = socket->request().rawHeader("Authorization");
            const QByteArray token = authorization.mid(7);
            bool valid = authorization.startsWith("Bearer ") && token.size() == 64;
            for (char character : token)
                valid = valid &&
                        ((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'));
            QString identity;
            if (valid)
            {
                const auto hash = QCryptographicHash::hash(token, QCryptographicHash::Sha256);
                for (const auto &credential : m_options.credentials)
                    if (constantEqual(hash, credential.tokenSha256))
                        identity = credential.clientId;
            }
            bool duplicate = false;
            for (const auto &client : m_clients.values())
                duplicate |= client->id == identity;
            if (identity.isEmpty() || !socket->origin().isEmpty() ||
                socket->requestUrl().path() != QStringLiteral("/") || socket->requestUrl().hasQuery() ||
                socket->requestUrl().hasFragment() || duplicate ||
                m_clients.size() >= m_options.maxConnections)
            {
                socket->abort();
                socket->deleteLater();
                emit m_owner.auditEvent(QStringLiteral("connection_rejected"), {});
                continue;
            }
            auto client = std::make_shared<Client>();
            client->socket = socket;
            client->id = identity;
            client->connectedAt = client->lastPong = client->lastPing = client->rateWindow =
                m_clock.elapsed();
            m_clients.insert(socket, client);
            QObject::connect(socket, &QWebSocket::disconnected, &m_owner,
                             [this, client] { disconnectClient(client); });
            QObject::connect(socket, &QWebSocket::textMessageReceived, &m_owner,
                             [this, client](const QString &text) { handleText(client, text); });
            QObject::connect(socket, &QWebSocket::binaryMessageReceived, &m_owner,
                             [this, client](const QByteArray &bytes) { handleBinary(client, bytes); });
            QObject::connect(socket, &QWebSocket::pong, &m_owner,
                             [this, client](quint64, const QByteArray &payload)
                             {
                                 if (payload == QByteArrayLiteral("RADP/1"))
                                     client->lastPong = m_clock.elapsed();
                             });
            emit m_owner.auditEvent(QStringLiteral("authenticated"), identity);
        }
    }
    bool sendWire(const ClientPtr &client, const QByteArray &bytes, bool binary)
    {
        if (client->closed || !client->socket)
            return false;
        // 额外保留 WebSocket 头空间，慢连接不反向阻塞会话读线程。
        if (client->socket->bytesToWrite() + bytes.size() + 16 > m_options.maxQueuedBytes)
        {
            emit m_owner.auditEvent(QStringLiteral("slow_client"), client->id);
            disconnectClient(client);
            return false;
        }
        const qint64 sent = binary ? client->socket->sendBinaryMessage(bytes)
                                   : client->socket->sendTextMessage(QString::fromUtf8(bytes));
        if (sent != bytes.size())
        {
            disconnectClient(client);
            return false;
        }
        return true;
    }
    bool sendJson(const ClientPtr &client, const wire::Envelope &message)
    {
        QByteArray bytes;
        wire::Error error;
        if (!wire::encodeJson(message, bytes, error))
        {
            disconnectClient(client);
            return false;
        }
        return sendWire(client, bytes, false);
    }
    void sendError(const ClientPtr &client, wire::ErrorCode code, const QString &requestId = {},
                   const QString &sessionId = {})
    {
        sendJson(client, wire::makeErrorEnvelope(code, requestId, sessionId));
    }
    bool admitMessage(const ClientPtr &client)
    {
        if (client->closed)
            return false;
        const auto now = m_clock.elapsed();
        if (now - client->rateWindow >= 1000)
        {
            client->rateWindow = now;
            client->messages = 0;
        }
        if (++client->messages > m_options.maxMessagesPerSecond)
        {
            disconnectClient(client);
            return false;
        }
        return true;
    }
    void handleText(const ClientPtr &client, const QString &text)
    {
        if (!admitMessage(client))
            return;
        wire::Envelope request;
        wire::Error error;
        if (!wire::decodeJson(text.toUtf8(), request, error))
        {
            sendError(client, error.code);
            return;
        }
        if (client->requestIds.contains(request.requestId))
        {
            sendError(client, wire::ErrorCode::InvalidMessage, request.requestId, request.sessionId);
            return;
        }
        if (client->requestIds.size() >= m_options.maxRequestsPerConnection)
        {
            disconnectClient(client);
            return;
        }
        client->requestIds.insert(request.requestId);
        if (!client->ready)
        {
            if (request.type != "auth.hello")
            {
                sendError(client, wire::ErrorCode::Unauthorized, request.requestId, request.sessionId);
                return;
            }
            QVector<int> versions;
            for (const auto &version : request.payload["versions"].toArray())
                versions.append(version.toInt());
            int selected = 0;
            if (!wire::negotiateVersion(versions, selected, error))
            {
                sendError(client, error.code, request.requestId);
                return;
            }
            client->ready = true;
            sendJson(
                client,
                {kProtocolVersion, "auth.welcome", request.requestId, {}, {{"selected_version", selected}}});
            return;
        }
        route(client, request);
    }
    void route(const ClientPtr &client, const wire::Envelope &request)
    {
        const auto &type = request.type;
        const auto &payload = request.payload;
        const auto reply = [&](const QString &replyType, const QJsonObject &body = QJsonObject(),
                               const QString &sessionId = QString())
        {
            return sendJson(client, {kProtocolVersion, replyType, request.requestId,
                                     sessionId.isEmpty() ? request.sessionId : sessionId, body});
        };
        const auto rejected = [&](const Error &error)
        {
            sendError(client, publicError(error.code), request.requestId, request.sessionId);
            emit m_owner.auditEvent(QStringLiteral("operation_rejected"), client->id);
        };
        if (type == "session.create")
        {
            SessionRequest creation{payload["name"].toString(),
                                    payload["provider_id"].toString(),
                                    payload["workspace_id"].toString(),
                                    {},
                                    {payload["columns"].toInt(), payload["rows"].toInt()}};
            const auto result = m_manager.create(creation, client->id);
            if (!result.isSuccess())
            {
                rejected(result.error());
                return;
            }
            const auto snapshot = m_manager.get(result.value(), client->id);
            if (!snapshot.isSuccess())
            {
                rejected(snapshot.error());
                return;
            }
            reply("session.created", statusPayload(snapshot.value()), result.value().toString());
            emit m_owner.auditEvent(QStringLiteral("session_created"), client->id);
            return;
        }
        if (type == "session.list")
        {
            const auto result = m_manager.list(client->id);
            if (!result.isSuccess())
            {
                rejected(result.error());
                return;
            }
            QJsonArray sessions;
            for (const auto &snapshot : result.value())
            {
                auto summary = statusPayload(snapshot);
                summary.insert("session_id", snapshot.id.toString());
                summary.insert("name", snapshot.name);
                summary.insert("provider_id", snapshot.providerId);
                summary.insert("workspace_id", snapshot.workspaceId);
                sessions.append(summary);
            }
            // 列表必须作为完整单帧交付；超限返回明确错误，不断开可继续使用的连接。
            QByteArray encoded;
            wire::Error error;
            if (!wire::encodeJson(
                    {kProtocolVersion, "session.listed", request.requestId, {}, {{"sessions", sessions}}},
                    encoded, error))
            {
                sendError(client,
                          error.code == wire::ErrorCode::FrameTooLarge ? wire::ErrorCode::Busy
                                                                       : wire::ErrorCode::InternalError,
                          request.requestId);
                return;
            }
            sendWire(client, encoded, false);
            return;
        }
        const QStringList commands{"session.attach", "session.detach", "session.terminate", "session.resize",
                                   "session.heartbeat"};
        if (!commands.contains(type))
        {
            sendError(client, wire::ErrorCode::InvalidMessage, request.requestId, request.sessionId);
            return;
        }
        const auto id = SessionId::parse(request.sessionId);
        if (!id.isSuccess())
        {
            rejected(id.error());
            return;
        }
        const auto snapshot = m_manager.get(id.value(), client->id);
        if (!snapshot.isSuccess())
        {
            rejected(snapshot.error());
            return;
        }
        if (payload.contains("last_ack_sequence"))
        {
            sendError(client, wire::ErrorCode::InvalidPayload, request.requestId, request.sessionId);
            return;
        }
        if (type == "session.attach")
        {
            const auto mode =
                payload["mode"] == QJsonValue("read_write") ? AttachMode::ReadWrite : AttachMode::ReadOnly;
            const auto result = m_manager.attach(id.value(), client->id, mode);
            if (!result.isSuccess())
            {
                rejected(result.error());
                return;
            }
            if (!client->subscriptions.contains(request.sessionId))
            {
                if (client->nextSubscriptionGeneration == std::numeric_limits<quint64>::max())
                {
                    m_manager.detach(id.value(), client->id);
                    disconnectClient(client);
                    return;
                }
                Subscription subscription;
                subscription.generation = ++client->nextSubscriptionGeneration;
                client->subscriptions.insert(request.sessionId, subscription);
            }
            const auto attached = m_manager.get(id.value(), client->id);
            if (!attached.isSuccess())
            {
                rejected(attached.error());
                return;
            }
            reply("session.attached",
                  {{"mode", payload["mode"]},
                   {"replay_end_sequence", QString::number(attached.value().outputSequence)}});
            scheduleOutput(client, request.sessionId);
            emit m_owner.auditEvent(QStringLiteral("session_attached"), client->id);
            return;
        }
        if (type == "session.detach")
        {
            const auto result = m_manager.detach(id.value(), client->id);
            if (!result.isSuccess())
            {
                rejected(result.error());
                return;
            }
            client->subscriptions.remove(request.sessionId);
            reply("session.detached");
            emit m_owner.auditEvent(QStringLiteral("session_detached"), client->id);
            return;
        }
        if (type == "session.terminate")
        {
            const auto result = m_manager.terminate(id.value(), client->id);
            if (!result.isSuccess())
            {
                rejected(result.error());
                return;
            }
            reply("session.terminated");
            emit m_owner.auditEvent(QStringLiteral("session_terminate_accepted"), client->id);
            return;
        }
        if (type == "session.resize")
        {
            const auto result = m_manager.resize(id.value(), client->id,
                                                 {payload["columns"].toInt(), payload["rows"].toInt()});
            if (!result.isSuccess())
            {
                rejected(result.error());
                return;
            }
            reply("session.resized");
            return;
        }
        if (!client->subscriptions.contains(request.sessionId))
        {
            sendError(client, wire::ErrorCode::Forbidden, request.requestId, request.sessionId);
            return;
        }
        // 过期的写者不能通过保活重新抢占控制权，只续已有租约。
        if (snapshot.value().writerClientId == client->id)
        {
            const auto result = m_manager.attach(id.value(), client->id, AttachMode::ReadWrite);
            if (!result.isSuccess())
            {
                rejected(result.error());
                return;
            }
        }
        reply("session.heartbeat_ack");
    }
    void handleBinary(const ClientPtr &client, const QByteArray &bytes)
    {
        if (!admitMessage(client))
            return;
        if (!client->ready)
        {
            sendError(client, wire::ErrorCode::Unauthorized);
            return;
        }
        wire::TerminalFrame frame;
        wire::Error error;
        if (!wire::decodeBinary(bytes, frame, error))
        {
            sendError(client, error.code);
            return;
        }
        auto subscription = client->subscriptions.find(frame.sessionId);
        if (frame.kind != wire::TerminalKind::Input || subscription == client->subscriptions.end())
        {
            sendError(client, wire::ErrorCode::Forbidden, {}, frame.sessionId);
            return;
        }
        if (frame.sequence != subscription->inputSequence ||
            frame.sequence == std::numeric_limits<quint64>::max())
        {
            sendError(client, wire::ErrorCode::InvalidPayload, {}, frame.sessionId);
            return;
        }
        const auto id = SessionId::parse(frame.sessionId);
        const auto result = m_manager.writeInput(id.value(), client->id, frame.bytes);
        if (!result.isSuccess())
        {
            sendError(client, publicError(result.error().code), {}, frame.sessionId);
            return;
        }
        ++subscription->inputSequence;
    }
    void scheduleOutput(const ClientPtr &client, const QString &sessionId)
    {
        auto subscription = client->subscriptions.find(sessionId);
        if (client->closed || subscription == client->subscriptions.end() || subscription->outputPending)
            return;
        subscription->outputPending = true;
        const auto generation = subscription->generation;
        QTimer::singleShot(0, &m_owner, [this, client, sessionId, generation]
                           { drainOutput(client, sessionId, generation); });
    }
    void drainOutput(const ClientPtr &client, const QString &sessionId, quint64 generation)
    {
        auto subscription = client->subscriptions.find(sessionId);
        if (client->closed || subscription == client->subscriptions.end() ||
            subscription->generation != generation)
            return;
        subscription->outputPending = false;
        const auto id = SessionId::parse(sessionId);
        for (int turn = 0; turn < 4; ++turn)
        {
            const auto result = m_manager.takeOutput(id.value(), client->id, 16 * 1024);
            if (!result.isSuccess())
            {
                sendError(client, publicError(result.error().code), {}, sessionId);
                return;
            }
            const auto &chunk = result.value();
            if (chunk.droppedBytes > std::numeric_limits<quint64>::max() - subscription->outputOffset ||
                static_cast<quint64>(chunk.bytes.size()) >
                    std::numeric_limits<quint64>::max() - subscription->outputOffset - chunk.droppedBytes)
            {
                disconnectClient(client);
                return;
            }
            subscription->outputOffset += chunk.droppedBytes;
            if (chunk.droppedBytes &&
                !sendJson(client, {kProtocolVersion,
                                   "terminal.overrun",
                                   {},
                                   sessionId,
                                   {{"next_sequence", QString::number(subscription->outputOffset)},
                                    {"dropped_bytes", QString::number(chunk.droppedBytes)}}}))
                return;
            if (!chunk.bytes.isEmpty())
            {
                QByteArray encoded;
                wire::Error error;
                if (!wire::encodeBinary({kProtocolVersion, wire::TerminalKind::Output, sessionId,
                                         subscription->outputOffset, chunk.bytes},
                                        encoded, error))
                {
                    disconnectClient(client);
                    return;
                }
                if (!sendWire(client, encoded, true))
                    return;
                subscription->outputOffset += static_cast<quint64>(chunk.bytes.size());
            }
            if (!chunk.hasMore)
                return;
        }
        scheduleOutput(client, sessionId);
    }
    void tick()
    {
        const auto now = m_clock.elapsed();
        for (const auto &client : m_clients.values())
        {
            if ((!client->ready && now - client->connectedAt >= m_options.handshakeTimeoutMs) ||
                now - client->lastPong >= m_options.idleTimeoutMs)
            {
                disconnectClient(client);
                continue;
            }
            if (now - client->lastPing >= m_options.idleTimeoutMs / 3)
            {
                if (client->socket->bytesToWrite() + 32 > m_options.maxQueuedBytes)
                {
                    disconnectClient(client);
                    continue;
                }
                client->lastPing = now;
                client->socket->ping(QByteArrayLiteral("RADP/1"));
            }
        }
    }
    AgentWebSocketServer &m_owner;
    ISessionManager &m_manager;
    Options m_options;
    QWebSocketServer m_server;
    QTimer m_timer;
    QElapsedTimer m_clock;
    QHash<QWebSocket *, ClientPtr> m_clients;
};

AgentWebSocketServer::AgentWebSocketServer(ISessionManager &manager, Options options, QObject *parent)
    : QObject(parent), m_state(std::make_unique<State>(*this, manager, std::move(options)))
{
}
AgentWebSocketServer::~AgentWebSocketServer()
{
    QObject::disconnect(&m_state->m_manager, nullptr, this, nullptr);
    m_state->stop();
}
OperationResult AgentWebSocketServer::start()
{
    return m_state->start();
}
void AgentWebSocketServer::stop()
{
    m_state->stop();
}
bool AgentWebSocketServer::isListening() const
{
    return m_state->m_server.isListening();
}
quint16 AgentWebSocketServer::serverPort() const
{
    return m_state->m_server.serverPort();
}
int AgentWebSocketServer::connectionCount() const
{
    return m_state->m_clients.size();
}
}
