#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QMap>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <WinConPtyBackend.h>
#include <functional>
#include <memory>
#include <provider/IProviderAdapter.h>
#include <session/SessionManager.h>
#include <transport/AgentWebSocketServer.h>
#include <transport/WebSocketAgentClient.h>

using namespace rad;
using namespace rad::client;

namespace
{
QByteArray testToken(char identity)
{
    // 固定凭据只用于回环测试，不接触真实配对密钥。
    return QByteArray(64, identity);
}

bool waitUntil(const std::function<bool()> &predicate, int timeout = 10000)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!predicate() && elapsed.elapsed() < timeout)
        QTest::qWait(10);
    return predicate();
}

struct CompletionResult
{
    bool completed = false;
    QString error;
    QString sessionId;
};

CompletionResult complete(const std::function<void(IAgentClient::Completion)> &operation)
{
    // 超时后回调仍可能到达，共享结果避免引用已离开栈的变量。
    auto result = std::make_shared<CompletionResult>();
    operation(
        [result](const QString &error, const QString &id)
        {
            result->error = error;
            result->sessionId = id;
            result->completed = true;
        });
    waitUntil([result] { return result->completed; });
    return *result;
}

class TestProvider final : public IProviderAdapter
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

SessionManager::Options sessionOptions(const QString &directory)
{
    SessionManager::Options options;
    options.workspaces = {{{"workspace", directory}, {"alice", "bob"}}};
    options.providers = {std::make_shared<TestProvider>()};
    options.writerLeaseMs = 120000;
    return options;
}

AgentWebSocketServer::Options serverOptions()
{
    AgentWebSocketServer::Options options;
    options.credentials = {
        {"alice", QCryptographicHash::hash(testToken('a'), QCryptographicHash::Sha256)},
        {"bob", QCryptographicHash::hash(testToken('b'), QCryptographicHash::Sha256)},
        {"outsider", QCryptographicHash::hash(testToken('c'), QCryptographicHash::Sha256)}};
    return options;
}

// 延迟启动事件，使创建成功和进程可写之间的窗口确定存在。
class DelayedBackend final : public ITerminalBackend
{
public:
    explicit DelayedBackend(int delayMs = 500) : m_delayMs(delayMs)
    {
        m_startTimer.setSingleShot(true);
        connect(&m_startTimer, &QTimer::timeout, this, [this] { emit started(); });
    }
    QByteArray input;
    OperationResult start(const LaunchSpec &) override
    {
        if (m_delayMs >= 0)
            m_startTimer.start(m_delayMs);
        return accepted();
    }
    OperationResult writeInput(const QByteArray &bytes) override
    {
        input += bytes;
        return accepted();
    }
    OperationResult resize(TerminalSize size) override
    {
        return size.validate();
    }
    OperationResult terminate() override
    {
        m_startTimer.stop();
        QTimer::singleShot(0, this, [this] { emit finished({0, true}); });
        return accepted();
    }
    Result<OutputChunk> takeOutput(int) override
    {
        return Result<OutputChunk>::success({});
    }

private:
    int m_delayMs;
    QTimer m_startTimer;
};

struct Fixture
{
    QTemporaryDir directory;
    SessionManager manager;
    AgentWebSocketServer server;
    explicit Fixture(SessionManager::BackendFactory factory = {})
        : manager(sessionOptions(directory.path()),
                  factory ? std::move(factory)
                          : SessionManager::BackendFactory(
                                [this]
                                {
                                    return std::make_unique<WinConPtyBackend>(
                                        WinConPtyBackend::Options{{directory.path()}});
                                })),
          server(manager, serverOptions())
    {
    }
    WebSocketAgentClient::Options clientOptions(char identity = 'a') const
    {
        WebSocketAgentClient::Options options;
        options.url = QUrl(QStringLiteral("ws://127.0.0.1:%1").arg(server.serverPort()));
        options.token = testToken(identity);
        options.workspaces = QStringList{QStringLiteral("workspace")};
        return options;
    }
    bool running(const QString &id)
    {
        const auto parsed = SessionId::parse(id);
        if (!parsed.isSuccess())
            return false;
        return waitUntil(
            [&]
            {
                const auto snapshot = manager.get(parsed.value(), "alice");
                return snapshot.isSuccess() && snapshot.value().status == SessionStatus::Running;
            });
    }
};

