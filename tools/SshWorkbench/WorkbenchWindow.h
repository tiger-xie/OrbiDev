#pragma once

#include "EnvironmentService.h"
#include "WorkbenchStore.h"
#include <QMainWindow>

class WorkbenchWindowTests;
class QAction;
class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QTabWidget;
class QSplitter;

namespace sshworkbench
{
// 窗口只编排项目编辑与会话；所有方法均在界面线程调用。
class WorkbenchWindow final : public QMainWindow
{
    Q_OBJECT
public:
    explicit WorkbenchWindow(const QString &profilePath, const QString &projectsPath,
                             const QString &legacyPath = {}, bool preview = false, QWidget *parent = nullptr);

    // 仅用于无副作用截图，不创建终端后端或读取连接凭据。
    void showPreviewTabs();

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    friend class ::WorkbenchWindowTests;
    void buildUi();
    void loadPreferences();
    void fitWindowToScreens(QRect restored);
    bool savePreferences(int fontSize = -1);
    void showAppearanceSettings();
    void setTerminalFontSize(int size);
    void renderHosts();
    void editHost(bool add, const QString &hostId = {});
    void removeHost();
    bool saveState(const WorkbenchState &state);
    int projectRow() const;
    void loadProjects(const QString &legacyPath);
    bool reloadProfile();
    void renderProjects();
    void editProject(bool add);
    void removeProject();
    bool saveProjects(const launcher::Configuration &configuration);
    void openSelected();
    void openCurrent(bool createNew = false);
    void openProject(const launcher::Project &project, bool createNew = false);
    void createTerminal(launcher::Project project, HostProfile host);
    void selectTerminalProject(QWidget *page);
    void updateProjectLabels();
    void updateTerminalLabel(QWidget *page);
    void updateTerminalBanner(QWidget *page);
    void closeTerminal(int index);
    void finishTerminalClose(QWidget *page);
    void openSetup();
    bool applySetupProfile(const QString &hostId, const sshsetup::ConnectionProfile &profile, bool adding);
    void showFeedback(const QString &message, bool error = false);
    void updateWorkspace();
    void updateActiveSession();

    QString m_preferencesPath;
    bool m_preferencesWritable = true;
    int m_terminalFontSize = 15;
    QMap<QString, QString> m_recentProjects;
    QSplitter *m_splitter = nullptr;
    QAction *m_editProject = nullptr;
    QAction *m_removeProject = nullptr;
    QLabel *m_emptyHint = nullptr;
    QString m_profilePath;
    QString m_projectsPath;
    WorkbenchState m_state;
    QString m_statePath;
    QString m_setupHostId;
    bool m_setupRunning = false;
    QComboBox *m_hostSelector = nullptr;
    sshsetup::ConnectionProfile m_profile;
    quint64 m_activationSerial = 0;
    quint64 m_terminalSerial = 0;
    bool m_closePromptActive = false;
    bool m_preview = false;
    bool m_closing = false;
    bool m_projectsReadable = true;

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
