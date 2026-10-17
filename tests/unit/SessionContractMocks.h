#pragma once
#include <QMap>
#include <QTimer>
#include <provider/IProviderAdapter.h>
#include <session/ISessionManager.h>
#include <terminal/ITerminalBackend.h>

namespace rad::test
{
// 仅演示接口的单会话内存替身，不创建进程、不访问磁盘、不代表生产实现。
inline Error invalidState()
{
    return {ErrorCode::InvalidState, QStringLiteral("测试状态不允许此操作")};
}

class MockProvider final : public IProviderAdapter
{
public:
    QString id() const override
    {
        return QStringLiteral("mock");
    }
    ProviderCapabilities capabilities() const override
    {
        return {};
    }
    Result<LaunchSpec> makeLaunchSpec(const SessionRequest &request,
                                      const WorkspaceContext &workspace) const override
    {
        if (request.providerId != id() || request.workspaceId != workspace.workspaceId ||
            workspace.workingDirectory.isEmpty())
            return Result<LaunchSpec>::failure(
                {ErrorCode::PermissionDenied, QStringLiteral("工作区上下文不匹配")});
        const auto sizeResult = request.size.validate();
        if (!sizeResult.isSuccess())
            return Result<LaunchSpec>::failure(sizeResult.error());
        return Result<LaunchSpec>::success({QStringLiteral("C:/mock/FakeCli.exe"),
                                            {QStringLiteral("参数 含空格")},
                                            workspace.workingDirectory,
                                            request.size});
    }
};

class MockTerminal final : public ITerminalBackend
{
public:
    explicit MockTerminal(bool failStart = false) : m_failStart(failStart)
    {
    }
    OperationResult start(const LaunchSpec &spec) override
    {
        if (m_started)
            return OperationResult::failure(invalidState());
        if (spec.executablePath.isEmpty() || spec.workingDirectory.isEmpty())
            return OperationResult::failure({ErrorCode::InvalidArgument, QStringLiteral("启动路径为空")});
        const auto sizeResult = spec.size.validate();
        if (!sizeResult.isSuccess())
            return sizeResult;
        m_started = true;
        QTimer::singleShot(0, this,
                           [this]
                           {
                               if (m_finished)
                                   return;
                               if (m_failStart)
                               {
                                   m_finished = true;
                                   emit failed({ErrorCode::LaunchFailed, QStringLiteral("模拟启动失败")},
                                               true);
                                   emit finished({0, false});
                                   return;
                               }
                               m_running = true;
                               emit started();
                           });
        return accepted();
    }
    OperationResult writeInput(const QByteArray &bytes) override
    {
        if (!m_running)
            return OperationResult::failure(invalidState());
        if (bytes.size() > 16)
            return OperationResult::failure({ErrorCode::QueueFull, QStringLiteral("测试输入容量不足")});
        m_input = bytes;
        return accepted();
    }
    OperationResult resize(TerminalSize size) override
    {
        return m_running ? size.validate() : OperationResult::failure(invalidState());
    }
    OperationResult terminate() override
    {
        if (!m_started || m_finished)
            return accepted();
        ++m_terminateCount;
        m_running = false;
        m_finished = true;
        QTimer::singleShot(0, this, [this] { emit finished({0, false}); });
        return accepted();
    }
    Result<OutputChunk> takeOutput(int maxBytes) override
    {
        if (maxBytes <= 0)
            return Result<OutputChunk>::failure(
                {ErrorCode::InvalidArgument, QStringLiteral("拉取容量必须为正")});
        const QByteArray bytes = m_output.left(maxBytes);
        m_output.remove(0, bytes.size());
        const auto dropped = m_dropped;
        m_dropped = 0;
        return Result<OutputChunk>::success({bytes, dropped, !m_output.isEmpty()});
    }
    void produce(const QByteArray &bytes)
    {
        const bool hadPending = !m_output.isEmpty() || m_dropped != 0;
        m_output += bytes;
        if (m_output.size() > 16)
        {
            m_dropped += static_cast<quint64>(m_output.size() - 16);
            m_output = m_output.right(16);
        }
        if (!hadPending && (!m_output.isEmpty() || m_dropped != 0))
            emit outputAvailable();
    }
    void failInput()
    {
        emit failed({ErrorCode::InputFailed, QStringLiteral("模拟写入失败")}, false);
    }
    const QByteArray &input() const
    {
        return m_input;
    }
    int terminateCount() const
    {
        return m_terminateCount;
    }

private:
    bool m_failStart = false;
    bool m_started = false;
    bool m_running = false;
    bool m_finished = false;
    int m_terminateCount = 0;
    QByteArray m_input;
    QByteArray m_output;
    quint64 m_dropped = 0;
};

// 固定一个已运行会话，权限仅用测试身份集合表达；不模拟认证或真实进程生命周期。
class MockSessionManager final : public ISessionManager
{
public:
    explicit MockSessionManager(bool running = true)
    {
        m_snapshot.id = SessionId::create();
        m_snapshot.creatorClientId = QStringLiteral("first");
        m_snapshot.status = running ? SessionStatus::Running : SessionStatus::Creating;
    }
    Result<SessionId> create(const SessionRequest &, const QString &) override
    {
        return Result<SessionId>::failure({ErrorCode::InvalidState, QStringLiteral("替身只提供固定会话")});
    }
    Result<QVector<SessionSnapshot>> list(const QString &clientId) const override
    {
        if (!authorized(clientId))
            return Result<QVector<SessionSnapshot>>::failure(denied());
        return Result<QVector<SessionSnapshot>>::success({m_snapshot});
    }
    Result<SessionSnapshot> get(SessionId id, const QString &clientId) const override
    {
        const auto checked = access(id, clientId);
        if (!checked.isSuccess())
            return Result<SessionSnapshot>::failure(checked.error());
        return Result<SessionSnapshot>::success(m_snapshot);
    }
    OperationResult attach(SessionId id, const QString &clientId, AttachMode mode) override
    {
        const auto checked = access(id, clientId);
        if (!checked.isSuccess())
            return checked;
        if (m_subscribers.contains(clientId))
            return m_subscribers.value(clientId) == mode ? accepted()
                                                         : OperationResult::failure(invalidState());
        if (mode == AttachMode::ReadWrite)
        {
            if (m_snapshot.status != SessionStatus::Running)
                return OperationResult::failure(invalidState());
            if (!m_snapshot.writerClientId.isEmpty())
                return OperationResult::failure({ErrorCode::ControlUnavailable, QStringLiteral("已有写者")});
            m_snapshot.writerClientId = clientId;
        }
        m_subscribers.insert(clientId, mode);
        m_outputs.insert(clientId, {QByteArray("retained"), 3, false});
        QTimer::singleShot(0, this,
                           [this, id, clientId]
                           {
                               if (m_subscribers.contains(clientId) &&
                                   (!m_outputs.value(clientId).bytes.isEmpty() ||
                                    m_outputs.value(clientId).droppedBytes != 0))
                                   emit outputAvailable(id, clientId);
                           });
        m_snapshot.subscriberCount = m_subscribers.size();
        emit stateChanged(m_snapshot);
        return accepted();
    }
    OperationResult detach(SessionId id, const QString &clientId) override
    {
        const auto checked = access(id, clientId);
        if (!checked.isSuccess())
            return checked;
        m_subscribers.remove(clientId);
        m_outputs.remove(clientId);
        if (m_snapshot.writerClientId == clientId)
            m_snapshot.writerClientId.clear();
        m_snapshot.subscriberCount = m_subscribers.size();
        emit stateChanged(m_snapshot);
        return accepted();
    }
    OperationResult writeInput(SessionId id, const QString &clientId, const QByteArray &) override
    {
        return writer(id, clientId);
    }
    OperationResult resize(SessionId id, const QString &clientId, TerminalSize size) override
    {
        const auto checked = writer(id, clientId);
        return checked.isSuccess() ? size.validate() : checked;
    }
    OperationResult terminate(SessionId id, const QString &clientId) override
    {
        const auto checked = access(id, clientId);
        if (!checked.isSuccess())
            return checked;
        if (m_snapshot.status == SessionStatus::Exiting || m_snapshot.status == SessionStatus::Exited)
            return accepted();
        const auto control =
            m_snapshot.status == SessionStatus::Creating && m_snapshot.creatorClientId == clientId
                ? accepted()
                : writer(id, clientId);
        if (!control.isSuccess())
            return control;
        m_snapshot.status = SessionStatus::Exiting;
        emit stateChanged(m_snapshot);
        QTimer::singleShot(0, this,
                           [this]
                           {
                               m_snapshot.status = SessionStatus::Exited;
                               emit stateChanged(m_snapshot);
                               emit finished(m_snapshot.id, {0, false});
                           });
        return accepted();
    }
    Result<OutputChunk> takeOutput(SessionId id, const QString &clientId, int maxBytes) override
    {
        const auto checked = access(id, clientId);
        if (!checked.isSuccess())
            return Result<OutputChunk>::failure(checked.error());
        if (!m_subscribers.contains(clientId))
            return Result<OutputChunk>::failure(denied());
        if (maxBytes <= 0)
            return Result<OutputChunk>::failure(
                {ErrorCode::InvalidArgument, QStringLiteral("拉取容量必须为正")});
        auto &pending = m_outputs[clientId];
        const auto bytes = pending.bytes.left(maxBytes);
        pending.bytes.remove(0, bytes.size());
        const auto dropped = pending.droppedBytes;
        pending.droppedBytes = 0;
        return Result<OutputChunk>::success({bytes, dropped, !pending.bytes.isEmpty()});
    }
    SessionId id() const
    {
        return m_snapshot.id;
    }

private:
    static bool authorized(const QString &clientId)
    {
        return clientId == QStringLiteral("first") || clientId == QStringLiteral("second");
    }
    static Error denied()
    {
        return {ErrorCode::PermissionDenied, QStringLiteral("测试身份无权限")};
    }
    OperationResult access(SessionId id, const QString &clientId) const
    {
        if (!authorized(clientId))
            return OperationResult::failure(denied());
        if (!id.isValid())
            return OperationResult::failure({ErrorCode::InvalidArgument, QStringLiteral("会话 ID 无效")});
        if (id != m_snapshot.id)
            return OperationResult::failure({ErrorCode::NotFound, QStringLiteral("会话不存在")});
        return accepted();
    }
    OperationResult writer(SessionId id, const QString &clientId) const
    {
        const auto checked = access(id, clientId);
        if (!checked.isSuccess())
            return checked;
        if (m_snapshot.writerClientId != clientId)
            return OperationResult::failure(denied());
        return m_snapshot.status == SessionStatus::Running ? accepted()
                                                           : OperationResult::failure(invalidState());
    }
    SessionSnapshot m_snapshot;
    QMap<QString, AttachMode> m_subscribers;
    QMap<QString, OutputChunk> m_outputs;
};
}
