#include "model/BuildBaseline.h"
#include "protocol/ProtocolVersion.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QMainWindow>
#include <QPointer>
#include <QStatusBar>
#include <QTimer>
#include <QVariant>
#include <QWebEnginePage>
#include <QWebEngineSettings>
#include <QWebEngineView>
#include <cstdio>

#include "WebEngineSmoke.h"

int runWebEngineSmoke(QApplication &application, const QString &screenshot)
{
    const bool smokeTest = true;
    bool failed = false;
    bool stopping = false;
    QMainWindow window;
    QObject::connect(&application, &QCoreApplication::aboutToQuit, &window, [&stopping] { stopping = true; });
    window.setWindowTitle(QStringLiteral("RADManager · 工程骨架"));
    window.resize(960, 640);
    auto *view = new QWebEngineView(&window);
    window.setCentralWidget(view);
    window.statusBar()->showMessage(rad::buildBaseline() +
                                    QStringLiteral(" · 协议版本 %1").arg(rad::protocolVersion()));
    view->setContextMenuPolicy(Qt::NoContextMenu);
    view->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
    view->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, false);
    view->settings()->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, false);
    const auto fail = [&window, &failed, smokeTest](const QString &reason)
    {
        failed = true;
        std::fprintf(stderr, "%s\n", reason.toUtf8().constData());
        window.statusBar()->showMessage(reason);
        if (smokeTest)
            QCoreApplication::exit(1);
    };
    QObject::connect(view->page(), &QWebEnginePage::renderProcessTerminated, &window,
                     [fail](QWebEnginePage::RenderProcessTerminationStatus, int code)
                     { fail(QStringLiteral("WebEngine 渲染进程退出，代码 %1").arg(code)); });
    QObject::connect(view, &QWebEngineView::loadFinished, &window,
                     [&, fail](bool loaded)
                     {
                         if (!loaded)
                         {
                             fail(QStringLiteral("工程骨架页面加载失败。"));
                             return;
                         }
                         if (!smokeTest)
                             return;
                         // 页面销毁时 Qt 仍可能回调 JavaScript；仅在窗口存活时继续验收。
                         const QPointer<QMainWindow> guard(&window);
                         view->page()->runJavaScript(
                             QStringLiteral("window.radSkeletonMarker"),
                             [&, guard, fail](const QVariant &marker)
                             {
                                 if (!guard || stopping)
                                     return;
                                 if (marker.toString() != QStringLiteral("RAD_M1_CLIENT_READY"))
                                 {
                                     fail(QStringLiteral("WebEngine JavaScript 标记不匹配。"));
                                     return;
                                 }
                                 QTimer::singleShot(
                                     500, &window,
                                     [&, fail]
                                     {
                                         if (failed)
                                             return;
                                         if (!screenshot.isEmpty() && !window.grab().save(screenshot))
                                         {
                                             fail(QStringLiteral("截图保存失败：%1").arg(screenshot));
                                             return;
                                         }
                                         std::fprintf(stdout, "PASS: client.smoke\n");
                                         QCoreApplication::exit(0);
                                     });
                             });
                     });
    if (smokeTest)
    {
        application.setQuitOnLastWindowClosed(false);
        QObject::connect(&application, &QApplication::lastWindowClosed, &window,
                         [fail] { fail(QStringLiteral("烟测完成前窗口已关闭。")); });
        QTimer::singleShot(20000, &window, [fail] { fail(QStringLiteral("WebEngine 烟测超时。")); });
    }
    window.show();
    view->setHtml(QStringLiteral(
        "<!doctype html><html lang='zh-CN'><head><meta charset='utf-8'>"
        "<meta http-equiv='Content-Security-Policy' content=\"default-src 'none'; style-src 'unsafe-inline'; "
        "script-src 'unsafe-inline'\">"
        "<title>RADManager 工程骨架</title><style>"
        "body{margin:0;background:#181b20;color:#e6e8eb;font:16px 'Microsoft YaHei',sans-serif;}"
        "main{padding:64px;}h1{font-size:30px;font-weight:500;}p{color:#b4bec9;line-height:1.8;}"
        "small{color:#32b8ad;}</style></head><body><main><small>RADManager</small>"
        "<h1>工程骨架，未连接Agent</h1><p>当前仅验证本机编译、启动与运行资源。"
        "<br>会话、终端与远程连接将在后续阶段实现。</p></main>"
        "<script>window.radSkeletonMarker='RAD_M1_CLIENT_READY';</script></body></html>"));
    return application.exec();
}
