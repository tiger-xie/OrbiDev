#pragma once

#include <QHostAddress>
#include <QHostInfo>
#include <QObject>
#include <QTimer>
#include <functional>

namespace sshworkbench
{
// 方法及信号均在创建线程；host 必须是实际地址或 DNS 名称，SSH 别名由调用方解析。
// 同一目标幂等，每五秒探测一次；切换及停止会丢弃旧结果，最多保留一个 ICMP 请求。
// 析构不等待网络；后台请求仅持有地址和结果副本，最长等待一秒后自行释放。
class PingService final : public QObject
{
    Q_OBJECT
public:
    enum class State
    {
        Idle,
        Checking,
        Reachable,
        Timeout,
        Unreachable,
        ResolutionFailed
    };
    Q_ENUM(State)
    explicit PingService(QObject *parent = nullptr);
    ~PingService() override;
    void setTarget(const QString &host, const QString &sessionKey);
    void stop();
    State state() const
    {
        return m_state;
    }
    int milliseconds() const
    {
        return m_milliseconds;
    }

signals:
    void resultChanged(sshworkbench::PingService::State state, int milliseconds, const QString &detail);

private:
    friend class PingServiceTests;
    void probe();
    void resolved(quint64 generation, const QHostInfo &info);
    void startProbe(quint64 generation, const QHostAddress &address);
    void completeProbe(quint64 generation, State state, int milliseconds, const QString &detail);
    void publish(State state, int milliseconds, const QString &detail);
    void cancelLookup();

    QString m_host;
    QString m_sessionKey;
    quint64 m_generation = 0;
    State m_state = State::Idle;
    int m_milliseconds = -1;
    int m_lookupId = -1;
    bool m_resolving = false;
    bool m_probeBusy = false;
    QTimer m_interval;
    QTimer m_resolutionTimeout;
    std::function<void(const QString &, quint64)> m_resolveForTest;
    std::function<void(const QHostAddress &, quint64)> m_probeForTest;
};
}
Q_DECLARE_METATYPE(sshworkbench::PingService::State)
