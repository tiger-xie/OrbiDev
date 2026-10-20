#include "WorkbenchWindow.h"
#include "HostDialog.h"
#include "SshSession.h"
#include "WindowFrame.h"
#include "terminal/TerminalView.h"

#include <QAction>
#include <QApplication>
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
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QSaveFile>
#include <QScopedValueRollback>
#include <QScreen>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTabBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <memory>

namespace sshworkbench
{
namespace
{
// 保留原生勾选、选中与键盘行为，只将项目文字分为名称和目录两层。
class ProjectDelegate final : public QStyledItemDelegate
{
public:
    explicit ProjectDelegate(QObject *parent) : QStyledItemDelegate(parent)
    {
    }
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        return QSize(QStyledItemDelegate::sizeHint(option, index).width(), 78);
    }
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem styled(option);
        initStyleOption(&styled, index);
        // Qt 的样式文本会将换行转换为 U+2028；两行内容必须从原始模型读取。
        const auto lines = index.data(Qt::DisplayRole).toString().split(QLatin1Char('\n'));
        const QWidget *widget = styled.widget;
        auto *style = widget ? widget->style() : QApplication::style();
        const QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &styled, widget);
        styled.text.clear();
        style->drawControl(QStyle::CE_ItemViewItem, &styled, painter, widget);
        painter->save();
        painter->setClipRect(option.rect);
        const QRect content(textRect.left() + 8, option.rect.top(),
                            qMax(0, option.rect.right() - textRect.left() - 24), option.rect.height());
        QFont nameFont = option.font;
        nameFont.setPointSizeF(nameFont.pointSizeF() + 1);
        painter->setFont(nameFont);
        painter->setPen(option.palette.color(QPalette::Text));
        const QFontMetrics nameMetrics(nameFont);
        const QFontMetrics pathMetrics(option.font);
        const int gap = 6;
        const int top =
            content.top() + (content.height() - nameMetrics.height() - pathMetrics.height() - gap) / 2;
        painter->drawText(QPoint(content.left(), top + nameMetrics.ascent()),
                          nameMetrics.elidedText(lines.value(0), Qt::ElideRight, content.width()));
        painter->setFont(option.font);
        painter->setPen(option.palette.color(QPalette::PlaceholderText));
        painter->drawText(QPoint(content.left(), top + nameMetrics.height() + gap + pathMetrics.ascent()),
                          pathMetrics.elidedText(lines.value(1), Qt::ElideMiddle, content.width()));
        painter->restore();
    }
};
}

WorkbenchWindow::WorkbenchWindow(const QString &profilePath, const QString &projectsPath,
                                 const QString &legacyPath, bool preview, QWidget *parent)
    : QMainWindow(parent), m_profilePath(profilePath), m_projectsPath(projectsPath), m_preview(preview)
{
    setWindowTitle(tr("OrbiDev · SSH 工作台"));
    resize(1280, 820);
    setMinimumSize(920, 600);
    m_statePath = m_projectsPath + QStringLiteral(".workbench.json");
    m_preferencesPath = m_projectsPath + QStringLiteral(".preferences.json");
    buildUi();
    fitWindowToScreens(geometry());
    if (m_preview)
    {
        m_state.projects.projects = {{tr("产品开发"), QStringLiteral("D:/Projects/Product"), true},
                                     {tr("日常工具"), QStringLiteral("D:/Projects/Tools"), false}};
        m_hostSelector->addItem(tr("家庭电脑（预览）"));
        showFeedback(tr("界面预览：未读取身份，未启动 SSH，会话不会自动打开。"));
    }
    else
    {
        loadProjects(legacyPath);
        renderHosts();
        reloadProfile();
    }
    if (!m_preview)
        loadPreferences();
    renderProjects();
    updateWorkspace();
}

void WorkbenchWindow::showPreviewTabs()
{
    if (!m_preview || m_tabs->count() != 0)
        return;
    for (int index = 0; index < m_state.projects.projects.size(); ++index)
    {
        auto &project = m_state.projects.projects[index];
        project.id = QStringLiteral("preview-%1").arg(index);
        auto *page = new QWidget(m_tabs);
        page->setProperty("projectId", project.id);
        page->setProperty("projectName", project.name);
        page->setProperty("hostName", tr("家庭电脑（预览）"));
        page->setProperty("sessionNumber", index + 1);
        page->setProperty("sessionState", tr("界面预览"));
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        auto *location = new QLabel(tr("家庭电脑（预览）  ·  %1").arg(project.directory), page);
        location->setObjectName(QStringLiteral("terminalLocation"));
        location->setTextFormat(Qt::PlainText);
        location->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        location->setToolTip(location->text());
        layout->addWidget(location);
        auto *preview = new QLabel(tr("终端区域预览\n\n此场景仅展示标签与目录布局，未启动 SSH。"), page);
        preview->setObjectName(QStringLiteral("terminalPreview"));
        preview->setAlignment(Qt::AlignCenter);
        layout->addWidget(preview, 1);
        m_tabs->addTab(page, project.name);
        updateTerminalLabel(page);
    }
    m_tabs->setCurrentIndex(0);
    m_projects->setCurrentRow(0);
    updateWorkspace();
}

