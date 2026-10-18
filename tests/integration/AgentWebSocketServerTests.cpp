#include "../unit/SessionContractMocks.h"
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QNetworkRequest>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QWebSocket>
#include <QtTest>
#include <WinConPtyBackend.h>
#include <functional>
#include <protocol/ProtocolCodec.h>
#include <provider/IProviderAdapter.h>
#include <session/SessionManager.h>
#include <transport/AgentWebSocketServer.h>

using namespace rad;
namespace
{
QByteArray token(char identity)
{
    // 仅用于回环测试的固定凭据，不是真实配对密钥。
    return QByteArray(64, identity);
}
bool waitUntil(const std::function<bool()> &predicate, int timeout = 15000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeout)
        QTest::qWait(10);
    return predicate();
}
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
SessionManager::Options sessionOptions(const QString &directory)
{
    SessionManager::Options options;
    options.workspaces = {{{"workspace", directory}, {"alice", "bob"}}};
    options.providers = {std::make_shared<FakeProvider>()};
    options.writerLeaseMs = 120000;
    return options;
}
AgentWebSocketServer::Options serverOptions()
{
    AgentWebSocketServer::Options options;
    options.credentials = {{"alice", QCryptographicHash::hash(token('a'), QCryptographicHash::Sha256)},
                           {"bob", QCryptographicHash::hash(token('b'), QCryptographicHash::Sha256)},
                           {"outsider", QCryptographicHash::hash(token('c'), QCryptographicHash::Sha256)}};
    return options;
}
struct Fixture
{
    QTemporaryDir directory;
    SessionManager manager;
    AgentWebSocketServer server;
    explicit Fixture(AgentWebSocketServer::Options options = serverOptions())
        : manager(
              sessionOptions(directory.path()), [this]
              { return std::make_unique<WinConPtyBackend>(WinConPtyBackend::Options{{directory.path()}}); }),
          server(manager, std::move(options))
    {
    }
};
class Peer final : public QObject
{
public:
    explicit Peer(const QString &origin = {}) : socket(origin)
    {
        connect(&socket, &QWebSocket::textMessageReceived, this,
                [this](const QString &text)
                {
                    protocol::Envelope envelope;
                    protocol::Error error;
                    if (!protocol::decodeJson(text.toUtf8(), envelope, error))
                        invalidMessage = true;
                    else
                        messages.append(envelope);
                });
        connect(&socket, &QWebSocket::binaryMessageReceived, this,
                [this](const QByteArray &bytes)
                {
                    protocol::TerminalFrame frame;
                    protocol::Error error;
                    if (!protocol::decodeBinary(bytes, frame, error) ||
                        frame.kind != protocol::TerminalKind::Output)
                        invalidMessage = true;
                    else
                    {
                        output[frame.sessionId] += frame.bytes;
                        frames.append(frame);
                    }
                });
    }
    bool open(quint16 port, const QByteArray &credential = token('a'))
    {
        QNetworkRequest request(QUrl(QStringLiteral("ws://127.0.0.1:%1").arg(port)));
        if (!credential.isEmpty())
            request.setRawHeader("Authorization", "Bearer " + credential);
        socket.open(request);
        return waitUntil([this] { return socket.state() == QAbstractSocket::ConnectedState; }, 3000);
    }
    QString send(const QString &type, const QString &session = {}, const QJsonObject &payload = {},
                 QString request = {})
    {
        if (request.isEmpty())
            request = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QByteArray wire;
        protocol::Error error;
        if (!protocol::encodeJson({1, type, request, session, payload}, wire, error))
            return {};
        socket.sendTextMessage(QString::fromUtf8(wire));
        return request;
    }
    protocol::Envelope response(const QString &request)
    {
        waitUntil(
            [this, &request]
            {
                for (const auto &message : messages)
                    if (message.requestId == request)
                        return true;
                return false;
            },
            5000);
        for (const auto &message : messages)
            if (message.requestId == request)
                return message;
        return {};
    }
    bool hello()
    {
        return response(send("auth.hello", {}, {{"versions", QJsonArray{1}}})).type == "auth.welcome";
    }
    QString create()
    {
        const auto reply = response(send("session.create", {},
                                         {{"name", "网络测试"},
                                          {"provider_id", "fake"},
                                          {"workspace_id", "workspace"},
                                          {"columns", 80},
                                          {"rows", 24}}));
        return reply.type == "session.created" ? reply.sessionId : QString();
    }
    bool attach(const QString &id, const QString &mode = "read_write")
    {
        return response(send("session.attach", id, {{"mode", mode}})).type == "session.attached";
    }
    bool input(const QString &id, quint64 sequence, const QByteArray &bytes)
    {
        QByteArray wire;
        protocol::Error error;
        if (!protocol::encodeBinary({1, protocol::TerminalKind::Input, id, sequence, bytes}, wire, error))
            return false;
        return socket.sendBinaryMessage(wire) >= 0;
    }
    bool hasError() const
    {
        for (const auto &message : messages)
            if (message.type == "error")
                return true;
        return false;
    }
    QWebSocket socket;
    QVector<protocol::Envelope> messages;
    QVector<protocol::TerminalFrame> frames;
    QMap<QString, QByteArray> output;
    bool invalidMessage = false;
};
bool running(Fixture &fixture, const QString &id)
{
    const auto parsed = SessionId::parse(id);
    if (!parsed.isSuccess())
        return false;
    return waitUntil(
        [&]
        {
            const auto state = fixture.manager.get(parsed.value(), "alice");
            return state.isSuccess() && state.value().status == SessionStatus::Running;
        });
}
}
class AgentWebSocketServerTests final : public QObject
{
    Q_OBJECT
private slots:
    void authentication();
    void negotiationAndTimeout();
    void terminalRoundTrip();
    void attachReplayBoundary();
    void reconnectPreservesProcess();
    void authorizationAndIsolation();
    void replayAndOversizedMessages();
    void boundedOutputQueue();
    void configurationAndConnectionLimits();
    void pingTimeout();
    void oversizedSessionList();
};
void AgentWebSocketServerTests::attachReplayBoundary()
{
    QTemporaryDir directory;
    auto options = sessionOptions(directory.path());
    options.outputCapacityBytes = 8;
    test::MockTerminal *backend = nullptr;
    SessionManager manager(options,
                           [&]
                           {
                               auto result = std::make_unique<test::MockTerminal>();
                               backend = result.get();
                               return result;
                           });
    AgentWebSocketServer server(manager, serverOptions());
    QVERIFY(server.start().isSuccess());
    Peer peer;
    QVERIFY(peer.open(server.serverPort()));
    QVERIFY(peer.hello());
    const QString id = peer.create();
    const auto parsed = SessionId::parse(id).value();
    QVERIFY(waitUntil([&] { return manager.get(parsed, "alice").value().status == SessionStatus::Running; }));
    auto attached = peer.response(peer.send("session.attach", id, {{"mode", "read_write"}}));
    QCOMPARE(attached.type, QString("session.attached"));
    QCOMPARE(attached.payload["replay_end_sequence"].toString(), QString("0"));
    QCOMPARE(peer.response(peer.send("session.detach", id)).type, QString("session.detached"));
    // 同时超过后端和管理器的缓存容量，边界必须包含两层丢弃量。
    backend->produce("0123456789abcdefghijklmn");
    QVERIFY(waitUntil([&] { return manager.get(parsed, "alice").value().outputSequence == 24; }));
    attached = peer.response(peer.send("session.attach", id, {{"mode", "read_write"}}));
    QCOMPARE(attached.type, QString("session.attached"));
    QCOMPARE(attached.payload["replay_end_sequence"].toString(), QString("24"));
    QVERIFY(waitUntil([&] { return peer.output[id] == "ghijklmn"; }));
    QVERIFY(!peer.frames.isEmpty());
    QCOMPARE(peer.frames.front().sequence, quint64(16));
    backend->produce("live");
    QVERIFY(waitUntil([&] { return peer.output[id] == "ghijklmnlive"; }));
    QCOMPARE(peer.frames.back().sequence, quint64(24));
    QCOMPARE(attached.payload["replay_end_sequence"].toString(), QString("24"));
}
void AgentWebSocketServerTests::authentication()
{
    Fixture fixture;
    QVERIFY(fixture.server.start().isSuccess());
    QSignalSpy audit(&fixture.server, &AgentWebSocketServer::auditEvent);
    for (const auto &credential : {QByteArray(), token('d')})
    {
        Peer rejected;
        rejected.open(fixture.server.serverPort(), credential);
        QVERIFY(waitUntil([&] { return rejected.socket.state() == QAbstractSocket::UnconnectedState; }));
        QCOMPARE(fixture.manager.list("alice").value().size(), 0);
    }
    Peer browser("https://untrusted.example");
    browser.open(fixture.server.serverPort());
    QVERIFY(waitUntil([&] { return browser.socket.state() == QAbstractSocket::UnconnectedState; }));
    Peer alice;
    QVERIFY(alice.open(fixture.server.serverPort()));
    QVERIFY(alice.hello());
    Peer duplicate;
    duplicate.open(fixture.server.serverPort());
    QVERIFY(waitUntil([&] { return duplicate.socket.state() == QAbstractSocket::UnconnectedState; }));
    QCOMPARE(alice.socket.state(), QAbstractSocket::ConnectedState);
    for (const auto &event : audit)
        for (const auto &argument : event)
            QVERIFY(!argument.toByteArray().contains(token('a')));
}
void AgentWebSocketServerTests::negotiationAndTimeout()
{
    auto options = serverOptions();
    options.handshakeTimeoutMs = 200;
    Fixture fixture(options);
    QVERIFY(fixture.server.start().isSuccess());
    {
        Peer silent;
        QVERIFY(silent.open(fixture.server.serverPort()));
        QVERIFY(waitUntil([&] { return silent.socket.state() == QAbstractSocket::UnconnectedState; }, 3000));
    }
    {
        Peer premature;
        QVERIFY(premature.open(fixture.server.serverPort()));
        premature.send("session.create", {},
                       {{"name", "blocked"},
                        {"provider_id", "fake"},
                        {"workspace_id", "workspace"},
                        {"columns", 80},
                        {"rows", 24}});
        QVERIFY(waitUntil(
            [&]
            {
                return premature.hasError() || premature.socket.state() == QAbstractSocket::UnconnectedState;
            }));
        QCOMPARE(fixture.manager.list("alice").value().size(), 0);
    }
    QVERIFY(waitUntil([&] { return fixture.server.connectionCount() == 0; }));
    Peer unsupported;
    QVERIFY(unsupported.open(fixture.server.serverPort()));
    const auto reply =
        unsupported.response(unsupported.send("auth.hello", {}, {{"versions", QJsonArray{2}}}));
    QCOMPARE(reply.type, QString("error"));
    QCOMPARE(reply.payload["error"].toObject()["code"].toString(), QString("unsupported_version"));
}
void AgentWebSocketServerTests::terminalRoundTrip()
{
    Fixture fixture;
    QVERIFY(fixture.server.start().isSuccess());
    Peer peer;
    QVERIFY(peer.open(fixture.server.serverPort()));
    QVERIFY(peer.hello());
    const QString id = peer.create();
    QVERIFY(running(fixture, id));
    QVERIFY(peer.attach(id));
    QVERIFY(waitUntil([&] { return peer.output[id].contains("[READY]"); }));
    QVERIFY(peer.input(id, 0, QStringLiteral("echo 中文网络\r").toUtf8()));
    QVERIFY(waitUntil([&] { return peer.output[id].contains(QStringLiteral("[ECHO]中文网络").toUtf8()); }));
    QCOMPARE(peer.response(peer.send("session.resize", id, {{"columns", 91}, {"rows", 27}})).type,
             QString("session.resized"));
    QVERIFY(peer.input(id, 1, "size\r"));
    QVERIFY(waitUntil([&] { return peer.output[id].contains("[SIZE]91x27"); }));
    QCOMPARE(peer.response(peer.send("session.heartbeat", id)).type, QString("session.heartbeat_ack"));
    QCOMPARE(peer.response(peer.send("session.list")).payload["sessions"].toArray().size(), 1);
    QCOMPARE(peer.response(peer.send("session.detach", id)).type, QString("session.detached"));
    QVERIFY(running(fixture, id));
    QVERIFY(peer.attach(id));
    QCOMPARE(peer.response(peer.send("session.terminate", id)).type, QString("session.terminated"));
    const auto parsed = SessionId::parse(id).value();
    QVERIFY(waitUntil(
        [&] { return fixture.manager.get(parsed, "alice").value().status == SessionStatus::Exited; }));
    QVERIFY(!peer.invalidMessage);
}
void AgentWebSocketServerTests::reconnectPreservesProcess()
{
    Fixture fixture;
    QVERIFY(fixture.server.start().isSuccess());
    Peer original;
    QVERIFY(original.open(fixture.server.serverPort()));
    QVERIFY(original.hello());
    const QString id = original.create();
    QVERIFY(running(fixture, id));
    QVERIFY(original.attach(id));
    QVERIFY(original.input(id, 0, "echo before-disconnect\r"));
    QVERIFY(waitUntil([&] { return original.output[id].contains("[ECHO]before-disconnect"); }));
    original.socket.abort();
    QVERIFY(waitUntil([&] { return fixture.server.connectionCount() == 0; }));
    const auto parsed = SessionId::parse(id).value();
    QCOMPARE(fixture.manager.get(parsed, "alice").value().subscriberCount, 0);
    QVERIFY(running(fixture, id));
    Peer restored;
    QVERIFY(restored.open(fixture.server.serverPort()));
    QVERIFY(restored.hello());
    QVERIFY(restored.attach(id));
    QVERIFY(restored.input(id, 0, "echo after-reconnect\r"));
    QVERIFY(waitUntil([&] { return restored.output[id].contains("[ECHO]after-reconnect"); }));
    fixture.server.stop();
    QCOMPARE(fixture.manager.get(parsed, "alice").value().subscriberCount, 0);
    QVERIFY(running(fixture, id));
}
void AgentWebSocketServerTests::authorizationAndIsolation()
{
    Fixture fixture;
    QVERIFY(fixture.server.start().isSuccess());
    Peer alice, bob, outsider;
    QVERIFY(alice.open(fixture.server.serverPort()));
    QVERIFY(alice.hello());
    QVERIFY(bob.open(fixture.server.serverPort(), token('b')));
    QVERIFY(bob.hello());
    QVERIFY(outsider.open(fixture.server.serverPort(), token('c')));
    QVERIFY(outsider.hello());
    const QString first = alice.create(), second = alice.create();
    QVERIFY(running(fixture, first));
    QVERIFY(running(fixture, second));
    QVERIFY(alice.attach(first));
    QVERIFY(alice.attach(second));
    QVERIFY(!bob.attach(first));
    QVERIFY(bob.attach(first, "read_only"));
    QVERIFY(!outsider.attach(first));
    QCOMPARE(outsider.response(outsider.send("session.list")).payload["sessions"].toArray().size(), 0);
    QVERIFY(outsider.create().isEmpty());
    bob.messages.clear();
    QVERIFY(bob.input(first, 0, "echo forbidden-write\r"));
    QVERIFY(waitUntil([&] { return bob.hasError(); }));
    QVERIFY(alice.input(first, 0, "echo first-only\r"));
    QVERIFY(alice.input(second, 0, "echo second-only\r"));
    QVERIFY(waitUntil(
        [&]
        {
            return alice.output[first].contains("[ECHO]first-only") &&
                   alice.output[second].contains("[ECHO]second-only");
        }));
    QVERIFY(!alice.output[first].contains("[ECHO]second-only"));
    QVERIFY(!alice.output[second].contains("[ECHO]first-only"));
    QVERIFY(!alice.output[first].contains("[ECHO]forbidden-write"));
    QVERIFY(outsider.output.isEmpty());
}
void AgentWebSocketServerTests::replayAndOversizedMessages()
{
    Fixture fixture;
    QVERIFY(fixture.server.start().isSuccess());
    Peer peer;
    QVERIFY(peer.open(fixture.server.serverPort()));
    QVERIFY(peer.hello());
    const QString id = peer.create();
    QVERIFY(running(fixture, id));
    QVERIFY(peer.attach(id));
    QVERIFY(peer.input(id, 0, "echo exactly-once\r"));
    QVERIFY(waitUntil([&] { return peer.output[id].contains("[ECHO]exactly-once"); }));
    peer.messages.clear();
    QVERIFY(peer.input(id, 0, "echo replayed\r"));
    QVERIFY(waitUntil([&] { return peer.hasError(); }));
    QVERIFY(peer.input(id, 1, "echo next-valid\r"));
    QVERIFY(waitUntil([&] { return peer.output[id].contains("[ECHO]next-valid"); }));
    QVERIFY(!peer.output[id].contains("[ECHO]replayed"));
    const QString request = peer.send("session.list");
    QCOMPARE(peer.response(request).type, QString("session.listed"));
    peer.messages.clear();
    peer.send("session.list", {}, {}, request);
    QCOMPARE(peer.response(request).type, QString("error"));
    peer.socket.sendBinaryMessage(QByteArray(protocol::kMaxFrameBytes + 1, 'x'));
    QVERIFY(waitUntil([&] { return peer.socket.state() == QAbstractSocket::UnconnectedState; }));
    QVERIFY(running(fixture, id));
}
void AgentWebSocketServerTests::boundedOutputQueue()
{
    auto options = serverOptions();
    options.maxQueuedBytes = 1024;
    Fixture fixture(options);
    QVERIFY(fixture.server.start().isSuccess());
    const auto created = fixture.manager.create({"历史输出", "fake", "workspace", {}, {80, 24}}, "alice");
    QVERIFY(created.isSuccess());
    const auto id = created.value();
    QVERIFY(running(fixture, id.toString()));
    QVERIFY(fixture.manager.attach(id, "alice", AttachMode::ReadWrite).isSuccess());
    QVERIFY(fixture.manager.writeInput(id, "alice", "flood\r").isSuccess());
    QByteArray history;
    QVERIFY(waitUntil(
        [&]
        {
            const auto chunk = fixture.manager.takeOutput(id, "alice", 65536);
            if (chunk.isSuccess())
                history += chunk.value().bytes;
            return history.contains("[FLOOD_DONE]");
        },
        30000));
    QVERIFY(fixture.manager.detach(id, "alice").isSuccess());
    Peer peer;
    QVERIFY(peer.open(fixture.server.serverPort()));
    QVERIFY(peer.hello());
    // 已积累的历史首块大于发送上限，确定性触发背压，不依赖操作系统缓冲区速度。
    peer.send("session.attach", id.toString(), {{"mode", "read_write"}});
    QVERIFY(waitUntil([&] { return peer.socket.state() == QAbstractSocket::UnconnectedState; }));
    QVERIFY(running(fixture, id.toString()));
    QCOMPARE(fixture.manager.get(id, "alice").value().subscriberCount, 0);
}
void AgentWebSocketServerTests::configurationAndConnectionLimits()
{
    auto invalid = serverOptions();
    invalid.bindAddress = QHostAddress::AnyIPv4;
    Fixture invalidFixture(invalid);
    QVERIFY(!invalidFixture.server.start().isSuccess());
    QVERIFY(!invalidFixture.server.isListening());
    auto limited = serverOptions();
    limited.maxConnections = 1;
    Fixture fixture(limited);
    QVERIFY(fixture.server.start().isSuccess());
    Peer alice, bob;
    QVERIFY(alice.open(fixture.server.serverPort()));
    QVERIFY(alice.hello());
    bob.open(fixture.server.serverPort(), token('b'));
    QVERIFY(waitUntil([&] { return bob.socket.state() == QAbstractSocket::UnconnectedState; }));
    QCOMPARE(alice.socket.state(), QAbstractSocket::ConnectedState);
}
void AgentWebSocketServerTests::pingTimeout()
{
    auto options = serverOptions();
    options.idleTimeoutMs = 600;
    options.handshakeTimeoutMs = 5000;
    Fixture fixture(options);
    QVERIFY(fixture.server.start().isSuccess());
    // 原始 TCP 客户端不自动响应 Ping，用于验证失联回收而非 hello 超时。
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, fixture.server.serverPort());
    QVERIFY(waitUntil([&] { return socket.state() == QAbstractSocket::ConnectedState; }));
    const QByteArray request = "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n"
                               "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                               "Sec-WebSocket-Version: 13\r\nAuthorization: Bearer " +
                               token('a') + "\r\n\r\n";
    QCOMPARE(socket.write(request), qint64(request.size()));
    QByteArray response;
    QVERIFY(waitUntil(
        [&]
        {
            response += socket.readAll();
            return response.contains("\r\n\r\n");
        }));
    QVERIFY(response.startsWith("HTTP/1.1 101"));
    QByteArray payload;
    protocol::Error error;
    QVERIFY(protocol::encodeJson({1,
                                  "auth.hello",
                                  QUuid::createUuid().toString(QUuid::WithoutBraces),
                                  {},
                                  {{"versions", QJsonArray{1}}}},
                                 payload, error));
    QVERIFY(payload.size() < 65536);
    QByteArray frame = QByteArray::fromHex("81");
    if (payload.size() < 126)
        frame.append(static_cast<char>(0x80 | payload.size()));
    else
    {
        frame += QByteArray::fromHex("fe");
        frame.append(static_cast<char>((payload.size() >> 8) & 0xff));
        frame.append(static_cast<char>(payload.size() & 0xff));
    }
    const QByteArray mask("test");
    frame += mask;
    for (int index = 0; index < payload.size(); ++index)
        frame.append(static_cast<char>(payload[index] ^ mask[index % 4]));
    QCOMPARE(socket.write(frame), qint64(frame.size()));
    response.clear();
    QVERIFY(waitUntil(
        [&]
        {
            response += socket.readAll();
            return response.contains("auth.welcome");
        }));
    QVERIFY(waitUntil([&] { return fixture.server.connectionCount() == 0; }, 2500));
}
void AgentWebSocketServerTests::oversizedSessionList()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto options = sessionOptions(directory.path());
    options.maxConcurrentSessions = 128;
    options.maxRetainedSessions = 128;
    // 复用内存后端验证协议容量边界，避免为列表测试启动 128 个进程。
    SessionManager manager(options, [] { return std::make_unique<rad::test::MockTerminal>(); });
    for (int index = 0; index < 128; ++index)
    {
        const auto created =
            manager.create({QString(256, QChar(0x4e2d)), "fake", "workspace", {}, {80, 24}}, "alice");
        QVERIFY(created.isSuccess());
    }
    QCOMPARE(manager.list("alice").value().size(), 128);
    AgentWebSocketServer server(manager, serverOptions());
    QVERIFY(server.start().isSuccess());
    Peer peer;
    QVERIFY(peer.open(server.serverPort()));
    QVERIFY(peer.hello());
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        const auto reply = peer.response(peer.send("session.list"));
        QCOMPARE(reply.type, QString("error"));
        QCOMPARE(reply.payload["error"].toObject()["code"].toString(), QString("busy"));
        QCOMPARE(peer.socket.state(), QAbstractSocket::ConnectedState);
    }
    QVERIFY(!peer.invalidMessage);
}
QTEST_GUILESS_MAIN(AgentWebSocketServerTests)
#include "AgentWebSocketServerTests.moc"
