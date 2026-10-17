#include "WebEngineSmoke.h"
#include "controllers/ClientController.h"
#include "ui/ClientWindow.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QRegularExpression>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>
#include <memory>

int main(int argc, char *argv[])
{
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("RADManager"));
    QCoreApplication::setApplicationName(QStringLiteral("RemoteClient"));
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringLiteral("smoke-test"), QStringLiteral("验证 WebEngine 页面及 JavaScript")});
    parser.addOption({QStringLiteral("ui-smoke-test"), QStringLiteral("验证基础界面并退出")});
    parser.addOption({QStringLiteral("demo"), QStringLiteral("连接模拟 Agent 并打开演示会话")});
    parser.addOption(
        {QStringLiteral("screenshot"), QStringLiteral("烟测成功后保存截图"), QStringLiteral("path")});
    parser.addOption({QStringLiteral("window-size"), QStringLiteral("界面尺寸，例如 1280x800"),
                      QStringLiteral("WxH"), QStringLiteral("1440x1024")});
    parser.process(application);
    const bool webSmoke = parser.isSet(QStringLiteral("smoke-test"));
    const bool uiSmoke = parser.isSet(QStringLiteral("ui-smoke-test"));
    const QString screenshot = parser.value(QStringLiteral("screenshot"));
    if ((webSmoke && uiSmoke) || (!webSmoke && !uiSmoke && !screenshot.isEmpty()))
        return 1;
    if (webSmoke)
        return runWebEngineSmoke(application, screenshot);
    const auto dimensions = QRegularExpression(QStringLiteral("\\A([0-9]{3,4})x([0-9]{3,4})\\z"))
                                .match(parser.value(QStringLiteral("window-size")));
    if (!dimensions.hasMatch())
        return 1;
    // 自动验收隔离用户设置，正常运行持久保存内置主题标识。
    QTemporaryDir temporarySettings;
    if (uiSmoke && !temporarySettings.isValid())
        return 1;
    std::unique_ptr<QSettings> settings =
        uiSmoke ? std::make_unique<QSettings>(temporarySettings.filePath(QStringLiteral("client.ini")),
                                              QSettings::IniFormat)
                : std::make_unique<QSettings>();
    rad::client::applyTheme(application, *settings);
    rad::client::FakeAgent agent;
    rad::client::ClientController controller(&agent);
    rad::client::ClientWindow window(&controller);
    window.resize(dimensions.captured(1).toInt(), dimensions.captured(2).toInt());
    if (uiSmoke)
    {
        application.setQuitOnLastWindowClosed(false);
        QObject::connect(&application, &QApplication::lastWindowClosed, &window,
                         [&] { application.exit(1); });
    }
    window.show();
    const bool demo = parser.isSet(QStringLiteral("demo"));
    bool opened = false;
    bool capturing = false;
    const auto ready = [&]
    {
        if (controller.isBusy())
            return;
        if (demo && !opened)
        {
            if (!controller.isConnected() || controller.sessions().isEmpty())
                return;
            opened = true;
            window.openSession(controller.sessions().first().id);
            return;
        }
        if (!uiSmoke || capturing || (demo && window.sessionTabCount() == 0))
            return;
        capturing = true;
        QTimer::singleShot(500, &window,
                           [&]
                           {
                               if (!screenshot.isEmpty() && !window.grab().save(screenshot))
                               {
                                   application.exit(1);
                                   return;
                               }
                               std::puts("PASS client.ui-smoke");
                               application.exit(0);
                           });
    };
    QObject::connect(&controller, &rad::client::ClientController::changed, &window, ready);
    QObject::connect(&controller, &rad::client::ClientController::sessionOpened, &window,
                     [&](const QString &) { ready(); });
    if (uiSmoke)
        QTimer::singleShot(10000, &window, [&] { application.exit(1); });
    if (demo)
        controller.connectAgent();
    else
        QTimer::singleShot(0, &window, ready);
    return application.exec();
}
