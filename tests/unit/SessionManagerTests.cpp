#include <QPointer>
#include <QtTest>
#include <limits>
#include <memory>
#include <persistence/ISessionRepository.h>
#include <provider/IProviderAdapter.h>
#include <session/SessionManager.h>
#include <stdexcept>
#include <terminal/ITerminalBackend.h>

using namespace rad;
namespace
{
class MemoryRepository final : public ISessionRepository
{
public:
    QVector<SessionSnapshot> records;
    bool rejectSave = false;
    OperationResult open(const QString &) override
    {
        return accepted();
    }
    OperationResult recover(qint64) override
    {
        return accepted();
    }
    Result<QVector<SessionSnapshot>> load(int) override
    {
        return Result<QVector<SessionSnapshot>>::success(records);
    }
    OperationResult save(const SessionSnapshot &snapshot) override
    {
        if (rejectSave)
            return OperationResult::failure({ErrorCode::SystemError, QStringLiteral("测试存储失败")});
        for (auto &record : records)
            if (record.id == snapshot.id)
            {
                record = snapshot;
                return accepted();
            }
        records.append(snapshot);
        return accepted();
    }
};
class TestProvider final : public IProviderAdapter
{
public:
    bool reject = false;
    bool changeWorkspace = false;
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
        if (reject)
            return Result<LaunchSpec>::failure({ErrorCode::ProviderUnavailable, QStringLiteral("测试拒绝")});
        return Result<LaunchSpec>::success(
            {QStringLiteral("C:/test/FakeCli.exe"),
             {},
             changeWorkspace ? QStringLiteral("C:/outside") : workspace.workingDirectory,
             request.size});
    }
};
class ControlledBackend final : public ITerminalBackend
{
public:
    bool rejectStart = false;
    bool throwStart = false;
    int terminateCount = 0;
    QByteArray input;
    QByteArray output;
    quint64 dropped = 0;
    OperationResult start(const LaunchSpec &) override
    {
        if (throwStart)
            throw std::runtime_error("fixture");
        return rejectStart
                   ? OperationResult::failure({ErrorCode::LaunchFailed, QStringLiteral("测试启动拒绝")})
                   : accepted();
    }
    OperationResult writeInput(const QByteArray &bytes) override
    {
        if (bytes.size() > 16)
            return OperationResult::failure({ErrorCode::QueueFull, QStringLiteral("测试队列已满")});
        input += bytes;
        return accepted();
    }
    OperationResult resize(TerminalSize size) override
    {
        return size.validate();
    }
    OperationResult terminate() override
    {
        ++terminateCount;
        return accepted();
    }
    Result<OutputChunk> takeOutput(int maxBytes) override
    {
        const QByteArray bytes = output.left(maxBytes);
        output.remove(0, bytes.size());
        const auto lost = dropped;
        dropped = 0;
        return Result<OutputChunk>::success({bytes, lost, !output.isEmpty()});
    }
    void produce(const QByteArray &bytes, quint64 lost = 0)
    {
        output += bytes;
        dropped += lost;
        emit outputAvailable();
    }
    void begin()
    {
        emit started();
    }
    void end()
    {
        emit finished({0, true});
    }
    void fail(bool fatal)
    {
        emit failed({ErrorCode::InputFailed, QStringLiteral("测试异步错误")}, fatal);
    }
};
SessionRequest request()
{
    return {QStringLiteral("测试会话"), QStringLiteral("fake"), QStringLiteral("workspace"), {}, {80, 24}};
}
struct Fixture
{
    std::shared_ptr<TestProvider> provider = std::make_shared<TestProvider>();
    QVector<ControlledBackend *> backends;
    bool rejectStart = false;
    bool throwStart = false;
    SessionManager::Options options() const
    {
        SessionManager::Options result;
        result.workspaces = {{{QStringLiteral("workspace"), QStringLiteral("C:/test")},
                              {QStringLiteral("alice"), QStringLiteral("bob"), QStringLiteral("carol")}}};
        result.providers = {provider};
        return result;
    }
    SessionManager::BackendFactory factory()
    {
        return [this]
        {
            auto backend = std::make_unique<ControlledBackend>();
            backend->rejectStart = rejectStart;
            backend->throwStart = throwStart;
            backends.append(backend.get());
            return backend;
        };
    }
};
}

