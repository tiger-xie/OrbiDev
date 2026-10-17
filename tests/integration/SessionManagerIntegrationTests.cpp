#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>
#include <WinConPtyBackend.h>
#include <array>
#include <memory>
#include <provider/IProviderAdapter.h>
#include <session/SessionManager.h>

using namespace rad;
namespace
{
class FakeProvider final : public IProviderAdapter
{
public:
    QString id() const override
    {
        return QStringLiteral("fake");
    }
    ProviderCapabilities capabilities() const override
    {
        return {};
    }
    Result<LaunchSpec> makeLaunchSpec(const SessionRequest &request,
                                      const WorkspaceContext &workspace) const override
    {
        return Result<LaunchSpec>::success(
            {qEnvironmentVariable("RAD_FAKECLI_PATH"), {}, workspace.workingDirectory, request.size});
    }
};
class OutputCapture final : public QObject
{
public:
    explicit OutputCapture(SessionManager &manager) : m_manager(manager)
    {
        connect(&manager, &ISessionManager::outputAvailable, this,
                [this](SessionId id, const QString &client) { drain(id, client); });
        connect(&manager, &ISessionManager::finished, this,
                [this](SessionId id, ExitStatus) { finished.append(id.toString()); });
    }
    void drain(SessionId id, const QString &client)
    {
        for (;;)
        {
            const auto result = m_manager.takeOutput(id, client, 4096);
            if (!result.isSuccess())
            {
                readFailed = true;
                return;
            }
            bytes[id.toString()] += result.value().bytes;
            dropped += result.value().droppedBytes;
            if (!result.value().hasMore)
                return;
        }
    }
    QMap<QString, QByteArray> bytes;
    QStringList finished;
    bool readFailed = false;
    quint64 dropped = 0;

private:
    SessionManager &m_manager;
};
}
class SessionManagerIntegrationTests final : public QObject
{
    Q_OBJECT
private slots:
    void detachedConcurrentProcesses();
};
void SessionManagerIntegrationTests::detachedConcurrentProcesses()
{
    QVERIFY(QFileInfo::exists(qEnvironmentVariable("RAD_FAKECLI_PATH")));
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    SessionManager::Options options;
    options.workspaces = {
        {{QStringLiteral("workspace"), workspace.path()}, {QStringLiteral("alice"), QStringLiteral("bob")}}};
    options.providers = {std::make_shared<FakeProvider>()};
    options.maxConcurrentSessions = 3;
    options.writerLeaseMs = 120000;
    SessionManager manager(
        options, [&workspace]
        { return std::make_unique<WinConPtyBackend>(WinConPtyBackend::Options{{workspace.path()}}); });
    OutputCapture capture(manager);
    QSignalSpy failures(&manager, &ISessionManager::failed);
    std::array<SessionId, 3> ids;
    for (int index = 0; index < 3; ++index)
    {
        const auto created = manager.create({QStringLiteral("会话%1").arg(index),
                                             QStringLiteral("fake"),
                                             QStringLiteral("workspace"),
                                             {},
                                             {80, 24}},
                                            "alice");
        QVERIFY(created.isSuccess());
        ids[static_cast<std::size_t>(index)] = created.value();
    }
    for (const auto id : ids)
    {
        QTRY_COMPARE_WITH_TIMEOUT(manager.get(id, "alice").value().status, SessionStatus::Running, 15000);
        QVERIFY(manager.attach(id, "alice", AttachMode::ReadWrite).isSuccess());
        QVERIFY(!manager.attach(id, "bob", AttachMode::ReadWrite).isSuccess());
        QVERIFY(!manager.writeInput(id, "bob", "echo WRONG\r").isSuccess());
        QTRY_VERIFY_WITH_TIMEOUT(capture.bytes[id.toString()].contains("[READY]"), 15000);
    }
    for (int index = 0; index < 3; ++index)
    {
        const auto id = ids[static_cast<std::size_t>(index)];
        const auto marker = QByteArray("session-") + QByteArray::number(index);
        QVERIFY(manager.writeInput(id, "alice", "echo " + marker + '\r').isSuccess());
        QTRY_VERIFY_WITH_TIMEOUT(capture.bytes[id.toString()].contains("[ECHO]" + marker), 15000);
    }
    for (int index = 0; index < 3; ++index)
    {
        const auto id = ids[static_cast<std::size_t>(index)];
        for (int other = 0; other < 3; ++other)
            if (other != index)
                QVERIFY(!capture.bytes[id.toString()].contains("[ECHO]session-" + QByteArray::number(other)));
        QVERIFY(manager.detach(id, "alice").isSuccess());
        QCOMPARE(manager.get(id, "alice").value().subscriberCount, 0);
        QCOMPARE(manager.get(id, "alice").value().status, SessionStatus::Running);
    }
    QTest::qWait(250);
    QVERIFY(capture.finished.isEmpty());
    for (const auto id : ids)
    {
        QVERIFY(manager.attach(id, "bob", AttachMode::ReadWrite).isSuccess());
        capture.bytes[id.toString()].clear();
        QVERIFY(manager.writeInput(id, "bob", "echo reattached\r").isSuccess());
        QTRY_VERIFY_WITH_TIMEOUT(capture.bytes[id.toString()].contains("[ECHO]reattached"), 15000);
        QVERIFY(manager.resize(id, "bob", {91, 27}).isSuccess());
    }
    // 正常退出验证最终尾部保留，其余会话显式终止验证权限与进程独立性。
    QVERIFY(manager.writeInput(ids[0], "bob", "exit\r").isSuccess());
    QTRY_VERIFY_WITH_TIMEOUT(capture.finished.contains(ids[0].toString()), 15000);
    capture.drain(ids[0], "bob");
    QVERIFY(capture.bytes[ids[0].toString()].contains("[BYE]"));
    for (int index = 1; index < 3; ++index)
    {
        const auto id = ids[static_cast<std::size_t>(index)];
        QCOMPARE(manager.get(id, "alice").value().status, SessionStatus::Running);
        QVERIFY(manager.terminate(id, "bob").isSuccess());
        QTRY_VERIFY_WITH_TIMEOUT(capture.finished.contains(id.toString()), 15000);
    }
    QCOMPARE(capture.finished.size(), 3);
    QCOMPARE(failures.count(), 0);
    QVERIFY(!capture.readFailed);
    QCOMPARE(capture.dropped, quint64(0));
}
QTEST_GUILESS_MAIN(SessionManagerIntegrationTests)
#include "SessionManagerIntegrationTests.moc"
