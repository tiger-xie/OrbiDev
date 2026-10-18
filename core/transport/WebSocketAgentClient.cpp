#include "WebSocketAgentClient.h"

#include <QElapsedTimer>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QNetworkProxy>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <QUuid>
#include <QWebSocket>
#include <limits>
#include <protocol/ProtocolCodec.h>

namespace rad::client
{
namespace
{
namespace wire = rad::protocol;
constexpr qint64 kQueueLimit = 1024 * 1024;
QString unavailable()
{
    return QStringLiteral("连接已断开；请求结果未确认，输入不会重放");
}
QString rejected(const QString &code)
{
    if (code == "forbidden")
        return QStringLiteral("服务器拒绝此操作，请检查权限或会话控制权");
    if (code == "busy")
        return QStringLiteral("服务器资源或会话控制权暂不可用");
    if (code == "session_not_found")
        return QStringLiteral("服务器中不存在此会话");
    if (code == "unauthorized")
        return QStringLiteral("Agent 身份验证失败");
    return QStringLiteral("Agent 拒绝了请求");
}
}
class WebSocketAgentClient::State
{
public:
    using Reply = std::function<void(const wire::Envelope &, const QString &)>;
    struct Pending
    {
        QString type;
        QString session;
        qint64 deadline;
        Reply reply;
    };
    struct WaitingAttachment
    {
        qint64 deadline;
        Completion completion;
    };
    struct Attachment
    {
        quint64 input = 0;
        quint64 output = 0;
        quint64 replayEnd = 0;
        bool hasOutput = false;
        bool historyTruncated = false;
    };
    State(WebSocketAgentClient &owner, Options options) : m_owner(owner), m_options(std::move(options))
    {
        m_clock.start();
        m_socket.setProxy(QNetworkProxy::NoProxy);
        m_socket.setReadBufferSize(wire::kMaxFrameBytes);
        m_socket.setMaxAllowedIncomingFrameSize(wire::kMaxFrameBytes);
        m_socket.setMaxAllowedIncomingMessageSize(wire::kMaxFrameBytes);
        QObject::connect(&m_socket, &QWebSocket::connected, &m_owner,
                         [this]
                         {
                             const auto generation = m_generation;
                             request("auth.hello", "auth.welcome", {},
                                     {{"versions", QJsonArray{rad::kProtocolVersion}}},
                                     [this, generation](const wire::Envelope &, const QString &error)
                                     {
                                         if (generation != m_generation)
                                             return;
                                         if (!error.isEmpty())
                                         {
                                             fail(QStringLiteral("Agent 协议协商失败"));
                                             return;
                                         }
                                         list(
                                             [this, generation](const QString &listError, const QString &)
                                             {
                                                 if (generation != m_generation)
                                                     return;
                                                 if (!listError.isEmpty())
                                                 {
                                                     fail(listError);
                                                     return;
                                                 }
                                                 m_ready = true;
                                                 m_lastPong = m_lastHeartbeat = m_clock.elapsed();
                                                 m_connecting = false;
                                                 auto completion = std::move(m_connectCompletion);
                                                 finish(std::move(completion), {}, {});
                                             });
                                     });
                         });
        QObject::connect(&m_socket, &QWebSocket::pong, &m_owner,
                         [this](quint64, const QByteArray &payload)
                         {
                             if (m_ready && payload == QByteArrayLiteral("RADP/1"))
                                 m_lastPong = m_clock.elapsed();
                         });
        QObject::connect(&m_socket, &QWebSocket::disconnected, &m_owner, [this] { fail(unavailable()); });
        QObject::connect(&m_socket, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error), &m_owner,
                         [this](QAbstractSocket::SocketError)
                         { fail(QStringLiteral("无法连接 Agent，请检查地址、网络和配对凭据")); });
        QObject::connect(&m_socket, &QWebSocket::textMessageReceived, &m_owner,
                         [this](const QString &text) { receive(text); });
        QObject::connect(&m_socket, &QWebSocket::binaryMessageReceived, &m_owner,
                         [this](const QByteArray &bytes) { receiveBinary(bytes); });
        QObject::connect(&m_timer, &QTimer::timeout, &m_owner, [this] { tick(); });
        m_timer.start(250);
    }
    ~State()
    {
        m_socket.disconnect(&m_owner);
        m_socket.abort();
    }
    void finish(Completion completion, const QString &error, const QString &id)
    {
        if (completion)
        {
            const auto generation = m_generation;
            QTimer::singleShot(
                0, &m_owner, [this, generation, completion = std::move(completion), error, id]
                { completion(error.isEmpty() && generation != m_generation ? unavailable() : error, id); });
        }
    }
    ClientSession *find(const QString &id)
    {
        for (auto &session : m_sessions)
            if (session.id == id)
                return &session;
        return nullptr;
    }
    void fail(const QString &error, bool notifyError = true)
    {
        if (m_failing)
            return;
        const bool active =
            m_connecting || m_ready || !m_pending.isEmpty() || !m_waitingAttachments.isEmpty();
        m_failing = true;
        ++m_generation;
        m_ready = false;
        m_connecting = false;
        m_socket.abort();
        m_attachments.clear();
        for (auto &session : m_sessions)
        {
            session.attached = false;
            session.readOnly = true;
        }
        auto pending = std::move(m_pending);
        m_pending.clear();
        auto waiting = std::move(m_waitingAttachments);
        m_waitingAttachments.clear();
        for (auto entry = waiting.begin(); entry != waiting.end(); ++entry)
            finish(std::move(entry->completion), error, entry.key());
        auto connecting = std::move(m_connectCompletion);
        for (auto &entry : pending)
            QTimer::singleShot(0, &m_owner, [reply = std::move(entry.reply)] { reply({}, "disconnected"); });
        finish(std::move(connecting), error, {});
        m_failing = false;
        if (active)
        {
            emit m_owner.sessionsChanged();
            emit m_owner.disconnected();
            if (notifyError)
                emit m_owner.errorOccurred(error);
        }
    }
    void request(const QString &type, const QString &response, const QString &id, const QJsonObject &payload,
                 Reply reply)
    {
        if (m_socket.state() != QAbstractSocket::ConnectedState || m_pending.size() >= 64)
        {
            QTimer::singleShot(0, &m_owner, [reply = std::move(reply)] { reply({}, "unavailable"); });
            return;
        }
        const QString requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QByteArray encoded;
        wire::Error error;
        if (!wire::encodeJson({rad::kProtocolVersion, type, requestId, id, payload}, encoded, error))
        {
            QTimer::singleShot(0, &m_owner, [reply = std::move(reply)] { reply({}, "invalid_payload"); });
            return;
        }
        m_pending.insert(requestId, {response, id, m_clock.elapsed() + 5000, std::move(reply)});
        if (m_socket.bytesToWrite() + encoded.size() + 16 > kQueueLimit ||
            m_socket.sendTextMessage(QString::fromUtf8(encoded)) != encoded.size())
            fail(QStringLiteral("发送队列已满或发送失败，连接已断开"));
    }
    void updateState(ClientSession &session, const QJsonObject &payload)
    {
        session.processState = payload["process_state"].toString();
        session.running = session.processState == "running";
        session.exitCodeKnown = false;
    }
    void receive(const QString &text)
    {
        wire::Envelope message;
        wire::Error error;
        if (!wire::decodeJson(text.toUtf8(), message, error))
        {
            fail(QStringLiteral("Agent 返回无效协议消息"));
            return;
        }
        if (message.type == "terminal.overrun")
        {
            auto attachment = m_attachments.find(message.sessionId);
            auto *session = find(message.sessionId);
            const auto next = message.payload["next_sequence"].toString().toULongLong();
            const auto dropped = message.payload["dropped_bytes"].toString().toULongLong();
            // 当前附加总是从零开始；首包前可接受一次明确的历史截断，后续不得跳过缺口。
            if (!m_ready || !session || attachment == m_attachments.end() || attachment->hasOutput ||
                attachment->historyTruncated || next == 0 || next != dropped ||
                next == std::numeric_limits<quint64>::max())
            {
                fail(QStringLiteral("终端输出溢出通知与当前附加不匹配，无法保证连续性；请重新附加"));
                return;
            }
            attachment->historyTruncated = true;
            attachment->output = next;
            session->historyNotice = QStringLiteral("历史输出已截断，之前内容不可恢复；当前仅重放保留的原始输"
                                                    "出，不能精确恢复终端画面。回放写入期间暂不可输入");
            emit m_owner.sessionsChanged();
            return;
        }
        if (message.type == "session.status")
        {
            if (auto *session = find(message.sessionId))
            {
                updateState(*session, message.payload);
                emit m_owner.sessionsChanged();
                resumeAttachment(message.sessionId);
            }
            return;
        }
        if (message.type == "error" && message.requestId.isEmpty())
        {
            // 无请求关联的拒绝可能对应输入，不能继续发送并假设顺序仍然一致。
            fail(QStringLiteral("Agent 异步拒绝了会话操作；连接已断开且输入不会重放"));
            return;
        }
        auto found = m_pending.find(message.requestId);
        if (found == m_pending.end())
        {
            fail(QStringLiteral("Agent 响应无法关联到当前请求"));
            return;
        }
        const bool created = found->type == "session.created" && message.type == "session.created";
        if ((!created && message.sessionId != found->session) ||
            (message.type != "error" && message.type != found->type))
        {
            fail(QStringLiteral("Agent 响应类型或会话不匹配"));
            return;
        }
        auto callback = std::move(found->reply);
        m_pending.erase(found);
        callback(message, message.type == "error" ? message.payload["error"].toObject()["code"].toString()
                                                  : QString());
    }
    void receiveBinary(const QByteArray &bytes)
    {
        wire::TerminalFrame frame;
        wire::Error error;
        if (!wire::decodeBinary(bytes, frame, error) || frame.kind != wire::TerminalKind::Output ||
            !m_ready || !m_attachments.contains(frame.sessionId))
        {
            fail(QStringLiteral("收到未附加会话或无效的终端输出"));
            return;
        }
        auto &attachment = m_attachments[frame.sessionId];
        if (frame.sequence != attachment.output ||
            frame.sequence > std::numeric_limits<quint64>::max() - static_cast<quint64>(frame.bytes.size()))
        {
            fail(QStringLiteral("终端输出序号不连续，连接已断开"));
            return;
        }
        const int historyBytes = frame.sequence < attachment.replayEnd
                                     ? static_cast<int>(qMin(attachment.replayEnd - frame.sequence,
                                                             static_cast<quint64>(frame.bytes.size())))
                                     : 0;
        attachment.hasOutput = true;
        attachment.output = frame.sequence + static_cast<quint64>(frame.bytes.size());
        if (m_ready)
            emit m_owner.outputReceived(frame.sessionId, frame.bytes, historyBytes);
    }
    void list(Completion completion)
    {
        request("session.list", "session.listed", {}, {},
                [this, completion = std::move(completion)](const wire::Envelope &message,
                                                           const QString &error) mutable
                {
                    if (!error.isEmpty())
                    {
                        finish(std::move(completion), rejected(error), {});
                        return;
                    }
                    QVector<ClientSession> sessions;
                    for (const auto &value : message.payload["sessions"].toArray())
                    {
                        const auto object = value.toObject();
                        ClientSession session;
                        const auto id = object["session_id"].toString();
                        if (const auto *existing = find(id))
                            session = *existing;
                        session.id = id;
                        session.name = object["name"].toString();
                        session.provider = object["provider_id"].toString();
                        session.workspace = object["workspace_id"].toString();
                        updateState(session, object);
                        sessions.append(session);
                    }
                    m_sessions = std::move(sessions);
                    finish(std::move(completion), {}, {});
                    emit m_owner.sessionsChanged();
                });
    }
    bool lifecyclePending(const QString &id) const
    {
        for (const auto &pending : m_pending)
            if (pending.session == id &&
                (pending.type == "session.attached" || pending.type == "session.detached" ||
                 pending.type == "session.terminated"))
                return true;
        return false;
    }
    bool requireSession(const QString &id, Completion &completion)
    {
        if (!m_ready || !find(id))
        {
            finish(std::move(completion), QStringLiteral("尚未连接或会话不存在"), id);
            return false;
        }
        if (m_waitingAttachments.contains(id))
        {
            finish(std::move(completion), QStringLiteral("正在等待会话启动后附加"), id);
            return false;
        }
        for (const auto &pending : m_pending)
            if (pending.session == id && pending.type != "session.heartbeat_ack")
            {
                finish(std::move(completion), QStringLiteral("此会话的上一项操作尚未完成"), id);
                return false;
            }
        return true;
    }
    void resumeAttachment(const QString &id)
    {
        auto waiting = m_waitingAttachments.find(id);
        if (waiting == m_waitingAttachments.end())
            return;
        const auto *session = find(id);
        const bool timedOut = m_clock.elapsed() >= waiting->deadline;
        if (session && session->processState == "creating" && !timedOut)
            return;
        auto completion = std::move(waiting->completion);
        m_waitingAttachments.erase(waiting);
        if (!session || timedOut)
        {
            finish(std::move(completion),
                   timedOut ? QStringLiteral("等待会话启动超时，请稍后重新附加")
                            : QStringLiteral("等待启动的会话已不存在"),
                   id);
            return;
        }
        attach(id, false, std::move(completion));
    }
    void attach(const QString &id, bool readOnly, Completion completion)
    {
        const auto generation = m_generation;
        request(
            "session.attach", "session.attached", id, {{"mode", readOnly ? "read_only" : "read_write"}},
            [this, id, readOnly, generation,
             completion = std::move(completion)](const wire::Envelope &message, const QString &error) mutable
            {
                if (generation != m_generation)
                {
                    finish(std::move(completion), unavailable(), id);
                    return;
                }
                if (!readOnly && (error == "busy" || error == "forbidden"))
                {
                    attach(id, true, std::move(completion));
                    return;
                }
                if (!error.isEmpty())
                {
                    finish(std::move(completion), rejected(error), id);
                    return;
                }
                auto *session = find(id);
                if (!session)
                {
                    fail(QStringLiteral("附加响应的会话已不在列表中"));
                    finish(std::move(completion), unavailable(), id);
                    return;
                }
                if (message.payload["mode"].toString() != (readOnly ? "read_only" : "read_write"))
                {
                    finish(std::move(completion), QStringLiteral("服务器附加模式与请求不匹配"), id);
                    fail(QStringLiteral("服务器附加模式与请求不匹配"));
                    return;
                }
                bool validReplayEnd = false;
                const auto replayEnd =
                    message.payload["replay_end_sequence"].toString().toULongLong(&validReplayEnd);
                if (!validReplayEnd)
                {
                    const auto failure = QStringLiteral("Agent 未提供历史回放边界，请升级 Agent 后重新连接");
                    finish(std::move(completion), failure, id);
                    fail(failure);
                    return;
                }
                session->attached = true;
                session->readOnly = message.payload["mode"] == QJsonValue("read_only");
                session->writerAvailable = !session->readOnly;
                if (!m_attachments.contains(id))
                {
                    Attachment attachment;
                    attachment.replayEnd = replayEnd;
                    m_attachments.insert(id, attachment);
                    session->historyNotice =
                        QStringLiteral("本次附加重放 Agent "
                                       "保留的有限原始输出，不保证精确恢复终端画面。回放写入期间暂不可输入");
                }
                const bool attachedReadOnly = session->readOnly;
                finish(std::move(completion), {}, id);
                emit m_owner.sessionsChanged();
                if (attachedReadOnly)
                    emit m_owner.errorOccurred(QStringLiteral("会话已以只读方式附加，未获得输入控制权"));
            });
    }
    void tick()
    {
        const auto now = m_clock.elapsed();
        if (m_connecting && now >= m_connectDeadline)
        {
            fail(QStringLiteral("连接或协议协商超时"));
            return;
        }
        for (const auto &pending : m_pending)
            if (now >= pending.deadline)
            {
                fail(QStringLiteral("Agent 请求超时，结果未确认；连接已断开"));
                return;
            }
        // 先复制键，完成等待会移除条目；回调不得使遍历迭代器失效。
        for (const auto &id : m_waitingAttachments.keys())
            resumeAttachment(id);
        if (m_ready && now - m_lastPong >= 15000)
        {
            fail(QStringLiteral("Agent 心跳响应超时，连接已断开"));
            return;
        }
        if (!m_ready || now - m_lastHeartbeat < 5000)
            return;
        m_lastHeartbeat = now;
        m_socket.ping(QByteArrayLiteral("RADP/1"));
        const auto generation = m_generation;
        for (const auto &id : m_attachments.keys())
        {
            // 分离尚未确认时，后续心跳可能在服务端移除订阅后到达并被拒绝。
            if (lifecyclePending(id))
                continue;
            request("session.heartbeat", "session.heartbeat_ack", id, {},
                    [this, generation](const wire::Envelope &, const QString &error)
                    {
                        if (generation == m_generation && !error.isEmpty())
                            fail(QStringLiteral("会话续租失败，输入控制权已释放"));
                    });
        }
    }
    WebSocketAgentClient &m_owner;
    Options m_options;
    QWebSocket m_socket;
    QTimer m_timer;
    QElapsedTimer m_clock;
    QHash<QString, Pending> m_pending;
    QHash<QString, Attachment> m_attachments;
    QHash<QString, WaitingAttachment> m_waitingAttachments;
    QVector<ClientSession> m_sessions;
    Completion m_connectCompletion;
    quint64 m_generation = 0;
    qint64 m_connectDeadline = 0;
    qint64 m_lastHeartbeat = 0;
    qint64 m_lastPong = 0;
    bool m_ready = false;
    bool m_connecting = false;
    bool m_failing = false;
};

