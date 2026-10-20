#include "SetupTheme.h"
#include "WorkbenchWindow.h"
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
    QApplication::setOrganizationName(QStringLiteral("OrbiDev"));
    QApplication::setApplicationName(QStringLiteral("OrbiDevSshSetup"));
    const QString setupPath = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
                              QStringLiteral("/connection.json");
    QApplication::setApplicationName(QStringLiteral("OrbiDevWorkbench"));
    sshsetup::applySetupTheme(application);
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("OrbiDev 内嵌 SSH 工作台"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption(
        {QStringLiteral("config"), QStringLiteral("首次设置保存的连接文件"), QStringLiteral("path")});
    parser.addOption(
        {QStringLiteral("projects"), QStringLiteral("工作台项目配置文件"), QStringLiteral("path")});
    parser.addOption(
        {QStringLiteral("legacy-config"), QStringLiteral("只读迁移旧启动器项目"), QStringLiteral("path")});
    parser.addOption({QStringLiteral("screenshot"), QStringLiteral("无 SSH、无配置写入的界面预览"),
                      QStringLiteral("path")});
    parser.addOption({QStringLiteral("preview-tabs"), QStringLiteral("截图时展示无连接的标签布局")});
    parser.process(application);
    const QString profile =
        parser.isSet(QStringLiteral("config")) ? parser.value(QStringLiteral("config")) : setupPath;
    const QString projects = parser.isSet(QStringLiteral("projects"))
                                 ? parser.value(QStringLiteral("projects"))
                                 : QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
                                       QStringLiteral("/projects.json");
    const bool preview = parser.isSet(QStringLiteral("screenshot"));
    sshworkbench::WorkbenchWindow window(profile, projects, parser.value(QStringLiteral("legacy-config")),
                                         preview);
    window.setWindowTitle(window.windowTitle() + QStringLiteral(" · ") + QApplication::applicationVersion());
    if (preview && parser.isSet(QStringLiteral("preview-tabs")))
        window.showPreviewTabs();
    window.show();
    if (preview)
    {
        const QString path = parser.value(QStringLiteral("screenshot"));
        QTimer::singleShot(350, &window,
                           [&window, &application, path]
                           {
                               const bool saved =
                                   QDir().mkpath(QFileInfo(path).absolutePath()) && window.grab().save(path);
                               application.exit(saved ? 0 : 2);
                           });
    }
    return application.exec();
}