class OutputCapture final : public QObject
{
public:
    explicit OutputCapture(IAgentClient &client)
    {
        connect(&client, &IAgentClient::outputReceived, this,
                [this](const QString &id, const QByteArray &bytes) { output[id] += bytes; });
    }
    QMap<QString, QByteArray> output;
};

bool connectClient(IAgentClient &client)
{
    const auto result =
        complete([&](IAgentClient::Completion done) { client.connectAgent(std::move(done)); });
    return result.completed && result.error.isEmpty() && client.isConnected();
}
QString createSession(IAgentClient &client, const QString &name = QStringLiteral("回环中文测试"))
{
    const auto result = complete([&](IAgentClient::Completion done)
                                 { client.createSession(name, "workspace", std::move(done)); });
    return result.completed && result.error.isEmpty() ? result.sessionId : QString();
}
bool attachSession(IAgentClient &client, const QString &id)
{
    const auto result =
        complete([&](IAgentClient::Completion done) { client.attachSession(id, std::move(done)); });
    return result.completed && result.error.isEmpty();
}
}

class WebSocketAgentClientTests final : public QObject
{
    Q_OBJECT
private slots:
    void endpointValidationAndAuthentication();
    void immediateAttachWaitsForWritableProcess();
    void creatingAttachTimesOutWithoutReadOnlyFallback();
    void disconnectCancelsCreatingAttachment();
    void exitedSessionRemainsReadable();
    void terminalBytesAndResize();
    void threeSessionsRemainIsolated();
    void detachAndDisconnectPreserveProcess();
    void pendingDetachRejectsInputWithoutBlockingOtherSessions();
    void writerConflictAndWorkspaceAuthorization();
    void serverStopClearsAttachmentState();
};

void WebSocketAgentClientTests::endpointValidationAndAuthentication()
{
    Fixture fixture;
    QVERIFY(fixture.server.start().isSuccess());
    for (const QString &url : {QStringLiteral("ws://8.8.8.8:17890"), QStringLiteral("ws://example.com:17890"),
                               QStringLiteral("ws://127.0.0.1:17890/?token=secret"),
                               QStringLiteral("ws://user:secret@127.0.0.1:17890")})
    {
        auto options = fixture.clientOptions();
        options.url = QUrl(url);
        const QString error = WebSocketAgentClient::validateOptions(options);
        QVERIFY(!error.isEmpty());
        QVERIFY(!error.contains("secret"));
        QVERIFY(!error.contains(QString::fromLatin1(options.token)));
    }
    WebSocketAgentClient rejected(fixture.clientOptions('d'));
    QSignalSpy errors(&rejected, &IAgentClient::errorOccurred);
    const auto result =
        complete([&](IAgentClient::Completion done) { rejected.connectAgent(std::move(done)); });
    QVERIFY(result.completed);
    QVERIFY(!result.error.isEmpty());
    QVERIFY(!rejected.isConnected());
    QVERIFY(!result.error.contains(QString::fromLatin1(testToken('d'))));
    QVERIFY(!rejected.displayName().contains(QString::fromLatin1(testToken('d'))));
    for (const auto &arguments : errors)
        QVERIFY(!arguments.at(0).toString().contains(QString::fromLatin1(testToken('d'))));
    QVERIFY(fixture.manager.list("alice").value().isEmpty());
}

void WebSocketAgentClientTests::immediateAttachWaitsForWritableProcess()
{
    DelayedBackend *backend = nullptr;
    Fixture fixture(
        [&]
        {
            auto instance = std::make_unique<DelayedBackend>();
            backend = instance.get();
            return instance;
        });
    QVERIFY(fixture.server.start().isSuccess());
    WebSocketAgentClient client(fixture.clientOptions());
    QVERIFY(connectClient(client));
    const QString id = createSession(client);
    QVERIFY(!id.isEmpty());
    QCOMPARE(client.sessions().first().processState, QStringLiteral("creating"));
    // 与界面一致，创建回调后立即附加，不在测试中等 Running。
    QVERIFY(attachSession(client, id));
    const auto session = client.sessions().first();
    QVERIFY(session.attached);
    QVERIFY(!session.readOnly);
    QVERIFY(session.running);
    QString error;
    const QByteArray input = QStringLiteral("echo 中文输入\r").toUtf8();
    QVERIFY2(client.sendInput(id, input, error), qPrintable(error));
    QVERIFY(waitUntil([&] { return backend->input == input; }));
}

