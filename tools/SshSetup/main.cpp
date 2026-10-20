#include "SetupTheme.h"
#include "SetupWindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QPixmap>
#include <QStandardPaths>
#include <QTimer>

int main(int argc, char *argv[])
{
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    QApplication application(argc, argv);
    QApplication::setApplicationVersion(QStringLiteral(ORBIDEV_VERSION));
    QApplication::setApplicationName(QStringLiteral("OrbiDevSshSetup"));
    QApplication::setOrganizationName(QStringLiteral("OrbiDev"));
    sshsetup::applySetupTheme(application);
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("OrbiDev SSH 首次配置与家庭端授权工具"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({QStringLiteral("home"), QStringLiteral("打开家庭端授权工具")});
    parser.addOption(
        {QStringLiteral("config"), QStringLiteral("客户端连接配置文件"), QStringLiteral("path")});
    parser.addOption({QStringLiteral("legacy-config"), QStringLiteral("只读导入旧启动器主机配置"),
                      QStringLiteral("path")});
    parser.addOption(
        {QStringLiteral("screenshot"), QStringLiteral("无副作用界面预览并保存截图"), QStringLiteral("path")});
    parser.addOption({QStringLiteral("page"), QStringLiteral("预览步骤，0 至 3"), QStringLiteral("number"),
                      QStringLiteral("0")});
    parser.process(application);
    bool homeMode = parser.isSet(QStringLiteral("home"));
#ifdef ORBIDEV_HOME_SETUP
    homeMode = true;
#endif
    const QString profile = parser.isSet(QStringLiteral("config"))
                                ? parser.value(QStringLiteral("config"))
                                : QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
                                      QStringLiteral("/connection.json");
    const bool preview = parser.isSet(QStringLiteral("screenshot"));
    sshsetup::SetupWindow window(homeMode, profile, parser.value(QStringLiteral("legacy-config")), preview);
    window.setWindowTitle(window.windowTitle() + QStringLiteral(" · ") + QApplication::applicationVersion());
    if (preview)
        window.setPreviewPage(parser.value(QStringLiteral("page")).toInt());
    window.show();
    if (preview)
    {
        const QString path = parser.value(QStringLiteral("screenshot"));
        QTimer::singleShot(350, &window,
                           [&window, &application, path]
                           {
                               const bool directoryReady = QDir().mkpath(QFileInfo(path).absolutePath());
                               const bool saved = directoryReady && window.grab().save(path);
                               application.exit(saved ? 0 : 2);
                           });
    }
    return application.exec();
}
