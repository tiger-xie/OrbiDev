#pragma once

#include <QHash>
#include <QStringList>
#include <QVector>
#include <client/IAgentClient.h>
#include <functional>

namespace rad::client
{
/// 仅用于界面验收的模拟 Agent，不启动进程、不访问网络。
/// 所有调用及回调均在所属线程；请求异步完成一次。销毁取消尚未完成的请求。
class FakeAgent final : public IAgentClient
{
    Q_OBJECT
public:
    enum class Operation
    {
        Connect,
        List,
        Create,
        Attach,
        Detach,
        Terminate
    };
    using Completion = std::function<void(const QString &error, const QString &sessionId)>;
    explicit FakeAgent(QObject *parent = nullptr);
    bool isConnected() const override;
    bool isSimulation() const override
    {
        return true;
    }
    QString displayName() const override
    {
        return tr("FakeAgent · 本机模拟");
    }
    bool sendInput(const QString &, const QByteArray &, QString &error) override
    {
        error = tr("模拟模式没有终端输入。");
        return false;
    }
    void resizeSession(const QString &id, int, int, Completion completion) override
    {
        completion({}, id);
    }
    QVector<ClientSession> sessions() const override;
    QStringList workspaces() const override;
    int terminateCount() const;
    void failNext(Operation operation, const QString &message);
    void connectAgent(Completion completion) override;
    void disconnectAgent() override;
    void listSessions(Completion completion) override;
    void createSession(const QString &name, const QString &workspace, Completion completion) override;
    void attachSession(const QString &id, Completion completion) override;
    void detachSession(const QString &id, Completion completion) override;
    void terminateSession(const QString &id, Completion completion) override;

private:
    void request(Operation operation, const QString &id, Completion completion,
                 std::function<QString()> mutation);
    ClientSession *findSession(const QString &id);
    QVector<ClientSession> m_sessions;
    bool m_connected = false;
    quint64 m_generation = 0;
    int m_terminateCount = 0;
    QHash<int, QString> m_failures;
};
}
