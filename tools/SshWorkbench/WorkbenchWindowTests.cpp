#include "SetupTheme.h"
#include "SshSession.h"
#include "WorkbenchWindow.h"
#include "terminal/TerminalView.h"
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QTabBar>
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
    void hostPopupOpensBelowSelector()
    {
        QTemporaryDir directory;
        sshworkbench::WorkbenchWindow window(directory.filePath(QStringLiteral("profile.json")),
                                             directory.filePath(QStringLiteral("projects.json")), {}, true);
        window.resize(920, 600);
        window.show();
        auto *selector = window.findChild<QComboBox *>(QStringLiteral("hostSelector"));
        QVERIFY(selector);
        selector->setEnabled(true);
        {
            const QSignalBlocker blocker(selector);
            selector->clear();
            selector->addItems({QStringLiteral("家庭笔记本"), QStringLiteral("家庭台式机")});
            selector->setCurrentIndex(1);
        }
        QTest::qWait(100);
        selector->showPopup();
        auto *popup = selector->view()->window();
        QTRY_VERIFY(popup->isVisible());
        QTest::qWait(100);
        QCOMPARE(selector->count(), 2);
        QVERIFY(selector->view()->viewport()->rect().contains(
            selector->view()->visualRect(selector->model()->index(0, 0))));
        const int bottom = selector->mapToGlobal(QPoint(0, selector->height())).y();
        QVERIFY(popup->geometry().top() >= bottom - 1);
        QVERIFY(popup->geometry().top() <= bottom + 2);
        const auto screenshot = qEnvironmentVariable("ORBIDEV_HOST_POPUP_SCREENSHOT");
        if (!screenshot.isEmpty())
        {
            QPixmap capture = window.grab();
            QPainter painter(&capture);
            painter.drawPixmap(window.mapFromGlobal(popup->pos()), popup->grab());
            painter.end();
            QVERIFY(capture.save(screenshot));
        }
        QTest::keyClick(selector->view(), Qt::Key_Escape);
        QTRY_VERIFY(!popup->isVisible());
        QCOMPARE(selector->currentIndex(), 1);
        QVERIFY(window.close());
    }
    void settingsPopupStaysInsideWindowAndBelowButton()
    {
        QTemporaryDir directory;
        sshworkbench::WorkbenchWindow window(directory.filePath(QStringLiteral("profile.json")),
                                             directory.filePath(QStringLiteral("projects.json")), {}, true);
        window.showPreviewTabs();
        window.resize(920, 600);
        window.show();
        QTest::qWait(100);
        auto *settings = window.findChild<QPushButton *>(QStringLiteral("connectionSettingsButton"));
        auto *menu = settings->findChild<QMenu *>();
        QVERIFY(menu);
        QVERIFY(!window.m_tabs->tabBar()->drawBase());
        settings->setEnabled(true);
        settings->click();
        QTRY_VERIFY(menu->isVisible());
        const QRect bounds(window.mapToGlobal(QPoint(0, 0)), window.size());
        QVERIFY(bounds.contains(menu->geometry()));
        QVERIFY(menu->geometry().top() >= settings->mapToGlobal(QPoint(0, settings->height())).y());
        QVERIFY(menu->geometry().right() <= settings->mapToGlobal(QPoint(settings->width(), 0)).x());
        const auto screenshot = qEnvironmentVariable("ORBIDEV_UI05_MENU_SCREENSHOT");
        if (!screenshot.isEmpty())
        {
            // 弹出菜单是独立窗口；组合两个真实控件的抓图以记录相对位置。
            QPixmap capture = window.grab();
            QPainter painter(&capture);
            painter.drawPixmap(window.mapFromGlobal(menu->pos()), menu->grab());
            painter.end();
            QVERIFY(capture.save(screenshot));
        }
        QTest::keyClick(menu, Qt::Key_Escape);
        QTRY_VERIFY(!menu->isVisible());
        QVERIFY(window.findChildren<sshworkbench::SshSession *>().isEmpty());
        QVERIFY(window.close());
        QVERIFY(QDir(directory.path()).entryList(QDir::Files).isEmpty());
    }

    void longProjectPreviewFitsNarrowWindow()
    {
        QTemporaryDir directory;
        sshworkbench::WorkbenchWindow window(directory.filePath(QStringLiteral("profile.json")),
                                             directory.filePath(QStringLiteral("projects.json")), {}, true);
        window.m_state.projects.projects[0].name = QString(80, QChar(0x9879));
        window.m_state.projects.projects[0].directory =
            QStringLiteral("D:/") + QString(1000, QLatin1Char('x'));
        window.renderProjects();
        window.showPreviewTabs();
        window.m_tabs->currentWidget()->setProperty("hostName", QString(80, QChar(0x5bb6)));
        window.updateWorkspace();
        window.resize(920, 600);
        window.show();
        QTest::qWait(100);
        QVERIFY(window.centralWidget()->width() <= window.width());
        QVERIFY(window.m_projects->width() < window.width() / 2);
        QCOMPARE(window.m_projects->horizontalScrollBar()->maximum(), 0);
        auto *location =
            window.m_tabs->currentWidget()->findChild<QLabel *>(QStringLiteral("terminalLocation"));
        QVERIFY(location);
        QVERIFY(location->width() < window.width());
        QVERIFY(location->toolTip().contains(window.m_state.projects.projects[0].directory));
        const auto screenshot = qEnvironmentVariable("ORBIDEV_UI05_NARROW_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(window.grab().save(screenshot));
        QVERIFY(window.close());
        QCOMPARE(QDir(directory.path()).entryList(QDir::Files).size(), 0);
    }

    void projectActionsStayLocalAndActiveTargetIsIndependent()
    {
        QTemporaryDir directory;
        sshworkbench::WorkbenchWindow window(directory.filePath(QStringLiteral("profile.json")),
                                             directory.filePath(QStringLiteral("projects.json")));
        auto *edit = window.findChild<QAction *>(QStringLiteral("editProjectAction"));
        auto *remove = window.findChild<QAction *>(QStringLiteral("removeProjectAction"));
        QVERIFY(edit);
        QVERIFY(remove);
        QCOMPARE(edit->shortcutContext(), Qt::WidgetShortcut);
        QCOMPARE(remove->shortcutContext(), Qt::WidgetShortcut);
        QVERIFY(window.m_projects->actions().contains(edit));
        QVERIFY(window.m_projects->actions().contains(remove));
        launcher::Project project{QStringLiteral("项目"), QStringLiteral("D:/Original"), true,
                                  QUuid::createUuid().toString(QUuid::WithoutBraces)};
        bindProject(window, project);
        window.m_state.projects.projects = {project};
        window.renderProjects();
        auto *page = openPending(window, project);
        auto *path = page->findChild<QLabel *>(QStringLiteral("terminalLocation"));
        QVERIFY(path);
        QVERIFY(path->text().contains(QStringLiteral("D:/Original")));
        auto other = window.m_state.hosts.front();
        other.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        other.name = QStringLiteral("另一台家庭电脑");
        window.m_state.hosts.append(other);
        window.m_state.currentHostId = other.id;
        window.renderHosts();
        window.reloadProfile();
        window.renderProjects();
        QCOMPARE(window.m_tabs->currentWidget(), page);
        QVERIFY(window.m_host->text().contains(QStringLiteral("测试机")));
        QVERIFY(!window.m_host->text().contains(other.name));
        QCOMPARE(window.m_hostSelector->currentText(), other.name);
        window.m_state.projects.projects[0].directory = QStringLiteral("D:/Changed");
        window.updateWorkspace();
        QVERIFY(path->text().contains(QStringLiteral("D:/Original")));
    }

    void previewTabsHaveNoSessionsOrConfigurationWrites()
    {
        QTemporaryDir directory;
        sshworkbench::WorkbenchWindow window(directory.filePath(QStringLiteral("profile.json")),
                                             directory.filePath(QStringLiteral("projects.json")), {}, true);
        window.showPreviewTabs();
        QCOMPARE(window.m_tabs->count(), 2);
        QVERIFY(window.findChildren<sshworkbench::SshSession *>().isEmpty());
        QVERIFY(window.findChildren<rad::client::TerminalView *>().isEmpty());
        QVERIFY(!window.m_editProject->isEnabled());
        window.showPreviewTabs();
        QCOMPARE(window.m_tabs->count(), 2);
        window.closeTerminal(1);
        window.closeTerminal(0);
        QVERIFY(window.m_host->text().contains(QStringLiteral("尚未打开终端")));
        QVERIFY(window.close());
        QCOMPARE(QDir(directory.path()).entryList(QDir::Files).size(), 0);
    }

    void freshConfigurationRestartStaysDisconnected()
    {
        QTemporaryDir directory;
        const auto profile = directory.filePath(QStringLiteral("connection.json"));
        const auto projects = directory.filePath(QStringLiteral("projects.json"));
        {
            sshworkbench::WorkbenchWindow firstRun(profile, projects);
            QVERIFY(firstRun.m_state.hosts.isEmpty());
            QVERIFY(firstRun.m_state.projects.projects.isEmpty());
            QVERIFY(firstRun.close());
        }
        sshworkbench::WorkbenchWindow secondRun(profile, projects);
        QCOMPARE(secondRun.m_tabs->count(), 0);
        QVERIFY(secondRun.findChildren<sshworkbench::SshSession *>().isEmpty());
        QVERIFY(!secondRun.m_newTerminal->isEnabled());
        QVERIFY(!QFile::exists(profile));
        QVERIFY(!QFile::exists(secondRun.m_statePath));
        QVERIFY(QFile::exists(projects + QStringLiteral(".preferences.json")));
        QVERIFY(secondRun.close());
    }

    void maximizedPreferenceKeepsNormalGeometry()
    {
        QTemporaryDir directory;
        const auto profile = directory.filePath(QStringLiteral("profile.json"));
        const auto projects = directory.filePath(QStringLiteral("projects.json"));
        sshworkbench::WorkbenchWindow window(profile, projects);
        window.show();
        window.fitWindowToScreens(QRect(60, 60, 1000, 650));
        const QRect normal = window.geometry();
        window.showMaximized();
        QTRY_VERIFY(window.isMaximized());
        QVERIFY(window.close());
        QFile saved(window.m_preferencesPath);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        const auto settings = QJsonDocument::fromJson(saved.readAll()).object();
        saved.close();
        QVERIFY(settings.value(QStringLiteral("maximized")).toBool());
        QCOMPARE(settings.value(QStringLiteral("width")).toInt(), normal.width());
        QCOMPARE(settings.value(QStringLiteral("height")).toInt(), normal.height());
        sshworkbench::WorkbenchWindow restored(profile, projects);
        QVERIFY(restored.isMaximized());
        restored.showNormal();
        QTRY_COMPARE(restored.size(), normal.size());
        QVERIFY(restored.close());
    }

    void failedPreferenceWriteIsReportedBeforeExit()
    {
        QTemporaryDir directory;
        sshworkbench::WorkbenchWindow window(directory.filePath(QStringLiteral("profile.json")),
                                             directory.filePath(QStringLiteral("projects.json")));
        QVERIFY(QDir().mkdir(window.m_preferencesPath));
        bool reported = false;
        QTimer::singleShot(0, &window,
                           [&reported]
                           {
                               for (auto *widget : QApplication::topLevelWidgets())
                                   if (auto *message = qobject_cast<QMessageBox *>(widget))
                                   {
                                       reported = message->windowTitle() == QStringLiteral("偏好未保存");
                                       message->accept();
                                   }
                           });
        QVERIFY(window.close());
        QVERIFY(reported);
        QVERIFY(QFileInfo(window.m_preferencesPath).isDir());
    }

    void preferencesRestoreWithoutStartingSessions()
    {
        QTemporaryDir directory;
        const auto profile = directory.filePath(QStringLiteral("profile.json"));
        const auto projects = directory.filePath(QStringLiteral("projects.json"));
        sshworkbench::WorkbenchWindow window(profile, projects);
        launcher::Project first{
            QStringLiteral("同名"), {}, true, QUuid::createUuid().toString(QUuid::WithoutBraces)};
        auto second = first;
        second.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        bindProject(window, first);
        bindProject(window, second);
        window.m_state.projects.projects = {first, second};
        QVERIFY(window.saveState(window.m_state));
        window.renderProjects();
        window.m_projects->setCurrentRow(1);
        window.setTerminalFontSize(21);
        window.resize(1100, 700);
        window.fitWindowToScreens(window.geometry());
        window.m_splitter->setSizes({320, 780});
        QVERIFY(window.close());
        sshworkbench::WorkbenchWindow restored(profile, projects);
        QCOMPARE(restored.m_terminalFontSize, 21);
        QCOMPARE(restored.projectRow(), 1);
        QCOMPARE(restored.m_tabs->count(), 0);
        QVERIFY(restored.findChildren<sshworkbench::SshSession *>().isEmpty());
        QCOMPARE(restored.size(), window.size());
        QCOMPARE(restored.m_splitter->sizes().front(), window.m_splitter->sizes().front());
    }

    void corruptPreferencesArePreservedAndPreviewDoesNotWrite()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("projects.json.preferences.json"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{broken");
        file.close();
        sshworkbench::WorkbenchWindow window(directory.filePath(QStringLiteral("profile.json")),
                                             directory.filePath(QStringLiteral("projects.json")));
        QVERIFY(!window.m_preferencesWritable);
        window.setTerminalFontSize(22);
        QVERIFY(window.close());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("{broken"));
        file.close();
        sshworkbench::WorkbenchWindow preview(directory.filePath(QStringLiteral("profile.json")),
                                              directory.filePath(QStringLiteral("projects.json")), {}, true);
        QCOMPARE(preview.m_terminalFontSize, 15);
        QVERIFY(preview.close());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("{broken"));
    }

    void offscreenPreferencesAreClampedAndUnknownVersionPreserved()
    {
        QTemporaryDir directory;
        const auto profile = directory.filePath(QStringLiteral("profile.json"));
        const auto projects = directory.filePath(QStringLiteral("projects.json"));
        sshworkbench::WorkbenchWindow initial(profile, projects);
        QVERIFY(initial.close());
        QFile file(projects + QStringLiteral(".preferences.json"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        auto settings = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        settings.insert(QStringLiteral("x"), 90000);
        settings.insert(QStringLiteral("y"), 90000);
        settings.insert(QStringLiteral("width"), 20000);
        settings.insert(QStringLiteral("height"), 20000);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(settings).toJson());
        file.close();
        sshworkbench::WorkbenchWindow restored(profile, projects);
        QVERIFY(QGuiApplication::primaryScreen()->availableGeometry().contains(restored.geometry()));
        settings.insert(QStringLiteral("version"), 99);
        const auto bytes = QJsonDocument(settings).toJson();
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(bytes);
        file.close();
        sshworkbench::WorkbenchWindow unknown(profile, projects);
        QVERIFY(!unknown.m_preferencesWritable);
        QVERIFY(unknown.close());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), bytes);
    }

    void globalFontAppliesToExistingAndNewTerminalsAndCloseCancelDoesNotSave()
    {
        QTemporaryDir directory;
        sshworkbench::WorkbenchWindow window(directory.filePath(QStringLiteral("profile.json")),
                                             directory.filePath(QStringLiteral("projects.json")));
        QVERIFY(window.savePreferences());
        QFile file(window.m_preferencesPath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto original = file.readAll();
        file.close();
        launcher::Project project{
            QStringLiteral("项目"), {}, true, QUuid::createUuid().toString(QUuid::WithoutBraces)};
        bindProject(window, project);
        auto *first = openPending(window, project);
        window.setTerminalFontSize(25);
        auto *second = openPending(window, project, true);
        QCOMPARE(first->findChild<rad::client::TerminalView *>()->terminalFontSize(), 25);
        QCOMPARE(second->findChild<rad::client::TerminalView *>()->terminalFontSize(), 25);
        answerClosePrompt(QMessageBox::No);
        QVERIFY(!window.close());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), original);
    }

    void recentProjectsAreRememberedSeparatelyPerHost()
    {
        QTemporaryDir directory;
        const auto profile = directory.filePath(QStringLiteral("profile.json"));
        const auto projects = directory.filePath(QStringLiteral("projects.json"));
        sshworkbench::WorkbenchWindow window(profile, projects);
        launcher::Project first{
            QStringLiteral("同名"), {}, true, QUuid::createUuid().toString(QUuid::WithoutBraces)};
        auto second = first;
        second.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        auto third = first;
        third.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        bindProject(window, first);
        bindProject(window, second);
        auto otherHost = window.m_state.hosts.front();
        otherHost.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        window.m_state.hosts.append(otherHost);
        window.m_state.projectHosts.insert(third.id, otherHost.id);
        window.m_state.projects.projects = {first, second, third};
        QVERIFY(window.saveState(window.m_state));
        window.renderHosts();
        window.renderProjects();
        window.m_projects->setCurrentRow(1);
        window.m_hostSelector->setCurrentIndex(1);
        QCOMPARE(window.projectRow(), 2);
        window.m_hostSelector->setCurrentIndex(0);
        QCOMPARE(window.projectRow(), 1);
        QVERIFY(window.close());
        sshworkbench::WorkbenchWindow restored(profile, projects);
        QCOMPARE(restored.projectRow(), 1);
        restored.m_hostSelector->setCurrentIndex(1);
        QCOMPARE(restored.projectRow(), 2);
        QCOMPARE(restored.m_tabs->count(), 0);
    }

    void projectDialogSaveAndOpen_data()
    {
        QTest::addColumn<bool>("add");
        QTest::addColumn<bool>("open");
        QTest::addColumn<bool>("fail");
        QTest::addColumn<bool>("cancel");
        QTest::newRow("create-only") << true << false << false << false;
        QTest::newRow("create-open") << true << true << false << false;
        QTest::newRow("save-fails") << true << true << true << false;
        QTest::newRow("cancel") << true << true << false << true;
        QTest::newRow("edit-only") << false << false << false << false;
    }

    void projectDialogSaveAndOpen()
    {
        QFETCH(bool, add);
        QFETCH(bool, open);
        QFETCH(bool, fail);
        QFETCH(bool, cancel);
        QTemporaryDir directory;
        sshworkbench::WorkbenchWindow window(directory.filePath(QStringLiteral("profile.json")),
                                             directory.filePath(QStringLiteral("projects.json")));
        launcher::Project project{QStringLiteral("原项目"), QStringLiteral("D:/Original"), true,
                                  QUuid::createUuid().toString(QUuid::WithoutBraces)};
        bindProject(window, project);
        window.m_state.projects.projects = {project};
        auto second = window.m_state.hosts.front();
        second.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        second.name = QStringLiteral("家庭笔记本（测试）");
        window.m_state.hosts.append(second);
        QVERIFY(window.saveState(window.m_state));
        window.renderHosts();
        window.renderProjects();
        if (fail)
            window.m_statePath = directory.path();
        window.show();
        QTimer::singleShot(
            50, &window,
            [&]
            {
                auto *dialog = window.findChild<QDialog *>(QStringLiteral("workbenchDialog"));
                QVERIFY(dialog);
                auto *name = dialog->findChild<QLineEdit *>(QStringLiteral("projectName"));
                auto *path = dialog->findChild<QLineEdit *>(QStringLiteral("projectDirectory"));
                auto *hosts = dialog->findChild<QComboBox *>(QStringLiteral("projectHostChoice"));
                auto *openOption = dialog->findChild<QCheckBox *>(QStringLiteral("openAfterSave"));
                QVERIFY(name && path && hosts);
                if (add)
                {
                    QVERIFY(openOption);
                    QVERIFY(!openOption->isChecked());
                    openOption->setChecked(open);
                }
                else
                    QVERIFY(!openOption);
                name->setText(QStringLiteral("文档工具"));
                path->setText(QStringLiteral("D:/Projects/DocsTools"));
                hosts->setCurrentIndex(hosts->findData(second.id));
                const auto screenshot = qEnvironmentVariable("ORBIDEV_UI06_PROJECT_SCREENSHOT");
                if (add && !open && !screenshot.isEmpty())
                    QVERIFY(dialog->grab().save(screenshot));
                auto *buttons = dialog->findChild<QDialogButtonBox *>();
                if (cancel)
                    buttons->button(QDialogButtonBox::Cancel)->click();
                else
                    buttons->button(QDialogButtonBox::Save)->click();
                if (fail)
                {
                    QVERIFY(dialog->isVisible());
                    QCOMPARE(name->text(), QStringLiteral("文档工具"));
                    QVERIFY(!dialog->findChild<QLabel *>(QStringLiteral("dialogError"))->text().isEmpty());
                    QCOMPARE(window.m_tabs->count(), 0);
                    dialog->reject();
                }
            });
        window.editProject(add);
        const bool opened = open && !fail && !cancel;
        QCOMPARE(window.m_tabs->count(), opened ? 1 : 0);
        if (opened)
        {
            auto *page = window.m_tabs->currentWidget();
            auto *terminal = page->findChild<rad::client::TerminalView *>();
            QObject::disconnect(terminal, nullptr, page, nullptr);
            QCOMPARE(page->property("hostId").toString(), second.id);
            QVERIFY(!page->findChild<sshworkbench::SshSession *>()->isActive());
        }
        QCOMPARE(window.m_state.projects.projects.size(), add && !fail && !cancel ? 2 : 1);
        if (!fail && !cancel)
            QCOMPARE(window.m_state.projectHosts.value(window.m_state.projects.projects.back().id),
                     second.id);
    }

    void projectDialogLongFieldsFit()
    {
        QTemporaryDir directory;
        sshworkbench::WorkbenchWindow window(directory.filePath(QStringLiteral("profile.json")),
                                             directory.filePath(QStringLiteral("projects.json")));
        launcher::Project project{
            QStringLiteral("项目"), {}, true, QUuid::createUuid().toString(QUuid::WithoutBraces)};
        bindProject(window, project);
        window.m_state.hosts.front().name = QStringLiteral("家庭开发电脑与长期项目验证环境").repeated(4);
        QTimer::singleShot(
            50, &window,
            [&]
            {
                auto *dialog = window.findChild<QDialog *>(QStringLiteral("workbenchDialog"));
                QVERIFY(dialog);
                dialog->resize(480, 440);
                auto *name = dialog->findChild<QLineEdit *>(QStringLiteral("projectName"));
                auto *path = dialog->findChild<QLineEdit *>(QStringLiteral("projectDirectory"));
                name->setText(QStringLiteral("很长的中文项目名称").repeated(8));
                path->setText(QStringLiteral("D:/") + QStringLiteral("long-folder/").repeated(60));
                QTest::qWait(50);
                QCOMPARE(dialog->size(), QSize(480, 440));
                auto *scroll = dialog->findChild<QScrollArea *>(QStringLiteral("dialogScroll"));
                QVERIFY(scroll);
                QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
                QVERIFY(scroll->verticalScrollBar()->maximum() > 0);
                auto *footer = dialog->findChild<QWidget *>(QStringLiteral("dialogFooter"));
                QVERIFY(dialog->rect().contains(QRect(footer->mapTo(dialog, QPoint()), footer->size())));
                QVERIFY(path->width() < dialog->width());
                const auto screenshot = qEnvironmentVariable("ORBIDEV_UI06_PROJECT_SCREENSHOT");
                if (!screenshot.isEmpty())
                    QVERIFY(dialog->grab().save(screenshot + QStringLiteral(".narrow.png")));
                QTest::keyClick(dialog, Qt::Key_Escape);
            });
        window.editProject(true);
        QCOMPARE(window.m_tabs->count(), 0);
        QVERIFY(!QFileInfo::exists(window.m_statePath));
    }

    void hostDialogSaveFailureCanRetry()
    {
        QTemporaryDir directory;
        sshworkbench::WorkbenchWindow window(directory.filePath(QStringLiteral("profile.json")),
                                             directory.filePath(QStringLiteral("projects.json")));
        launcher::Project project{
            QStringLiteral("项目"), {}, true, QUuid::createUuid().toString(QUuid::WithoutBraces)};
        bindProject(window, project);
        window.m_state.projects.projects = {project};
        QVERIFY(window.saveState(window.m_state));
        const QString validPath = window.m_statePath;
        window.m_statePath = directory.path();
        QTimer::singleShot(50, &window,
                           [&]
                           {
                               auto *dialog = window.findChild<QDialog *>(QStringLiteral("workbenchDialog"));
                               QVERIFY(dialog);
                               auto *name = dialog->findChild<QLineEdit *>(QStringLiteral("hostNameEdit"));
                               QVERIFY(name);
                               name->setText(QStringLiteral("更新后的家庭主机"));
                               auto *save =
                                   dialog->findChild<QPushButton *>(QStringLiteral("hostSaveButton"));
                               save->click();
                               QVERIFY(dialog->isVisible());
                               QCOMPARE(window.m_state.hosts.front().name, QStringLiteral("测试机"));
                               QCOMPARE(name->text(), QStringLiteral("更新后的家庭主机"));
                               window.m_statePath = validPath;
                               save->click();
                           });
        window.editHost(false);
        QCOMPARE(window.m_state.hosts.size(), 1);
        QCOMPARE(window.m_state.hosts.front().name, QStringLiteral("更新后的家庭主机"));
        QCOMPARE(window.m_tabs->count(), 0);
    }

    void appearanceSaveFailureKeepsInput()
    {
        QTemporaryDir directory;
        sshworkbench::WorkbenchWindow window(directory.filePath(QStringLiteral("profile.json")),
                                             directory.filePath(QStringLiteral("projects.json")));
        window.m_preferencesPath = directory.path();
        QTimer::singleShot(
            50, &window,
            [&]
            {
                auto *dialog = window.findChild<QDialog *>(QStringLiteral("appearanceDialog"));
                QVERIFY(dialog);
                auto *font = dialog->findChild<QSpinBox *>(QStringLiteral("terminalFontSize"));
                font->setValue(24);
                dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
                QVERIFY(dialog->isVisible());
                QCOMPARE(font->value(), 24);
                QCOMPARE(window.m_terminalFontSize, 15);
                QVERIFY(!dialog->findChild<QLabel *>(QStringLiteral("dialogError"))->text().isEmpty());
                dialog->reject();
            });
        window.showAppearanceSettings();
        QCOMPARE(window.m_terminalFontSize, 15);
    }

    void appearanceCancelAndConfirm()
    {
        QTemporaryDir directory;
        sshworkbench::WorkbenchWindow window(directory.filePath(QStringLiteral("profile.json")),
                                             directory.filePath(QStringLiteral("projects.json")));
        window.show();
        for (bool accept : {false, true})
        {
            QTimer::singleShot(
                50, &window,
                [accept]
                {
                    for (auto *widget : QApplication::topLevelWidgets())
                        if (auto *dialog = qobject_cast<QDialog *>(widget))
                            if (dialog->objectName() == QStringLiteral("appearanceDialog"))
                            {
                                auto *font =
                                    dialog->findChild<QSpinBox *>(QStringLiteral("terminalFontSize"));
                                QVERIFY(font);
                                font->setValue(23);
                                const auto screenshot =
                                    qEnvironmentVariable("ORBIDEV_UI06_APPEARANCE_SCREENSHOT");
                                if (!screenshot.isEmpty())
                                    QVERIFY(dialog->grab().save(screenshot));
                                dialog->findChild<QDialogButtonBox *>()
                                    ->button(accept ? QDialogButtonBox::Save : QDialogButtonBox::Cancel)
                                    ->click();
                            }
                });
            window.showAppearanceSettings();
            QCOMPARE(window.m_terminalFontSize, accept ? 23 : 15);
        }
        QVERIFY(window.close());
    }

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

    void statusBannerVisualStates()
    {
        const auto outputDirectory = qEnvironmentVariable("ORBIDEV_UI07_SCREENSHOT");
        if (outputDirectory.isEmpty())
            QSKIP("仅在指定截图目录时运行真实控件视觉场景");
        QVERIFY(QDir().mkpath(outputDirectory));
        QTemporaryDir directory;
        sshworkbench::WorkbenchWindow window(directory.filePath(QStringLiteral("profile.json")),
                                             directory.filePath(QStringLiteral("projects.json")));
        launcher::Project project{QStringLiteral("产品开发（预览）"), QStringLiteral("D:/Projects/Product"),
                                  true, QUuid::createUuid().toString(QUuid::WithoutBraces)};
        bindProject(window, project);
        window.m_state.hosts.front().name = QStringLiteral("家庭电脑（预览）");
        window.m_state.projects.projects = {project};
        window.renderHosts();
        window.renderProjects();
        auto *page = openPending(window, project);
        auto *session = page->findChild<sshworkbench::SshSession *>();
        auto *terminal = page->findChild<rad::client::TerminalView *>();
        QSignalSpy ready(terminal, &rad::client::TerminalView::ready);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty(), 20000);
        terminal->appendOutput(
            QStringLiteral("[界面预览，未启动 SSH]\r\n历史终端输出保留在这里。\r\n").toUtf8());
        const auto capture = [&](const QString &name)
        {
            QTest::qWait(150);
            QVERIFY(window.grab().save(outputDirectory + QLatin1Char('/') + name + QStringLiteral(".png")));
        };
        capture(QStringLiteral("preparing"));
        session->hostVerificationStarted();
        capture(QStringLiteral("verifying"));
        session->started();
        capture(QStringLiteral("running"));
        QCOMPARE(page->findChild<QScrollArea *>(QStringLiteral("terminalStatusDetails"))
                     ->verticalScrollBar()
                     ->maximum(),
                 0);
        session->exited(0);
        capture(QStringLiteral("exited"));
        session->errorOccurred(QStringLiteral("主机校验未完成。请检查连接配置与家庭电脑当前状态。"));
        session->exited(255);
        capture(QStringLiteral("failed"));
        session->errorOccurred(
            QStringLiteral("主机校验未完成。请检查连接配置与家庭电脑当前状态。") +
            QStringLiteral("此处为较长诊断信息，用于检查自动换行和滚动，不代表真实连接结果。").repeated(8));
        session->exited(255);
        capture(QStringLiteral("failed-long"));
        window.resize(920, 600);
        capture(QStringLiteral("failed-narrow"));
        auto *details = page->findChild<QScrollArea *>(QStringLiteral("terminalStatusDetails"));
        QCOMPARE(details->horizontalScrollBar()->maximum(), 0);
        QVERIFY(details->verticalScrollBar()->maximum() > 0);
        QVERIFY(!session->isActive());
    }

    void bannerStatesAndHostSettingsAreAccurate()
    {
        QTemporaryDir directory;
        sshworkbench::WorkbenchWindow window(directory.filePath(QStringLiteral("profile.json")),
                                             directory.filePath(QStringLiteral("projects.json")));
        launcher::Project project{QStringLiteral("项目"), QStringLiteral("D:/Original"), true,
                                  QUuid::createUuid().toString(QUuid::WithoutBraces)};
        bindProject(window, project);
        window.m_state.projects.projects = {project};
        const auto originalHost = window.m_state.hosts.front();
        auto other = originalHost;
        other.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        other.name = QStringLiteral("另一主机");
        window.m_state.hosts.append(other);
        auto *page = openPending(window, project);
        auto *session = page->findChild<sshworkbench::SshSession *>();
        auto *title = page->findChild<QLabel *>(QStringLiteral("terminalStatusTitle"));
        QVERIFY(title);
        QCOMPARE(title->text(), QStringLiteral("正在准备终端"));
        session->hostVerificationStarted();
        QCOMPARE(title->text(), QStringLiteral("正在校验主机"));
        session->started();
        QCOMPARE(title->text(), QStringLiteral("SSH 进程运行中"));
        session->exited(255);
        QCOMPARE(title->text(), QStringLiteral("SSH 已退出（退出码 255）"));
        QVERIFY(!page->findChild<QLabel *>(QStringLiteral("terminalStatus"))
                     ->text()
                     .contains(QStringLiteral("认证失败")));
        window.m_state.currentHostId = other.id;
        auto *settings = page->findChild<QPushButton *>(QStringLiteral("terminalSettingsButton"));
        QVERIFY(settings && !settings->isHidden());
        QTimer::singleShot(50, &window,
                           [&]
                           {
                               auto *dialog = window.findChild<QDialog *>(QStringLiteral("workbenchDialog"));
                               QVERIFY(dialog);
                               QCOMPARE(
                                   dialog->findChild<QLineEdit *>(QStringLiteral("hostNameEdit"))->text(),
                                   originalHost.name);
                               dialog->reject();
                           });
        settings->click();
        QCOMPARE(window.m_state.currentHostId, other.id);
        window.m_state.hosts.removeFirst();
        settings->click();
        QVERIFY(window.m_feedback->text().contains(QStringLiteral("不存在")));
        QCOMPARE(window.m_tabs->widget(0), page);
        page->setProperty("terminalFailed", true);
        session->exited(0);
        QCOMPARE(title->text(), QStringLiteral("终端不可用"));
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
        window.m_tabs->setCurrentWidget(page);
        reconnect->click();
        QCOMPARE(window.m_tabs->count(), 2);
        QCOMPARE(window.m_tabs->currentWidget(), replacement);
        replacement->findChild<sshworkbench::SshSession *>()->exited(0);
        window.m_tabs->setCurrentWidget(page);
        answerClosePrompt(QMessageBox::Yes);
        reconnect->click();
        QCOMPARE(window.m_tabs->count(), 3);
        auto *third = window.m_tabs->currentWidget();
        QObject::disconnect(third->findChild<rad::client::TerminalView *>(), nullptr, third, nullptr);
        QCOMPARE(third->property("sessionTarget").toStringList(), original);
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
