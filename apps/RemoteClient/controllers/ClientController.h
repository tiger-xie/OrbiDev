#pragma once

#include <QHash>
#include <QPointer>
#include <QSet>
#include <QSize>
#include <client/IAgentClient.h>

namespace rad::client
{
/// 窗口操作协调器。借用同线程 Agent，Agent 须比协调器存活更久。
/// 串行提交操作；忙时重复请求忽略，失败不改变已打开页面。
class ClientController final : public QObject
{
    Q_OBJECT
public:
    explicit ClientController(IAgentClient *agent, QObject *parent = nullptr);
    bool isConnected() const;
    bool isSimulation() const;
    QString displayName() const;
    void sendInput(const QString &id, const QByteArray &bytes);
    void resizeTerminal(const QString &id, int columns, int rows);
    void terminalReady(const QString &id);
    bool terminalIsReady(const QString &id) const;
    void terminalFailed(const QString &message);
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
    void outputReady(const QString &id, const QByteArray &bytes, int historyBytes);
    void sessionOpened(const QString &id);
    void sessionCreated(const QString &id);
    void sessionDetached(const QString &id);
    void operationFailed(const QString &message);

private:
    bool begin(bool requireConnection = true);
    IAgentClient::Completion completion(const QString &action);
    IAgentClient *m_agent;
    QVector<ClientSession> m_sessions;
    QString m_error;
    bool m_busy = false;
    quint64 m_generation = 0;
    QHash<QString, QByteArray> m_pendingOutput;
    QHash<QString, int> m_pendingHistoryBytes;
    QSet<QString> m_readyTerminals;
    QSet<QString> m_resizePending;
    QHash<QString, QSize> m_latestSizes;
};
}
