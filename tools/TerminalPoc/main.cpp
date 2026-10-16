#include "TerminalBridge.h"

#include <QApplication>
#include <QClipboard>
#include <QCommandLineParser>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMainWindow>
#include <QShortcut>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QWebChannel>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineView>

#include <cstdio>

class OfflineRequests final : public QWebEngineUrlRequestInterceptor
{
public:
    void interceptRequest(QWebEngineUrlRequestInfo &request) override
    {
        const QString scheme = request.requestUrl().scheme();
        request.block(scheme != QStringLiteral("qrc") && scheme != QStringLiteral("data"));
    }
};

class OfflinePage final : public QWebEnginePage
{
public:
    explicit OfflinePage(QWebEngineProfile *profile, QObject *parent) : QWebEnginePage(profile, parent)
    {
    }

protected:
    void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel level, const QString &message, int line,
                                  const QString &source) override
    {
        if (level == ErrorMessageLevel)
            std::fprintf(stderr, "JavaScript %s:%d: %s\n", source.toUtf8().constData(), line,
                         message.toUtf8().constData());
    }

    bool acceptNavigationRequest(const QUrl &url, NavigationType, bool isMainFrame) override
    {
        return isMainFrame && url == QUrl(QStringLiteral("qrc:/terminal/index.html"));
    }

    QWebEnginePage *createWindow(WebWindowType) override
    {
        return nullptr;
    }
};

