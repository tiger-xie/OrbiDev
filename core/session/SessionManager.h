#pragma once

#include <functional>
#include <memory>
#include <session/ISessionManager.h>
#include <vector>

namespace rad
{
class IProviderAdapter;
class ISessionRepository;
class ITerminalBackend;

// 仅在所属控制线程调用。配置是可信静态授权表；空工作区 ACL 拒绝所有身份。
// 管理器独占后端；detach 仅取消订阅，析构兜底关闭仍存活的进程。
class SessionManager final : public ISessionManager
{
    Q_OBJECT
public:
    struct Workspace
    {
        WorkspaceContext context;
        QStringList allowedClientIds;
    };
    struct Options
    {
        QVector<Workspace> workspaces;
        std::vector<std::shared_ptr<const IProviderAdapter>> providers;
        std::shared_ptr<ISessionRepository> repository;
        int maxConcurrentSessions = 4;
        int maxRetainedSessions = 128;
        int maxSubscribersPerSession = 16;
        int outputCapacityBytes = 1024 * 1024;
        int writerLeaseMs = 30000;
    };
    using BackendFactory = std::function<std::unique_ptr<ITerminalBackend>()>;

    explicit SessionManager(Options options, BackendFactory backendFactory, QObject *parent = nullptr);
    ~SessionManager() override;

    // 仓库已打开并完成重启标记后，在监听网络之前仅调用一次；不启动历史进程。
    OperationResult restoreSessions();

    Result<SessionId> create(const SessionRequest &request, const QString &clientId) override;
    Result<QVector<SessionSnapshot>> list(const QString &clientId) const override;
    Result<SessionSnapshot> get(SessionId id, const QString &clientId) const override;
    OperationResult attach(SessionId id, const QString &clientId, AttachMode mode) override;
    OperationResult detach(SessionId id, const QString &clientId) override;
    OperationResult writeInput(SessionId id, const QString &clientId, const QByteArray &bytes) override;
    OperationResult resize(SessionId id, const QString &clientId, TerminalSize size) override;
    OperationResult terminate(SessionId id, const QString &clientId) override;
    Result<OutputChunk> takeOutput(SessionId id, const QString &clientId, int maxBytes) override;

private:
    class State;
    std::unique_ptr<State> m_state;
};
}
