#include "SessionManager.h"

#include <QDir>
#include <QElapsedTimer>
#include <QMap>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <map>
#include <provider/IProviderAdapter.h>
#include <terminal/ITerminalBackend.h>

namespace rad
{
namespace
{
Error error(ErrorCode code, const char *message)
{
    return {code, QString::fromUtf8(message)};
}
OperationResult reject(ErrorCode code, const char *message)
{
    return OperationResult::failure(error(code, message));
}
bool validIdentity(const QString &identity)
{
    return !identity.trimmed().isEmpty() && !identity.contains(QChar::Null);
}
}

class SessionManager::State
{
public:
    struct Subscriber
    {
        AttachMode mode = AttachMode::ReadOnly;
        quint64 cursor = 0;
        quint64 generation = 0;
        bool pending = false;
    };
    struct Session
    {
        SessionSnapshot snapshot;
        std::unique_ptr<ITerminalBackend> backend;
        QMap<QString, Subscriber> subscribers;
        QByteArray history;
        quint64 begin = 0;
        quint64 end = 0;
        qint64 leaseDeadline = 0;
        bool completed = false;
        bool fatal = false;
        bool drainPending = false;
    };

    State(SessionManager *owner, Options options, BackendFactory factory)
        : m_owner(owner), m_options(std::move(options)), m_factory(std::move(factory))
    {
        m_clock.start();
        m_valid = validateOptions();
        m_timer.setInterval(std::max(1, std::min(m_options.writerLeaseMs, 1000)));
        QObject::connect(&m_timer, &QTimer::timeout, owner,
                         [this]
                         {
                             for (auto &entry : m_sessions)
                                 expireLease(*entry.second);
                         });
        if (m_valid)
            m_timer.start();
    }
    ~State()
    {
        m_timer.stop();
        // 断开事件后再销毁后端，避免析构兜底回收过程中回调管理器。
        for (auto &entry : m_sessions)
            if (entry.second->backend)
                QObject::disconnect(entry.second->backend.get(), nullptr, m_owner, nullptr);
    }

