#include "PingService.h"
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QtTest>

namespace sshworkbench
{
class PingServiceTests : public QObject
{
    Q_OBJECT
private slots:
    void startsImmediatelyAndSameTargetIsIdempotent()
    {
        PingService service;
        int requests = 0;
        service.m_probeForTest = [&](const QHostAddress &, quint64) { ++requests; };
        QSignalSpy changes(&service, &PingService::resultChanged);
        service.setTarget(QStringLiteral("127.0.0.1"), QStringLiteral("tab1"));
        QCOMPARE(requests, 1);
        QCOMPARE(service.state(), PingService::State::Checking);
        QCOMPARE(service.m_interval.interval(), 5000);
        service.setTarget(QStringLiteral("127.0.0.1"), QStringLiteral("tab1"));
        QCOMPARE(changes.count(), 1);
        QCOMPARE(requests, 1);
        service.probe();
        QCOMPARE(requests, 1);
    }
    void changedTargetWaitsAndRejectsOldResult()
    {
        PingService service;
        QList<QHostAddress> addresses;
        QList<quint64> generations;
        service.m_probeForTest = [&](const QHostAddress &address, quint64 generation)
        {
            addresses.append(address);
            generations.append(generation);
        };
        service.setTarget(QStringLiteral("127.0.0.1"), QStringLiteral("first"));
        service.setTarget(QStringLiteral("::1"), QStringLiteral("second"));
        service.setTarget(QStringLiteral("127.0.0.2"), QStringLiteral("third"));
        QCOMPARE(addresses.size(), 1);
        service.completeProbe(generations.first(), PingService::State::Reachable, 55, QString());
        QCOMPARE(service.state(), PingService::State::Checking);
        QCOMPARE(service.milliseconds(), -1);
        QCOMPARE(addresses.size(), 2);
        QCOMPARE(addresses.last(), QHostAddress(QStringLiteral("127.0.0.2")));
        service.completeProbe(generations.last(), PingService::State::Reachable, 0, QString());
        QCOMPARE(service.milliseconds(), 0);
    }
    void stopRejectsLateResultAndEmptyTargetIsIdle()
    {
        PingService service;
        quint64 request = 0;
        service.m_probeForTest = [&](const QHostAddress &, quint64 generation) { request = generation; };
        service.setTarget(QStringLiteral("::1"), QStringLiteral("tab"));
        service.stop();
        QVERIFY(!service.m_interval.isActive());
        service.completeProbe(request, PingService::State::Reachable, 10, QString());
        QCOMPARE(service.state(), PingService::State::Idle);
        QCOMPARE(service.milliseconds(), -1);
        service.setTarget(QString(), QStringLiteral("tab"));
        QCOMPARE(service.state(), PingService::State::Idle);
        service.setTarget(QStringLiteral("127.0.0.1"), QString());
        QCOMPARE(service.state(), PingService::State::Idle);
    }
    void timerDoesNotAccumulateRequests()
    {
        PingService service;
        int requests = 0;
        quint64 generation = 0;
        service.m_probeForTest = [&](const QHostAddress &, quint64 value)
        {
            ++requests;
            generation = value;
        };
        service.m_interval.setInterval(10);
        service.setTarget(QStringLiteral("127.0.0.1"), QStringLiteral("tab"));
        QTest::qWait(50);
        QCOMPARE(requests, 1);
        service.completeProbe(generation, PingService::State::Timeout, -1, QStringLiteral("超时"));
        QCOMPARE(service.state(), PingService::State::Timeout);
        QTRY_COMPARE(requests, 2);
        QTest::qWait(30);
        QCOMPARE(requests, 2);
    }
    void dnsFailureAndLateResolution()
    {
        PingService service;
        quint64 oldGeneration = 0;
        int requests = 0;
        service.m_resolveForTest = [&](const QString &, quint64 generation) { oldGeneration = generation; };
        service.m_probeForTest = [&](const QHostAddress &, quint64) { ++requests; };
        service.setTarget(QStringLiteral("first.invalid"), QStringLiteral("first"));
        QHostInfo failed;
        failed.setError(QHostInfo::HostNotFound);
        service.resolved(oldGeneration, failed);
        QCOMPARE(service.state(), PingService::State::ResolutionFailed);
        QCOMPARE(requests, 0);
        service.probe();
        const auto staleGeneration = oldGeneration;
        service.setTarget(QStringLiteral("second.invalid"), QStringLiteral("second"));
        QHostInfo valid;
        valid.setAddresses({QHostAddress(QStringLiteral("::1"))});
        service.resolved(staleGeneration, valid);
        QCOMPARE(requests, 0);
        service.resolved(oldGeneration, valid);
        QCOMPARE(requests, 1);
    }
    void dnsTimeoutRejectsLateReply()
    {
        PingService service;
        quint64 generation = 0;
        int requests = 0;
        service.m_resolveForTest = [&](const QString &, quint64 value) { generation = value; };
        service.m_probeForTest = [&](const QHostAddress &, quint64) { ++requests; };
        service.m_resolutionTimeout.setInterval(10);
        service.setTarget(QStringLiteral("timeout.invalid"), QStringLiteral("tab"));
        const auto stale = generation;
        QTRY_COMPARE(service.state(), PingService::State::ResolutionFailed);
        service.probe();
        QHostInfo valid;
        valid.setAddresses({QHostAddress(QStringLiteral("127.0.0.1"))});
        service.resolved(stale, valid);
        QCOMPARE(requests, 0);
        service.resolved(generation, valid);
        QCOMPARE(requests, 1);
    }
    void literalIpv6AndDnsIpv4Preference()
    {
        PingService service;
        QHostAddress selected;
        quint64 request = 0;
        service.m_probeForTest = [&](const QHostAddress &address, quint64 generation)
        {
            selected = address;
            request = generation;
        };
        service.setTarget(QStringLiteral("::1"), QStringLiteral("ipv6"));
        QCOMPARE(selected.protocol(), QAbstractSocket::IPv6Protocol);
        service.completeProbe(request, PingService::State::Reachable, 2, QString());
        service.m_resolveForTest = [&](const QString &, quint64 generation) { request = generation; };
        service.setTarget(QStringLiteral("dual.example"), QStringLiteral("dns"));
        QHostInfo valid;
        valid.setAddresses({QHostAddress(QStringLiteral("::1")), QHostAddress(QStringLiteral("127.0.0.1"))});
        service.resolved(request, valid);
        QCOMPARE(selected.protocol(), QAbstractSocket::IPv4Protocol);
    }
    void destructionDoesNotWaitForProbe()
    {
        QElapsedTimer elapsed;
        elapsed.start();
        {
            PingService service;
            service.m_probeForTest = [](const QHostAddress &, quint64) {};
            service.setTarget(QStringLiteral("127.0.0.1"), QStringLiteral("tab"));
        }
        QVERIFY(elapsed.elapsed() < 100);
    }
    void nativeLoopback_data()
    {
        QTest::addColumn<QString>("address");
        QTest::newRow("IPv4") << QStringLiteral("127.0.0.1");
        QTest::newRow("IPv6") << QStringLiteral("::1");
    }
    void nativeLoopback()
    {
        if (!qEnvironmentVariableIsSet("ORBIDEV_TEST_ICMP"))
            QSKIP("原生回环探测仅在显式设置 ORBIDEV_TEST_ICMP 时运行");
        QFETCH(QString, address);
        PingService service;
        service.setTarget(address, QStringLiteral("loopback"));
        QTRY_COMPARE_WITH_TIMEOUT(service.state(), PingService::State::Reachable, 3000);
        QVERIFY(service.milliseconds() >= 0);
    }
};
}
QTEST_GUILESS_MAIN(sshworkbench::PingServiceTests)
#include "PingServiceTests.moc"
