#pragma once

#include "FakeAgent.h"
#include <QPointer>

namespace rad::client
{
/// 窗口操作协调器。借用同线程 Agent，Agent 须比协调器存活更久。
/// 串行提交操作；忙时重复请求忽略，失败不改变已打开页面。
class ClientController final : public QObject
{
    Q_OBJECT
public:
    explicit ClientController(FakeAgent *agent, QObject *parent = nullptr);
    bool isConnected() const;
    bool isBusy() const;
    QVector<ClientSession> sessions() const;
    QStringList workspaces() const;
    QString error() const;
    void connectAgent();
    void disconnectAgent();
    void refresh();
    void createSession(const QString &name, const QString &workspace);
    void openSession(const QString &id);
    void detachSession(const QString &id);
    void terminateSession(const QString &id);
signals:
    void changed();
    void sessionOpened(const QString &id);
    void sessionCreated(const QString &id);
    void sessionDetached(const QString &id);
    void operationFailed(const QString &message);

private:
    bool begin(bool requireConnection = true);
    FakeAgent::Completion completion(const QString &action);
    FakeAgent *m_agent;
    QVector<ClientSession> m_sessions;
    QString m_error;
    bool m_busy = false;
    quint64 m_generation = 0;
};
}