void WebSocketAgentClientTests::creatingAttachTimesOutWithoutReadOnlyFallback()
{
    Fixture fixture([] { return std::make_unique<DelayedBackend>(-1); });
    QVERIFY(fixture.server.start().isSuccess());
    WebSocketAgentClient client(fixture.clientOptions());
    QVERIFY(connectClient(client));
    const QString id = createSession(client);
    QVERIFY(!id.isEmpty());
    const auto result =
        complete([&](IAgentClient::Completion done) { client.attachSession(id, std::move(done)); });
    QVERIFY(result.completed);
    QVERIFY(result.error.contains(QStringLiteral("超时")));
    QVERIFY(client.isConnected());
    QVERIFY(!client.sessions().first().attached);
    QCOMPARE(fixture.manager.get(SessionId::parse(id).value(), "alice").value().subscriberCount, 0);
}

void WebSocketAgentClientTests::disconnectCancelsCreatingAttachment()
{
    Fixture fixture([] { return std::make_unique<DelayedBackend>(); });
    QVERIFY(fixture.server.start().isSuccess());
    WebSocketAgentClient client(fixture.clientOptions());
    QVERIFY(connectClient(client));
    const QString id = createSession(client);
    QVERIFY(!id.isEmpty());
    auto calls = std::make_shared<int>(0);
    auto result = std::make_shared<CompletionResult>();
    client.attachSession(id,
                         [calls, result](const QString &error, const QString &sessionId)
                         {
                             ++*calls;
                             result->completed = true;
                             result->error = error;
                             result->sessionId = sessionId;
                         });
    client.disconnectAgent();
    QVERIFY(waitUntil([&] { return result->completed; }));
    QVERIFY(!result->error.isEmpty());
    QCOMPARE(result->sessionId, id);
    QVERIFY(connectClient(client));
    QVERIFY(waitUntil([&] { return client.sessions().first().running; }));
    QVERIFY(!client.sessions().first().attached);
    QCOMPARE(*calls, 1);
    QCOMPARE(fixture.manager.get(SessionId::parse(id).value(), "alice").value().subscriberCount, 0);
}

void WebSocketAgentClientTests::exitedSessionRemainsReadable()
{
    Fixture fixture([] { return std::make_unique<DelayedBackend>(); });
    QVERIFY(fixture.server.start().isSuccess());
    WebSocketAgentClient client(fixture.clientOptions());
    QVERIFY(connectClient(client));
    const QString id = createSession(client);
    QVERIFY(attachSession(client, id));
    const auto terminated =
        complete([&](IAgentClient::Completion done) { client.terminateSession(id, std::move(done)); });
    QVERIFY(terminated.completed && terminated.error.isEmpty());
    QVERIFY(waitUntil([&] { return client.sessions().first().processState == "exited"; }));
    const auto detached =
        complete([&](IAgentClient::Completion done) { client.detachSession(id, std::move(done)); });
    QVERIFY(detached.completed && detached.error.isEmpty());
    QVERIFY(attachSession(client, id));
    QVERIFY(client.sessions().first().attached);
    QVERIFY(client.sessions().first().readOnly);
}

void WebSocketAgentClientTests::terminalBytesAndResize()
{
    Fixture fixture;
    QVERIFY(fixture.server.start().isSuccess());
    WebSocketAgentClient client(fixture.clientOptions());
    OutputCapture capture(client);
    QVERIFY(connectClient(client));
    QVERIFY(!client.isSimulation());
    QVERIFY(client.sessions().isEmpty());
    const QString id = createSession(client);
    QVERIFY(fixture.running(id));
    QVERIFY(attachSession(client, id));
    QVERIFY(waitUntil([&] { return capture.output[id].contains("[READY]"); }));
    QString error;
    QVERIFY(client.sendInput(id, QStringLiteral("echo 中文透传\r").toUtf8(), error));
    QVERIFY(
        waitUntil([&] { return capture.output[id].contains(QStringLiteral("[ECHO]中文透传").toUtf8()); }));
    QVERIFY(client.sendInput(id, "ansi\r", error));
    QVERIFY(waitUntil([&] { return capture.output[id].contains("[COLOR]"); }));
    QVERIFY(capture.output[id].contains('\x1b'));
    QVERIFY(client.sendInput(id, QByteArray("\x03\x1b[A"), error));
    QVERIFY(waitUntil(
        [&] { return capture.output[id].contains("[CTRL_C]") && capture.output[id].contains("[UP]"); }));
    const auto resized =
        complete([&](IAgentClient::Completion done) { client.resizeSession(id, 91, 27, std::move(done)); });
    QVERIFY(resized.completed);
    QVERIFY2(resized.error.isEmpty(), qPrintable(resized.error));
    QVERIFY(client.sendInput(id, "size\r", error));
    QVERIFY(waitUntil([&] { return capture.output[id].contains("[SIZE]91x27"); }));
    const auto unknown = complete([&](IAgentClient::Completion done)
                                  { client.attachSession(SessionId::create().toString(), std::move(done)); });
    QVERIFY(unknown.completed);
    QVERIFY(!unknown.error.isEmpty());
    QVERIFY(client.isConnected());
}

