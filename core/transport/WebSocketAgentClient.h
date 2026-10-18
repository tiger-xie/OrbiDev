#pragma once

#include <QUrl>
#include <client/IAgentClient.h>
#include <memory>

namespace rad::client
{
// 在所属 Qt 事件线程调用；不自动重连、重放输入或在断开时终止会话。
// 最多 64 个待响应请求，连接/请求超时 5 秒；每 5 秒心跳，15 秒无响应断开。
// 发送队列上限 1 MiB，单次输入最多 256 KiB，按 16 KiB 分块。
// 首包非零偏移仅表示部分历史输出并明确告警；后续断序或溢出立即断开。
// 主动断开仍完成待决回调并通知 disconnected，但不额外发布错误事件。
class WebSocketAgentClient final : public IAgentClient
{
    Q_OBJECT
public:
    struct Options
    {
        QUrl url;
        QByteArray token;
        // 仅用于本地选择，实际授权始终由服务器检查。
        QStringList workspaces;
        QString providerId = QStringLiteral("fake");
    };
    explicit WebSocketAgentClient(Options options, QObject *parent = nullptr);
    ~WebSocketAgentClient() override;
    static QString validateOptions(const Options &options);
    bool isConnected() const override;
    bool isSimulation() const override;
    QString displayName() const override;
    QVector<ClientSession> sessions() const override;
    QStringList workspaces() const override;
    void connectAgent(Completion completion) override;
    void disconnectAgent() override;
    void listSessions(Completion completion) override;
    void createSession(const QString &name, const QString &workspace, Completion completion) override;
    void attachSession(const QString &id, Completion completion) override;
    void detachSession(const QString &id, Completion completion) override;
    void terminateSession(const QString &id, Completion completion) override;
    bool sendInput(const QString &id, const QByteArray &bytes, QString &error) override;
    void resizeSession(const QString &id, int columns, int rows, Completion completion) override;

private:
    class State;
    std::unique_ptr<State> m_state;
};
}
