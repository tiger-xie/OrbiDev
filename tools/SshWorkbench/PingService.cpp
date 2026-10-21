#include "PingService.h"

#include <QNetworkInterface>
#include <QThread>
#include <QtEndian>
#include <cstring>
#include <memory>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// Winsock 类型必须先于 Windows 与 ICMP 声明。
#include <winsock2.h>
#include <ws2tcpip.h>
// Windows 句柄必须先于 IP Helper 声明。
#include <windows.h>
// ICMP API 依赖 IP Helper 的地址与回复结构。
#include <iphlpapi.h>
// ICMP 函数声明依赖上一项头文件。
#include <icmpapi.h>
namespace sshworkbench
{
namespace
{
struct ProbeResult
{
    PingService::State state = PingService::State::Unreachable;
    int milliseconds = -1;
    QString detail;
};
struct IcmpHandle
{
    HANDLE value = INVALID_HANDLE_VALUE;
    ~IcmpHandle()
    {
        if (value != INVALID_HANDLE_VALUE)
            IcmpCloseHandle(value);
    }
};
ProbeResult nativeProbe(const QHostAddress &address)
{
    const bool ipv6 = address.protocol() == QAbstractSocket::IPv6Protocol;
    IcmpHandle handle{ipv6 ? Icmp6CreateFile() : IcmpCreateFile()};
    if (handle.value == INVALID_HANDLE_VALUE)
        return {PingService::State::Unreachable, -1,
                QStringLiteral("无法创建 ICMP 请求（%1）").arg(GetLastError())};
    char payload[] = "OrbiDev";
    // 使用对齐缓冲区，满足 IPv4/IPv6 回复结构、数据和 ICMP 错误附加字节的空间要求。
    std::vector<quint64> reply(128, 0);
    DWORD count = 0;
    if (ipv6)
    {
        sockaddr_in6 source{};
        source.sin6_family = AF_INET6;
        sockaddr_in6 destination{};
        destination.sin6_family = AF_INET6;
        const auto bytes = address.toIPv6Address();
        std::memcpy(&destination.sin6_addr, bytes.c, sizeof(bytes.c));
        bool numericScope = false;
        destination.sin6_scope_id = address.scopeId().toUInt(&numericScope);
        if (!numericScope && !address.scopeId().isEmpty())
            destination.sin6_scope_id = QNetworkInterface::interfaceIndexFromName(address.scopeId());
        count = Icmp6SendEcho2(handle.value, nullptr, nullptr, nullptr, &source, &destination, payload,
                               sizeof(payload), nullptr, reply.data(), DWORD(reply.size() * sizeof(quint64)),
                               1000);
    }
    else
    {
        count = IcmpSendEcho(handle.value, qToBigEndian(address.toIPv4Address()), payload, sizeof(payload),
                             nullptr, reply.data(), DWORD(reply.size() * sizeof(quint64)), 1000);
    }
    DWORD status = count ? IP_SUCCESS : GetLastError();
    DWORD milliseconds = 0;
    if (count)
    {
        if (ipv6)
        {
            const auto *echo = reinterpret_cast<const ICMPV6_ECHO_REPLY *>(reply.data());
            status = echo->Status;
            milliseconds = echo->RoundTripTime;
        }
        else
        {
            const auto *echo = reinterpret_cast<const ICMP_ECHO_REPLY *>(reply.data());
            status = echo->Status;
            milliseconds = echo->RoundTripTime;
        }
    }
    if (status == IP_SUCCESS)
        return {PingService::State::Reachable, int(milliseconds),
                QStringLiteral("ICMP 往返延迟，不包含 DNS 时间")};
    if (status == IP_REQ_TIMED_OUT || status == WAIT_TIMEOUT)
        return {PingService::State::Timeout, -1,
                QStringLiteral("Ping 超时；主机可能禁止 ICMP，SSH 仍可能正常")};
    return {PingService::State::Unreachable, -1,
            QStringLiteral("Ping 不可测（%1）；不代表 SSH 已断开").arg(status)};
}
}

PingService::PingService(QObject *parent) : QObject(parent)
{
    qRegisterMetaType<State>();
    m_interval.setInterval(5000);
    connect(&m_interval, &QTimer::timeout, this, &PingService::probe);
    m_resolutionTimeout.setSingleShot(true);
    m_resolutionTimeout.setInterval(5000);
    connect(&m_resolutionTimeout, &QTimer::timeout, this,
            [this]
            {
                cancelLookup();
                ++m_generation;
                publish(State::ResolutionFailed, -1, QStringLiteral("主机名解析超时"));
            });
}
PingService::~PingService()
{
    cancelLookup();
}
void PingService::setTarget(const QString &host, const QString &sessionKey)
{
    const QString trimmedHost = host.trimmed();
    if (trimmedHost.isEmpty() || sessionKey.isEmpty())
    {
        stop();
        return;
    }
    if (m_host == trimmedHost && m_sessionKey == sessionKey)
        return;
    cancelLookup();
    ++m_generation;
    m_host = trimmedHost;
    m_sessionKey = sessionKey;
    publish(State::Checking, -1, QStringLiteral("正在检测网络往返延迟"));
    m_interval.start();
    probe();
}
void PingService::stop()
{
    cancelLookup();
    ++m_generation;
    m_interval.stop();
    m_host.clear();
    m_sessionKey.clear();
    publish(State::Idle, -1, QStringLiteral("没有活动会话"));
}
void PingService::cancelLookup()
{
    if (m_lookupId != -1)
        QHostInfo::abortHostLookup(m_lookupId);
    m_lookupId = -1;
    m_resolving = false;
    m_resolutionTimeout.stop();
}
void PingService::probe()
{
    if (m_host.isEmpty() || m_probeBusy || m_resolving)
        return;
    const quint64 generation = m_generation;
    QHostAddress literal;
    if (literal.setAddress(m_host))
    {
        startProbe(generation, literal);
        return;
    }
    m_resolving = true;
    m_resolutionTimeout.start();
    if (m_resolveForTest)
        m_resolveForTest(m_host, generation);
    else
        m_lookupId = QHostInfo::lookupHost(m_host, this, [this, generation](const QHostInfo &info)
                                           { resolved(generation, info); });
}
void PingService::resolved(quint64 generation, const QHostInfo &info)
{
    if (generation != m_generation || !m_resolving)
        return;
    m_lookupId = -1;
    m_resolving = false;
    m_resolutionTimeout.stop();
    if (info.error() != QHostInfo::NoError || info.addresses().isEmpty())
    {
        publish(State::ResolutionFailed, -1, QStringLiteral("无法解析主机地址"));
        return;
    }
    // 优先 IPv4，与现有 Tailscale 家庭地址一致；仅 IPv6 的主机仍可探测。
    QHostAddress selected = info.addresses().first();
    for (const auto &address : info.addresses())
        if (address.protocol() == QAbstractSocket::IPv4Protocol)
        {
            selected = address;
            break;
        }
    startProbe(generation, selected);
}
void PingService::startProbe(quint64 generation, const QHostAddress &address)
{
    m_probeBusy = true;
    if (m_probeForTest)
    {
        m_probeForTest(address, generation);
        return;
    }
    const auto result = std::make_shared<ProbeResult>();
    auto *thread = QThread::create([address, result] { *result = nativeProbe(address); });
    connect(thread, &QThread::finished, this, [this, generation, result]
            { completeProbe(generation, result->state, result->milliseconds, result->detail); });
    // 线程不归 service 所有，关闭窗口不会等待 ICMP；完成通知的上下文自动阻止悬空访问。
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}
void PingService::completeProbe(quint64 generation, State state, int milliseconds, const QString &detail)
{
    m_probeBusy = false;
    if (generation == m_generation && !m_host.isEmpty())
        publish(state, milliseconds, detail);
    else if (!m_host.isEmpty())
        probe();
}
void PingService::publish(State state, int milliseconds, const QString &detail)
{
    m_state = state;
    m_milliseconds = milliseconds;
    emit resultChanged(state, milliseconds, detail);
}
}
