#pragma once

#include <QByteArray>
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
    QString processState;
    bool exitCodeKnown = true;
};

// 同一 Qt 事件线程调用；借用者必须先于本对象销毁。
// 命令异步回调一次，销毁取消未完成回调。断线不终止进程，不重放输入。
// outputReceived 为原始 VT 字节；消费者必须有界缓存，溢出显式报告并断开。
class IAgentClient : public QObject
{
    Q_OBJECT
public:
    using Completion = std::function<void(const QString &error, const QString &sessionId)>;
    explicit IAgentClient(QObject *parent = nullptr) : QObject(parent)
    {
    }
    virtual bool isConnected() const = 0;
    virtual bool isSimulation() const = 0;
    virtual QString displayName() const = 0;
    virtual QVector<ClientSession> sessions() const = 0;
    virtual QStringList workspaces() const = 0;
    virtual void connectAgent(Completion completion) = 0;
    virtual void disconnectAgent() = 0;
    virtual void listSessions(Completion completion) = 0;
    virtual void createSession(const QString &name, const QString &workspace, Completion completion) = 0;
    virtual void attachSession(const QString &id, Completion completion) = 0;
    virtual void detachSession(const QString &id, Completion completion) = 0;
    virtual void terminateSession(const QString &id, Completion completion) = 0;
    // true 仅表示已提交；服务端异步拒绝经 errorOccurred 通知，不自动重试。
    virtual bool sendInput(const QString &id, const QByteArray &bytes, QString &error) = 0;
    virtual void resizeSession(const QString &id, int columns, int rows, Completion completion) = 0;
signals:
    void disconnected();
    void sessionsChanged();
    void outputReceived(const QString &sessionId, const QByteArray &bytes);
    void errorOccurred(const QString &message);
};
}
