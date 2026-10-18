#include <QApplication>
#include <QClipboard>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QInputMethodEvent>
#include <QLabel>
#include <QMap>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QWebEnginePage>
#include <QWebEngineView>
#include <QtTest>
#include <WinConPtyBackend.h>
#include <controllers/ClientController.h>
#include <functional>
#include <memory>
#include <provider/IProviderAdapter.h>
#include <session/SessionManager.h>
#include <terminal/TerminalView.h>
#include <transport/AgentWebSocketServer.h>
#include <transport/WebSocketAgentClient.h>
#include <ui/ClientWindow.h>
#include <ui/Theme.h>

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
        return Result<LaunchSpec>::success({qEnvironmentVariable("RAD_FAKECLI_PATH"),
                                            {QStringLiteral("--interactive")},
                                            workspace.workingDirectory,
                                            request.size});
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

struct Fixture
{
    QTemporaryDir directory;
    int backendCount = 0;
    SessionManager manager;
    AgentWebSocketServer server;
    Fixture()
        : manager(sessionOptions(directory.path()),
                  [this]
                  {
                      ++backendCount;
                      return std::make_unique<WinConPtyBackend>(
                          WinConPtyBackend::Options{{directory.path()}});
                  }),
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

QVariant evaluate(QWebEnginePage *page, const QString &script)
{
    struct Evaluation
    {
        bool done = false;
        QVariant value;
    };
    auto result = std::make_shared<Evaluation>();
    page->runJavaScript(script,
                        [result](const QVariant &value)
                        {
                            result->value = value;
                            result->done = true;
                        });
    waitUntil([result] { return result->done; }, 3000);
    return result->value;
}

QString terminalText(TerminalView *terminal)
{
    return evaluate(
               terminal->webView()->page(),
               QStringLiteral(
                   "(function(){if(!window.clientTerminal)return '';var b=clientTerminal.term.buffer.active;"
                   "var s=[];for(var i=0;i<b.length;i++)s.push(b.getLine(i).translateToString(true));return "
                   "s.join('\\n');})()"))
        .toString();
}

QWidget *inputTarget(TerminalView *terminal)
{
    terminal->window()->activateWindow();
    terminal->webView()->setFocus(Qt::OtherFocusReason);
    terminal->focusTerminal();
    auto *target =
        terminal->webView()->focusProxy() ? terminal->webView()->focusProxy() : terminal->webView();
    const bool focused = waitUntil(
        [&]
        {
            const auto *focus = QApplication::focusWidget();
            return focus && (focus == target || target->isAncestorOf(focus)) &&
                   evaluate(terminal->webView()->page(),
                            QStringLiteral(
                                "!!window.clientTerminal && !clientTerminal.term.options.disableStdin && "
                                "document.activeElement.classList.contains('xterm-helper-textarea')"))
                       .toBool();
        },
        5000);
    if (!focused)
        qWarning() << "终端输入焦点未就绪" << QApplication::focusWidget() << "目标" << target;
    return focused ? target : nullptr;
}

bool typeCommand(TerminalView *terminal, const char *command)
{
    auto *target = inputTarget(terminal);
    if (!target)
        return false;
    QTest::keyClicks(target, QString::fromLatin1(command));
    QTest::keyClick(target, Qt::Key_Return);
    return true;
}
}

class RemoteTerminalTests final : public QObject
{
    Q_OBJECT
private slots:
    void realWindowTerminalRoundTrip();
    void reconnectAfterOneMinute();
};