class SessionManagerTests final : public QObject
{
    Q_OBJECT
private slots:
    void launchExceptionIsPersisted();
    void persistenceLifecycle();
    void restoreMetadataOnly();
    void persistenceFailureIsObservable();
    void recoveredHistoryDoesNotExhaustCapacity();
    void authorization();
    void rejectedCreation();
    void lifecycleAndCapacity();
    void writerAndDetach();
    void independentHistory();
    void outputSequenceDoesNotWrap();
    void leaseExpiry();
    void limits();
    void reentrantNotification();
    void tailAndFinalization();
};

void SessionManagerTests::launchExceptionIsPersisted()
{
    Fixture fixture;
    fixture.throwStart = true;
    auto repository = std::make_shared<MemoryRepository>();
    auto options = fixture.options();
    options.repository = repository;
    SessionManager manager(options, fixture.factory());
    QVERIFY(manager.restoreSessions().isSuccess());
    QVERIFY(!manager.create(request(), "alice").isSuccess());
    QCOMPARE(repository->records.size(), 1);
    QCOMPARE(repository->records.first().status, SessionStatus::Failed);
    QCOMPARE(repository->records.first().failureCode, QStringLiteral("launch_failed"));
    QCOMPARE(manager.list("alice").value().size(), 1);
    QVERIFY(!repository->records.first().exitCodeValid);
}

void SessionManagerTests::persistenceLifecycle()
{
    Fixture fixture;
    auto repository = std::make_shared<MemoryRepository>();
    auto options = fixture.options();
    options.repository = repository;
    SessionManager manager(options, fixture.factory());
    QVERIFY(!manager.create(request(), "alice").isSuccess());
    QVERIFY(manager.restoreSessions().isSuccess());
    repository->rejectSave = true;
    QVERIFY(!manager.create(request(), "alice").isSuccess());
    repository->rejectSave = false;
    const auto created = manager.create(request(), "alice");
    QVERIFY(created.isSuccess());
    QVERIFY(repository->records.first().createdAtUtcMs > 0);
    fixture.backends.last()->begin();
    QTRY_COMPARE(repository->records.last().status, SessionStatus::Running);
    fixture.backends.last()->end();
    QTRY_COMPARE(repository->records.last().status, SessionStatus::Exited);
    QVERIFY(repository->records.last().exitCodeValid);
    QCOMPARE(repository->records.last().exitCode, 0);
    QVERIFY(repository->records.last().updatedAtUtcMs >= repository->records.last().createdAtUtcMs);
}
void SessionManagerTests::recoveredHistoryDoesNotExhaustCapacity()
{
    Fixture fixture;
    auto repository = std::make_shared<MemoryRepository>();
    SessionSnapshot snapshot;
    snapshot.id = SessionId::create();
    snapshot.workspaceId = QStringLiteral("workspace");
    snapshot.status = SessionStatus::Exited;
    repository->records.append(snapshot);
    auto options = fixture.options();
    options.repository = repository;
    options.maxRetainedSessions = 1;
    SessionManager manager(options, fixture.factory());
    QVERIFY(manager.restoreSessions().isSuccess());
    const auto created = manager.create(request(), "alice");
    QVERIFY(created.isSuccess());
    QVERIFY(!manager.get(snapshot.id, "alice").isSuccess());
    QCOMPARE(repository->records.size(), 2);
    QVERIFY(!manager.create(request(), "alice").isSuccess());
    fixture.backends.last()->begin();
    QTRY_COMPARE(manager.get(created.value(), "alice").value().status, SessionStatus::Running);
    QVERIFY(manager.attach(created.value(), "alice", AttachMode::ReadOnly).isSuccess());
    fixture.backends.last()->end();
    QTRY_COMPARE(manager.get(created.value(), "alice").value().status, SessionStatus::Exited);
    QVERIFY(!manager.create(request(), "alice").isSuccess());
    QVERIFY(manager.detach(created.value(), "alice").isSuccess());
    QVERIFY(manager.create(request(), "alice").isSuccess());
}

void SessionManagerTests::persistenceFailureIsObservable()
{
    Fixture fixture;
    auto repository = std::make_shared<MemoryRepository>();
    auto options = fixture.options();
    options.repository = repository;
    SessionManager manager(options, fixture.factory());
    QVERIFY(manager.restoreSessions().isSuccess());
    const auto created = manager.create(request(), "alice");
    QVERIFY(created.isSuccess());
    QSignalSpy failure(&manager, &ISessionManager::failed);
    repository->rejectSave = true;
    fixture.backends.last()->begin();
    QTRY_COMPARE(failure.count(), 1);
    QCOMPARE(manager.get(created.value(), "alice").value().status, SessionStatus::Running);
    QCOMPARE(repository->records.first().status, SessionStatus::Creating);
    QVERIFY(manager.attach(created.value(), "alice", AttachMode::ReadOnly).isSuccess());
    QCOMPARE(manager.takeOutput(created.value(), "alice", 1024).value().bytes, QByteArray());
}

