#pragma once

#include <QHostAddress>
#include <QObject>
#include <memory>
#include <model/SessionTypes.h>

namespace rad
{
class ISessionManager;

// 同一控制线程使用；借用的管理器必须比服务器存活更久，且不得迁移线程。
// 每个身份最多一个连接；关闭连接或服务器只分离订阅，不终止会话。
class AgentWebSocketServer final : public QObject
{
    Q_OBJECT
public:
    struct Credential
    {
        QString clientId;
        QByteArray tokenSha256; // 64 位小写十六进制令牌的 SHA-256，保存原始 32 字节摘要。
    };
    struct Options
    {
        QHostAddress bindAddress{QHostAddress::LocalHost};
        quint16 port = 0;
        QVector<Credential> credentials;
        int maxConnections = 8;
        int handshakeTimeoutMs = 5000;
        int idleTimeoutMs = 30000;
        qint64 maxQueuedBytes = 1024 * 1024;
        int maxMessagesPerSecond = 256;
        int maxRequestsPerConnection = 4096;
    };
    explicit AgentWebSocketServer(ISessionManager &manager, Options options, QObject *parent = nullptr);
    ~AgentWebSocketServer() override;
    // 仅允许本机 IPv4 回环或本机 Tailscale 100.64/10 地址；失败不进入监听。
    OperationResult start();
    void stop();
    bool isListening() const;
    quint16 serverPort() const;
    int connectionCount() const;

signals:
    // 固定事件名和经配置的身份，不携带凭据、请求载荷或终端内容。
    void auditEvent(const QString &event, const QString &clientId);

private:
    class State;
    std::unique_ptr<State> m_state;
};
}
