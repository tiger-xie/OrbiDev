#include "SetupTheme.h"
#include "SshSession.h"
#include "WorkbenchWindow.h"
#include "terminal/TerminalView.h"
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <QtTest>

class WorkbenchWindowTests final : public QObject
{
    Q_OBJECT
private:
    static void bindProject(sshworkbench::WorkbenchWindow &window, const launcher::Project &project)
    {
        if (window.m_state.hosts.isEmpty())
        {
            sshworkbench::HostProfile host;
            host.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            host.name = QStringLiteral("测试机");
            host.connection.host = QStringLiteral("test.invalid");
            host.connection.user = QStringLiteral("test-user");
            window.m_state.hosts.append(host);
            window.m_state.currentHostId = host.id;
        }
        window.m_state.projectHosts.insert(project.id, window.m_state.currentHostId);
    }
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
    static QWidget *openPending(sshworkbench::WorkbenchWindow &window, const launcher::Project &project,
                                bool createNew = false)
    {
        window.openProject(project, createNew);
        auto *page = window.m_tabs->currentWidget();
        auto *terminal = page->findChild<rad::client::TerminalView *>();
        // 保留会话信号接线，但禁止测试访问网络。
        QObject::disconnect(terminal, nullptr, page, nullptr);
        return page;
    }

private slots:
    void projectCountsSelectionAndNamesFollowTabs()
    {
        QTemporaryDir directory;
        sshworkbench::WorkbenchWindow window(directory.filePath(QStringLiteral("profile.json")),
                                             directory.filePath(QStringLiteral("projects.json")));
        launcher::Project first{QStringLiteral("同名项目"), QStringLiteral("D:/First"), true,
                                QUuid::createUuid().toString(QUuid::WithoutBraces)};
        auto second = first;
        second.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        second.directory = QStringLiteral("D:/Second");
        bindProject(window, first);
        bindProject(window, second);
        window.m_state.projects.projects = {first, second};
        window.renderProjects();
        auto *firstTab = openPending(window, first);
        openPending(window, first, true);
        openPending(window, second);
        QVERIFY(window.m_projects->item(0)->text().contains(QStringLiteral("2 个标签")));
        QVERIFY(window.m_projects->item(1)->text().contains(QStringLiteral("1 个标签")));
        QVERIFY(!QFile::exists(window.m_statePath));
        QVERIFY(window.m_tabs->tabText(0) != window.m_tabs->tabText(1));
        QVERIFY(window.m_tabs->tabText(0) != window.m_tabs->tabText(2));
        window.m_tabs->setCurrentWidget(firstTab);
        QCOMPARE(window.projectRow(), 0);
        window.m_projects->setCurrentRow(1);
        QVERIFY(QMetaObject::invokeMethod(window.m_tabs, "tabBarClicked", Q_ARG(int, 0)));
        QCOMPARE(window.projectRow(), 0);
        const auto target = firstTab->property("sessionTarget").toStringList();
        window.m_state.projects.projects[0].name = QStringLiteral("改名后");
        window.renderProjects();
        QVERIFY(window.m_tabs->tabText(0).contains(QStringLiteral("改名后")));
        QCOMPARE(firstTab->property("sessionTarget").toStringList(), target);
        QVERIFY(window.m_tabs->tabToolTip(0).contains(QStringLiteral("测试机")));
        QVERIFY(window.m_tabs->tabToolTip(0).contains(QStringLiteral("D:/First")));
        firstTab->setProperty("sessionPending", false);
        window.closeTerminal(0);
        QVERIFY(window.m_projects->item(0)->text().contains(QStringLiteral("1 个标签")));
    }