void WorkbenchWindow::loadPreferences()
{
    QFile file(m_preferencesPath);
    if (!file.exists())
        return;
    QJsonParseError error;
    QJsonObject settings;
    if (file.open(QIODevice::ReadOnly) && file.size() <= 65536)
    {
        const auto document = QJsonDocument::fromJson(file.readAll(), &error);
        if (error.error == QJsonParseError::NoError && document.isObject())
            settings = document.object();
    }
    const auto integer = [&settings](const QString &key, int minimum, int maximum)
    {
        const auto value = settings.value(key);
        return value.isDouble() && value.toDouble() == value.toInt() && value.toInt() >= minimum &&
               value.toInt() <= maximum;
    };
    bool valid =
        integer(QStringLiteral("version"), 1, 1) && integer(QStringLiteral("fontSize"), 10, 30) &&
        integer(QStringLiteral("railWidth"), 230, 32768) && integer(QStringLiteral("x"), -1000000, 1000000) &&
        integer(QStringLiteral("y"), -1000000, 1000000) && integer(QStringLiteral("width"), 1, 32768) &&
        integer(QStringLiteral("height"), 1, 32768) && settings.value(QStringLiteral("maximized")).isBool() &&
        settings.value(QStringLiteral("recentProjects")).isObject();
    const auto recent = settings.value(QStringLiteral("recentProjects")).toObject();
    for (auto entry = recent.begin(); entry != recent.end(); ++entry)
        valid = valid && !entry.key().isEmpty() && entry.value().isString();
    if (!valid)
    {
        m_preferencesWritable = false;
        showFeedback(tr("外观偏好无法读取或版本不支持，本次使用默认值并保留原文件；修复该文件后重启工作台。"),
                     true);
        return;
    }
    for (auto entry = recent.begin(); entry != recent.end(); ++entry)
        m_recentProjects.insert(entry.key(), entry.value().toString());
    setTerminalFontSize(settings.value(QStringLiteral("fontSize")).toInt());
    QRect restored(settings.value(QStringLiteral("x")).toInt(), settings.value(QStringLiteral("y")).toInt(),
                   settings.value(QStringLiteral("width")).toInt(),
                   settings.value(QStringLiteral("height")).toInt());
    fitWindowToScreens(restored);
    m_splitter->setSizes({settings.value(QStringLiteral("railWidth")).toInt(),
                          qMax(1, width() - settings.value(QStringLiteral("railWidth")).toInt())});
    if (settings.value(QStringLiteral("maximized")).toBool())
        setWindowState(windowState() | Qt::WindowMaximized);
}

void WorkbenchWindow::fitWindowToScreens(QRect restored)
{
    auto *target = QGuiApplication::primaryScreen();
    int largestIntersection = 0;
    for (auto *screen : QGuiApplication::screens())
    {
        const auto overlap = screen->availableGeometry().intersected(restored);
        const int area = overlap.width() * overlap.height();
        if (area > largestIntersection)
        {
            largestIntersection = area;
            target = screen;
        }
    }
    if (target)
    {
        // 为系统标题栏和窗口边框留出余量；缩放或移除显示器后不把窗口留在屏幕外。
        const auto available = target->availableGeometry().adjusted(12, 40, -12, -12);
        if (available.width() <= 0 || available.height() <= 0)
            return;
        setMinimumSize(qMin(920, available.width()), qMin(600, available.height()));
        restored.setSize(restored.size().expandedTo(minimumSize()).boundedTo(available.size()));
        restored.moveLeft(
            qBound(available.left(), restored.left(), available.right() - restored.width() + 1));
        restored.moveTop(qBound(available.top(), restored.top(), available.bottom() - restored.height() + 1));
    }
    setGeometry(restored);
}

bool WorkbenchWindow::savePreferences()
{
    if (m_preview)
        return true;
    if (!m_preferencesWritable)
    {
        showFeedback(tr("外观仅在本次生效：原偏好文件无法读取，未覆盖该文件。"), true);
        return false;
    }
    const QRect normal = isMaximized() || isMinimized() ? normalGeometry() : geometry();
    QJsonObject recent;
    for (auto entry = m_recentProjects.cbegin(); entry != m_recentProjects.cend(); ++entry)
        recent.insert(entry.key(), entry.value());
    const QJsonObject settings{{QStringLiteral("version"), 1},
                               {QStringLiteral("fontSize"), m_terminalFontSize},
                               {QStringLiteral("railWidth"), qBound(230, m_splitter->sizes().front(), 32768)},
                               {QStringLiteral("x"), normal.x()},
                               {QStringLiteral("y"), normal.y()},
                               {QStringLiteral("width"), normal.width()},
                               {QStringLiteral("height"), normal.height()},
                               {QStringLiteral("maximized"), isMaximized()},
                               {QStringLiteral("recentProjects"), recent}};
    QSaveFile file(m_preferencesPath);
    const auto bytes = QJsonDocument(settings).toJson();
    if (!QDir().mkpath(QFileInfo(m_preferencesPath).absolutePath()) || !file.open(QIODevice::WriteOnly) ||
        file.write(bytes) != bytes.size() || !file.commit())
    {
        showFeedback(tr("外观偏好未保存：%1").arg(file.errorString()), true);
        return false;
    }
    return true;
}