WebSocketAgentClient::WebSocketAgentClient(Options options, QObject *parent)
    : IAgentClient(parent), m_state(std::make_unique<State>(*this, std::move(options)))
{
}
WebSocketAgentClient::~WebSocketAgentClient() = default;
QString WebSocketAgentClient::validateOptions(const Options &options)
{
    const auto &url = options.url;
    const QHostAddress address(url.host());
    if (!url.isValid() || url.scheme() != "ws" || !url.userInfo().isEmpty() || url.hasQuery() ||
        url.hasFragment() || (!url.path().isEmpty() && url.path() != "/") || url.port() < 1 ||
        url.port() > 65535 || address.protocol() != QAbstractSocket::IPv4Protocol ||
        address.toString() != url.host() ||
        !(address.isLoopback() || address.isInSubnet(QHostAddress(QStringLiteral("100.64.0.0")), 10)))
        return QStringLiteral("地址必须是本机或 Tailscale IPv4 的 ws://地址:端口/，不能包含用户信息或参数");
    if (!QRegularExpression(QStringLiteral("\\A[0-9a-f]{64}\\z"))
             .match(QString::fromLatin1(options.token))
             .hasMatch())
        return QStringLiteral("配对凭据格式无效");
    const QRegularExpression id(QStringLiteral("\\A[A-Za-z0-9][A-Za-z0-9_.-]{0,63}\\z"));
    if (!id.match(options.providerId).hasMatch() || options.workspaces.isEmpty() ||
        options.workspaces.size() > 128)
        return QStringLiteral("Provider 或工作区配置无效");
    QSet<QString> known;
    for (const auto &workspace : options.workspaces)
    {
        if (!id.match(workspace).hasMatch() || known.contains(workspace))
            return QStringLiteral("工作区配置无效");
        known.insert(workspace);
    }
    return {};
}
bool WebSocketAgentClient::isConnected() const
{
    return m_state->m_ready;
}
bool WebSocketAgentClient::isSimulation() const
{
    return false;
}
QString WebSocketAgentClient::displayName() const
{
    return QStringLiteral("Agent · %1:%2")
        .arg(m_state->m_options.url.host())
        .arg(m_state->m_options.url.port());
}
QVector<ClientSession> WebSocketAgentClient::sessions() const
{
    return m_state->m_sessions;
}
QStringList WebSocketAgentClient::workspaces() const
{
    return m_state->m_options.workspaces;
}
void WebSocketAgentClient::connectAgent(Completion completion)
{
    auto &state = *m_state;
    const auto invalid = validateOptions(state.m_options);
    if (!invalid.isEmpty() || state.m_ready || state.m_connecting)
    {
        state.finish(std::move(completion),
                     invalid.isEmpty() ? QStringLiteral("连接已存在或正在建立") : invalid, {});
        return;
    }
    ++state.m_generation;
    state.m_connecting = true;
    state.m_connectCompletion = std::move(completion);
    state.m_lastPong = state.m_lastHeartbeat = state.m_clock.elapsed();
    state.m_connectDeadline = state.m_clock.elapsed() + 5000;
    QNetworkRequest request(state.m_options.url);
    request.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + state.m_options.token);
    state.m_socket.open(request);
}
void WebSocketAgentClient::disconnectAgent()
{
    m_state->fail(QStringLiteral("连接已断开，远端会话继续运行"), false);
}
void WebSocketAgentClient::listSessions(Completion completion)
{
    if (!isConnected())
    {
        m_state->finish(std::move(completion), unavailable(), {});
        return;
    }
    m_state->list(std::move(completion));
}
void WebSocketAgentClient::createSession(const QString &name, const QString &workspace, Completion completion)
{
    if (!isConnected() || !workspaces().contains(workspace))
    {
        m_state->finish(std::move(completion), QStringLiteral("尚未连接或工作区不在本地配置中"), {});
        return;
    }
    m_state->request("session.create", "session.created", {},
                     {{"name", name.trimmed()},
                      {"workspace_id", workspace},
                      {"provider_id", m_state->m_options.providerId},
                      {"columns", 80},
                      {"rows", 24}},
                     [this, name, workspace, completion = std::move(completion)](
                         const wire::Envelope &message, const QString &error) mutable
                     {
                         if (!error.isEmpty())
                         {
                             m_state->finish(std::move(completion), rejected(error), {});
                             return;
                         }
                         ClientSession session;
                         session.id = message.sessionId;
                         session.name = name.trimmed();
                         session.workspace = workspace;
                         session.provider = m_state->m_options.providerId;
                         m_state->updateState(session, message.payload);
                         m_state->m_sessions.append(session);
                         m_state->finish(std::move(completion), {}, session.id);
                         emit sessionsChanged();
                     });
}
void WebSocketAgentClient::attachSession(const QString &id, Completion completion)
{
    if (!m_state->requireSession(id, completion))
        return;
    if (m_state->m_attachments.contains(id))
    {
        m_state->finish(std::move(completion), {}, id);
        return;
    }
    // 创建响应仅表示接受启动，Creating 时申请写控制权会被误判为只读冲突。
    if (m_state->find(id)->processState == "creating")
    {
        m_state->m_waitingAttachments.insert(id, {m_state->m_clock.elapsed() + 5000, std::move(completion)});
        return;
    }
    m_state->attach(id, false, std::move(completion));
}
void WebSocketAgentClient::detachSession(const QString &id, Completion completion)
{
    if (!m_state->requireSession(id, completion))
        return;
    m_state->request(
        "session.detach", "session.detached", id, {},
        [this, id, completion = std::move(completion)](const wire::Envelope &, const QString &error) mutable
        {
            if (error.isEmpty())
            {
                m_state->m_attachments.remove(id);
                if (auto *session = m_state->find(id))
                {
                    session->attached = false;
                    session->readOnly = true;
                }
                emit sessionsChanged();
            }
            m_state->finish(std::move(completion), error.isEmpty() ? QString() : rejected(error), id);
        });
}
void WebSocketAgentClient::terminateSession(const QString &id, Completion completion)
{
    if (!m_state->requireSession(id, completion))
        return;
    m_state->request(
        "session.terminate", "session.terminated", id, {},
        [this, id, completion = std::move(completion)](const wire::Envelope &, const QString &error) mutable
        { m_state->finish(std::move(completion), error.isEmpty() ? QString() : rejected(error), id); });
}
bool WebSocketAgentClient::sendInput(const QString &id, const QByteArray &bytes, QString &error)
{
    const auto *session = m_state->find(id);
    if (!isConnected() || !session || !session->running || session->readOnly ||
        !m_state->m_attachments.contains(id) || m_state->lifecyclePending(id))
    {
        error = QStringLiteral("会话当前不可输入");
        return false;
    }
    if (bytes.isEmpty() || bytes.size() > 256 * 1024 ||
        m_state->m_socket.bytesToWrite() + bytes.size() + ((bytes.size() + 16383) / 16384) * 64 > kQueueLimit)
    {
        error = QStringLiteral("输入为空或超过有界发送队列容量");
        return false;
    }
    for (int offset = 0; offset < bytes.size(); offset += 16384)
    {
        auto &attachment = m_state->m_attachments[id];
        QByteArray encoded;
        wire::Error encodingError;
        if (attachment.input == std::numeric_limits<quint64>::max() ||
            !wire::encodeBinary({rad::kProtocolVersion, wire::TerminalKind::Input, id, attachment.input,
                                 bytes.mid(offset, 16384)},
                                encoded, encodingError) ||
            m_state->m_socket.sendBinaryMessage(encoded) != encoded.size())
        {
            error = QStringLiteral("输入发送失败，可能部分已发送；不会自动重放");
            m_state->fail(error);
            return false;
        }
        ++attachment.input;
    }
    error.clear();
    return true;
}
void WebSocketAgentClient::resizeSession(const QString &id, int columns, int rows, Completion completion)
{
    if (!m_state->requireSession(id, completion))
        return;
    const auto *session = m_state->find(id);
    if (!session->attached || session->readOnly || !session->running)
    {
        m_state->finish(std::move(completion), QStringLiteral("只有运行中会话的写入者可以调整窗口"), id);
        return;
    }
    m_state->request(
        "session.resize", "session.resized", id, {{"columns", columns}, {"rows", rows}},
        [this, id, completion = std::move(completion)](const wire::Envelope &, const QString &error) mutable
        { m_state->finish(std::move(completion), error.isEmpty() ? QString() : rejected(error), id); });
}
}
