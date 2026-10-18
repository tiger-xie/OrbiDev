#pragma once

#include "EnvironmentService.h"
#include "LauncherCore.h"
#include <QMainWindow>

class WorkbenchWindowTests;
class QLabel;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QTabWidget;

namespace sshworkbench
{
// 窗口只编排项目编辑与会话；所有方法均在界面线程调用。
class WorkbenchWindow final : public QMainWindow
{
    Q_OBJECT
public:
    explicit WorkbenchWindow(const QString &profilePath, const QString &projectsPath,
                             const QString &legacyPath = {}, bool preview = false, QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    friend class ::WorkbenchWindowTests;
    void buildUi();
    void loadProjects(const QString &legacyPath);
    bool reloadProfile();
    void renderProjects();
    void editProject(bool add);
    void removeProject();
    bool saveProjects(const launcher::Configuration &configuration);
    void openSelected();
    void openCurrent(bool createNew = false);
    void openProject(const launcher::Project &project, bool createNew = false);
    void closeTerminal(int index);
    void finishTerminalClose(QWidget *page);
    void openSetup();
    void showFeedback(const QString &message, bool error = false);
    void updateWorkspace();

    QString m_profilePath;
    QString m_projectsPath;
    launcher::Configuration m_configuration;
    sshsetup::ConnectionProfile m_profile;
    quint64 m_activationSerial = 0;
    bool m_preview = false;
    bool m_closing = false;
    bool m_projectsReadable = true;
    bool m_projectsPersisted = false;
    bool m_profileReady = false;
    bool m_renderingProjects = false;
    QLabel *m_host = nullptr;
    QLabel *m_feedback = nullptr;
    QLabel *m_summary = nullptr;
    QListWidget *m_projects = nullptr;
    QTabWidget *m_tabs = nullptr;
    QStackedWidget *m_workspace = nullptr;
    QPushButton *m_openSelected = nullptr;
    QPushButton *m_newTerminal = nullptr;
};
}