    bool onOwnerThread() const
    {
        return QThread::currentThread() == m_owner->thread();
    }
    bool validateOptions() const
    {
        if (!m_factory || m_options.maxConcurrentSessions <= 0 || m_options.maxRetainedSessions <= 0 ||
            m_options.maxSubscribersPerSession <= 0 || m_options.outputCapacityBytes <= 0 ||
            m_options.writerLeaseMs <= 0)
            return false;
        QSet<QString> workspaceIds;
        for (const auto &workspace : m_options.workspaces)
        {
            if (!validIdentity(workspace.context.workspaceId) ||
                workspaceIds.contains(workspace.context.workspaceId) ||
                !QDir::isAbsolutePath(workspace.context.workingDirectory) ||
                workspace.context.workingDirectory.contains(QChar::Null))
                return false;
            workspaceIds.insert(workspace.context.workspaceId);
            for (const auto &client : workspace.allowedClientIds)
                if (!validIdentity(client))
                    return false;
        }
        QSet<QString> providerIds;
        try
        {
            for (const auto &provider : m_options.providers)
            {
                if (!provider)
                    return false;
                const QString id = provider->id();
                if (!validIdentity(id) || providerIds.contains(id))
                    return false;
                providerIds.insert(id);
            }
        }
        catch (...)
        {
            return false;
        }
        return true;
    }
    const Workspace *workspace(const QString &id) const
    {
        for (const auto &candidate : m_options.workspaces)
            if (candidate.context.workspaceId == id)
                return &candidate;
        return nullptr;
    }
    bool authorized(const QString &workspaceId, const QString &clientId) const
    {
        const auto *configured = workspace(workspaceId);
        return validIdentity(clientId) && configured && configured->allowedClientIds.contains(clientId);
    }
    Session *find(SessionId id) const
    {
        const auto found = m_sessions.find(id.toString());
        return found == m_sessions.end() ? nullptr : found->second.get();
    }
    Result<Session *> access(SessionId id, const QString &clientId) const
    {
        if (!onOwnerThread())
            return Result<Session *>::failure(error(ErrorCode::InvalidState, "必须在管理器所属线程调用"));
        if (!id.isValid() || !validIdentity(clientId))
            return Result<Session *>::failure(error(ErrorCode::InvalidArgument, "会话或身份标识无效"));
        Session *session = find(id);
        if (!session)
            return Result<Session *>::failure(error(ErrorCode::NotFound, "会话不存在"));
        if (!authorized(session->snapshot.workspaceId, clientId))
            return Result<Session *>::failure(error(ErrorCode::PermissionDenied, "无工作区访问权限"));
        return Result<Session *>::success(session);
    }
    void publish(const Session &session)
    {
        const auto snapshot = session.snapshot;
        QMetaObject::invokeMethod(
            m_owner, [owner = m_owner, snapshot] { emit owner->stateChanged(snapshot); },
            Qt::QueuedConnection);
    }
    void releaseWriter(Session &session)
    {
        const auto writer = session.subscribers.find(session.snapshot.writerClientId);
        if (writer != session.subscribers.end())
            writer->mode = AttachMode::ReadOnly;
        session.snapshot.writerClientId.clear();
        session.leaseDeadline = 0;
    }
    void expireLease(Session &session)
    {
        if (!session.snapshot.writerClientId.isEmpty() && m_clock.elapsed() >= session.leaseDeadline)
        {
            releaseWriter(session);
            publish(session);
        }
    }
    void renewLease(Session &session)
    {
        session.leaseDeadline = m_clock.elapsed() + m_options.writerLeaseMs;
    }
    OperationResult requireWriter(Session &session, const QString &clientId)
    {
        expireLease(session);
        if (session.snapshot.status != SessionStatus::Running)
            return reject(ErrorCode::InvalidState, "会话尚未运行或已结束");
        if (session.snapshot.writerClientId != clientId)
            return reject(ErrorCode::ControlUnavailable, "当前身份未持有输入控制权");
        return accepted();
    }
    void notify(Session &session, const QString &clientId)
    {
        auto subscriber = session.subscribers.find(clientId);
        if (subscriber == session.subscribers.end() || subscriber->pending ||
            subscriber->cursor >= session.end)
            return;
        subscriber->pending = true;
        const auto id = session.snapshot.id;
        const auto generation = subscriber->generation;
        QMetaObject::invokeMethod(
            m_owner,
            [this, id, clientId, generation]
            {
                auto *current = find(id);
                if (!current)
                    return;
                const auto attached = current->subscribers.constFind(clientId);
                if (attached == current->subscribers.cend() || attached->generation != generation ||
                    !attached->pending)
                    return;
                emit m_owner->outputAvailable(id, clientId);
            },
            Qt::QueuedConnection);
    }
    void append(Session &session, const OutputChunk &chunk)
    {
        if (chunk.droppedBytes != 0)
        {
            // 后端缺口前的历史不能与缺口后的字节拼接，否则 VT 流会伪装为连续输出。
            session.history.clear();
            session.end += chunk.droppedBytes;
            session.begin = session.end;
        }
        session.end += static_cast<quint64>(chunk.bytes.size());
        session.history.append(chunk.bytes);
        if (session.history.size() > m_options.outputCapacityBytes)
            session.history.remove(0, session.history.size() - m_options.outputCapacityBytes);
        session.begin = session.end - static_cast<quint64>(session.history.size());
        const auto clients = session.subscribers.keys();
        for (const auto &client : clients)
            notify(session, client);
    }
    void reportError(SessionId id, Error failure, bool fatal)
    {
        QMetaObject::invokeMethod(
            m_owner, [owner = m_owner, id, failure, fatal] { emit owner->failed(id, failure, fatal); },
            Qt::QueuedConnection);
    }
    void drain(Session &session, bool finalDrain = false)
    {
        if (!session.backend)
            return;
        try
        {
            for (int blocks = 0;; ++blocks)
            {
                const auto output = session.backend->takeOutput(65536);
                if (!output.isSuccess())
                {
                    reportError(session.snapshot.id, output.error(), false);
                    return;
                }
                append(session, output.value());
                if (!output.value().hasMore)
                    return;
                if (!finalDrain && blocks >= 15)
                {
                    scheduleDrain(session);
                    return;
                }
            }
        }
        catch (...)
        {
            reportError(session.snapshot.id, error(ErrorCode::SystemError, "读取终端输出失败"), false);
        }
    }
    void scheduleDrain(Session &session)
    {
        if (session.drainPending)
            return;
        session.drainPending = true;
        const auto id = session.snapshot.id;
        QMetaObject::invokeMethod(
            m_owner,
            [this, id]
            {
                auto *current = find(id);
                if (!current || current->completed)
                    return;
                current->drainPending = false;
                drain(*current);
            },
            Qt::QueuedConnection);
    }
    void connectBackend(Session &session)
    {
        const auto id = session.snapshot.id;
        auto *backend = session.backend.get();
        QObject::connect(
            backend, &ITerminalBackend::started, m_owner,
            [this, id]
            {
                auto *current = find(id);
                if (!current || current->completed || current->snapshot.status != SessionStatus::Creating)
                    return;
                current->snapshot.status = SessionStatus::Running;
                publish(*current);
            },
            Qt::QueuedConnection);
        QObject::connect(
            backend, &ITerminalBackend::outputAvailable, m_owner,
            [this, id]
            {
                auto *current = find(id);
                if (current && !current->completed && !current->drainPending)
                    drain(*current);
            },
            Qt::QueuedConnection);
        QObject::connect(
            backend, &ITerminalBackend::failed, m_owner,
            [this, id](const Error &failure, bool fatal)
            {
                auto *current = find(id);
                if (!current || current->completed || (fatal && current->fatal))
                    return;
                if (fatal)
                {
                    current->fatal = true;
                    current->snapshot.status = SessionStatus::Failed;
                    releaseWriter(*current);
                    publish(*current);
                }
                reportError(id, failure, fatal);
            },
            Qt::QueuedConnection);
        QObject::connect(
            backend, &ITerminalBackend::finished, m_owner,
            [this, id](ExitStatus status)
            {
                auto *current = find(id);
                if (!current || current->completed)
                    return;
                drain(*current, true);
                current->completed = true;
                current->snapshot.status = current->fatal ? SessionStatus::Failed : SessionStatus::Exited;
                releaseWriter(*current);
                // 此处理器由队列调用，后端原始 finished 发射栈已返回，可安全释放资源。
                current->backend.reset();
                publish(*current);
                QMetaObject::invokeMethod(
                    m_owner, [owner = m_owner, id, status] { emit owner->finished(id, status); },
                    Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
    }

    SessionManager *m_owner;
    Options m_options;
    BackendFactory m_factory;
    std::map<QString, std::unique_ptr<Session>> m_sessions;
    QElapsedTimer m_clock;
    QTimer m_timer;
    quint64 m_nextGeneration = 0;
    bool m_valid = false;
};

SessionManager::SessionManager(Options options, BackendFactory backendFactory, QObject *parent)
    : ISessionManager(parent),
      m_state(std::make_unique<State>(this, std::move(options), std::move(backendFactory)))
{
}
SessionManager::~SessionManager() = default;

Result<SessionId> SessionManager::create(const SessionRequest &request, const QString &clientId)
{
    auto &state = *m_state;
    if (!state.onOwnerThread())
        return Result<SessionId>::failure(error(ErrorCode::InvalidState, "必须在管理器所属线程调用"));
    if (!state.m_valid || !validIdentity(clientId) || !validIdentity(request.workspaceId) ||
        !validIdentity(request.providerId) || !request.worktreeName.isEmpty() ||
        !request.size.validate().isSuccess())
        return Result<SessionId>::failure(error(ErrorCode::InvalidArgument, "配置或创建参数无效"));
    if (!state.authorized(request.workspaceId, clientId))
        return Result<SessionId>::failure(error(ErrorCode::PermissionDenied, "无工作区访问权限"));
    const auto live = std::count_if(state.m_sessions.cbegin(), state.m_sessions.cend(),
                                    [](const auto &entry) { return !entry.second->completed; });
    if (live >= state.m_options.maxConcurrentSessions ||
        state.m_sessions.size() >= static_cast<size_t>(state.m_options.maxRetainedSessions))
        return Result<SessionId>::failure(error(ErrorCode::QueueFull, "会话容量已满"));
    try
    {
        const IProviderAdapter *selected = nullptr;
        for (const auto &provider : state.m_options.providers)
            if (provider->id() == request.providerId)
                selected = provider.get();
        if (!selected || !selected->capabilities().interactivePty)
            return Result<SessionId>::failure(error(ErrorCode::ProviderUnavailable, "终端 Provider 不可用"));
        const auto &context = state.workspace(request.workspaceId)->context;
        auto launch = selected->makeLaunchSpec(request, context);
        if (!launch.isSuccess())
            return Result<SessionId>::failure(launch.error());
        LaunchSpec spec = launch.value();
        // Provider 只控制程序及参数，不能改写授权工作区或请求尺寸。
        if (spec.workingDirectory != context.workingDirectory || spec.size.columns != request.size.columns ||
            spec.size.rows != request.size.rows)
            return Result<SessionId>::failure(
                error(ErrorCode::InvalidArgument, "Provider 改写了可信启动上下文"));
        auto session = std::make_unique<State::Session>();
        session->backend = state.m_factory();
        if (!session->backend || session->backend->parent() || session->backend->thread() != thread())
            return Result<SessionId>::failure(
                error(ErrorCode::InvalidState, "终端工厂返回了无效所有权或线程归属"));
        session->snapshot.id = SessionId::create();
        session->snapshot.name = request.name;
        session->snapshot.providerId = request.providerId;
        session->snapshot.workspaceId = request.workspaceId;
        session->snapshot.creatorClientId = clientId;
        state.connectBackend(*session);
        const auto started = session->backend->start(spec);
        if (!started.isSuccess())
            return Result<SessionId>::failure(started.error());
        const auto id = session->snapshot.id;
        const auto inserted = state.m_sessions.emplace(id.toString(), std::move(session));
        state.publish(*inserted.first->second);
        return Result<SessionId>::success(id);
    }
    catch (...)
    {
        return Result<SessionId>::failure(error(ErrorCode::SystemError, "创建终端会话失败"));
    }
}

Result<QVector<SessionSnapshot>> SessionManager::list(const QString &clientId) const
{
    if (!m_state->onOwnerThread())
        return Result<QVector<SessionSnapshot>>::failure(
            error(ErrorCode::InvalidState, "必须在管理器所属线程调用"));
    if (!validIdentity(clientId))
        return Result<QVector<SessionSnapshot>>::failure(error(ErrorCode::InvalidArgument, "身份标识无效"));
    QVector<SessionSnapshot> snapshots;
    for (const auto &entry : m_state->m_sessions)
    {
        auto &session = *entry.second;
        if (m_state->authorized(session.snapshot.workspaceId, clientId))
        {
            m_state->expireLease(session);
            snapshots.append(session.snapshot);
        }
    }
    return Result<QVector<SessionSnapshot>>::success(std::move(snapshots));
}
Result<SessionSnapshot> SessionManager::get(SessionId id, const QString &clientId) const
{
    const auto access = m_state->access(id, clientId);
    if (!access.isSuccess())
        return Result<SessionSnapshot>::failure(access.error());
    m_state->expireLease(*access.value());
    return Result<SessionSnapshot>::success(access.value()->snapshot);
}
OperationResult SessionManager::attach(SessionId id, const QString &clientId, AttachMode mode)
{
    const auto access = m_state->access(id, clientId);
    if (!access.isSuccess())
        return OperationResult::failure(access.error());
    if (mode != AttachMode::ReadOnly && mode != AttachMode::ReadWrite)
        return reject(ErrorCode::InvalidArgument, "订阅模式无效");
    auto &session = *access.value();
    m_state->expireLease(session);
    const auto existing = session.subscribers.constFind(clientId);
    if (existing != session.subscribers.cend())
    {
        if (existing->mode != mode)
            return reject(ErrorCode::InvalidState, "切换模式前须先分离订阅");
        if (mode == AttachMode::ReadWrite)
        {
            if (session.snapshot.writerClientId != clientId)
                return reject(ErrorCode::ControlUnavailable, "租约已释放，请分离后重新申请");
            m_state->renewLease(session);
        }
        return accepted();
    }
    if (session.subscribers.size() >= m_state->m_options.maxSubscribersPerSession)
        return reject(ErrorCode::QueueFull, "订阅容量已满");
    if (mode == AttachMode::ReadWrite)
    {
        if (session.snapshot.status != SessionStatus::Running)
            return reject(ErrorCode::InvalidState, "仅运行中的会话可申请控制权");
        if (!session.snapshot.writerClientId.isEmpty())
            return reject(ErrorCode::ControlUnavailable, "控制权已被其他身份持有");
        session.snapshot.writerClientId = clientId;
        m_state->renewLease(session);
    }
    State::Subscriber subscriber;
    subscriber.mode = mode;
    subscriber.generation = ++m_state->m_nextGeneration;
    session.subscribers.insert(clientId, subscriber);
    session.snapshot.subscriberCount = session.subscribers.size();
    m_state->publish(session);
    m_state->notify(session, clientId);
    return accepted();
}
OperationResult SessionManager::detach(SessionId id, const QString &clientId)
{
    const auto access = m_state->access(id, clientId);
    if (!access.isSuccess())
        return OperationResult::failure(access.error());
    auto &session = *access.value();
    m_state->expireLease(session);
    if (session.subscribers.remove(clientId) == 0)
        return accepted();
    if (session.snapshot.writerClientId == clientId)
    {
        session.snapshot.writerClientId.clear();
        session.leaseDeadline = 0;
    }
    session.snapshot.subscriberCount = session.subscribers.size();
    m_state->publish(session);
    return accepted();
}
OperationResult SessionManager::writeInput(SessionId id, const QString &clientId, const QByteArray &bytes)
{
    const auto access = m_state->access(id, clientId);
    if (!access.isSuccess())
        return OperationResult::failure(access.error());
    auto &session = *access.value();
    const auto permission = m_state->requireWriter(session, clientId);
    if (!permission.isSuccess())
        return permission;
    try
    {
        const auto result = session.backend->writeInput(bytes);
        if (result.isSuccess())
            m_state->renewLease(session);
        return result;
    }
    catch (...)
    {
        return reject(ErrorCode::InputFailed, "终端输入失败");
    }
}
OperationResult SessionManager::resize(SessionId id, const QString &clientId, TerminalSize size)
{
    const auto access = m_state->access(id, clientId);
    if (!access.isSuccess())
        return OperationResult::failure(access.error());
    auto &session = *access.value();
    const auto permission = m_state->requireWriter(session, clientId);
    if (!permission.isSuccess())
        return permission;
    const auto valid = size.validate();
    if (!valid.isSuccess())
        return valid;
    try
    {
        const auto result = session.backend->resize(size);
        if (result.isSuccess())
            m_state->renewLease(session);
        return result;
    }
    catch (...)
    {
        return reject(ErrorCode::ResizeFailed, "调整终端尺寸失败");
    }
}
OperationResult SessionManager::terminate(SessionId id, const QString &clientId)
{
    const auto access = m_state->access(id, clientId);
    if (!access.isSuccess())
        return OperationResult::failure(access.error());
    auto &session = *access.value();
    m_state->expireLease(session);
    if (session.completed || session.snapshot.status == SessionStatus::Exiting || session.fatal)
        return accepted();
    if (session.snapshot.status == SessionStatus::Creating)
    {
        if (session.snapshot.creatorClientId != clientId)
            return reject(ErrorCode::PermissionDenied, "只有创建者可取消启动");
    }
    else
    {
        const auto permission = m_state->requireWriter(session, clientId);
        if (!permission.isSuccess())
            return permission;
    }
    try
    {
        const auto result = session.backend->terminate();
        if (!result.isSuccess())
            return result;
        session.snapshot.status = SessionStatus::Exiting;
        m_state->releaseWriter(session);
        m_state->publish(session);
        return accepted();
    }
    catch (...)
    {
        return reject(ErrorCode::SystemError, "终止终端请求失败");
    }
}
Result<OutputChunk> SessionManager::takeOutput(SessionId id, const QString &clientId, int maxBytes)
{
    const auto access = m_state->access(id, clientId);
    if (!access.isSuccess())
        return Result<OutputChunk>::failure(access.error());
    if (maxBytes <= 0)
        return Result<OutputChunk>::failure(error(ErrorCode::InvalidArgument, "读取长度必须为正"));
    auto &session = *access.value();
    auto attached = session.subscribers.find(clientId);
    if (attached == session.subscribers.end())
        return Result<OutputChunk>::failure(error(ErrorCode::PermissionDenied, "当前身份尚未附加"));
    OutputChunk output;
    if (attached->cursor < session.begin)
    {
        output.droppedBytes = session.begin - attached->cursor;
        attached->cursor = session.begin;
    }
    const auto available = session.end - attached->cursor;
    const int length = static_cast<int>(std::min(available, static_cast<quint64>(maxBytes)));
    output.bytes = session.history.mid(static_cast<int>(attached->cursor - session.begin), length);
    attached->cursor += static_cast<quint64>(length);
    output.hasMore = attached->cursor < session.end;
    if (!output.hasMore)
    {
        attached->pending = false;
        attached->generation = ++m_state->m_nextGeneration;
    }
    return Result<OutputChunk>::success(std::move(output));
}
}