    void sshStatusExitReasonAndReconnectAreExplicit()
    {
        QTemporaryDir directory;
        sshworkbench::WorkbenchWindow window(directory.filePath(QStringLiteral("profile.json")),
                                             directory.filePath(QStringLiteral("projects.json")));
        launcher::Project project{QStringLiteral("项目"), QStringLiteral("D:/Original"), true,
                                  QUuid::createUuid().toString(QUuid::WithoutBraces)};
        bindProject(window, project);
        window.m_state.projects.projects = {project};
        auto *page = openPending(window, project);
        auto *session = page->findChild<sshworkbench::SshSession *>();
        auto *status = page->findChild<QLabel *>(QStringLiteral("terminalStatus"));
        auto *reconnect = page->findChild<QPushButton *>(QStringLiteral("reconnectTerminalButton"));
        QVERIFY(status);
        QVERIFY(reconnect);
        QVERIFY(reconnect->isHidden());
        QVERIFY(QMetaObject::invokeMethod(session, "hostVerificationStarted"));
        QVERIFY(status->text().contains(QStringLiteral("校验主机")));
        session->started();
        QVERIFY(status->text().contains(QStringLiteral("SSH 进程运行")));
        QVERIFY(!status->text().contains(QStringLiteral("正在连接")));
        session->errorOccurred(QStringLiteral("主机校验失败"));
        session->exited(255);
        page->setProperty("sessionPending", false);
        QVERIFY(status->text().contains(QStringLiteral("主机校验失败")));
        QVERIFY(status->text().contains(QStringLiteral("255")));
        QVERIFY(window.m_tabs->tabText(0).contains(QStringLiteral("失败")));
        QVERIFY(!reconnect->isHidden());
        const auto screenshot = qEnvironmentVariable("ORBIDEV_SSH03_SCREENSHOT");
        if (!screenshot.isEmpty())
        {
            window.renderHosts();
            window.reloadProfile();
            window.renderProjects();
            auto *terminal = page->findChild<rad::client::TerminalView *>();
            QSignalSpy ready(terminal, &rad::client::TerminalView::ready);
            window.show();
            QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty(), 20000);
            terminal->appendOutput(QStringLiteral("[本地界面替身]\r\nSSH 主机校验失败，退出码 255。\r\n"
                                                  "重新连接会创建新会话；信任提示由用户确认。\r\n")
                                       .toUtf8());
            QTest::qWait(300);
            QVERIFY(window.grab().save(screenshot));
        }
        const auto original = page->property("sessionTarget").toStringList();
        window.m_state.hosts[0].connection.host = QStringLiteral("changed.invalid");
        window.m_state.projects.projects[0].directory = QStringLiteral("D:/Changed");
        answerClosePrompt(QMessageBox::No);
        reconnect->click();
        QCOMPARE(window.m_tabs->count(), 1);
        answerClosePrompt(QMessageBox::Yes);
        reconnect->click();
        QCOMPARE(window.m_tabs->count(), 2);
        auto *replacement = window.m_tabs->currentWidget();
        auto *terminal = replacement->findChild<rad::client::TerminalView *>();
        QObject::disconnect(terminal, nullptr, replacement, nullptr);
        QCOMPARE(replacement->property("sessionTarget").toStringList(), original);
        QVERIFY(replacement != page);
    }

    void closeCancellationPreservesExitAndLateSignalsDoNotReviveClosingTab()
    {
        QTemporaryDir directory;
        sshworkbench::WorkbenchWindow window(directory.filePath(QStringLiteral("profile.json")),
                                             directory.filePath(QStringLiteral("projects.json")));
        launcher::Project project{
            QStringLiteral("项目"), {}, true, QUuid::createUuid().toString(QUuid::WithoutBraces)};
        bindProject(window, project);
        auto *page = openPending(window, project);
        auto *session = page->findChild<sshworkbench::SshSession *>();
        QTimer::singleShot(0, &window,
                           [session]
                           {
                               session->exited(7);
                               for (QWidget *widget : QApplication::topLevelWidgets())
                                   if (auto *box = qobject_cast<QMessageBox *>(widget))
                                       box->button(QMessageBox::No)->click();
                           });
        window.closeTerminal(0);
        QCOMPARE(window.m_tabs->count(), 1);
        QVERIFY(!page->property("sessionClosing").toBool());
        QVERIFY(page->findChild<QLabel *>(QStringLiteral("terminalStatus"))->text().contains('7'));
        page->setProperty("sessionClosing", true);
        const auto before = window.m_tabs->tabText(0);
        session->started();
        QVERIFY(QMetaObject::invokeMethod(session, "hostVerificationStarted"));
        QCOMPARE(window.m_tabs->tabText(0), before);
    }

    void sessionStartingDuringTabCloseIsStoppedAsynchronously()
    {
        QTemporaryDir directory;
        sshworkbench::WorkbenchWindow window(directory.filePath(QStringLiteral("profile.json")),
                                             directory.filePath(QStringLiteral("projects.json")));
        launcher::Project project{
            QStringLiteral("项目"), {}, true, QUuid::createUuid().toString(QUuid::WithoutBraces)};
        bindProject(window, project);
        auto *page = openPending(window, project);
        auto *session = page->findChild<sshworkbench::SshSession *>();
        QTimer::singleShot(0, &window,
                           [session]
                           {
                               sshsetup::ConnectionProfile profile;
                               profile.host = QStringLiteral("-invalid");
                               session->start(profile, {});
                               session->started();
                               for (QWidget *widget : QApplication::topLevelWidgets())
                                   if (auto *box = qobject_cast<QMessageBox *>(widget))
                                       box->button(QMessageBox::Yes)->click();
                           });
        window.closeTerminal(0);
        QVERIFY(page->property("sessionClosing").toBool());
        QVERIFY(window.m_tabs->tabText(0).contains(QStringLiteral("正在关闭")));
        QTRY_COMPARE_WITH_TIMEOUT(window.m_tabs->count(), 0, 5000);
    }

    void exitDuringCloseConfirmation_data()
    {
        QTest::addColumn<bool>("wholeWindow");
        QTest::addColumn<bool>("acceptClose");
        QTest::newRow("tab-cancel") << false << false;
        QTest::newRow("tab-confirm") << false << true;
        QTest::newRow("window-cancel") << true << false;
        QTest::newRow("window-confirm") << true << true;
    }

    void exitDuringCloseConfirmation()
    {
        QFETCH(bool, wholeWindow);
        QFETCH(bool, acceptClose);
        QTemporaryDir directory;
        sshworkbench::WorkbenchWindow window(directory.filePath(QStringLiteral("profile.json")),
                                             directory.filePath(QStringLiteral("projects.json")));
        launcher::Project project{
            QStringLiteral("项目"), {}, true, QUuid::createUuid().toString(QUuid::WithoutBraces)};
        bindProject(window, project);
        auto *page = openPending(window, project);
        auto *session = page->findChild<sshworkbench::SshSession *>();
        QTimer::singleShot(0, &window,
                           [session, acceptClose]
                           {
                               session->exited(7);
                               for (QWidget *widget : QApplication::topLevelWidgets())
                                   if (auto *box = qobject_cast<QMessageBox *>(widget))
                                       box->button(acceptClose ? QMessageBox::Yes : QMessageBox::No)->click();
                           });
        if (wholeWindow)
        {
            QCOMPARE(window.close(), acceptClose);
            QCOMPARE(window.m_closing, acceptClose);
        }
        else
        {
            window.closeTerminal(0);
            QCOMPARE(window.m_tabs->count(), acceptClose ? 0 : 1);
        }
        if (!acceptClose)
        {
            QVERIFY(!page->property("sessionClosing").toBool());
            QVERIFY(page->findChild<QLabel *>(QStringLiteral("terminalStatus"))->text().contains('7'));
            QVERIFY(!page->findChild<QPushButton *>(QStringLiteral("reconnectTerminalButton"))->isHidden());
        }
    }

    void repeatedOpenAndExplicitNewUseDifferentSemantics()
    {
        QTemporaryDir directory;
        const auto profilePath = directory.filePath(QStringLiteral("profile.json"));
        sshsetup::ConnectionProfile profile;
        profile.host = QStringLiteral("test.invalid");
        QString error;
        QVERIFY(sshsetup::saveProfile(profilePath, profile, error));
        sshworkbench::WorkbenchWindow window(profilePath,
                                             directory.filePath(QStringLiteral("projects.json")));
        window.m_state.projects.projects = {
            {QStringLiteral("项目"), {}, true, QUuid::createUuid().toString(QUuid::WithoutBraces)}};
        bindProject(window, window.m_state.projects.projects.front());
        window.renderProjects();
        auto *list = window.m_projects;
        auto *tabs = window.m_tabs;
        auto doubleClick = [&]
        {
            QVERIFY(QMetaObject::invokeMethod(list, "itemDoubleClicked",
                                              Q_ARG(QListWidgetItem *, list->item(0))));
            // 本测试仅验证界面路由和准备中去重，阻止终端就绪后访问网络。
            for (auto *terminal : window.findChildren<rad::client::TerminalView *>())
                QObject::disconnect(terminal, nullptr, terminal->parent(), nullptr);
        };
        doubleClick();
        QCOMPARE(tabs->count(), 1);
        auto *first = tabs->widget(0);
        for (int i = 0; i < 5; ++i)
            doubleClick();
        window.m_openSelected->click();
        window.m_openSelected->click();
        QCOMPARE(tabs->count(), 1);
        window.m_newTerminal->click();
        QCOMPARE(tabs->count(), 2);
        doubleClick();
        QCOMPARE(tabs->currentIndex(), 1);
        tabs->setCurrentWidget(first);
        doubleClick();
        QCOMPARE(tabs->currentWidget(), first);
        QCOMPARE(tabs->count(), 2);
        first->setProperty("sessionClosing", true);
        doubleClick();
        QCOMPARE(tabs->currentIndex(), 1);
        tabs->widget(1)->setProperty("sessionPending", false);
        doubleClick();
        QCOMPARE(tabs->count(), 3);
        auto second = window.m_state.projects.projects.front();
        second.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        window.m_state.projects.projects.append(second);
        bindProject(window, second);
        window.renderProjects();
        window.m_openSelected->click();
        window.m_openSelected->click();
        QCOMPARE(tabs->count(), 4);
        doubleClick();
        window.m_closing = true;
        doubleClick();
        QCOMPARE(tabs->count(), 4);
    }

    void activeSessionIsReusedEvenAfterNonfatalError()
    {
        QTemporaryDir directory;
        sshworkbench::WorkbenchWindow window(directory.filePath(QStringLiteral("profile.json")),
                                             directory.filePath(QStringLiteral("projects.json")));
        launcher::Project project{
            QStringLiteral("项目"), {}, true, QUuid::createUuid().toString(QUuid::WithoutBraces)};
        bindProject(window, project);
        window.openProject(project);
        auto *page = window.m_tabs->widget(0);
        auto *terminal = page->findChild<rad::client::TerminalView *>();
        QObject::disconnect(terminal, nullptr, page, nullptr);
        page->setProperty("sessionPending", false);
        page->setProperty("sessionError", QStringLiteral("无效尺寸"));
        auto *session = page->findChild<sshworkbench::SshSession *>();
        sshsetup::ConnectionProfile invalid;
        invalid.host = QStringLiteral("-invalid");
        session->start(invalid, {});
        QVERIFY(session->isActive());
        window.openProject(project);
        QCOMPARE(window.m_tabs->count(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(!session->isActive(), 5000);
    }

    void projectIdentityAndConnectionSnapshotPreventWrongReuse()
    {
        QTemporaryDir directory;
        sshworkbench::WorkbenchWindow window(directory.filePath(QStringLiteral("profile.json")),
                                             directory.filePath(QStringLiteral("projects.json")));
        launcher::Project project{QStringLiteral("项目"), QStringLiteral("D:/One"), true,
                                  QUuid::createUuid().toString(QUuid::WithoutBraces)};
        auto open = [&](const launcher::Project &value)
        {
            bindProject(window, value);
            window.openProject(value);
            for (auto *terminal : window.findChildren<rad::client::TerminalView *>())
                QObject::disconnect(terminal, nullptr, terminal->parent(), nullptr);
        };
        open(project);
        project.name = QStringLiteral("重命名");
        open(project);
        QCOMPARE(window.m_tabs->count(), 1);
        auto other = project;
        other.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        open(other);
        QCOMPARE(window.m_tabs->count(), 2);
        project.directory = QStringLiteral("D:/Two");
        open(project);
        QCOMPARE(window.m_tabs->count(), 3);
        window.m_state.hosts[0].connection.host = QStringLiteral("other.invalid");
        open(project);
        QCOMPARE(window.m_tabs->count(), 4);
        window.m_state.hosts[0].connection.user = QStringLiteral("other");
        open(project);
        window.m_state.hosts[0].connection.port = 2222;
        open(project);
        window.m_state.hosts[0].connection.identityFile = QStringLiteral("C:/key");
        open(project);
        window.m_state.hosts[0].connection.hostFingerprint = QStringLiteral("changed");
        open(project);
        QCOMPARE(window.m_tabs->count(), 8);
        open(project);
        QCOMPARE(window.m_tabs->count(), 8);
        window.m_tabs->currentWidget()->setProperty("sessionError", QStringLiteral("failed"));
        open(project);
        QCOMPARE(window.m_tabs->count(), 8);
        window.m_tabs->currentWidget()->setProperty("terminalFailed", true);
        open(project);
        QCOMPARE(window.m_tabs->count(), 9);
    }

    void setupUpdatesCapturedHostInsteadOfCurrentSelection()
    {
        QTemporaryDir directory;
        sshworkbench::WorkbenchWindow window(directory.filePath(QStringLiteral("profile.json")),
                                             directory.filePath(QStringLiteral("projects.json")));
        sshsetup::ConnectionProfile first;
        first.host = QStringLiteral("first.invalid");
        const auto firstId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QVERIFY(window.applySetupProfile(firstId, first, true));
        auto second = first;
        second.host = QStringLiteral("second.invalid");
        const auto secondId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QVERIFY(window.applySetupProfile(secondId, second, true));
        window.m_state.currentHostId = secondId;
        first.host = QStringLiteral("updated.invalid");
        QVERIFY(window.applySetupProfile(firstId, first, false));
        QCOMPARE(window.m_state.currentHostId, secondId);
        QCOMPARE(sshworkbench::findHost(window.m_state, secondId)->connection.host, second.host);
        QCOMPARE(sshworkbench::findHost(window.m_state, firstId)->connection.host, first.host);
        QVERIFY(!window.applySetupProfile(QStringLiteral("removed"), first, false));
        QCOMPARE(window.m_state.hosts.size(), 2);
    }

    void switchingHostsKeepsProjectBindingsAndExistingTabs()
    {
        QTemporaryDir directory;
        const auto profilePath = directory.filePath(QStringLiteral("profile.json"));
        const auto projectsPath = directory.filePath(QStringLiteral("projects.json"));
        sshworkbench::WorkbenchWindow window(profilePath, projectsPath);
        launcher::Project first{QStringLiteral("同名项目"), QStringLiteral("D:/Same"), true,
                                QUuid::createUuid().toString(QUuid::WithoutBraces)};
        bindProject(window, first);
        auto second = first;
        second.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        auto secondHost = window.m_state.hosts.front();
        secondHost.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        secondHost.name = QStringLiteral("另一台");
        secondHost.connection.host = QStringLiteral("second.invalid");
        window.m_state.hosts.append(secondHost);
        window.m_state.projects.projects = {first, second};
        window.m_state.projectHosts.insert(second.id, secondHost.id);
        QVERIFY(window.saveState(window.m_state));
        window.renderHosts();
        window.reloadProfile();
        window.renderProjects();
        const auto screenshot = qEnvironmentVariable("ORBIDEV_WORKBENCH_SCREENSHOT");
        if (!screenshot.isEmpty())
        {
            window.show();
            QTest::qWait(100);
            QVERIFY(window.grab().save(screenshot));
        }
        QCOMPARE(window.m_projects->count(), 1);
        window.openCurrent();
        QCOMPARE(window.m_tabs->count(), 1);
        auto *firstTab = window.m_tabs->widget(0);
        const auto originalTarget = firstTab->property("sessionTarget").toStringList();
        for (auto *terminal : window.findChildren<rad::client::TerminalView *>())
            QObject::disconnect(terminal, nullptr, terminal->parent(), nullptr);
        window.m_hostSelector->setCurrentIndex(1);
        QCOMPARE(window.m_projects->count(), 1);
        QCOMPARE(window.projectRow(), 1);
        window.openSelected();
        QCOMPARE(window.m_tabs->count(), 2);
        QCOMPARE(firstTab->property("sessionTarget").toStringList().join('|'), originalTarget.join('|'));
        for (auto *terminal : window.findChildren<rad::client::TerminalView *>())
            QObject::disconnect(terminal, nullptr, terminal->parent(), nullptr);
        window.removeHost();
        QCOMPARE(window.m_state.hosts.size(), 2);
        sshworkbench::WorkbenchWindow reloaded(profilePath, projectsPath);
        QCOMPARE(reloaded.m_state.hosts.size(), 2);
        QCOMPARE(reloaded.m_state.currentHostId, secondHost.id);
        QCOMPARE(reloaded.projectRow(), 1);
        window.m_hostSelector->setCurrentIndex(0);
        window.openCurrent();
        QCOMPARE(window.m_tabs->count(), 2);
        QCOMPARE(window.m_tabs->currentWidget(), firstTab);
        auto changed = window.m_state;
        changed.hosts[0].connection.host = QStringLiteral("changed.invalid");
        QVERIFY(window.saveState(changed));
        window.openCurrent();
        QCOMPARE(window.m_tabs->count(), 3);
        QCOMPARE(firstTab->property("sessionTarget").toStringList().join('|'), originalTarget.join('|'));
        for (auto *terminal : window.findChildren<rad::client::TerminalView *>())
            QObject::disconnect(terminal, nullptr, terminal->parent(), nullptr);
        window.m_tabs->setCurrentIndex(1);
        QCOMPARE(window.m_state.currentHostId, secondHost.id);
        QCOMPARE(window.projectRow(), 1);
        window.m_tabs->setCurrentWidget(firstTab);
        QCOMPARE(window.m_state.currentHostId, window.m_state.projectHosts.value(first.id));
        QCOMPARE(window.projectRow(), 0);
    }

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
        const auto profilePath = directory.filePath(QStringLiteral("profile.json"));
        sshsetup::ConnectionProfile profile;
        profile.host = QStringLiteral("test.invalid");
        QVERIFY(sshsetup::saveProfile(profilePath, profile, error));
        sshworkbench::WorkbenchWindow window(profilePath, projects, legacy);
        auto *list = window.findChild<QListWidget *>(QStringLiteral("projectList"));
        QCOMPARE(list->count(), 2);
        QCOMPARE(list->item(0)->checkState(), Qt::Checked);
        QCOMPARE(list->item(1)->checkState(), Qt::Unchecked);
        QVERIFY(!QFile::exists(projects));
        list->item(1)->setCheckState(Qt::Checked);
        sshworkbench::WorkbenchState state;
        QVERIFY2(sshworkbench::loadWorkbenchState(projects + QStringLiteral(".workbench.json"), state, error),
                 qPrintable(error));
        const auto &saved = state.projects;
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
int main(int argc, char **argv)
{
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS",
            qgetenv("QTWEBENGINE_CHROMIUM_FLAGS") + " --disable-background-timer-throttling");
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    // 使用正式工作台的主题，并在创建控件前设置，避免截图期间切换原生样式。
    sshsetup::applySetupTheme(application);
    WorkbenchWindowTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "WorkbenchWindowTests.moc"