void RemoteTerminalTests::reconnectAfterOneMinute()
{
    Fixture fixture;
    QVERIFY(fixture.server.start().isSuccess());
    const auto port = fixture.server.serverPort();
    WebSocketAgentClient agent(fixture.clientOptions());
    ClientController controller(&agent);
    ClientWindow window(&controller);
    window.show();
    controller.connectAgent();
    QVERIFY(waitUntil([&] { return controller.isConnected() && !controller.isBusy(); }));
    controller.createSession(QStringLiteral("断线恢复"), QStringLiteral("workspace"));
    QVERIFY(waitUntil([&] { return !controller.isBusy() && window.sessionTabCount() == 1; }));
    const QString id = window.currentSessionId();
    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("sessionTabs"));
    QVERIFY(tabs);
    auto *terminal = tabs->currentWidget()->findChild<TerminalView *>();
    QVERIFY(terminal);
    QVERIFY(waitUntil([&] { return terminalText(terminal).contains("[READY]"); }, 20000));
    QVERIFY(typeCommand(terminal, "pause"));
    QVERIFY(waitUntil([&] { return terminalText(terminal).contains("[PAUSED]"); }));

    // 只中断网络监听与连接，管理器和真实子进程仍运行；不修改本机网卡。
    fixture.server.stop();
    QVERIFY(waitUntil([&] { return !controller.isConnected() && !controller.isBusy(); }));
    QString rejectedInput;
    QVERIFY(!agent.sendInput(id, "echo must-not-replay\r", rejectedInput));
    QVERIFY(!rejectedInput.isEmpty());
    QTest::qWait(60000);
    QVERIFY(fixture.running(id));
    QCOMPARE(fixture.backendCount, 1);

    auto options = serverOptions();
    options.port = port;
    AgentWebSocketServer restored(fixture.manager, options);
    QVERIFY(restored.start().isSuccess());
    controller.connectAgent();
    QVERIFY(waitUntil([&] { return controller.isConnected() && !controller.isBusy(); }));
    QCOMPARE(controller.sessions().size(), 1);
    QCOMPARE(controller.sessions().front().id, id);
    QVERIFY(!controller.sessions().front().attached);
    bool staleEventsDelivered = false;
    bool stalePageRemainedInactive = false;
    const auto staleEvents = connect(&agent, &IAgentClient::sessionsChanged, &window,
                                     [&]
                                     {
                                         if (staleEventsDelivered || !agent.sessions().front().attached)
                                             return;
                                         // 附加响应先刷新状态，再排队替换页面，覆盖旧页面晚到事件窗口。
                                         staleEventsDelivered = true;
                                         terminal->inputReady("echo stale-page-input\r");
                                         terminal->terminalResized(80, 24);
                                         terminal->ready();
                                         terminal->failed(QStringLiteral("旧页面延迟失败"));
                                         stalePageRemainedInactive = !controller.terminalIsReady(id);
                                     });
    window.openSession(id);
    QVERIFY(waitUntil([&] { return !controller.isBusy() && controller.sessions().front().attached; }));
    disconnect(staleEvents);
    QVERIFY(staleEventsDelivered && stalePageRemainedInactive);
    QVERIFY(controller.isConnected());
    QCOMPARE(window.sessionTabCount(), 1);
    terminal = tabs->currentWidget()->findChild<TerminalView *>();
    QVERIFY(terminal);
    QVERIFY(waitUntil([&] { return terminalText(terminal).contains("[RESUMED]"); }, 20000));
    QCOMPARE(terminalText(terminal).count("[READY]"), 1);
    QVERIFY(!terminalText(terminal).contains("must-not-replay"));
    auto *notice = tabs->currentWidget()->findChild<QLabel *>(QStringLiteral("terminalHistoryNotice"));
    QVERIFY(notice);
    QVERIFY(notice->isVisible());
    QVERIFY(!notice->text().isEmpty());
    QVERIFY(typeCommand(terminal, "echo after-reconnect"));
    QVERIFY2(waitUntil([&] { return terminalText(terminal).contains("[ECHO]after-reconnect"); }),
             qPrintable(controller.error() + QLatin1Char('\n') + terminalText(terminal)));
    QVERIFY(!terminalText(terminal).contains("stale-page-input"));
    QCOMPARE(fixture.backendCount, 1);
    const QString screenshot = qEnvironmentVariable("RAD_CLIENT_SCREENSHOT");
    if (!screenshot.isEmpty())
        QVERIFY(window.grab().save(screenshot));
    window.close();
    QVERIFY(waitUntil([&] { return restored.connectionCount() == 0; }));
    QVERIFY(fixture.running(id));
}