void SessionManagerTests::restoreMetadataOnly()
{
    Fixture fixture;
    auto repository = std::make_shared<MemoryRepository>();
    SessionSnapshot snapshot;
    snapshot.id = SessionId::create();
    snapshot.workspaceId = QStringLiteral("workspace");
    snapshot.status = SessionStatus::Failed;
    snapshot.failureCode = QStringLiteral("agent_restarted");
    repository->records.append(snapshot);
    auto options = fixture.options();
    options.repository = repository;
    SessionManager manager(options, fixture.factory());
    QVERIFY(manager.restoreSessions().isSuccess());
    QVERIFY(fixture.backends.isEmpty());
    QCOMPARE(manager.list("alice").value().size(), 1);
    QVERIFY(manager.list("intruder").value().isEmpty());
    QVERIFY(!manager.attach(snapshot.id, "alice", AttachMode::ReadOnly).isSuccess());
    QVERIFY(!manager.writeInput(snapshot.id, "alice", "x").isSuccess());
    QCOMPARE(manager.get(snapshot.id, "alice").value().failureCode, QStringLiteral("agent_restarted"));
    QVERIFY(!manager.restoreSessions().isSuccess());
}

void SessionManagerTests::authorization()
{
    Fixture fixture;
    SessionManager denied({}, fixture.factory());
    QVERIFY(!denied.create(request(), "alice").isSuccess());
    SessionManager manager(fixture.options(), fixture.factory());
    const auto created = manager.create(request(), "alice");
    QVERIFY(created.isSuccess());
    const auto id = created.value();
    QVERIFY(!manager.create(request(), "intruder").isSuccess());
    QVERIFY(!manager.get(id, "intruder").isSuccess());
    QVERIFY(!manager.attach(id, "intruder", AttachMode::ReadOnly).isSuccess());
    QVERIFY(!manager.detach(id, "intruder").isSuccess());
    QVERIFY(!manager.writeInput(id, "intruder", "x").isSuccess());
    QVERIFY(!manager.resize(id, "intruder", {80, 24}).isSuccess());
    QVERIFY(!manager.terminate(id, "intruder").isSuccess());
    QVERIFY(!manager.takeOutput(id, "intruder", 1).isSuccess());
    const auto listed = manager.list("intruder");
    QVERIFY(!listed.isSuccess() || listed.value().isEmpty());
    QVERIFY(!manager.get({}, "alice").isSuccess());
    QVERIFY(!manager.get(SessionId::create(), "alice").isSuccess());
    QCOMPARE(fixture.backends.size(), 1);
}
void SessionManagerTests::rejectedCreation()
{
    Fixture fixture;
    SessionManager manager(fixture.options(), fixture.factory());
    QSignalSpy state(&manager, &ISessionManager::stateChanged);
    fixture.provider->reject = true;
    QVERIFY(!manager.create(request(), "alice").isSuccess());
    QVERIFY(fixture.backends.isEmpty());
    fixture.provider->reject = false;
    auto worktree = request();
    worktree.worktreeName = QStringLiteral("unresolved");
    QVERIFY(!manager.create(worktree, "alice").isSuccess());
    fixture.provider->changeWorkspace = true;
    QVERIFY(!manager.create(request(), "alice").isSuccess());
    fixture.provider->changeWorkspace = false;
    fixture.rejectStart = true;
    QVERIFY(!manager.create(request(), "alice").isSuccess());
    QCoreApplication::processEvents();
    QVERIFY(manager.list("alice").isSuccess());
    QVERIFY(manager.list("alice").value().isEmpty());
    QCOMPARE(state.count(), 0);
}
void SessionManagerTests::lifecycleAndCapacity()
{
    Fixture fixture;
    auto options = fixture.options();
    options.maxConcurrentSessions = 1;
    SessionManager manager(options, fixture.factory());
    QSignalSpy finished(&manager, &ISessionManager::finished);
    QSignalSpy failed(&manager, &ISessionManager::failed);
    const auto created = manager.create(request(), "alice");
    QVERIFY(created.isSuccess());
    const auto id = created.value();
    QCOMPARE(manager.get(id, "alice").value().status, SessionStatus::Creating);
    QVERIFY(!manager.terminate(id, "bob").isSuccess());
    QVERIFY(manager.terminate(id, "alice").isSuccess());
    QVERIFY(manager.terminate(id, "alice").isSuccess());
    QCOMPARE(fixture.backends[0]->terminateCount, 1);
    QVERIFY(!manager.create(request(), "alice").isSuccess());
    fixture.backends[0]->end();
    QTRY_COMPARE(finished.count(), 1);
    const auto second = manager.create(request(), "alice");
    QVERIFY(second.isSuccess());
    fixture.backends[1]->fail(true);
    QVERIFY(!manager.create(request(), "alice").isSuccess());
    fixture.backends[1]->end();
    QTRY_COMPARE(finished.count(), 2);
    QTRY_COMPARE(failed.count(), 1);
    QCOMPARE(manager.get(second.value(), "alice").value().status, SessionStatus::Failed);
}
void SessionManagerTests::writerAndDetach()
{
    Fixture fixture;
    SessionManager manager(fixture.options(), fixture.factory());
    const auto created = manager.create(request(), "alice");
    QVERIFY(created.isSuccess());
    const auto id = created.value();
    fixture.backends[0]->begin();
    QTRY_COMPARE(manager.get(id, "alice").value().status, SessionStatus::Running);
    QVERIFY(manager.attach(id, "alice", AttachMode::ReadWrite).isSuccess());
    QVERIFY(manager.attach(id, "alice", AttachMode::ReadWrite).isSuccess());
    QVERIFY(!manager.attach(id, "alice", AttachMode::ReadOnly).isSuccess());
    QVERIFY(!manager.attach(id, "bob", AttachMode::ReadWrite).isSuccess());
    QVERIFY(manager.attach(id, "bob", AttachMode::ReadOnly).isSuccess());
    QVERIFY(!manager.writeInput(id, "bob", "forbidden").isSuccess());
    QVERIFY(!manager.resize(id, "bob", {90, 30}).isSuccess());
    QVERIFY(!manager.terminate(id, "bob").isSuccess());
    const auto full = manager.writeInput(id, "alice", QByteArray(17, 'x'));
    QVERIFY(!full.isSuccess());
    QCOMPARE(full.error().code, ErrorCode::QueueFull);
    QVERIFY(manager.writeInput(id, "alice", "hello").isSuccess());
    QCOMPARE(fixture.backends[0]->input, QByteArray("hello"));
    QVERIFY(manager.detach(id, "alice").isSuccess());
    QVERIFY(manager.detach(id, "alice").isSuccess());
    QVERIFY(manager.detach(id, "bob").isSuccess());
    QCOMPARE(fixture.backends[0]->terminateCount, 0);
    QCOMPARE(manager.get(id, "alice").value().status, SessionStatus::Running);
    QVERIFY(manager.attach(id, "bob", AttachMode::ReadWrite).isSuccess());
    QVERIFY(manager.terminate(id, "bob").isSuccess());
    QCOMPARE(fixture.backends[0]->terminateCount, 1);
}
void SessionManagerTests::independentHistory()
{
    Fixture fixture;
    auto options = fixture.options();
    options.outputCapacityBytes = 8;
    SessionManager manager(options, fixture.factory());
    const auto created = manager.create(request(), "alice");
    QVERIFY(created.isSuccess());
    const auto id = created.value();
    QVERIFY(manager.attach(id, "alice", AttachMode::ReadOnly).isSuccess());
    QVERIFY(manager.attach(id, "bob", AttachMode::ReadOnly).isSuccess());
    QCOMPARE(manager.get(id, "alice").value().outputSequence, quint64(0));
    QSignalSpy pending(&manager, &ISessionManager::outputAvailable);
    fixture.backends[0]->produce("123456");
    QTRY_VERIFY(pending.count() >= 2);
    QCOMPARE(manager.get(id, "alice").value().outputSequence, quint64(6));
    auto first = manager.takeOutput(id, "alice", 3);
    QVERIFY(first.isSuccess());
    QCOMPARE(first.value().bytes, QByteArray("123"));
    QVERIFY(first.value().hasMore);
    fixture.backends[0]->produce("789ABC");
    QCoreApplication::processEvents();
    auto alice = manager.takeOutput(id, "alice", 100);
    auto bob = manager.takeOutput(id, "bob", 100);
    QVERIFY(alice.isSuccess() && bob.isSuccess());
    QCOMPARE(alice.value().bytes, QByteArray("56789ABC"));
    QCOMPARE(alice.value().droppedBytes, quint64(1));
    QCOMPARE(bob.value().bytes, alice.value().bytes);
    QCOMPARE(bob.value().droppedBytes, quint64(4));
    QCOMPARE(manager.get(id, "alice").value().outputSequence, quint64(12));
    QVERIFY(!manager.takeOutput(id, "carol", 10).isSuccess());
    QVERIFY(!manager.takeOutput(id, "alice", 0).isSuccess());
    QSignalSpy notification(&manager, &ISessionManager::outputAvailable);
    QVERIFY(manager.attach(id, "carol", AttachMode::ReadOnly).isSuccess());
    QCOMPARE(notification.count(), 0);
    QTRY_VERIFY(notification.count() > 0);
    const auto history = manager.takeOutput(id, "carol", 100);
    QVERIFY(history.isSuccess());
    QCOMPARE(history.value().bytes, QByteArray("56789ABC"));
    QCOMPARE(history.value().droppedBytes, quint64(4));
    notification.clear();
    fixture.backends[0]->produce({}, 5);
    QTRY_VERIFY(notification.count() > 0);
    const auto lost = manager.takeOutput(id, "alice", 100);
    QVERIFY(lost.isSuccess());
    QCOMPARE(lost.value().droppedBytes, quint64(5));
    QCOMPARE(manager.get(id, "alice").value().outputSequence, quint64(17));
    fixture.backends[0]->produce("tail");
    QTRY_COMPARE(manager.get(id, "alice").value().outputSequence, quint64(21));
    QVERIFY(manager.detach(id, "alice").isSuccess());
    QVERIFY(manager.attach(id, "alice", AttachMode::ReadOnly).isSuccess());
    QCOMPARE(manager.get(id, "alice").value().outputSequence, quint64(21));
}
void SessionManagerTests::outputSequenceDoesNotWrap()
{
    Fixture fixture;
    SessionManager manager(fixture.options(), fixture.factory());
    const auto created = manager.create(request(), "alice");
    QVERIFY(created.isSuccess());
    const auto id = created.value();
    QSignalSpy failed(&manager, &ISessionManager::failed);
    fixture.backends[0]->produce({}, std::numeric_limits<quint64>::max());
    QTRY_COMPARE(manager.get(id, "alice").value().outputSequence, std::numeric_limits<quint64>::max());
    fixture.backends[0]->produce("x");
    QTRY_COMPARE(failed.count(), 1);
    QCOMPARE(manager.get(id, "alice").value().outputSequence, std::numeric_limits<quint64>::max());
    fixture.backends[0]->produce({}, 1);
    QTRY_COMPARE(failed.count(), 2);
    QCOMPARE(manager.get(id, "alice").value().outputSequence, std::numeric_limits<quint64>::max());
}
void SessionManagerTests::leaseExpiry()
{
    Fixture fixture;
    auto options = fixture.options();
    options.writerLeaseMs = 80;
    SessionManager manager(options, fixture.factory());
    const auto created = manager.create(request(), "alice");
    QVERIFY(created.isSuccess());
    const auto id = created.value();
    fixture.backends[0]->begin();
    QTRY_COMPARE(manager.get(id, "alice").value().status, SessionStatus::Running);
    QVERIFY(manager.attach(id, "alice", AttachMode::ReadWrite).isSuccess());
    QTest::qWait(40);
    QVERIFY(manager.attach(id, "alice", AttachMode::ReadWrite).isSuccess());
    QTest::qWait(50);
    QCOMPARE(manager.get(id, "alice").value().writerClientId, QStringLiteral("alice"));
    QVERIFY(manager.writeInput(id, "alice", "renew").isSuccess());
    QTest::qWait(50);
    QCOMPARE(manager.get(id, "alice").value().writerClientId, QStringLiteral("alice"));
    QThread::msleep(100);
    QVERIFY(manager.get(id, "alice").value().writerClientId.isEmpty());
    QVERIFY(!manager.writeInput(id, "alice", "expired").isSuccess());
    QVERIFY(manager.attach(id, "alice", AttachMode::ReadOnly).isSuccess());
    QVERIFY(!manager.attach(id, "alice", AttachMode::ReadWrite).isSuccess());
    QVERIFY(manager.detach(id, "alice").isSuccess());
    QVERIFY(manager.attach(id, "bob", AttachMode::ReadWrite).isSuccess());
    QCOMPARE(fixture.backends[0]->terminateCount, 0);
}
void SessionManagerTests::limits()
{
    Fixture fixture;
    auto options = fixture.options();
    options.maxRetainedSessions = 1;
    options.maxConcurrentSessions = 1;
    options.maxSubscribersPerSession = 1;
    SessionManager manager(options, fixture.factory());
    const auto created = manager.create(request(), "alice");
    QVERIFY(created.isSuccess());
    const auto id = created.value();
    QVERIFY(manager.attach(id, "alice", AttachMode::ReadOnly).isSuccess());
    QVERIFY(!manager.attach(id, "bob", AttachMode::ReadOnly).isSuccess());
    QVERIFY(manager.detach(id, "alice").isSuccess());
    QVERIFY(manager.attach(id, "bob", AttachMode::ReadOnly).isSuccess());
    fixture.backends[0]->end();
    QTRY_COMPARE(manager.get(id, "alice").value().status, SessionStatus::Exited);
    QVERIFY(!manager.create(request(), "alice").isSuccess());
}
void SessionManagerTests::reentrantNotification()
{
    Fixture fixture;
    SessionManager manager(fixture.options(), fixture.factory());
    const auto created = manager.create(request(), "alice");
    QVERIFY(created.isSuccess());
    const auto id = created.value();
    QVERIFY(manager.attach(id, "alice", AttachMode::ReadOnly).isSuccess());
    QVERIFY(manager.attach(id, "bob", AttachMode::ReadOnly).isSuccess());
    int notifications = 0;
    bool reattached = false;
    QObject::connect(&manager, &ISessionManager::outputAvailable, &manager,
                     [&](SessionId notifiedId, const QString &client)
                     {
                         QCOMPARE(notifiedId, id);
                         ++notifications;
                         if (client == "alice" && !reattached)
                         {
                             reattached = true;
                             QVERIFY(manager.detach(id, "bob").isSuccess());
                             QVERIFY(manager.attach(id, "bob", AttachMode::ReadOnly).isSuccess());
                         }
                         const auto chunk = manager.takeOutput(id, client, 100);
                         QVERIFY(chunk.isSuccess());
                         QCOMPARE(chunk.value().bytes, QByteArray("payload"));
                     });
    fixture.backends[0]->produce("payload");
    QTRY_VERIFY(reattached && notifications >= 2);
    QCoreApplication::processEvents();
    QVERIFY(notifications <= 3);
}
void SessionManagerTests::tailAndFinalization()
{
    Fixture fixture;
    SessionManager manager(fixture.options(), fixture.factory());
    const auto created = manager.create(request(), "alice");
    QVERIFY(created.isSuccess());
    const auto id = created.value();
    QVERIFY(manager.attach(id, "alice", AttachMode::ReadOnly).isSuccess());
    QPointer<ControlledBackend> backend = fixture.backends[0];
    QSignalSpy finished(&manager, &ISessionManager::finished);
    QSignalSpy failed(&manager, &ISessionManager::failed);
    bool sawTailAtFinished = false;
    connect(&manager, &ISessionManager::finished, &manager,
            [&](SessionId, ExitStatus)
            {
                const auto tail = manager.takeOutput(id, "alice", 100);
                sawTailAtFinished = tail.isSuccess() && tail.value().bytes == QByteArray("tail");
            });
    fixture.backends[0]->begin();
    QTRY_COMPARE(manager.get(id, "alice").value().status, SessionStatus::Running);
    fixture.backends[0]->fail(false);
    QTRY_COMPARE(failed.count(), 1);
    QCOMPARE(manager.get(id, "alice").value().status, SessionStatus::Running);
    // 不发输出提示，要求 finished 处理主动归并最后留在后端的字节。
    fixture.backends[0]->output = "tail";
    fixture.backends[0]->end();
    fixture.backends[0]->end();
    QTRY_COMPARE(finished.count(), 1);
    QVERIFY(sawTailAtFinished);
    QTRY_VERIFY(backend.isNull());
    QCoreApplication::processEvents();
    QCOMPARE(finished.count(), 1);
}
QTEST_GUILESS_MAIN(SessionManagerTests)
#include "SessionManagerTests.moc"
