#include "RemoteDirectoryDialog.h"
#include "SetupTheme.h"
#include <QApplication>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QtTest>

namespace sshworkbench
{
class RemoteDirectoryDialogTests final : public QObject
{
    Q_OBJECT
private slots:
    void revisitingUsesCacheAndRefreshInvalidates()
    {
        RemoteDirectoryDialog dialog({}, {});
        dialog.m_started = true;
        RemoteDirectoryResult result;
        result.success = true;
        result.homePath = QStringLiteral("C:/Users/Test");
        result.currentPath = QStringLiteral("C:/");
        dialog.applyResult(result);
        result.currentPath = QStringLiteral("D:/");
        dialog.applyResult(result);
        dialog.requestDirectory(QStringLiteral("C:\\"));
        QVERIFY(!dialog.m_service->isBusy());
        QCOMPARE(dialog.m_location->text(), QStringLiteral("C:/"));
        QVERIFY(dialog.m_select->isEnabled());
        QVERIFY(dialog.m_feedback->text().contains(QStringLiteral("缓存")));
        dialog.requestDirectory(QStringLiteral("D:/"));
        QVERIFY(!dialog.m_service->isBusy());
        QCOMPARE(dialog.m_location->text(), QStringLiteral("D:/"));
        dialog.m_refresh->click();
        QVERIFY(!dialog.m_cache.contains(QStringLiteral("D:/")));
        QVERIFY(!dialog.m_select->isEnabled());
        QTRY_VERIFY(!dialog.m_service->isBusy());
        QVERIFY(!dialog.m_result.success);
        QVERIFY(!dialog.m_cache.contains(QStringLiteral("D:/")));
        dialog.requestDirectory(QStringLiteral("C:/"));
        QVERIFY(dialog.m_result.success);
        RemoteDirectoryDialog another({}, {});
        QCOMPARE(another.m_cache.size(), 0);
    }

    void cacheBoundAndHomeNavigation()
    {
        RemoteDirectoryDialog dialog({}, {});
        dialog.m_started = true;
        RemoteDirectoryResult result;
        result.success = true;
        result.homePath = QStringLiteral("C:/Users/Test");
        for (int i = 0; i < 40; ++i)
        {
            result.currentPath = QStringLiteral("D:/Folder%1").arg(i);
            dialog.applyResult(result);
        }
        QVERIFY(dialog.m_cache.size() <= 32);
        QVERIFY(dialog.m_cache.totalCost() <= 8192);
        QVERIFY(!dialog.m_cache.contains(QStringLiteral("D:/Folder0")));
        QVERIFY(dialog.m_cache.contains(QStringLiteral("D:/Folder39")));
        result.currentPath = result.homePath;
        dialog.applyResult(result);
        dialog.requestDirectory(QStringLiteral("D:/Folder39"));
        dialog.m_home->click();
        QVERIFY(!dialog.m_service->isBusy());
        QCOMPARE(dialog.m_location->text(), result.homePath);
        result.success = false;
        result.currentPath = QStringLiteral("D:/Failed");
        dialog.applyResult(result);
        QVERIFY(!dialog.m_cache.contains(result.currentPath));
    }
    void loadingFailureEmptyAndSelection()
    {
        HostProfile host;
        host.name = QStringLiteral("家庭电脑（测试）");
        RemoteDirectoryDialog dialog(host, QStringLiteral("D:/Old"));
        dialog.m_started = true;
        QVERIFY(!dialog.m_select->isEnabled());
        RemoteDirectoryResult result;
        result.error = QStringLiteral("目录不存在或无权限");
        dialog.applyResult(result);
        QVERIFY(!dialog.m_select->isEnabled());
        QVERIFY(dialog.m_home->isEnabled());
        QCOMPARE(dialog.m_location->text(), QStringLiteral("D:/Old"));
        result.success = true;
        result.currentPath = QStringLiteral("D:/项目 空间");
        result.parentPath = QStringLiteral("D:/");
        result.drives = {{QStringLiteral("D:"), QStringLiteral("D:/")}};
        dialog.applyResult(result);
        QVERIFY(dialog.m_select->isEnabled());
        QVERIFY(dialog.m_feedback->text().contains(QStringLiteral("没有子文件夹")));
        QVERIFY(dialog.selectedPath().isEmpty());
        QTest::keyClicks(dialog.m_location, "x");
        QVERIFY(!dialog.m_select->isEnabled());
        dialog.applyResult(result);
        dialog.m_select->click();
        QCOMPARE(dialog.selectedPath(), result.currentPath);
    }

    void cancelDoesNotReturnPath()
    {
        RemoteDirectoryDialog dialog({}, {});
        dialog.m_started = true;
        RemoteDirectoryResult result;
        result.success = true;
        result.currentPath = QStringLiteral("C:/Users/Test");
        dialog.applyResult(result);
        dialog.reject();
        QVERIFY(dialog.selectedPath().isEmpty());
    }

    void longPathsAndNarrowWindow()
    {
        HostProfile host;
        host.name = QStringLiteral("家庭开发电脑");
        RemoteDirectoryDialog dialog(host, {});
        dialog.m_started = true;
        RemoteDirectoryResult result;
        result.success = true;
        result.currentPath = QStringLiteral("D:/") + QStringLiteral("很长的目录/").repeated(35);
        result.parentPath = QStringLiteral("D:/");
        result.drives = {{QStringLiteral("本地磁盘 C:"), QStringLiteral("C:/")},
                         {QStringLiteral("项目磁盘 D:"), QStringLiteral("D:/")}};
        result.directories = {
            {QStringLiteral("OrbiDev"), result.currentPath + QStringLiteral("OrbiDev")},
            {QStringLiteral("中文项目名称").repeated(15), result.currentPath + QStringLiteral("长目录")}};
        dialog.applyResult(result);
        dialog.resize(600, 480);
        dialog.show();
        QTest::qWait(100);
        QVERIFY(dialog.rect().contains(QRect(dialog.m_select->mapTo(&dialog, {}), dialog.m_select->size())));
        QCOMPARE(dialog.m_directories->count(), 2);
        QCOMPARE(dialog.m_directories->item(1)->toolTip(), result.directories[1].path);
        const auto output = qEnvironmentVariable("ORBIDEV_FE_SCREENSHOT");
        if (!output.isEmpty())
        {
            QVERIFY(QDir().mkpath(output));
            QVERIFY(dialog.grab().save(output + QStringLiteral("/directory.png")));
            dialog.setBusy(true);
            dialog.m_feedback->setText(QStringLiteral("正在读取远程目录…可取消关闭。"));
            QTest::qWait(50);
            QVERIFY(dialog.grab().save(output + QStringLiteral("/directory-loading.png")));
            RemoteDirectoryResult failed;
            failed.error = QStringLiteral("目录不存在或无权限。");
            dialog.applyResult(failed);
            QTest::qWait(50);
            QVERIFY(dialog.grab().save(output + QStringLiteral("/directory-error.png")));
            result.directories.clear();
            result.currentPath = QStringLiteral("D:/空目录");
            dialog.applyResult(result);
            dialog.m_location->setFocus();
            QTest::qWait(50);
            QVERIFY(dialog.grab().save(output + QStringLiteral("/directory-empty.png")));
        }
        dialog.reject();
    }
};
}

int main(int argc, char **argv)
{
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    QApplication app(argc, argv);
    sshsetup::applySetupTheme(app);
    sshworkbench::RemoteDirectoryDialogTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "RemoteDirectoryDialogTests.moc"