void RemoteTerminalTests::realWindowTerminalRoundTrip()
{
    Fixture fixture;
    QVERIFY(fixture.server.start().isSuccess());
    WebSocketAgentClient agent(fixture.clientOptions());
    ClientController controller(&agent);
    ClientWindow window(&controller);
    window.resize(1280, 800);
    window.show();
    window.activateWindow();
    QApplication::processEvents();
    // 桌面遮挡不等于终端失败；以下用真实 DOM 与 VT 回显验证就绪。
    QVERIFY(window.isVisible());
    controller.connectAgent();
    QVERIFY(waitUntil([&] { return controller.isConnected() && !controller.isBusy(); }));
    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("sessionTabs"));
    QVERIFY(tabs);
    QVector<TerminalView *> terminals;
    QStringList ids;
    for (int index = 0; index < 3; ++index)
    {
        controller.createSession(QStringLiteral("远程终端 %1").arg(index + 1), QStringLiteral("workspace"));
        QVERIFY(waitUntil([&] { return !controller.isBusy() && window.sessionTabCount() == index + 1; }));
        const QString id = window.currentSessionId();
        QVERIFY(fixture.running(id));
        ids.append(id);
        auto *terminal = tabs->currentWidget()->findChild<TerminalView *>();
        QVERIFY(terminal);
        terminals.append(terminal);
        QVERIFY(waitUntil(
            [&] { return controller.terminalIsReady(id) && terminalText(terminal).contains("[READY]"); },
            20000));
        const QByteArray command = "echo tab-marker-" + QByteArray::number(index);
        QVERIFY2(typeCommand(terminal, command.constData()), "标签终端输入焦点未就绪");
        const bool echoed = waitUntil(
            [&]
            { return terminalText(terminal).contains(QStringLiteral("[ECHO]tab-marker-%1").arg(index)); });
        const QString diagnostic = QStringLiteral("index=%1; controller.error=%2; terminal tail:\n%3")
                                       .arg(index)
                                       .arg(controller.error(), terminalText(terminal).right(6000));
        if (!echoed)
        {
            const QString capturePath =
                qEnvironmentVariable("RAD_CLIENT_SCREENSHOT", "remote-terminal-input-failure.png");
            if (!window.grab().save(capturePath))
                qWarning("保存终端输入失败截图未成功");
        }
        QVERIFY2(echoed, qPrintable(diagnostic));
    }
    QCOMPARE(window.sessionTabCount(), 3);
    for (int index = 0; index < terminals.size(); ++index)
    {
        const QString contents = terminalText(terminals[index]);
        for (int other = 0; other < terminals.size(); ++other)
            if (other != index)
                QVERIFY(!contents.contains(QStringLiteral("[ECHO]tab-marker-%1").arg(other)));
    }
    window.openSession(ids.front());
    QVERIFY(waitUntil([&] { return !controller.isBusy() && window.currentSessionId() == ids.front(); }));
    auto *terminal = terminals.front();
    auto *target = inputTarget(terminal);
    QVERIFY2(target, "中文输入前焦点未就绪");
    // 真实 Qt 输入法提交进入 WebEngine 文本区，再经过 xterm、网络与 ConPTY 回显。
    QInputMethodEvent commit;
    commit.setCommitString(QStringLiteral("中文输入验错"));
    QApplication::sendEvent(target, &commit);
    QVERIFY(waitUntil([&] { return terminalText(terminal).contains(QStringLiteral("中文输入验错")); }));
    QVERIFY(!terminalText(terminal).contains(QStringLiteral("[TEXT]中文输入")));
    QTest::keyClick(target, Qt::Key_Backspace);
    commit.setCommitString(QStringLiteral("证"));
    QApplication::sendEvent(target, &commit);
    QTest::keyClick(target, Qt::Key_Return);
    QVERIFY(waitUntil([&] { return terminalText(terminal).contains(QStringLiteral("[TEXT]中文输入验证")); }));
    QVERIFY(typeCommand(terminal, "ansi"));
    QVERIFY(waitUntil([&] { return terminalText(terminal).contains(QStringLiteral("[COLOR]绿色输出")); }));
    target = inputTarget(terminal);
    QVERIFY2(target, "快捷键输入前焦点未就绪");
    QTest::keyClick(target, Qt::Key_C, Qt::ControlModifier);
    QVERIFY(waitUntil([&] { return terminalText(terminal).contains("[CTRL_C]"); }));

    for (const QSize &size : {QSize(1200, 780), QSize(1000, 700), QSize(1350, 850), QSize(1100, 740)})
    {
        window.resize(size);
        QTest::qWait(30);
    }
    auto *sizeInputTarget = inputTarget(terminal);
    QVERIFY2(sizeInputTarget, "尺寸检查输入前焦点未就绪");
    QVariantList finalGeometry;
    QVariantList previousGeometry;
    QElapsedTimer geometryStable;
    geometryStable.start();
    // Qt 布局、Chromium viewport 与 ResizeObserver 分属异步更新，固定延时可能读到旧尺寸。
    // 只读取 FitAddon 的建议值，不主动 fit，避免测试替产品修正布局。
    const bool geometryReady = waitUntil(
        [&]
        {
            finalGeometry =
                evaluate(
                    terminal->webView()->page(),
                    QStringLiteral(
                        "(function(){var t=clientTerminal.term;var f=new FitAddon.FitAddon();f.activate(t);"
                        "var p=f.proposeDimensions();return "
                        "[innerWidth,innerHeight,t.cols,t.rows,p?p.cols:0,p?p.rows:0];})()"))
                    .toList();
            if (finalGeometry != previousGeometry)
            {
                previousGeometry = finalGeometry;
                geometryStable.restart();
            }
            if (finalGeometry.size() != 6)
                return false;
            return finalGeometry[0].toInt() == terminal->webView()->width() &&
                   finalGeometry[1].toInt() == terminal->webView()->height() &&
                   finalGeometry[2].toInt() == finalGeometry[4].toInt() &&
                   finalGeometry[3].toInt() == finalGeometry[5].toInt() && geometryStable.elapsed() >= 500;
        });
    if (!geometryReady)
    {
        const QString capturePath =
            qEnvironmentVariable("RAD_CLIENT_SCREENSHOT", "remote-terminal-size-failure.png");
        if (!window.grab().save(capturePath))
            qWarning("保存终端几何失败截图未成功");
        qWarning() << "最终几何未稳定" << finalGeometry << "Qt视图" << terminal->webView()->size();
    }
    QVERIFY2(geometryReady, "最终 WebEngine viewport 与终端 Fit 尺寸未稳定一致");
    const QString expectedSize =
        QStringLiteral("%1x%2").arg(finalGeometry[2].toInt()).arg(finalGeometry[3].toInt());
    QTest::keyClicks(sizeInputTarget, QStringLiteral("size"));
    QTest::keyClick(sizeInputTarget, Qt::Key_Return);
    const bool sizeMatched =
        waitUntil([&] { return terminalText(terminal).contains(QStringLiteral("[SIZE]") + expectedSize); });
    const QString sizeDiagnostic =
        QStringLiteral("expected=%1; controller.error=%2; terminal tail:\n%3")
            .arg(expectedSize, controller.error(), terminalText(terminal).right(6000));
    if (!sizeMatched)
    {
        QString failureScreenshot = qEnvironmentVariable("RAD_CLIENT_SCREENSHOT");
        if (failureScreenshot.isEmpty())
            failureScreenshot = QStringLiteral("remote-terminal-size-failure.png");
        if (!window.grab().save(failureScreenshot))
            qWarning("保存终端尺寸失败截图未成功");
    }
    QVERIFY2(sizeMatched, qPrintable(sizeDiagnostic));

    // 仅用公开 xterm 选择 API 建立选择，复制快捷键仍从 Qt 键盘事件进入。
    evaluate(terminal->webView()->page(), QStringLiteral("clientTerminal.term.selectAll(); true"));
    QApplication::clipboard()->clear();
    target = inputTarget(terminal);
    QVERIFY2(target, "复制前焦点未就绪");
    QTest::keyClick(target, Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
    QVERIFY(waitUntil(
        [&] { return QApplication::clipboard()->text().contains(QStringLiteral("[TEXT]中文输入验证")); }));
    evaluate(terminal->webView()->page(), QStringLiteral("clientTerminal.term.clearSelection(); true"));
    QApplication::clipboard()->setText(QStringLiteral("echo paste-roundtrip"));
    QSignalSpy pasted(terminal, &TerminalView::inputReady);
    target = inputTarget(terminal);
    QVERIFY2(target, "粘贴前焦点未就绪");
    QTest::keyClick(target, Qt::Key_V, Qt::ControlModifier | Qt::ShiftModifier);
    QVERIFY(waitUntil([&] { return !pasted.isEmpty(); }));
    QTest::keyClick(target, Qt::Key_Return);
    QVERIFY(waitUntil([&] { return terminalText(terminal).contains("[ECHO]paste-roundtrip"); }));

    const QString screenshot = qEnvironmentVariable("RAD_CLIENT_SCREENSHOT");
    if (!screenshot.isEmpty())
        QVERIFY(window.grab().save(screenshot));
    window.close();
    QVERIFY(waitUntil([&] { return fixture.server.connectionCount() == 0; }));
    for (const auto &id : ids)
        QVERIFY(fixture.running(id));
}

int main(int argc, char **argv)
{
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    QTemporaryDir settingsDirectory;
    QSettings settings(settingsDirectory.filePath(QStringLiteral("theme.ini")), QSettings::IniFormat);
    applyTheme(application, settings);
    RemoteTerminalTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "RemoteTerminalTests.moc"
