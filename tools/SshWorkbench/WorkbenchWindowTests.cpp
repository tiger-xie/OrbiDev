#include "SshSession.h"
#include "WorkbenchWindow.h"
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

class WorkbenchWindowTests final : public QObject
{
    Q_OBJECT
private:
    static void answerClosePrompt(QMessageBox::StandardButton answer)
    {
        QTimer::singleShot(0, qApp,
                           [answer]
                           {
                               for (QWidget *widget : QApplication::topLevelWidgets())
                                   if (auto *box = qobject_cast<QMessageBox *>(widget))
                                       box->button(answer)->click();
                           });
    }

private slots:
    void sessionStartingDuringCloseConfirmationIsAwaited()
    {
        QTemporaryDir directory;
        sshworkbench::WorkbenchWindow window(directory.filePath(QStringLiteral("profile.json")),
                                             directory.filePath(QStringLiteral("projects.json")));
        auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("terminalTabs"));
        auto *page = new QWidget(tabs);
        page->setProperty("sessionPending", true);
        auto *session = new sshworkbench::SshSession(page);
        tabs->addTab(page, QStringLiteral("pending"));
        QTimer::singleShot(0, &window,
                           [session]
                           {
                               // 非法主机在后台立即失败，不访问网络；模拟确认期间从准备转为活动。
                               sshsetup::ConnectionProfile profile;
                               profile.host = QStringLiteral("-invalid");
                               session->start(profile, {});
                               for (QWidget *widget : QApplication::topLevelWidgets())
                                   if (auto *box = qobject_cast<QMessageBox *>(widget))
                                       box->button(QMessageBox::Yes)->click();
                           });
        QVERIFY(!window.close());
        QVERIFY(page->property("sessionClosing").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(!session->isActive(), 5000);
    }
    void pendingTerminalCloseRequiresConfirmation()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        sshworkbench::WorkbenchWindow window(directory.filePath(QStringLiteral("profile.json")),
                                             directory.filePath(QStringLiteral("projects.json")));
        auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("terminalTabs"));
        auto *page = new QWidget(tabs);
        page->setProperty("sessionPending", true);
        tabs->addTab(page, QStringLiteral("pending"));
        answerClosePrompt(QMessageBox::No);
        QVERIFY(QMetaObject::invokeMethod(tabs, "tabCloseRequested", Q_ARG(int, 0)));
        QCOMPARE(tabs->count(), 1);
        QVERIFY(!page->property("sessionClosing").toBool());
        answerClosePrompt(QMessageBox::Yes);
        QVERIFY(QMetaObject::invokeMethod(tabs, "tabCloseRequested", Q_ARG(int, 0)));
        QCOMPARE(tabs->count(), 0);
        QVERIFY(page->property("sessionClosing").toBool());
    }
    void startupDoesNotCreateSessionOrConfiguration()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString profile = directory.filePath(QStringLiteral("connection.json"));
        const QString projects = directory.filePath(QStringLiteral("projects.json"));
        sshworkbench::WorkbenchWindow window(profile, projects);
        QVERIFY(window.findChildren<sshworkbench::SshSession *>().isEmpty());
        QCOMPARE(window.findChild<QTabWidget *>(QStringLiteral("terminalTabs"))->count(), 0);
        QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("newTerminalButton"))->isEnabled());
        QVERIFY(!QFile::exists(profile));
        QVERIFY(!QFile::exists(projects));
    }

    void previewIgnoresConfigurationAndCannotConnect()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        sshworkbench::WorkbenchWindow window(directory.filePath(QStringLiteral("profile.json")),
                                             directory.filePath(QStringLiteral("projects.json")), {}, true);
        QCOMPARE(window.findChild<QListWidget *>(QStringLiteral("projectList"))->count(), 2);
        QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("newTerminalButton"))->isEnabled());
        QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("connectionSettingsButton"))->isEnabled());
        QVERIFY(window.findChildren<sshworkbench::SshSession *>().isEmpty());
        QVERIFY(QDir(directory.path()).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
    }

    void legacyProjectsPreservedAndSavedSeparately()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString legacy = directory.filePath(QStringLiteral("legacy.json"));
        const QString projects = directory.filePath(QStringLiteral("projects.json"));
        launcher::Configuration configuration;
        configuration.terminalPath = QStringLiteral("C:/Tools/wt.exe");
        configuration.projects = {{QStringLiteral("中文项目"), QStringLiteral("D:/Projects/中文"), true},
                                  {QStringLiteral("第二项目"), QString(), false}};
        QString error;
        QVERIFY2(launcher::saveConfiguration(legacy, configuration, error), qPrintable(error));
        QFile original(legacy);
        QVERIFY(original.open(QIODevice::ReadOnly));
        const auto before = original.readAll();
        original.close();
        sshworkbench::WorkbenchWindow window(directory.filePath(QStringLiteral("profile.json")), projects,
                                             legacy);
        auto *list = window.findChild<QListWidget *>(QStringLiteral("projectList"));
        QCOMPARE(list->count(), 2);
        QCOMPARE(list->item(0)->checkState(), Qt::Checked);
        QCOMPARE(list->item(1)->checkState(), Qt::Unchecked);
        QVERIFY(!QFile::exists(projects));
        list->item(1)->setCheckState(Qt::Checked);
        launcher::Configuration saved;
        QVERIFY2(launcher::loadConfiguration(projects, saved, error), qPrintable(error));
        QCOMPARE(saved.projects.at(0).name, configuration.projects.at(0).name);
        QCOMPARE(saved.projects.at(0).directory, configuration.projects.at(0).directory);
        QVERIFY(saved.projects.at(1).enabled);
        QCOMPARE(saved.terminalPath, configuration.terminalPath);
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), before);
        QVERIFY(window.findChildren<sshworkbench::SshSession *>().isEmpty());
    }

    void invalidExistingProjectsAreNotReplacedWithLegacy()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString projects = directory.filePath(QStringLiteral("projects.json"));
        QFile file(projects);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("invalid");
        file.close();
        launcher::Configuration legacy;
        legacy.projects = {{QStringLiteral("legacy"), {}, true}};
        QString error;
        const QString legacyPath = directory.filePath(QStringLiteral("legacy.json"));
        QVERIFY(launcher::saveConfiguration(legacyPath, legacy, error));
        sshworkbench::WorkbenchWindow window(directory.filePath(QStringLiteral("profile.json")), projects,
                                             legacyPath);
        QCOMPARE(window.findChild<QListWidget *>(QStringLiteral("projectList"))->count(), 0);
        QVERIFY(window.findChild<QLabel *>(QStringLiteral("feedback"))
                    ->text()
                    .contains(QStringLiteral("原文件保留")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("invalid"));
    }
};
QTEST_MAIN(WorkbenchWindowTests)
#include "WorkbenchWindowTests.moc"