void WorkbenchWindow::setTerminalFontSize(int size)
{
    m_terminalFontSize = qBound(10, size, 30);
    for (auto *terminal : findChildren<rad::client::TerminalView *>())
        terminal->setTerminalFontSize(m_terminalFontSize);
}

void WorkbenchWindow::showAppearanceSettings()
{
    if (m_preview || m_closing)
        return;
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("appearanceDialog"));
    dialog.setWindowTitle(tr("工作台外观"));
    dialog.setMinimumWidth(430);
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    form->addRow(tr("主题"), new QLabel(tr("石墨灰／青绿（深色）"), &dialog));
    auto *fontSize = new QSpinBox(&dialog);
    fontSize->setObjectName(QStringLiteral("terminalFontSize"));
    fontSize->setRange(10, 30);
    fontSize->setValue(m_terminalFontSize);
    form->addRow(tr("终端字号"), fontSize);
    layout->addLayout(form);
    auto *hint = new QLabel(tr("应用于所有已有和新建终端。\nCtrl+- / Ctrl+= 也会调整并保存全局字号。\n复制 "
                               "Ctrl+Shift+C；粘贴 Ctrl+V / Ctrl+Shift+V。"),
                            &dialog);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("保存"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    fontSize->setFocus();
    if (dialog.exec() == QDialog::Accepted)
    {
        setTerminalFontSize(fontSize->value());
        if (savePreferences())
            showFeedback(tr("外观偏好已保存，字号已应用于全部终端。"));
    }
}

