#pragma once

#include <QHash>
#include <QObject>
#include <QStringList>
#include <QVector>
#include <functional>

namespace rad::client
{
struct ClientSession
{
    QString id;
    QString name;
    QString workspace;
    QString provider = QStringLiteral("FakeCli");
    bool running = true;
    bool attached = false;
    bool readOnly = true;
    bool writerAvailable = true;
    int exitCode = 0;
};

/// 仅用于界面验收的模拟 Agent，不启动进程、不访问网络。
/// 所有调用及回调均在所属线程；请求异步完成一次。销毁取消尚未完成的请求。
class FakeAgent final : public QObject
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
    bool isConnected() const;
    QVector<ClientSession> sessions() const;
    QStringList workspaces() const;
    int terminateCount() const;
    void failNext(Operation operation, const QString &message);
    void connectAgent(Completion completion);
    void disconnectAgent();
    void listSessions(Completion completion);
    void createSession(const QString &name, const QString &workspace, Completion completion);
    void attachSession(const QString &id, Completion completion);
    void detachSession(const QString &id, Completion completion);
    void terminateSession(const QString &id, Completion completion);
signals:
    void disconnected();

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