void WebSocketAgentClientTests::threeSessionsRemainIsolated()
{
    Fixture fixture;
    QVERIFY(fixture.server.start().isSuccess());
    WebSocketAgentClient client(fixture.clientOptions());
    OutputCapture capture(client);
    QVERIFY(connectClient(client));
    QStringList ids;
    for (int index = 0; index < 3; ++index)
    {
        const QString id = createSession(client, QStringLiteral("独立会话 %1").arg(index));
        QVERIFY(fixture.running(id));
        QVERIFY(attachSession(client, id));
        ids.append(id);
    }
    const auto listed =
        complete([&](IAgentClient::Completion done) { client.listSessions(std::move(done)); });
    QVERIFY(listed.completed && listed.error.isEmpty());
    QCOMPARE(client.sessions().size(), 3);
    QString error;
    for (int index = 0; index < ids.size(); ++index)
        QVERIFY(client.sendInput(ids[index], "echo isolate-" + QByteArray::number(index) + "\r", error));
    for (int index = 0; index < ids.size(); ++index)
    {
        const QByteArray marker = "[ECHO]isolate-" + QByteArray::number(index);
        QVERIFY(waitUntil([&] { return capture.output[ids[index]].contains(marker); }));
        for (int other = 0; other < ids.size(); ++other)
            if (other != index)
                QVERIFY(!capture.output[ids[index]].contains("[ECHO]isolate-" + QByteArray::number(other)));
    }
}

void WebSocketAgentClientTests::detachAndDisconnectPreserveProcess()
{
    Fixture fixture;
    QVERIFY(fixture.server.start().isSuccess());
    WebSocketAgentClient client(fixture.clientOptions());
    OutputCapture capture(client);
    QVERIFY(connectClient(client));
    const QString id = createSession(client);
    QVERIFY(fixture.running(id));
    QVERIFY(attachSession(client, id));
    const auto detached =
        complete([&](IAgentClient::Completion done) { client.detachSession(id, std::move(done)); });
    QVERIFY(detached.completed && detached.error.isEmpty());
    QVERIFY(fixture.running(id));
    QCOMPARE(fixture.manager.get(SessionId::parse(id).value(), "alice").value().subscriberCount, 0);
    QVERIFY(attachSession(client, id));
    QString error;
    QVERIFY(client.sendInput(id, "echo exactly-once\r", error));
    QVERIFY(waitUntil([&] { return capture.output[id].contains("[ECHO]exactly-once"); }));
    client.disconnectAgent();
    QVERIFY(waitUntil([&] { return fixture.server.connectionCount() == 0; }));
    QVERIFY(!client.sendInput(id, "echo forbidden-replay\r", error));
    QVERIFY(!error.isEmpty());
    QVERIFY(fixture.running(id));
    QVERIFY(connectClient(client));
    QVERIFY(attachSession(client, id));
    // 重附加允许历史输出，但离线拒绝的输入永远不能送达进程。
    QVERIFY(client.sendInput(id, "echo reconnect-barrier\r", error));
    QVERIFY(waitUntil([&] { return capture.output[id].contains("[ECHO]reconnect-barrier"); }));
    QVERIFY(!capture.output[id].contains("[ECHO]forbidden-replay"));
    QCOMPARE(fixture.manager.list("alice").value().size(), 1);
    const auto terminated =
        complete([&](IAgentClient::Completion done) { client.terminateSession(id, std::move(done)); });
    QVERIFY(terminated.completed && terminated.error.isEmpty());
    QVERIFY(waitUntil(
        [&]
        {
            return fixture.manager.get(SessionId::parse(id).value(), "alice").value().status ==
                   SessionStatus::Exited;
        }));
}