void WorkbenchWindow::buildUi()
{
    setMenuWidget(new sshsetup::WindowFrame(this));
    auto *central = new QWidget(this);
    central->setObjectName(QStringLiteral("workbench"));
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    auto *toolbarPanel = new QWidget(central);
    toolbarPanel->setObjectName(QStringLiteral("workbenchToolbar"));
    auto *toolbar = new QHBoxLayout(toolbarPanel);
    toolbar->setContentsMargins(18, 12, 18, 12);
    toolbar->setSpacing(14);
    m_host = new QLabel(tr("尚未配置家庭电脑"), central);
    m_host->setObjectName(QStringLiteral("hostSummary"));
    m_host->setTextFormat(Qt::PlainText);
    m_host->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_hostSelector = new QComboBox(central);
    m_hostSelector->setObjectName(QStringLiteral("hostSelector"));
    m_hostSelector->setMinimumWidth(180);
    m_hostSelector->setMaximumWidth(280);
    m_hostSelector->setEnabled(!m_preview);
    m_hostSelector->setAccessibleName(tr("浏览家庭主机的项目"));
    m_hostSelector->setToolTip(tr("选择要浏览的主机；已有终端保留创建时的目标"));
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
    auto *settings = new QPushButton(tr("设置"), central);
    settings->setObjectName(QStringLiteral("connectionSettingsButton"));
    settings->setIcon(QIcon(QStringLiteral(":/resources/icon/workbench/settings.svg")));
    auto *hostMenu = new QMenu(settings);
    hostMenu->addAction(tr("添加家庭电脑…"), this, [this] { editHost(true); });
    hostMenu->addAction(tr("编辑当前主机／测试连接…"), this, [this] { editHost(false); });
    hostMenu->addAction(tr("首次配置向导（当前主机）…"), this, &WorkbenchWindow::openSetup);
    hostMenu->addSeparator();
    hostMenu->addAction(tr("移除当前主机…"), this, &WorkbenchWindow::removeHost);
    hostMenu->addSeparator();
    auto *about = hostMenu->addAction(
        tr("关于 SSH 工作台…"), this,
        [this]
        {
            QMessageBox::about(
                this, tr("关于 OrbiDev SSH 工作台"),
                tr("OrbiDev SSH 工作台 %1\nQt %2 · Windows x64\n\n"
                   "候选验证版，完整发布验收尚未完成。\n"
                   "通过 SSH 操作家庭电脑上的 Claude；原 Client/Agent 暂停开发。\n"
                   "关闭客户端或断线不保证远程任务继续运行。\n\n"
                   "版本与依赖清单见包内 manifest.json，已知限制见发布说明。")
                    .arg(QCoreApplication::applicationVersion(), QString::fromLatin1(qVersion())));
        });
    about->setObjectName(QStringLiteral("aboutWorkbenchAction"));
    settings->setAccessibleDescription(tr("打开设置菜单"));
    connect(settings, &QPushButton::clicked, this,
            [this, settings, hostMenu]
            {
                hostMenu->ensurePolished();
                const QSize menuSize = hostMenu->sizeHint();
                QRect bounds(mapToGlobal(QPoint(0, 0)), size());
                if (auto *screen = settings->screen())
                    bounds = bounds.intersected(screen->availableGeometry());
                // 右对齐并留在当前窗口内，避免菜单被右侧桌面或窗口边界截断。
                QPoint position = settings->mapToGlobal(
                    QPoint(settings->width() - menuSize.width(), settings->height() + 6));
                position.setX(qBound(bounds.left(), position.x(),
                                     qMax(bounds.left(), bounds.right() - menuSize.width() + 1)));
                position.setY(qBound(bounds.top(), position.y(),
                                     qMax(bounds.top(), bounds.bottom() - menuSize.height() + 1)));
                hostMenu->popup(position);
            });
    auto *openSettings = new QShortcut(QKeySequence(QStringLiteral("Alt+Down")), settings);
    openSettings->setContext(Qt::WidgetShortcut);
    connect(openSettings, &QShortcut::activated, settings, &QPushButton::click);
    toolbar->addWidget(settings);
    hostMenu->insertSeparator(about);
    auto *appearance = new QAction(tr("外观设置…"), hostMenu);
    appearance->setObjectName(QStringLiteral("appearanceSettingsAction"));
    connect(appearance, &QAction::triggered, this, &WorkbenchWindow::showAppearanceSettings);
    hostMenu->insertAction(about, appearance);
    for (int delta : {-1, 1})
    {
        auto *zoom = new QAction(delta < 0 ? tr("缩小终端文字") : tr("放大终端文字"), hostMenu);
        zoom->setShortcut(QKeySequence(delta < 0 ? QStringLiteral("Ctrl+-") : QStringLiteral("Ctrl+=")));
        addAction(zoom);
        hostMenu->insertAction(about, zoom);
        connect(zoom, &QAction::triggered, this,
                [this, delta]
                {
                    if (m_preview || m_closing)
                        return;
                    setTerminalFontSize(m_terminalFontSize + delta);
                    savePreferences();
                });
    }
    m_newTerminal = new QPushButton(tr("新建终端"), central);
    m_newTerminal->setObjectName(QStringLiteral("newTerminalButton"));
    m_newTerminal->setProperty("role", "primary");
    m_newTerminal->setIcon(QIcon(QStringLiteral(":/resources/icon/workbench/terminal.svg")));
    connect(m_newTerminal, &QPushButton::clicked, this, [this] { openCurrent(true); });
    m_newTerminal->setToolTip(tr("为选中项目额外创建一个 SSH 会话"));
    toolbar->addWidget(m_newTerminal);
    root->addWidget(toolbarPanel);
    m_feedback = new QLabel(central);
    m_feedback->setObjectName(QStringLiteral("feedback"));
    m_feedback->setTextFormat(Qt::PlainText);
    m_feedback->setWordWrap(true);
    m_feedback->hide();
    root->addWidget(m_feedback);
    auto *splitter = new QSplitter(central);
    m_splitter = splitter;
    splitter->setChildrenCollapsible(false);
    auto *rail = new QWidget(splitter);
    rail->setObjectName(QStringLiteral("projectRail"));
    rail->setMinimumWidth(230);
    auto *railLayout = new QVBoxLayout(rail);
    railLayout->setContentsMargins(0, 12, 0, 14);
    railLayout->setSpacing(10);
    auto *railHeading = new QHBoxLayout;
    railHeading->setContentsMargins(18, 0, 16, 0);
    auto *title = new QLabel(tr("项目"), rail);
    title->setProperty("role", "section");
    railHeading->addWidget(title, 1);
    auto *add = new QPushButton(rail);
    add->setObjectName(QStringLiteral("addProjectButton"));
    add->setIcon(QIcon(QStringLiteral(":/resources/icon/workbench/add.svg")));
    add->setAccessibleName(tr("新建项目"));
    add->setToolTip(tr("新建项目"));
    add->setFixedSize(30, 30);
    add->setEnabled(!m_preview);
    connect(add, &QPushButton::clicked, this, [this] { editProject(true); });
    railHeading->addWidget(add);
    railLayout->addLayout(railHeading);
    m_projects = new QListWidget(rail);
    m_projects->setObjectName(QStringLiteral("projectList"));
    m_projects->setWordWrap(false);
    m_projects->setItemDelegate(new ProjectDelegate(m_projects));
    m_projects->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_projects->setToolTip(tr("双击复用已有终端；空格勾选；F2 编辑；Delete 移除"));
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
                updateWorkspace();
            });
    connect(m_projects, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *) { openCurrent(); });
    connect(m_projects, &QListWidget::currentRowChanged, this,
            [this]
            {
                if (m_renderingProjects)
                    return;
                const int row = projectRow();
                if (!m_preview && row >= 0)
                    m_recentProjects.insert(m_state.currentHostId, m_state.projects.projects[row].id);
                updateWorkspace();
            });
    m_projects->setContextMenuPolicy(Qt::ActionsContextMenu);
    m_editProject = new QAction(tr("编辑项目…"), m_projects);
    m_editProject->setObjectName(QStringLiteral("editProjectAction"));
    m_editProject->setShortcut(QKeySequence(Qt::Key_F2));
    m_removeProject = new QAction(tr("移除项目…"), m_projects);
    m_removeProject->setObjectName(QStringLiteral("removeProjectAction"));
    m_removeProject->setShortcut(QKeySequence(Qt::Key_Delete));
    for (auto *action : {m_editProject, m_removeProject})
    {
        action->setShortcutContext(Qt::WidgetShortcut);
        m_projects->addAction(action);
    }
    connect(m_editProject, &QAction::triggered, this, [this] { editProject(false); });
    connect(m_removeProject, &QAction::triggered, this, &WorkbenchWindow::removeProject);
    m_openSelected = new QPushButton(tr("打开勾选项目"), rail);
    m_openSelected->setObjectName(QStringLiteral("openSelectedButton"));
    connect(m_openSelected, &QPushButton::clicked, this, &WorkbenchWindow::openSelected);
    m_openSelected->setIcon(QIcon(QStringLiteral(":/resources/icon/setup/folder.svg")));
    auto *batchActions = new QHBoxLayout;
    batchActions->setContentsMargins(14, 0, 14, 0);
    batchActions->addWidget(m_openSelected);
    railLayout->addLayout(batchActions);
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
    m_emptyHint = emptyHint;
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
    m_tabs->setDocumentMode(true);
    m_tabs->setElideMode(Qt::ElideRight);
    m_tabs->tabBar()->setExpanding(false);
    m_tabs->tabBar()->setDrawBase(false);
    connect(m_tabs, &QTabWidget::currentChanged, this,
            [this](int index)
            {
                if (auto *page = m_tabs->widget(index))
                {
                    page->setProperty("lastActivated", QVariant::fromValue(++m_activationSerial));
                    selectTerminalProject(page);
                    updateActiveSession();
                    if (auto *terminal = page->findChild<rad::client::TerminalView *>())
                        terminal->focusTerminal();
                }
            });
    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &WorkbenchWindow::closeTerminal);
    connect(m_tabs, &QTabWidget::tabBarClicked, this,
            [this](int index)
            {
                if (auto *page = m_tabs->widget(index))
                {
                    page->setProperty("lastActivated", QVariant::fromValue(++m_activationSerial));
                    selectTerminalProject(page);
                    updateActiveSession();
                    if (auto *terminal = page->findChild<rad::client::TerminalView *>())
                        terminal->focusTerminal();
                }
            });
    m_workspace->addWidget(m_tabs);
    splitter->addWidget(rail);
    splitter->addWidget(m_workspace);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({280, 1000});
    root->addWidget(splitter, 1);
    m_summary = new QLabel(tr("没有活动终端"), this);
    statusBar()->addWidget(m_summary);
    auto *shortcuts = new QLabel(tr("Ctrl+Shift+C 复制  ·  Ctrl+Shift+V 粘贴"), this);
    shortcuts->setObjectName(QStringLiteral("workbenchShortcuts"));
    statusBar()->addPermanentWidget(shortcuts);
    auto *sessionHelp = new QPushButton(tr("断线说明"), this);
    sessionHelp->setObjectName(QStringLiteral("sessionHelpButton"));
    sessionHelp->setToolTip(tr("关闭客户端将断开 SSH，不保证远程任务继续运行"));
    connect(sessionHelp, &QPushButton::clicked, this,
            [this]
            {
                QMessageBox::information(this, tr("SSH 会话与断线"),
                                         tr("关闭客户端或断线会中断 SSH 连接，不保证远程任务继续运行。\n"
                                            "重新连接将创建新会话，不会恢复原进程。"));
            });
    statusBar()->addPermanentWidget(sessionHelp);
    setCentralWidget(central);
    settings->setEnabled(!m_preview);
    setTabOrder(m_hostSelector, settings);
    setTabOrder(settings, m_newTerminal);
    setTabOrder(m_newTerminal, m_projects);
    setTabOrder(m_projects, add);
    setTabOrder(add, m_openSelected);
    m_tabs->setToolTip(tr(
        "复制 Ctrl+Shift+C；粘贴 Ctrl+V / Ctrl+Shift+V；字号 Ctrl+- / Ctrl+=。Tab 在终端中交给远程程序。"));
    m_newTerminal->setToolTip(tr("先选择项目，再额外创建 SSH 会话；双击项目会复用已有终端"));
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
    }
    updateActiveSession();
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
    const QString previous = m_recentProjects.value(m_state.currentHostId);
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
    {
        int selected = 0;
        for (int row = 0; row < m_projects->count(); ++row)
            if (m_state.projects.projects[m_projects->item(row)->data(Qt::UserRole).toInt()].id == previous)
                selected = row;
        m_projects->setCurrentRow(selected);
        if (!m_preview)
            m_recentProjects.insert(m_state.currentHostId, m_state.projects.projects[projectRow()].id);
    }
    m_renderingProjects = false;
    updateWorkspace();
    for (int index = 0; index < m_tabs->count(); ++index)
        updateTerminalLabel(m_tabs->widget(index));
}

