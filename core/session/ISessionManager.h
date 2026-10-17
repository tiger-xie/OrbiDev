#pragma once
#include <QObject>
#include <model/SessionTypes.h>

namespace rad
{
// 同线程 parent 或调用方拥有；管理器拥有其创建的后端，销毁前终止并排空所有后端。
// 普通方法/信号只在对象所属 Agent 控制线程使用，非阻塞；跨线程必须受控调度。
// clientId 是经鉴权边界提供的身份标识，不是凭据；每次操作仍须校验会话访问权限。
// 参数借用到返回，异步操作复制；同步拒绝无完成事件、不改变状态。
// 异步启动/失败/退出事件须在请求返回后发送；fatal 错误后恰好一次 finished，
// 非致命错误不结束进程；Creating 正常取消不必发送 Running 或失败事件。
// finished 在尾部输出归并后发送，之后只可拉取已有缓存。
class ISessionManager : public QObject
{
    Q_OBJECT
public:
    explicit ISessionManager(QObject *parent = nullptr) : QObject(parent)
    {
        registerSessionMetaTypes();
    }
    ~ISessionManager() override = default;
    // 返回 ID 仅表示已接受；Creating→Running/Failed 经 stateChanged 报告，最终 finished 恰好一次。
    // Provider、工作区授权与容量由实现核验；成功创建不隐式附加或取得输入权。
    virtual Result<SessionId> create(const SessionRequest &request, const QString &clientId) = 0;
    virtual Result<QVector<SessionSnapshot>> list(const QString &clientId) const = 0;
    virtual Result<SessionSnapshot> get(SessionId id, const QString &clientId) const = 0;
    // attach 同步完成订阅；同一身份/模式重复幂等，模式变更拒绝，需先 detach。
    // 新附加/重附加从当前保留缓冲起点读取并报告此前截断量，不承诺完整画面恢复。
    // 已有数据/丢弃计数时在返回后发 outputAvailable，不能只等下一次后端输出。
    // ReadWrite 仅 Running 可申请且至多一名写者，冲突返回 ControlUnavailable，不自动抢占。
    virtual OperationResult attach(SessionId id, const QString &clientId, AttachMode mode) = 0;
    // 同步取消该身份订阅/释放控制权；重复分离幂等，不触发进程终止或改变进程状态。
    virtual OperationResult detach(SessionId id, const QString &clientId) = 0;
    // 输入/resize 仅 Running 写者可调用；有界入队与错误事件语义同 ITerminalBackend。
    virtual OperationResult writeInput(SessionId id, const QString &clientId, const QByteArray &bytes) = 0;
    virtual OperationResult resize(SessionId id, const QString &clientId, TerminalSize size) = 0;
    // 调用边界必须先取得用户显式确认；管理器仍验证访问权限。
    // Creating 允许创建者取消启动，Running 要求当前写者，避免启动阶段无法取消。
    // Exiting/终态的已授权重复请求幂等；接受不等于已退出，最终通过 finished 报告。
    virtual OperationResult terminate(SessionId id, const QString &clientId) = 0;
    // 仅已附加客户端可拉取独立游标，maxBytes>0；有界缓存/丢弃报告同后端。
    virtual Result<OutputChunk> takeOutput(SessionId id, const QString &clientId, int maxBytes) = 0;
signals:
    void stateChanged(const rad::SessionSnapshot &snapshot);
    void failed(rad::SessionId id, const rad::Error &error, bool fatal);
    void finished(rad::SessionId id, rad::ExitStatus status);
    // 每个订阅者合并通知，拉空前不重复；detach 后停止该身份通知，其他订阅不受影响。
    void outputAvailable(rad::SessionId id, const QString &clientId);
};
}