void WebSocketAgentClientTests::pendingDetachRejectsInputWithoutBlockingOtherSessions()
{
    Fixture fixture;
    QVERIFY(fixture.server.start().isSuccess());
    WebSocketAgentClient client(fixture.clientOptions());
    OutputCapture capture(client);
    QVERIFY(connectClient(client));
    const QString detachingId = createSession(client, QStringLiteral("等待分离"));
    const QString activeId = createSession(client, QStringLiteral("保持输入"));
    QVERIFY(fixture.running(detachingId));
    QVERIFY(fixture.running(activeId));
    QVERIFY(attachSession(client, detachingId));
    QVERIFY(attachSession(client, activeId));
    auto detached = std::make_shared<CompletionResult>();
    client.detachSession(detachingId,
                         [detached](const QString &error, const QString &id)
                         {
                             detached->completed = true;
                             detached->error = error;
                             detached->sessionId = id;
                         });
    // 此处不处理事件，保证服务端尚未返回分离确认，覆盖用户键盘抢先到达的竞态。
    QVERIFY(!detached->completed);
    QString error;
    QVERIFY(!client.sendInput(detachingId, "echo forbidden-during-detach\r", error));
    QVERIFY(!error.isEmpty());
    QVERIFY(client.sendInput(activeId, "echo unaffected-session\r", error));
    QVERIFY(waitUntil([&] { return detached->completed; }));
    QVERIFY2(detached->error.isEmpty(), qPrintable(detached->error));
    QCOMPARE(detached->sessionId, detachingId);
    QVERIFY(waitUntil([&] { return capture.output[activeId].contains("[ECHO]unaffected-session"); }));
    QVERIFY(fixture.running(detachingId));
    QVERIFY(attachSession(client, detachingId));
    QVERIFY(client.sendInput(detachingId, "echo reattach-barrier\r", error));
    QVERIFY(waitUntil([&] { return capture.output[detachingId].contains("[ECHO]reattach-barrier"); }));
    QVERIFY(!capture.output[detachingId].contains("[ECHO]forbidden-during-detach"));
}

void WebSocketAgentClientTests::writerConflictAndWorkspaceAuthorization()
{
    Fixture fixture;
    QVERIFY(fixture.server.start().isSuccess());
    WebSocketAgentClient alice(fixture.clientOptions()), bob(fixture.clientOptions('b')),
        outsider(fixture.clientOptions('c'));
    OutputCapture observer(bob);
    QVERIFY(connectClient(alice));
    QVERIFY(connectClient(bob));
    QVERIFY(connectClient(outsider));
    const QString id = createSession(alice);
    QVERIFY(fixture.running(id));
    QVERIFY(attachSession(alice, id));
    const auto listed = complete([&](IAgentClient::Completion done) { bob.listSessions(std::move(done)); });
    QVERIFY(listed.completed && listed.error.isEmpty());
    QVERIFY(attachSession(bob, id));
    bool readOnly = false;
    for (const auto &session : bob.sessions())
        if (session.id == id)
            readOnly = session.attached && session.readOnly;
    QVERIFY(readOnly);
    QString error;
    QVERIFY(!bob.sendInput(id, "echo rejected-writer\r", error));
    QVERIFY(!error.isEmpty());
    QVERIFY(alice.sendInput(id, "echo visible-to-reader\r", error));
    QVERIFY(waitUntil([&] { return observer.output[id].contains("[ECHO]visible-to-reader"); }));
    QVERIFY(!observer.output[id].contains("[ECHO]rejected-writer"));
    QVERIFY(!attachSession(outsider, id));
    QVERIFY(createSession(outsider).isEmpty());
    QVERIFY(outsider.sessions().isEmpty());
}

void WebSocketAgentClientTests::serverStopClearsAttachmentState()
{
    Fixture fixture;
    QVERIFY(fixture.server.start().isSuccess());
    WebSocketAgentClient client(fixture.clientOptions());
    QVERIFY(connectClient(client));
    const QString id = createSession(client);
    QVERIFY(fixture.running(id));
    QVERIFY(attachSession(client, id));
    QSignalSpy disconnected(&client, &IAgentClient::disconnected);
    fixture.server.stop();
    QVERIFY(waitUntil([&] { return !client.isConnected() && disconnected.count() > 0; }));
    for (const auto &session : client.sessions())
        QVERIFY(!session.attached);
    QString error;
    QVERIFY(!client.sendInput(id, "echo after-stop\r", error));
    QVERIFY(fixture.running(id));
    QCOMPARE(fixture.manager.get(SessionId::parse(id).value(), "alice").value().subscriberCount, 0);
}

QTEST_GUILESS_MAIN(WebSocketAgentClientTests)
#include "WebSocketAgentClientTests.moc"