void WorkbenchWindow::updateProjectLabels()
{
    const QSignalBlocker blocker(m_projects);
    for (int row = 0; row < m_projects->count(); ++row)
    {
        auto *item = m_projects->item(row);
        const auto &project = m_state.projects.projects[item->data(Qt::UserRole).toInt()];
        int count = 0;
        for (int index = 0; index < m_tabs->count(); ++index)
            if (m_tabs->widget(index)->property("projectId").toString() == project.id)
                ++count;
        item->setText(tr("%1 · %2 个标签\n%3")
                          .arg(project.name)
                          .arg(count)
                          .arg(project.directory.isEmpty() ? tr("家庭用户目录") : project.directory));
        item->setToolTip(item->text() + tr("\n包含准备中、运行中及保留的退出标签；右键编辑或移除。"));
    }
}

void WorkbenchWindow::selectTerminalProject(QWidget *page)
{
    const auto projectId = page->property("projectId").toString();
    const auto hostId = m_state.projectHosts.value(projectId);
    if (hostId.isEmpty())
        return;
    if (m_state.currentHostId != hostId)
    {
        auto next = m_state;
        next.currentHostId = hostId;
        if (!saveState(next))
            return;
        renderHosts();
        reloadProfile();
        renderProjects();
    }
    for (int row = 0; row < m_projects->count(); ++row)
        if (m_state.projects.projects[m_projects->item(row)->data(Qt::UserRole).toInt()].id == projectId)
        {
            m_projects->setCurrentRow(row);
            m_projects->scrollToItem(m_projects->item(row));
            return;
        }
}

