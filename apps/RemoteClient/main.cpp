#include "WebEngineSmoke.h"
#include "controllers/ClientController.h"
#include "controllers/FakeAgent.h"
#include "ui/ClientWindow.h"
#include "ui/Theme.h"
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <transport/WebSocketAgentClient.h>

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
    QByteArray deviceToken = qgetenv("RAD_AGENT_TOKEN");
    qunsetenv("RAD_AGENT_TOKEN");
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
    parser.addOption({QStringLiteral("agent-url"),
                      QStringLiteral("Agent 的回环或 Tailscale IPv4 WebSocket 地址"), QStringLiteral("url")});
    parser.addOption({QStringLiteral("workspace"), QStringLiteral("Agent 已配置的工作区 ID，可重复指定"),
                      QStringLiteral("id")});
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
    std::unique_ptr<rad::client::IAgentClient> agent;
    if (parser.isSet(QStringLiteral("agent-url")))
    {
        if (parser.isSet(QStringLiteral("demo")) || uiSmoke)
            return 1;
        rad::client::WebSocketAgentClient::Options options;
        options.url = QUrl(parser.value(QStringLiteral("agent-url")));
        options.workspaces = parser.values(QStringLiteral("workspace"));
        // 令牌不出现在命令行、日志或设置；在启动 WebEngine 子进程前移除继承环境。
        options.token = std::move(deviceToken);
        if (options.token.isEmpty())
        {
            bool accepted = false;
            options.token =
                QInputDialog::getText(nullptr, QStringLiteral("Agent 连接认证"),
                                      QStringLiteral("输入已配对的 Agent 设备令牌（本次运行有效）"),
                                      QLineEdit::Password, {}, &accepted)
                    .toLatin1();
            if (!accepted)
                return 0;
        }
        const auto error = rad::client::WebSocketAgentClient::validateOptions(options);
        if (!error.isEmpty())
        {
            QMessageBox::critical(nullptr, QStringLiteral("连接配置无效"), error);
            return 1;
        }
        agent = std::make_unique<rad::client::WebSocketAgentClient>(std::move(options));
    }
    else
        agent = std::make_unique<rad::client::FakeAgent>();
    rad::client::ClientController controller(agent.get());
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
