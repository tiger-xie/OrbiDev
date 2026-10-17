#include "SessionContractMocks.h"
#include <QtTest>
#include <model/SessionTypes.h>
#include <provider/IProviderAdapter.h>
#include <session/ISessionManager.h>
#include <terminal/ITerminalBackend.h>

using namespace rad;

class SessionContractTests final : public QObject
{
    Q_OBJECT
private slots:
    void providerUsesResolvedWorkspace()
    {
        test::MockProvider provider;
        SessionRequest request;
        request.providerId = QStringLiteral("mock");
        request.workspaceId = QStringLiteral("workspace");
        const auto result =
            provider.makeLaunchSpec(request, {request.workspaceId, QStringLiteral("C:/trusted workspace")});
        QVERIFY(result.isSuccess());
        QCOMPARE(result.value().workingDirectory, QStringLiteral("C:/trusted workspace"));
        QCOMPARE(result.value().arguments.size(), 1);
        QCOMPARE(result.value().arguments.first(), QStringLiteral("参数 含空格"));
        const auto denied =
            provider.makeLaunchSpec(request, {QStringLiteral("other"), QStringLiteral("C:/other")});
        QVERIFY(!denied.isSuccess());
        QCOMPARE(denied.error().code, ErrorCode::PermissionDenied);
    }
    void asynchronousLaunchFailure()
    {
        test::MockTerminal terminal(true);
        QSignalSpy started(&terminal, &ITerminalBackend::started);
        QSignalSpy failed(&terminal, &ITerminalBackend::failed);
        QSignalSpy finished(&terminal, &ITerminalBackend::finished);
        QCOMPARE(terminal.start({}).error().code, ErrorCode::InvalidArgument);
        QVERIFY(terminal.start({QStringLiteral("C:/mock/FakeCli.exe"), {}, QStringLiteral("C:/mock"), {}})
                    .isSuccess());
        QCOMPARE(failed.count(), 0);
        QCOMPARE(finished.count(), 0);
        QTRY_COMPARE(finished.count(), 1);
        QCOMPARE(started.count(), 0);
        QCOMPARE(failed.count(), 1);
        QCOMPARE(qvariant_cast<Error>(failed.first().at(0)).code, ErrorCode::LaunchFailed);
        QVERIFY(failed.first().at(1).toBool());
        QVERIFY(terminal.terminate().isSuccess());
        QCOMPARE(finished.count(), 1);
    }
    void backendEventsAndBytes()
    {
        test::MockTerminal terminal;
        QSignalSpy started(&terminal, &ITerminalBackend::started);
        QSignalSpy failed(&terminal, &ITerminalBackend::failed);
        QSignalSpy finished(&terminal, &ITerminalBackend::finished);
        QVERIFY(terminal.start({QStringLiteral("C:/mock/FakeCli.exe"), {}, QStringLiteral("C:/mock"), {}})
                    .isSuccess());
        QCOMPARE(started.count(), 0);
        QVERIFY(!terminal.start({QStringLiteral("C:/mock/FakeCli.exe"), {}, QStringLiteral("C:/mock"), {}})
                     .isSuccess());
        QTRY_COMPARE(started.count(), 1);
        const QByteArray bytes = QByteArray::fromHex("2f1b5b33316de4b8ad00091b5b41");
        QVERIFY(terminal.writeInput(bytes).isSuccess());
        QCOMPARE(terminal.input(), bytes);
        QCOMPARE(terminal.writeInput(QByteArray(17, 'x')).error().code, ErrorCode::QueueFull);
        QCOMPARE(terminal.input(), bytes);
        terminal.failInput();
        QCOMPARE(failed.count(), 1);
        QCOMPARE(qvariant_cast<Error>(failed.first().at(0)).code, ErrorCode::InputFailed);
        QCOMPARE(failed.first().at(1).toBool(), false);
        QCOMPARE(finished.count(), 0);
        QVERIFY(terminal.terminate().isSuccess());
        QVERIFY(terminal.terminate().isSuccess());
        QCOMPARE(terminal.terminateCount(), 1);
        QTRY_COMPARE(finished.count(), 1);
        QVERIFY(!qvariant_cast<ExitStatus>(finished.first().at(0)).exitCodeValid);
        QVERIFY(!terminal.writeInput(bytes).isSuccess());
    }
    void boundedOutputAndNotifications()
    {
        test::MockTerminal terminal;
        QSignalSpy ready(&terminal, &ITerminalBackend::outputAvailable);
        const QByteArray bytes = QByteArray::fromHex("2f1b5b33316de4b8ad00091b5b41");
        terminal.produce(bytes);
        QCOMPARE(ready.count(), 1);
        QByteArray received;
        bool hasMore = true;
        while (hasMore)
        {
            const auto result = terminal.takeOutput(2);
            QVERIFY(result.isSuccess());
            QVERIFY(result.value().bytes.size() <= 2);
            received += result.value().bytes;
            hasMore = result.value().hasMore;
        }
        QCOMPARE(received, bytes);
        terminal.produce(QByteArray(10, 'a'));
        terminal.produce(QByteArray(10, 'b'));
        QCOMPARE(ready.count(), 2);
        const auto output = terminal.takeOutput(16);
        QCOMPARE(output.value().droppedBytes, quint64(4));
        QCOMPARE(output.value().bytes, QByteArray(6, 'a') + QByteArray(10, 'b'));
        QVERIFY(!output.value().hasMore);
        QVERIFY(terminal.takeOutput(16).value().bytes.isEmpty());
        QCOMPARE(terminal.takeOutput(16).value().droppedBytes, quint64(0));
        QVERIFY(!terminal.takeOutput(0).isSuccess());
    }
    void singleWriterAndDetach()
    {
        test::MockSessionManager manager;
        const auto id = manager.id();
        const QString first = QStringLiteral("first");
        const QString second = QStringLiteral("second");
        QSignalSpy finished(&manager, &ISessionManager::finished);
        QVERIFY(manager.attach(id, first, AttachMode::ReadWrite).isSuccess());
        QCOMPARE(manager.attach(id, second, AttachMode::ReadWrite).error().code,
                 ErrorCode::ControlUnavailable);
        QVERIFY(manager.attach(id, second, AttachMode::ReadOnly).isSuccess());
        QVERIFY(!manager.writeInput(id, second, "x").isSuccess());
        QVERIFY(!manager.resize(id, second, {}).isSuccess());
        QVERIFY(!manager.terminate(id, second).isSuccess());
        QVERIFY(manager.writeInput(id, first, "x").isSuccess());
        QVERIFY(manager.detach(id, first).isSuccess());
        QVERIFY(manager.detach(id, first).isSuccess());
        const auto detached = manager.get(id, first).value();
        QCOMPARE(detached.status, SessionStatus::Running);
        QCOMPARE(detached.subscriberCount, 1);
        QVERIFY(detached.writerClientId.isEmpty());
        QCOMPARE(finished.count(), 0);
        QVERIFY(!manager.takeOutput(id, first, 16).isSuccess());
        QVERIFY(manager.detach(id, second).isSuccess());
        QVERIFY(manager.attach(id, second, AttachMode::ReadWrite).isSuccess());
        QVERIFY(manager.terminate(id, second).isSuccess());
        QCOMPARE(manager.get(id, second).value().status, SessionStatus::Exiting);
        QTRY_COMPARE(finished.count(), 1);
        QCOMPARE(manager.get(id, second).value().status, SessionStatus::Exited);
        QVERIFY(manager.terminate(id, second).isSuccess());
        QCOMPARE(finished.count(), 1);
        QVERIFY(!manager.get(SessionId(), second).isSuccess());
        QCOMPARE(manager.get(SessionId::create(), second).error().code, ErrorCode::NotFound);
        QCOMPARE(manager.get(id, QStringLiteral("unknown")).error().code, ErrorCode::PermissionDenied);
    }
    void reattachNotifiesRetainedOutput()
    {
        test::MockSessionManager manager;
        const auto id = manager.id();
        const auto client = QStringLiteral("first");
        QSignalSpy ready(&manager, &ISessionManager::outputAvailable);
        QVERIFY(manager.attach(id, client, AttachMode::ReadOnly).isSuccess());
        QCOMPARE(ready.count(), 0);
        QTRY_COMPARE(ready.count(), 1);
        const auto output = manager.takeOutput(id, client, 100);
        QCOMPARE(output.value().bytes, QByteArray("retained"));
        QCOMPARE(output.value().droppedBytes, quint64(3));
        QVERIFY(manager.detach(id, client).isSuccess());
        QVERIFY(manager.attach(id, client, AttachMode::ReadOnly).isSuccess());
        QTRY_COMPARE(ready.count(), 2);
        QCOMPARE(manager.takeOutput(id, client, 100).value().bytes, QByteArray("retained"));
    }
    void backendCanCancelBeforeStarted()
    {
        test::MockTerminal terminal;
        QSignalSpy started(&terminal, &ITerminalBackend::started);
        QSignalSpy finished(&terminal, &ITerminalBackend::finished);
        QVERIFY(terminal.start({QStringLiteral("C:/mock/FakeCli.exe"), {}, QStringLiteral("C:/mock"), {}})
                    .isSuccess());
        QVERIFY(terminal.terminate().isSuccess());
        QCOMPARE(finished.count(), 0);
        QTRY_COMPARE(finished.count(), 1);
        QCOMPARE(started.count(), 0);
    }
    void subscriberReadsAreIndependent()
    {
        test::MockSessionManager manager;
        const auto id = manager.id();
        const auto first = QStringLiteral("first");
        const auto second = QStringLiteral("second");
        QVERIFY(manager.attach(id, first, AttachMode::ReadOnly).isSuccess());
        QVERIFY(manager.attach(id, second, AttachMode::ReadOnly).isSuccess());
        const auto prefix = manager.takeOutput(id, first, 3);
        QVERIFY(prefix.isSuccess());
        QCOMPARE(prefix.value().bytes, QByteArray("ret"));
        QVERIFY(prefix.value().hasMore);
        const auto other = manager.takeOutput(id, second, 100);
        QVERIFY(other.isSuccess());
        QCOMPARE(other.value().bytes, QByteArray("retained"));
        QCOMPARE(other.value().droppedBytes, quint64(3));
        const auto suffix = manager.takeOutput(id, first, 100);
        QVERIFY(suffix.isSuccess());
        QCOMPARE(suffix.value().bytes, QByteArray("ained"));
        QCOMPARE(suffix.value().droppedBytes, quint64(0));
        QVERIFY(!suffix.value().hasMore);
    }
    void creatorCanCancelDuringStartup()
    {
        test::MockSessionManager manager(false);
        QSignalSpy finished(&manager, &ISessionManager::finished);
        QVERIFY(!manager.terminate(manager.id(), QStringLiteral("second")).isSuccess());
        QVERIFY(manager.terminate(manager.id(), QStringLiteral("first")).isSuccess());
        QTRY_COMPARE(finished.count(), 1);
    }
    void queuedEventsKeepValueTypes()
    {
        test::MockSessionManager manager;
        SessionSnapshot received;
        int count = 0;
        connect(
            &manager, &ISessionManager::stateChanged, this,
            [&](const SessionSnapshot &snapshot)
            {
                received = snapshot;
                ++count;
            },
            Qt::QueuedConnection);
        QVERIFY(manager.attach(manager.id(), QStringLiteral("first"), AttachMode::ReadWrite).isSuccess());
        QCOMPARE(count, 0);
        QTRY_COMPARE(count, 1);
        QVERIFY(received.id == manager.id());
        QCOMPARE(received.writerClientId, QStringLiteral("first"));
    }
    void sessionIdValidation()
    {
        QVERIFY(!SessionId().isValid());
        QVERIFY(!SessionId::parse(QStringLiteral("not-an-id")).isSuccess());
        QVERIFY(!SessionId::parse(QStringLiteral("00000000-0000-0000-0000-000000000000")).isSuccess());
        const auto id = SessionId::create();
        const auto parsed = SessionId::parse(id.toString());
        QVERIFY(parsed.isSuccess());
        QVERIFY(parsed.value() == id);
        QCOMPARE(id.toString().size(), 36);
    }
    void terminalSizeValidation()
    {
        QVERIFY(TerminalSize().validate().isSuccess());
        QVERIFY((TerminalSize{1, 32767}).validate().isSuccess());
        QVERIFY(!(TerminalSize{0, 24}).validate().isSuccess());
        QVERIFY(!(TerminalSize{80, 32768}).validate().isSuccess());
        QCOMPARE((TerminalSize{-1, 24}).validate().error().code, ErrorCode::InvalidArgument);
    }
    void resultKeepsError()
    {
        const auto failed =
            Result<LaunchSpec>::failure({ErrorCode::PermissionDenied, QStringLiteral("工作区未授权")});
        QVERIFY(!failed.isSuccess());
        QCOMPARE(failed.error().code, ErrorCode::PermissionDenied);
        QCOMPARE(failed.error().message, QStringLiteral("工作区未授权"));
    }
};
QTEST_GUILESS_MAIN(SessionContractTests)
#include "SessionContractTests.moc"