void WorkbenchWindow::updateTerminalLabel(QWidget *page)
{
    const int index = m_tabs->indexOf(page);
    if (index < 0 || page->property("projectId").toString().isEmpty())
        return;
    QString name = page->property("projectName").toString();
    for (const auto &project : m_state.projects.projects)
        if (project.id == page->property("projectId").toString())
        {
            name = project.name;
            break;
        }
    const auto state = page->property("sessionClosing").toBool() ? tr("正在关闭")
                                                                 : page->property("sessionState").toString();
    m_tabs->setTabText(
        index, tr("%1 #%2 · %3").arg(name).arg(page->property("sessionNumber").toULongLong()).arg(state));
    updateActiveSession();
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
        selectTerminalProject(existing);
        existing->setProperty("lastActivated", QVariant::fromValue(++m_activationSerial));
        if (auto *terminal = existing->findChild<rad::client::TerminalView *>())
            terminal->focusTerminal();
        return;
    }
    createTerminal(project, *host);
}

void WorkbenchWindow::createTerminal(launcher::Project project, HostProfile host)
{
    if (m_closing || m_preview)
        return;
    if (m_tabs->count() >= 16)
    {
        showFeedback(tr("最多同时保留 16 个终端标签，请先关闭不再使用的标签。"), true);
        return;
    }
    const auto profile = host.connection;
    auto *page = new QWidget(m_tabs);
    page->setProperty("hostId", host.id);
    page->setProperty("hostName", host.name);
    page->setProperty("projectId", project.id);
    page->setProperty("projectName", project.name);
    page->setProperty("sessionNumber", QVariant::fromValue(++m_terminalSerial));
    page->setProperty("sessionState", tr("准备终端"));
    page->setProperty("sessionTarget", QStringList{host.id, project.id, project.directory, profile.host,
                                                   profile.user, QString::number(profile.port),
                                                   profile.identityFile, profile.hostFingerprint});
    page->setProperty("sessionPending", true);
    page->setProperty("columns", 120);
    page->setProperty("rows", 32);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto *location = new QLabel(host.name + QStringLiteral("  ·  ") +
                                    (project.directory.isEmpty() ? tr("家庭用户目录") : project.directory),
                                page);
    location->setObjectName(QStringLiteral("terminalLocation"));
    location->setTextFormat(Qt::PlainText);
    location->setToolTip(location->text());
    location->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(location);
    auto *status = new QLabel(tr("正在准备终端…"), page);
    status->setObjectName(QStringLiteral("terminalStatus"));
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    status->setContentsMargins(16, 10, 16, 10);
    auto *statusRow = new QHBoxLayout;
    statusRow->addWidget(status, 1);
    auto *reconnect = new QPushButton(tr("重新连接…"), page);
    reconnect->setObjectName(QStringLiteral("reconnectTerminalButton"));
    reconnect->setToolTip(tr("使用此标签创建时的主机与目录新建会话，不恢复原进程"));
    reconnect->hide();
    statusRow->addWidget(reconnect);
    layout->addLayout(statusRow);
    auto *terminal = new rad::client::TerminalView(page);
    terminal->setTerminalFontSize(m_terminalFontSize);
    layout->addWidget(terminal, 1);
    auto *session = new SshSession(page);
    const QString directory = project.directory;
    const int index = m_tabs->addTab(page, project.name);
    updateTerminalLabel(page);
    const auto destination = tr("主机：%1\n用户与地址：%2@%3:%4\n远程目录：%5")
                                 .arg(host.name, profile.user, profile.host)
                                 .arg(profile.port)
                                 .arg(directory.isEmpty() ? tr("家庭用户目录") : directory);
    page->setProperty("sessionDestination", destination);
    m_tabs->setTabToolTip(index, destination);
    m_tabs->setCurrentIndex(index);
    connect(reconnect, &QPushButton::clicked, page,
            [this, page, project, host, destination]
            {
                if (m_closing || m_closePromptActive || page->property("sessionClosing").toBool())
                    return;
                QScopedValueRollback<bool> confirming(m_closePromptActive, true);
                if (QMessageBox::question(this, tr("重新连接：新建会话"),
                                          tr("将使用此标签创建时的目标新建 SSH 会话，不会恢复原进程。\n%1\n"
                                             "Claude 工作区信任仍需自行确认。继续？")
                                              .arg(destination),
                                          QMessageBox::Yes | QMessageBox::No,
                                          QMessageBox::No) == QMessageBox::Yes)
                    createTerminal(project, host);
            });
    connect(terminal, &rad::client::TerminalView::ready, page,
            [page, terminal, session, profile, directory]
            {
                if (page->property("sessionClosing").toBool() || page->property("terminalFailed").toBool() ||
                    !page->property("sessionPending").toBool())
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
    connect(session, &SshSession::hostVerificationStarted, page,
            [this, page, status]
            {
                if (page->property("sessionClosing").toBool() || page->property("terminalFailed").toBool() ||
                    page->property("sessionExited").toBool())
                    return;
                page->setProperty("sessionState", tr("校验主机"));
                status->setText(tr("正在校验主机…认证与 Claude 状态随后请查看终端。"));
                updateTerminalLabel(page);
            });
    connect(session, &SshSession::started, page,
            [this, page, status]
            {
                if (page->property("sessionClosing").toBool() || page->property("terminalFailed").toBool() ||
                    page->property("sessionExited").toBool())
                    return;
                page->setProperty("sessionState", tr("SSH 运行中"));
                status->setText(
                    tr("SSH 进程运行中 · 仅确认本地进程已启动；认证与 Claude 是否就绪请查看终端。"));
                updateTerminalLabel(page);
            });
    connect(session, &SshSession::errorOccurred, page,
            [this, page, status](const QString &error)
            {
                page->setProperty("sessionError", error);
                if (!page->property("sessionClosing").toBool())
                {
                    status->setText(error);
                    page->setProperty("sessionState", tr("出错"));
                    updateTerminalLabel(page);
                }
            });
    connect(session, &SshSession::exited, page,
            [this, page, terminal, status, reconnect](int code)
            {
                terminal->setInputEnabled(false);
                page->setProperty("sessionPending", false);
                page->setProperty("sessionExited", true);
                page->setProperty("sessionExitCode", code);
                const QString error = page->property("sessionError").toString();
                status->setText((error.isEmpty() ? QString() : error + QLatin1Char('\n')) +
                                tr("SSH 已退出（退出码 %1）。重新连接将新建会话，不会恢复原进程。"
                                   "退出原因请结合上方错误或终端输出查看。")
                                    .arg(code));
                page->setProperty("sessionState", error.isEmpty() && code == 0 ? tr("已退出") : tr("失败"));
                m_tabs->setTabToolTip(m_tabs->indexOf(page), page->property("sessionDestination").toString() +
                                                                 QLatin1Char('\n') + status->text());
                updateTerminalLabel(page);
                reconnect->setVisible(!page->property("sessionClosing").toBool());
                if (page->property("sessionClosing").toBool())
                    finishTerminalClose(page);
                updateWorkspace();
                if (m_closing)
                    QTimer::singleShot(0, this, [this] { close(); });
            });
    connect(terminal, &rad::client::TerminalView::failed, page,
            [this, page, status, session, terminal, reconnect](const QString &error)
            {
                page->setProperty("sessionPending", false);
                page->setProperty("terminalFailed", true);
                session->stop();
                terminal->setInputEnabled(false);
                page->setProperty("sessionError", error);
                if (page->property("sessionClosing").toBool())
                    return;
                status->setText(error + tr("\n终端失败；重新连接将新建会话，不会恢复原进程。"));
                page->setProperty("sessionState", tr("失败"));
                updateTerminalLabel(page);
                reconnect->setVisible(!session->isActive());
            });
    updateWorkspace();
}

void WorkbenchWindow::closeTerminal(int index)
{
    const QPointer<QWidget> page(m_tabs->widget(index));
    if (!page || m_closePromptActive || page->property("sessionClosing").toBool())
        return;
    auto *session = page->findChild<SshSession *>();
    if (page->property("sessionPending").toBool() || (session && session->isActive()))
    {
        QScopedValueRollback<bool> confirming(m_closePromptActive, true);
        if (QMessageBox::warning(this, tr("关闭终端"),
                                 tr("将断开此 SSH 会话，无法保证远程任务继续运行。确定关闭？"),
                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;
    }
    if (!page || m_tabs->indexOf(page) < 0)
        return;
    page->setProperty("sessionClosing", true);
    page->setEnabled(false);
    page->setProperty("sessionPending", false);
    if (auto *status = page->findChild<QLabel *>(QStringLiteral("terminalStatus")))
        status->setText(tr("正在关闭 SSH…等待后台清理完成。"));
    updateTerminalLabel(page);
    if (session && session->isActive())
    {
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
    if (m_closePromptActive)
    {
        event->ignore();
        return;
    }
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
    if (!m_closing && (active || pending))
    {
        QScopedValueRollback<bool> confirming(m_closePromptActive, true);
        if (QMessageBox::warning(this, tr("退出工作台"),
                                 tr("将断开所有 SSH 会话，无法保证远程任务继续运行。确定退出？"),
                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        {
            event->ignore();
            return;
        }
    }
    m_closing = true;
    active = false;
    for (int i = 0; i < m_tabs->count(); ++i)
    {
        m_tabs->widget(i)->setProperty("sessionClosing", true);
        m_tabs->widget(i)->setProperty("sessionPending", false);
        updateTerminalLabel(m_tabs->widget(i));
        // 确认对话框期间仍会处理事件，需要重新检查刚启动的会话。
        auto *session = m_tabs->widget(i)->findChild<SshSession *>();
        active = active || (session && session->isActive());
    }
    if (!active)
    {
        if (!savePreferences() && m_preferencesWritable)
            QMessageBox::warning(this, tr("偏好未保存"),
                                 tr("本次窗口布局和最近选择未能保存，旧偏好已保留。\n工作台仍将退出。"));
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
    m_feedback->style()->unpolish(m_feedback);
    m_feedback->style()->polish(m_feedback);
    m_feedback->update();
    m_feedback->setVisible(!message.isEmpty());
}

void WorkbenchWindow::updateActiveSession()
{
    const auto *page = m_tabs->currentWidget();
    if (page)
    {
        const QString state = page->property("sessionClosing").toBool()
                                  ? tr("正在关闭")
                                  : page->property("sessionState").toString();
        m_host->setText(tr("当前标签：%1 · %2").arg(page->property("hostName").toString(), state));
        m_host->setToolTip(m_host->text() + QLatin1Char('\n') +
                           page->property("sessionDestination").toString());
    }
    else
    {
        m_host->setText(tr("尚未打开终端"));
        m_host->setToolTip(tr("左侧选择浏览的主机；打开项目后，此处显示当前标签的实际状态。"));
    }
}

void WorkbenchWindow::updateWorkspace()
{
    updateProjectLabels();
    m_workspace->setCurrentIndex(m_tabs->count() == 0 ? 0 : 1);
    updateActiveSession();
    m_summary->setText(m_tabs->count() == 0 ? tr("SSH  ·  UTF-8  ·  没有活动终端")
                                            : tr("SSH  ·  UTF-8  ·  %1 个标签").arg(m_tabs->count()));
    const bool editable = !m_preview && !m_closing && m_projectsReadable;
    const bool selected = projectRow() >= 0;
    m_editProject->setEnabled(editable && selected);
    m_removeProject->setEnabled(editable && selected);
    m_newTerminal->setEnabled(editable && m_profileReady && selected);
    bool checked = false;
    for (int row = 0; row < m_projects->count(); ++row)
        checked = checked || m_projects->item(row)->checkState() == Qt::Checked;
    m_openSelected->setEnabled(editable && m_profileReady && checked);
    m_emptyHint->setText(
        !m_projectsReadable ? tr("配置读取失败，请先处理上方提示。原配置文件不会被覆盖。")
        : !m_profileReady   ? tr("从“设置”添加家庭电脑或运行首次配置向导。\n配置完成后，添加项目并打开终端。")
        : m_projects->count() == 0
            ? tr("此主机尚无项目。点击“项目”旁的新增按钮，填写家庭电脑上的项目目录。")
            : tr("双击左侧项目打开终端，或勾选后批量打开。\n重启工作台只恢复选择，不会自动启动 "
                 "Claude。\n复制 Ctrl+Shift+C · 粘贴 Ctrl+V · 字号 Ctrl+- / Ctrl+="));
}
}