int main(int argc, char *argv[])
{
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("RADManager Terminal POC"));
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringLiteral("self-test"), QStringLiteral("运行离线终端自动测试")});
    parser.addOption(
        {QStringLiteral("screenshot"), QStringLiteral("保存窗口截图并退出"), QStringLiteral("path")});
    parser.process(application);
    const bool selfTest = parser.isSet(QStringLiteral("self-test"));
    const QString screenshot = parser.value(QStringLiteral("screenshot"));
    // 自动测试窗口可能不在前台，避免 Chromium 将写入/确认定时器节流为每秒一次。
    // 普通运行保留默认策略；这不是性能基准，也不关闭浏览器沙箱。
    if (selfTest)
        qputenv("QTWEBENGINE_CHROMIUM_FLAGS",
                qgetenv("QTWEBENGINE_CHROMIUM_FLAGS") + " --disable-background-timer-throttling");

    // profile 必须比 page 长寿；无名称的 profile 不把浏览历史或缓存写入磁盘。
    OfflineRequests interceptor;
    QWebEngineProfile profile;
    profile.setUrlRequestInterceptor(&interceptor);
    QMainWindow window;
    window.setWindowTitle(QStringLiteral("RADManager · 本地终端验证"));
    window.resize(1120, 740);
    window.setStyleSheet(QStringLiteral(
        "QMainWindow,QToolBar,QStatusBar { background:#20252d; color:#cbd5df; }"
        "QToolBar { border:0; spacing:8px; padding:8px; }"
        "QToolButton { color:#cbd5df; background:#303743; padding:7px 12px; border-radius:4px; }"
        "QToolButton:hover { background:#255d60; color:#c3fff0; }"));
    auto *view = new QWebEngineView(&window);
    auto *page = new OfflinePage(&profile, view);
    view->setPage(page);
    view->setContextMenuPolicy(Qt::NoContextMenu);
    page->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
    page->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, false);
    page->settings()->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, false);
    page->settings()->setAttribute(QWebEngineSettings::JavascriptCanAccessClipboard, false);
    auto *bridge = new TerminalBridge(page);
    auto *channel = new QWebChannel(page);
    channel->registerObject(QStringLiteral("bridge"), bridge);
    page->setWebChannel(channel);
    window.setCentralWidget(view);
    window.statusBar()->showMessage(QStringLiteral("正在加载离线终端…"));
    QObject::connect(bridge, &TerminalBridge::status, &window,
                     [&window](const QString &text) { window.statusBar()->showMessage(text); });
    auto *toolbar = window.addToolBar(QStringLiteral("终端操作"));
    toolbar->setMovable(false);
    toolbar->setEnabled(false);
    QObject::connect(bridge, &TerminalBridge::terminalReady, toolbar,
                     [toolbar] { toolbar->setEnabled(true); });

    const auto copy = [page]()
    {
        page->runJavaScript(QStringLiteral("window.terminalPoc ? terminalPoc.copy() : ''"),
                            [](const QVariant &selection)
                            {
                                if (selection.isValid() && !selection.toString().isEmpty())
                                    QApplication::clipboard()->setText(selection.toString());
                            });
    };
    const auto paste = [page]()
    {
        const QString text = QApplication::clipboard()->text();
        if (text.size() > 65536)
        {
            qWarning("粘贴内容超过 65536 字符，本次输入已拒绝。");
            return;
        }
        const QByteArray arguments = QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact);
        page->runJavaScript(
            QStringLiteral("window.terminalPoc && terminalPoc.bridge && terminalPoc.paste.apply(null,%1)")
                .arg(QString::fromUtf8(arguments)));
    };
    QObject::connect(toolbar->addAction(QStringLiteral("复制")), &QAction::triggered, page, copy);
    QObject::connect(toolbar->addAction(QStringLiteral("粘贴")), &QAction::triggered, page, paste);
    QObject::connect(new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+C")), &window),
                     &QShortcut::activated, page, copy);
    QObject::connect(new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+V")), &window),
                     &QShortcut::activated, page, paste);
    QObject::connect(toolbar->addAction(QStringLiteral("字号 +")), &QAction::triggered, page,
                     [page] { page->runJavaScript(QStringLiteral("terminalPoc.zoom(1)")); });
    QObject::connect(toolbar->addAction(QStringLiteral("字号 −")), &QAction::triggered, page,
                     [page] { page->runJavaScript(QStringLiteral("terminalPoc.zoom(-1)")); });
    QObject::connect(toolbar->addAction(QStringLiteral("ANSI 颜色")), &QAction::triggered, bridge,
                     [bridge]
                     {
                         bridge->input(QString(QChar(3)));
                         bridge->input(QStringLiteral("ansi\r"));
                     });
    QObject::connect(toolbar->addAction(QStringLiteral("高吞吐输出")), &QAction::triggered, bridge,
                     [bridge]
                     {
                         bridge->input(QString(QChar(3)));
                         bridge->input(QStringLiteral("flood\r"));
                     });

    QObject::connect(page, &QWebEnginePage::loadFinished, &window,
                     [&window, selfTest](bool loaded)
                     {
                         if (!loaded)
                         {
                             qCritical("离线终端页面加载失败。");
                             window.statusBar()->showMessage(
                                 QStringLiteral("离线终端页面加载失败，请检查资源及 WebEngine 运行环境。"));
                             if (selfTest)
                                 QCoreApplication::exit(1);
                         }
                     });
    QObject::connect(bridge, &TerminalBridge::terminalReady, page,
                     [page, selfTest]
                     {
                         if (selfTest)
                             QTimer::singleShot(
                                 100, page,
                                 [page] { page->runJavaScript(QStringLiteral("terminalPoc.runTests()")); });
                     });
    QObject::connect(bridge, &TerminalBridge::testReported, &window,
                     [&window, selfTest, screenshot](bool passed, const QString &detail)
                     {
                         std::fprintf(stderr, "%s: %s\n", passed ? "PASS" : "FAIL",
                                      detail.toUtf8().constData());
                         if (!selfTest)
                             return;
                         QTimer::singleShot(500, &window,
                                            [&window, passed, screenshot]
                                            {
                                                bool saved = true;
                                                if (!screenshot.isEmpty())
                                                    saved = window.grab().save(screenshot);
                                                QCoreApplication::exit(passed && saved ? 0 : 1);
                                            });
                     });
    if (selfTest)
        QTimer::singleShot(90000, &application,
                           [&window, screenshot]
                           {
                               std::fprintf(stderr, "Terminal self-test timed out before completion.\n");
                               if (!screenshot.isEmpty())
                                   window.grab().save(screenshot);
                               QCoreApplication::exit(1);
                           });
    else if (!screenshot.isEmpty())
        QObject::connect(bridge, &TerminalBridge::terminalReady, &window,
                         [&window, screenshot]
                         {
                             QTimer::singleShot(1500, &window,
                                                [&window, screenshot]
                                                {
                                                    const bool saved = window.grab().save(screenshot);
                                                    if (!saved)
                                                        std::fprintf(stderr,
                                                                     "Cannot save window screenshot.\n");
                                                    QCoreApplication::exit(saved ? 0 : 1);
                                                });
                         });
    window.show();
    view->load(QUrl(QStringLiteral("qrc:/terminal/index.html")));
    return application.exec();
}
