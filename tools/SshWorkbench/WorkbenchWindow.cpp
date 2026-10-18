#include "WorkbenchWindow.h"
#include "HostDialog.h"
#include "SshSession.h"
#include "terminal/TerminalView.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <memory>

namespace sshworkbench
{
WorkbenchWindow::WorkbenchWindow(const QString &profilePath, const QString &projectsPath,
                                 const QString &legacyPath, bool preview, QWidget *parent)
    : QMainWindow(parent), m_profilePath(profilePath), m_projectsPath(projectsPath), m_preview(preview)
{
    setWindowTitle(tr("OrbiDev · SSH 工作台"));
    resize(1280, 820);
    setMinimumSize(920, 600);
    m_statePath = m_projectsPath + QStringLiteral(".workbench.json");
    buildUi();
    if (m_preview)
    {
        m_state.projects.projects = {{tr("产品开发"), QStringLiteral("D:/Projects/Product"), true},
                                     {tr("日常工具"), QStringLiteral("D:/Projects/Tools"), false}};
        m_host->setText(tr("家庭电脑  ·  界面预览"));
        showFeedback(tr("界面预览：未读取身份，未启动 SSH，会话不会自动打开。"));
    }
    else
    {
        loadProjects(legacyPath);
        renderHosts();
        reloadProfile();
    }
    renderProjects();
    updateWorkspace();
}

void WorkbenchWindow::buildUi()
{
    auto *central = new QWidget(this);
    central->setObjectName(QStringLiteral("workbench"));
    central->setStyleSheet(QStringLiteral(R"(
        QWidget#workbench { background: #20262c; }
        QWidget#projectRail { background: #242d35; border-right: 1px solid #36414a; }
        QListWidget { background: transparent; border: none; outline: none; }
        QListWidget::item { padding: 12px 8px; margin: 2px 0; border-left: 3px solid transparent; }
        QListWidget::item:selected { background: #29454a; border-left-color: #38c4bb; }
        QListWidget::item:hover { background: #2c3942; }
        QTabWidget::pane { border: none; background: #181c20; }
        QTabBar::tab { background: #252d34; padding: 12px 18px; border-bottom: 2px solid transparent; }
        QTabBar::tab:selected { background: #181c20; border-bottom-color: #38c4bb; }
        QTabBar::tab:hover { background: #303e48; }
        QPushButton[role="compact"] { padding: 7px 10px; }
        QLabel#workspaceEmptyTitle { font-size: 22px; font-weight: 600; }
    )"));
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    auto *toolbar = new QHBoxLayout;
    toolbar->setContentsMargins(22, 16, 22, 16);
    m_host = new QLabel(tr("尚未配置家庭电脑"), central);
    m_host->setObjectName(QStringLiteral("hostSummary"));
    m_host->setTextFormat(Qt::PlainText);
    m_hostSelector = new QComboBox(central);
    m_hostSelector->setObjectName(QStringLiteral("hostSelector"));
    m_hostSelector->setMinimumWidth(180);
    m_hostSelector->setMaximumWidth(280);
    m_hostSelector->setEnabled(!m_preview);
    toolbar->addWidget(m_hostSelector);
    toolbar->addWidget(m_host, 1);
    connect(m_hostSelector, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int index)
            {
                if (index < 0 || m_preview)
                    return;
                auto next = m_state;
                next.currentHostId = m_hostSelector->itemData(index).toString();
                if (!saveState(next))
                    renderHosts();
                reloadProfile();
                renderProjects();
            });
    auto *settings = new QPushButton(tr("连接设置…"), central);
    settings->setObjectName(QStringLiteral("connectionSettingsButton"));
    auto *hostMenu = new QMenu(settings);
    hostMenu->addAction(tr("添加家庭电脑…"), this, [this] { editHost(true); });
    hostMenu->addAction(tr("编辑当前主机／测试连接…"), this, [this] { editHost(false); });
    hostMenu->addAction(tr("首次配置向导（当前主机）…"), this, &WorkbenchWindow::openSetup);
    hostMenu->addSeparator();
    hostMenu->addAction(tr("移除当前主机…"), this, &WorkbenchWindow::removeHost);
    settings->setMenu(hostMenu);
    toolbar->addWidget(settings);
    for (int delta : {-1, 1})
    {
        auto *zoom = new QPushButton(delta < 0 ? tr("A−") : tr("A+"), central);
        zoom->setProperty("role", "compact");
        zoom->setToolTip(delta < 0 ? tr("缩小终端文字（Ctrl+-）") : tr("放大终端文字（Ctrl+=）"));
        zoom->setEnabled(!m_preview);
        const auto applyZoom = [this, delta]
        {
            QWidget *page = m_tabs->currentWidget();
            if (page)
                if (auto *terminal = page->findChild<rad::client::TerminalView *>())
                    terminal->zoomBy(delta);
        };
        connect(zoom, &QPushButton::clicked, this, applyZoom);
        auto *shortcut = new QShortcut(
            QKeySequence(delta < 0 ? QStringLiteral("Ctrl+-") : QStringLiteral("Ctrl+=")), this);
        connect(shortcut, &QShortcut::activated, this, applyZoom);
        toolbar->addWidget(zoom);
    }
    m_newTerminal = new QPushButton(tr("新建终端"), central);
    m_newTerminal->setObjectName(QStringLiteral("newTerminalButton"));
    m_newTerminal->setProperty("role", "primary");
    connect(m_newTerminal, &QPushButton::clicked, this, [this] { openCurrent(true); });
    m_newTerminal->setToolTip(tr("为选中项目额外创建一个 SSH 会话"));
    toolbar->addWidget(m_newTerminal);
    root->addLayout(toolbar);
    m_feedback = new QLabel(central);
    m_feedback->setObjectName(QStringLiteral("feedback"));
    m_feedback->setTextFormat(Qt::PlainText);
    m_feedback->setWordWrap(true);
    m_feedback->hide();
    root->addWidget(m_feedback);
    auto *splitter = new QSplitter(central);
    auto *rail = new QWidget(splitter);
    rail->setObjectName(QStringLiteral("projectRail"));
    rail->setMinimumWidth(230);
    auto *railLayout = new QVBoxLayout(rail);
    railLayout->setContentsMargins(12, 20, 12, 16);
    auto *title = new QLabel(tr("项目"), rail);
    title->setProperty("role", "section");
    railLayout->addWidget(title);
    auto *hint = new QLabel(tr("双击切换已有终端；勾选后可批量打开"), rail);
    hint->setProperty("role", "muted");
    hint->setWordWrap(true);
    railLayout->addWidget(hint);
    m_projects = new QListWidget(rail);
    m_projects->setObjectName(QStringLiteral("projectList"));
    m_projects->setWordWrap(true);
    railLayout->addWidget(m_projects, 1);
    connect(m_projects, &QListWidget::itemChanged, this,
            [this](QListWidgetItem *item)
            {
                if (m_renderingProjects || m_preview)
                    return;
                auto next = m_state.projects;
                next.projects[item->data(Qt::UserRole).toInt()].enabled = item->checkState() == Qt::Checked;
                if (!saveProjects(next))
                    renderProjects();
            });
    connect(m_projects, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *) { openCurrent(); });
    auto *actions = new QHBoxLayout;
    auto *add = new QPushButton(tr("添加"), rail);
    add->setObjectName(QStringLiteral("addProjectButton"));
    auto *edit = new QPushButton(tr("编辑"), rail);
    auto *remove = new QPushButton(tr("移除"), rail);
    for (auto *button : {add, edit, remove})
    {
        button->setProperty("role", "compact");
        button->setEnabled(!m_preview);
        actions->addWidget(button);
    }
    connect(add, &QPushButton::clicked, this, [this] { editProject(true); });
    connect(edit, &QPushButton::clicked, this, [this] { editProject(false); });
    connect(remove, &QPushButton::clicked, this, &WorkbenchWindow::removeProject);
    railLayout->addLayout(actions);
    m_openSelected = new QPushButton(tr("打开勾选项目"), rail);
    m_openSelected->setObjectName(QStringLiteral("openSelectedButton"));
    connect(m_openSelected, &QPushButton::clicked, this, &WorkbenchWindow::openSelected);
    railLayout->addWidget(m_openSelected);
    m_workspace = new QStackedWidget(splitter);
    auto *empty = new QWidget(m_workspace);
    auto *emptyLayout = new QVBoxLayout(empty);
    emptyLayout->setContentsMargins(40, 40, 40, 40);
    emptyLayout->addStretch();
    auto *emptyTitle = new QLabel(tr("从一个项目开始"), empty);
    emptyTitle->setObjectName(QStringLiteral("workspaceEmptyTitle"));
    emptyTitle->setAlignment(Qt::AlignCenter);
    emptyLayout->addWidget(emptyTitle);
    auto *emptyHint = new QLabel(
        tr("添加家庭电脑上的项目目录，再打开终端。\n每个标签页是独立的 SSH 会话，Claude 在家庭电脑上运行。"),
        empty);
    emptyHint->setAlignment(Qt::AlignCenter);
    emptyHint->setWordWrap(true);
    emptyHint->setProperty("role", "muted");
    emptyLayout->addWidget(emptyHint);
    emptyLayout->addStretch();
    m_workspace->addWidget(empty);
    m_tabs = new QTabWidget(m_workspace);
    m_tabs->setObjectName(QStringLiteral("terminalTabs"));
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    connect(m_tabs, &QTabWidget::currentChanged, this,
            [this](int index)
            {
                if (auto *page = m_tabs->widget(index))
                    page->setProperty("lastActivated", QVariant::fromValue(++m_activationSerial));
            });
    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &WorkbenchWindow::closeTerminal);
    m_workspace->addWidget(m_tabs);
    splitter->addWidget(rail);
    splitter->addWidget(m_workspace);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({280, 1000});
    root->addWidget(splitter, 1);
    m_summary = new QLabel(tr("没有活动终端"), this);
    statusBar()->addWidget(m_summary);
    statusBar()->addPermanentWidget(new QLabel(tr("关闭客户端将断开 SSH · 不保证任务继续运行"), this));
    setCentralWidget(central);
    settings->setEnabled(!m_preview);
}

void WorkbenchWindow::renderHosts()
{
    const QSignalBlocker blocker(m_hostSelector);
    m_hostSelector->clear();
    for (const auto &host : m_state.hosts)
        m_hostSelector->addItem(host.name, host.id);
    m_hostSelector->setCurrentIndex(m_hostSelector->findData(m_state.currentHostId));
}

bool WorkbenchWindow::reloadProfile()
{
    const auto *host = findHost(m_state, m_state.currentHostId);
    m_profileReady = host && m_projectsReadable;
    if (m_profileReady)
    {
        m_profile = host->connection;
        m_host->setText(tr("%1@%2:%3").arg(m_profile.user, m_profile.host).arg(m_profile.port));
        m_host->setToolTip(tr("已配置，实际连接状态请查看各终端"));
    }
    else
        m_host->setText(tr("请先添加或配置家庭电脑"));
    m_openSelected->setEnabled(m_profileReady && !m_preview);
    m_newTerminal->setEnabled(m_profileReady && !m_preview);
    return m_profileReady;
}

void WorkbenchWindow::loadProjects(const QString &legacyPath)
{
    QString error;
    m_projectsReadable =
        loadOrMigrateWorkbenchState(m_statePath, m_profilePath, m_projectsPath, legacyPath, m_state, error);
    if (!m_projectsReadable)
        showFeedback(tr("配置未加载，原文件保留：%1").arg(error) +
                         (!QFileInfo::exists(m_profilePath) ? tr(" 若旧项目缺少连接，请先运行首次设置工具完成"
                                                                 "连接配置，再重新打开工作台。连接文件：%1")
                                                                  .arg(m_profilePath)
                                                            : QString()),
                     true);
}

int WorkbenchWindow::projectRow() const
{
    const auto *item = m_projects->currentItem();
    return item ? item->data(Qt::UserRole).toInt() : -1;
}

bool WorkbenchWindow::saveState(const WorkbenchState &state)
{
    if (m_preview || !m_projectsReadable)
    {
        showFeedback(tr("配置不可写，请先处理读取错误；原文件保留。"), true);
        return false;
    }
    QString error;
    if (!saveWorkbenchState(m_statePath, state, error))
    {
        showFeedback(error, true);
        return false;
    }
    m_state = state;
    return true;
}

void WorkbenchWindow::editHost(bool add)
{
    if (m_preview || !m_projectsReadable)
        return;
    HostProfile host;
    const auto *current = findHost(m_state, m_state.currentHostId);
    if (!add && m_setupRunning && current && current->id == m_setupHostId)
    {
        showFeedback(tr("当前主机的首次配置向导仍在运行，请完成或关闭向导后再编辑。"), true);
        return;
    }
    if (!add && !current)
    {
        showFeedback(tr("请先添加家庭电脑。"));
        return;
    }
    if (add)
    {
        host.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (current)
            host.connection.identityFile = current->connection.identityFile;
    }
    else
        host = *current;
    HostDialog dialog(host, this);
    dialog.setWindowTitle(add ? tr("添加家庭主机") : tr("编辑家庭主机"));
    if (dialog.exec() != QDialog::Accepted)
        return;
    auto next = m_state;
    if (add)
        next.hosts.append(dialog.host());
    else
        for (auto &entry : next.hosts)
            if (entry.id == host.id)
                entry = dialog.host();
    next.currentHostId = host.id;
    if (saveState(next))
    {
        renderHosts();
        reloadProfile();
        renderProjects();
        showFeedback(tr("主机已保存；已有终端继续使用创建时的连接配置。"));
    }
}

void WorkbenchWindow::removeHost()
{
    const auto *host = findHost(m_state, m_state.currentHostId);
    if (m_preview || !host)
        return;
    const QString hostId = host->id;
    const QString hostName = host->name;
    const auto removable = [this, hostId]
    {
        QSet<QString> references;
        QStringList labels;
        for (const auto &project : m_state.projects.projects)
            if (m_state.projectHosts.value(project.id) == hostId)
                labels.append(tr("项目：%1").arg(project.name));
        for (int i = 0; i < m_tabs->count(); ++i)
            if (m_tabs->widget(i)->property("hostId").toString() == hostId)
            {
                references.insert(hostId);
                labels.append(tr("标签：%1").arg(m_tabs->tabText(i)));
            }
        if (m_setupRunning && m_setupHostId == hostId)
        {
            references.insert(hostId);
            labels.append(tr("首次配置向导"));
        }
        QString error;
        if (canRemoveHost(m_state, hostId, references, error))
            return true;
        showFeedback(error + QLatin1Char(' ') + labels.join(QStringLiteral("；")), true);
        return false;
    };
    if (!removable())
        return;
    if (QMessageBox::question(this, tr("移除主机"),
                              tr("从本机档案移除“%1”？不会修改家庭电脑或删除密钥。").arg(hostName),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    // 确认框期间向导和会话仍会处理事件，按原 ID 重新检查引用。
    if (!removable())
        return;
    auto next = m_state;
    for (int i = 0; i < next.hosts.size(); ++i)
        if (next.hosts[i].id == hostId)
        {
            next.hosts.removeAt(i);
            break;
        }
    if (next.currentHostId == hostId)
        next.currentHostId = next.hosts.isEmpty() ? QString() : next.hosts.front().id;
    if (saveState(next))
    {
        renderHosts();
        reloadProfile();
        renderProjects();
    }
}

void WorkbenchWindow::renderProjects()
{
    m_renderingProjects = true;
    const int previous = m_projects->currentRow();
    m_projects->clear();
    for (int row = 0; row < m_state.projects.projects.size(); ++row)
    {
        const auto &project = m_state.projects.projects[row];
        if (!m_preview && m_state.projectHosts.value(project.id) != m_state.currentHostId)
            continue;
        auto *item =
            new QListWidgetItem(project.name + QLatin1Char('\n') +
                                    (project.directory.isEmpty() ? tr("家庭用户目录") : project.directory),
                                m_projects);
        item->setData(Qt::UserRole, row);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable |
                       (m_preview ? Qt::NoItemFlags : Qt::ItemIsUserCheckable));
        item->setCheckState(project.enabled ? Qt::Checked : Qt::Unchecked);
        item->setToolTip(project.directory);
    }
    if (m_projects->count() > 0)
        m_projects->setCurrentRow(qBound(0, previous, m_projects->count() - 1));
    m_renderingProjects = false;
}

bool WorkbenchWindow::saveProjects(const launcher::Configuration &configuration)
{
    auto next = m_state;
    next.projects = configuration;
    QMap<QString, QString> bindings;
    for (const auto &project : configuration.projects)
        bindings.insert(project.id, next.projectHosts.value(project.id, next.currentHostId));
    next.projectHosts = bindings;
    return saveState(next);
}

void WorkbenchWindow::editProject(bool add)
{
    if (m_preview || !reloadProfile())
        return;
    const int row = projectRow();
    if (!add && row < 0)
        return;
    QDialog dialog(this);
    dialog.setWindowTitle(add ? tr("添加项目") : tr("编辑项目"));
    dialog.resize(560, 220);
    auto *layout = new QFormLayout(&dialog);
    auto *name = new QLineEdit(&dialog);
    name->setMaxLength(80);
    auto *directory = new QLineEdit(&dialog);
    directory->setPlaceholderText(tr("D:\\Projects\\项目名称；留空使用家庭用户目录"));
    directory->setMaxLength(1024);
    if (!add)
    {
        name->setText(m_state.projects.projects[row].name);
        directory->setText(m_state.projects.projects[row].directory);
    }
    layout->addRow(tr("项目名称"), name);
    layout->addRow(tr("远程目录"), directory);
    auto *hostChoice = new QComboBox(&dialog);
    hostChoice->setObjectName(QStringLiteral("projectHostChoice"));
    for (const auto &host : m_state.hosts)
        hostChoice->addItem(host.name, host.id);
    hostChoice->setCurrentIndex(hostChoice->findData(
        add ? m_state.currentHostId : m_state.projectHosts.value(m_state.projects.projects[row].id)));
    layout->addRow(tr("所属主机"), hostChoice);
    auto *errorLabel = new QLabel(&dialog);
    errorLabel->setWordWrap(true);
    errorLabel->setTextFormat(Qt::PlainText);
    layout->addRow(errorLabel);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Save)->setText(tr("保存"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog,
            [&, this]
            {
                auto next = m_state.projects;
                launcher::Project project{name->text().trimmed(), directory->text().trimmed(), true};
                if (add)
                {
                    project.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
                    next.projects.append(project);
                }
                else
                {
                    project.enabled = next.projects[row].enabled;
                    project.id = next.projects[row].id;
                    next.projects[row] = project;
                }
                QString error;
                if (!launcher::validateConfiguration(next, error))
                {
                    errorLabel->setText(error);
                    return;
                }
                auto state = m_state;
                state.projects = next;
                state.projectHosts.insert(project.id, hostChoice->currentData().toString());
                if (saveState(state))
                    dialog.accept();
                else
                    errorLabel->setText(m_feedback->text());
            });
    if (dialog.exec() == QDialog::Accepted)
        renderProjects();
}

void WorkbenchWindow::removeProject()
{
    const int row = projectRow();
    if (m_preview || row < 0)
        return;
    if (QMessageBox::question(this, tr("移除项目"),
                              tr("只从项目列表移除，不会删除远程文件，也不会关闭已有终端。"),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    auto next = m_state.projects;
    next.projects.removeAt(row);
    if (saveProjects(next))
        renderProjects();
}

void WorkbenchWindow::openCurrent(bool createNew)
{
    if (m_preview || m_closing || !reloadProfile())
        return;
    const int row = projectRow();
    if (row < 0)
    {
        showFeedback(tr("请先添加并选择一个项目。"));
        return;
    }

    openProject(m_state.projects.projects[row], createNew);
}

void WorkbenchWindow::openSelected()
{
    if (m_preview || m_closing || !reloadProfile())
        return;

    bool selected = false;
    for (const auto &project : m_state.projects.projects)
        if (project.enabled && m_state.projectHosts.value(project.id) == m_state.currentHostId)
        {
            selected = true;
            openProject(project);
        }
    if (!selected)
        showFeedback(tr("请先勾选需要打开的项目。"));
}

void WorkbenchWindow::openProject(const launcher::Project &project, bool createNew)
{
    if (m_closing || m_preview)
        return;
    const auto hostId = m_state.projectHosts.value(project.id);
    const auto *host = findHost(m_state, hostId);
    if (!host)
    {
        showFeedback(tr("项目所属主机不存在，请编辑项目重新选择。"), true);
        return;
    }
    const auto profile = host->connection;
    // 保存创建时的目标快照；改名不改变身份，改目录或连接则不能复用旧会话。
    const QStringList target{hostId,
                             project.id,
                             project.directory,
                             profile.host,
                             profile.user,
                             QString::number(profile.port),
                             profile.identityFile,
                             profile.hostFingerprint};
    QWidget *existing = nullptr;
    quint64 latest = 0;
    if (!createNew && !project.id.isEmpty())
        for (int i = 0; i < m_tabs->count(); ++i)
        {
            auto *candidate = m_tabs->widget(i);
            auto *session = candidate->findChild<SshSession *>();
            const auto previousTarget = candidate->property("sessionTarget").toStringList();
            if (candidate->property("sessionClosing").toBool() ||
                candidate->property("terminalFailed").toBool() ||
                !std::equal(previousTarget.cbegin(), previousTarget.cend(), target.cbegin(), target.cend()) ||
                !(candidate->property("sessionPending").toBool() || (session && session->isActive())))
                continue;
            const auto activated = candidate->property("lastActivated").toULongLong();
            if (!existing || activated > latest)
            {
                existing = candidate;
                latest = activated;
            }
        }
    if (existing)
    {
        m_tabs->setCurrentWidget(existing);
        existing->setProperty("lastActivated", QVariant::fromValue(++m_activationSerial));
        if (auto *terminal = existing->findChild<rad::client::TerminalView *>())
            terminal->focusTerminal();
        return;
    }
    if (m_tabs->count() >= 16)
    {
        showFeedback(tr("最多同时保留 16 个终端标签，请先关闭不再使用的标签。"), true);
        return;
    }
    auto *page = new QWidget(m_tabs);
    page->setProperty("hostId", hostId);
    page->setProperty("sessionTarget", target);
    page->setProperty("sessionPending", true);
    page->setProperty("columns", 120);
    page->setProperty("rows", 32);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *status = new QLabel(tr("正在准备终端…"), page);
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    status->setContentsMargins(16, 10, 16, 10);
    layout->addWidget(status);
    auto *terminal = new rad::client::TerminalView(page);
    layout->addWidget(terminal, 1);
    auto *session = new SshSession(page);
    const QString name = project.name;
    const QString directory = project.directory;
    const int index = m_tabs->addTab(page, name + tr(" · 准备中"));
    m_tabs->setTabToolTip(index, tr("%1@%2\n%3").arg(profile.user, profile.host, directory));
    m_tabs->setCurrentIndex(index);
    connect(terminal, &rad::client::TerminalView::ready, page,
            [page, terminal, session, profile, directory]
            {
                if (page->property("sessionClosing").toBool())
                    return;
                page->setProperty("sessionPending", false);
                terminal->setInputEnabled(true);
                session->start(profile, directory, page->property("columns").toInt(),
                               page->property("rows").toInt());
                terminal->focusTerminal();
            });
    connect(terminal, &rad::client::TerminalView::inputReady, session, &SshSession::writeInput);
    connect(terminal, &rad::client::TerminalView::terminalResized, page,
            [page, session](int columns, int rows)
            {
                page->setProperty("columns", columns);
                page->setProperty("rows", rows);
                session->resize(columns, rows);
            });
    connect(session, &SshSession::outputReady, terminal,
            [terminal](const QByteArray &bytes) { terminal->appendOutput(bytes); });
    connect(session, &SshSession::started, page,
            [this, page, status, name]
            {
                status->setText(tr("SSH 进程已启动 · 正在连接；认证与 Claude 状态请查看终端。"));
                m_tabs->setTabText(m_tabs->indexOf(page), name + tr(" · SSH 运行中"));
            });
    connect(session, &SshSession::errorOccurred, page,
            [this, page, status, name](const QString &error)
            {
                status->setText(error);
                page->setProperty("sessionError", error);
                m_tabs->setTabText(m_tabs->indexOf(page), name + tr(" · 出错"));
            });
    connect(session, &SshSession::exited, page,
            [this, page, terminal, status, name](int code)
            {
                terminal->setInputEnabled(false);
                const QString error = page->property("sessionError").toString();
                status->setText((error.isEmpty() ? QString() : error + QLatin1Char('\n')) +
                                tr("SSH 已退出（%1）。再次打开项目将创建新会话，不会恢复原进程。").arg(code));
                m_tabs->setTabText(m_tabs->indexOf(page), name + tr(" · 已退出"));
                if (page->property("sessionClosing").toBool())
                    finishTerminalClose(page);
                updateWorkspace();
                if (m_closing)
                    QTimer::singleShot(0, this, [this] { close(); });
            });
    connect(terminal, &rad::client::TerminalView::failed, page,
            [this, page, status, session, name](const QString &error)
            {
                page->setProperty("sessionPending", false);
                page->setProperty("terminalFailed", true);
                session->stop();
                status->setText(error);
                page->setProperty("sessionError", error);
                m_tabs->setTabText(m_tabs->indexOf(page), name + tr(" · 终端出错"));
            });
    updateWorkspace();
}

void WorkbenchWindow::closeTerminal(int index)
{
    QWidget *page = m_tabs->widget(index);
    if (!page || page->property("sessionClosing").toBool())
        return;
    auto *session = page->findChild<SshSession *>();
    if ((page->property("sessionPending").toBool() || (session && session->isActive())) &&
        QMessageBox::warning(this, tr("关闭终端"),
                             tr("将断开此 SSH 会话，无法保证远程任务继续运行。确定关闭？"),
                             QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    page->setProperty("sessionClosing", true);
    page->setEnabled(false);
    if (session && session->isActive())
    {
        m_tabs->setTabText(index, tr("正在断开…"));
        session->stop();
        return;
    }
    finishTerminalClose(page);
}

void WorkbenchWindow::finishTerminalClose(QWidget *page)
{
    const int index = m_tabs->indexOf(page);
    if (index < 0)
        return;
    m_tabs->removeTab(index);
    page->deleteLater();
    updateWorkspace();
}

void WorkbenchWindow::closeEvent(QCloseEvent *event)
{
    if (m_setupRunning)
    {
        showFeedback(tr("首次配置向导仍在运行，请完成或关闭向导后再退出工作台。"), true);
        event->ignore();
        return;
    }
    bool active = false;
    bool pending = false;
    for (int i = 0; i < m_tabs->count(); ++i)
    {
        auto *page = m_tabs->widget(i);
        auto *session = page->findChild<SshSession *>();
        pending = pending || page->property("sessionPending").toBool();
        active = active || (session && session->isActive());
    }
    if (!m_closing && (active || pending) &&
        QMessageBox::warning(this, tr("退出工作台"),
                             tr("将断开所有 SSH 会话，无法保证远程任务继续运行。确定退出？"),
                             QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
    {
        event->ignore();
        return;
    }
    m_closing = true;
    active = false;
    for (int i = 0; i < m_tabs->count(); ++i)
    {
        m_tabs->widget(i)->setProperty("sessionClosing", true);
        // 确认对话框期间仍会处理事件，需要重新检查刚启动的会话。
        auto *session = m_tabs->widget(i)->findChild<SshSession *>();
        active = active || (session && session->isActive());
    }
    if (!active)
    {
        event->accept();
        return;
    }
    // 等待退出事件后再销毁会话，避免在界面线程等待后台主机校验。
    centralWidget()->setEnabled(false);
    showFeedback(tr("正在断开 SSH，请稍候…"));
    for (auto *session : findChildren<SshSession *>())
        if (session->isActive())
            session->stop();
    event->ignore();
}
void WorkbenchWindow::openSetup()
{
    if (m_preview || !m_projectsReadable || m_setupRunning)
        return;
    const QString executable = QCoreApplication::applicationDirPath() + QStringLiteral("/OrbiDevSetup.exe");
    if (!QFileInfo::exists(executable))
    {
        showFeedback(tr("未找到首次设置工具，请将 OrbiDevSetup.exe 与工作台放在同一目录。"), true);
        return;
    }
    auto temporary = std::make_shared<QTemporaryDir>();
    if (!temporary->isValid())
    {
        showFeedback(tr("无法创建向导临时配置目录。"), true);
        return;
    }
    const QString output = temporary->filePath(QStringLiteral("connection.json"));
    const auto *host = findHost(m_state, m_state.currentHostId);
    const QString hostId = host ? host->id : QUuid::createUuid().toString(QUuid::WithoutBraces);
    const bool adding = !host;
    QString error;
    if (host && !sshsetup::saveProfile(output, host->connection, error))
    {
        showFeedback(error, true);
        return;
    }
    QFile original(output);
    QByteArray originalBytes;
    if (original.open(QIODevice::ReadOnly))
        originalBytes = original.readAll();
    original.close();
    auto *process = new QProcess(this);
    m_setupRunning = true;
    m_setupHostId = hostId;
    connect(process, &QProcess::errorOccurred, this,
            [this, process, temporary](QProcess::ProcessError)
            {
                m_setupRunning = false;
                showFeedback(tr("无法运行首次设置：%1").arg(process->errorString()), true);
                process->deleteLater();
            });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, process, temporary, output, hostId, adding, originalBytes](int code,
                                                                              QProcess::ExitStatus status)
            {
                m_setupRunning = false;
                process->deleteLater();
                if (code != 0 || status != QProcess::NormalExit || !QFileInfo::exists(output))
                    return;
                QFile result(output);
                if (!result.open(QIODevice::ReadOnly))
                {
                    showFeedback(tr("无法读取向导输出，未更新主机。"), true);
                    return;
                }
                if (result.readAll() == originalBytes)
                    return;
                result.close();
                sshsetup::ConnectionProfile profile;
                QString error;
                if (!sshsetup::loadProfile(output, profile, error))
                {
                    showFeedback(error, true);
                    return;
                }
                if (applySetupProfile(hostId, profile, adding))
                {
                    renderHosts();
                    reloadProfile();
                    renderProjects();
                    showFeedback(tr("已更新向导对应的主机，其他主机和已有终端保持原配置。"));
                }
            });
    process->start(executable, {QStringLiteral("--config"), output});
}

bool WorkbenchWindow::applySetupProfile(const QString &hostId, const sshsetup::ConnectionProfile &profile,
                                        bool adding)
{
    auto next = m_state;
    bool found = false;
    for (auto &entry : next.hosts)
        if (entry.id == hostId)
        {
            entry.connection = profile;
            found = true;
        }
    if (adding && !found)
        next.hosts.append({hostId, profile.host, profile});
    else if (!found || adding)
    {
        showFeedback(tr("向导目标主机已改变，未覆盖其他档案。"), true);
        return false;
    }
    if (next.currentHostId.isEmpty())
        next.currentHostId = hostId;
    return saveState(next);
}

void WorkbenchWindow::showFeedback(const QString &message, bool error)
{
    m_feedback->setText(message);
    m_feedback->setProperty("error", error);
    m_feedback->setStyleSheet(error ? QStringLiteral("background: #3a292c; color: #ffb9ad;") : QString());
    m_feedback->setVisible(!message.isEmpty());
}

void WorkbenchWindow::updateWorkspace()
{
    m_workspace->setCurrentIndex(m_tabs->count() == 0 ? 0 : 1);
    m_summary->setText(m_tabs->count() == 0 ? tr("没有活动终端") : tr("%1 个终端标签").arg(m_tabs->count()));
    if (m_preview)
    {
        m_openSelected->setEnabled(false);
        m_newTerminal->setEnabled(false);
    }
}
}
