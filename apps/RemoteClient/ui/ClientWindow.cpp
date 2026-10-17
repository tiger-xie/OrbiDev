#include "ClientWindow.h"

#include "../controllers/ClientController.h"

#include <QAction>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QStyle>
#include <QTabBar>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

namespace rad::client
{
namespace
{
// 创建已提交后不可假装取消；等待结果再允许 Esc、关闭按钮或取消操作。
class CreateSessionDialog final : public QDialog
{
public:
    CreateSessionDialog(ClientController *controller, QWidget *parent)
        : QDialog(parent), m_controller(controller)
    {
    }
    void reject() override
    {
        if (!m_controller->isBusy())
            QDialog::reject();
    }

private:
    ClientController *m_controller;
};

QString processText(const ClientSession &session, bool connected)
{
    const auto state =
        session.running ? QObject::tr("运行中") : QObject::tr("已退出 · 退出码 %1").arg(session.exitCode);
    return connected ? state : QObject::tr("最后已知：%1").arg(state);
}
QString attachmentText(const ClientSession &session)
{
    if (!session.attached)
        return QObject::tr("未附加");
    return session.readOnly ? QObject::tr("已附加 · 只读") : QObject::tr("已附加 · 持有控制权");
}
}

ClientWindow::ClientWindow(ClientController *controller, QWidget *parent)
    : QMainWindow(parent), m_controller(controller)
{
    Q_ASSERT(controller);
    setupUi();
    connect(controller, &ClientController::changed, this, &ClientWindow::refreshUi);
    connect(controller, &ClientController::sessionOpened, this, &ClientWindow::showSession);
    connect(controller, &ClientController::sessionDetached, this, &ClientWindow::removeSession);
    refreshUi();
}

void ClientWindow::setupUi()
{
    setWindowTitle(tr("RADManager · 远程 CLI 管理器"));
    resize(1280, 800);
    setMinimumSize(800, 540);
    auto *fileMenu = menuBar()->addMenu(tr("文件(&F)"));
    auto *sessionMenu = menuBar()->addMenu(tr("会话(&S)"));
    auto *viewMenu = menuBar()->addMenu(tr("视图(&V)"));
    auto *helpMenu = menuBar()->addMenu(tr("帮助(&H)"));
    fileMenu->addAction(tr("退出"), this, &QWidget::close);
    m_createAction = sessionMenu->addAction(tr("新建会话…"), this, &ClientWindow::createSession);
    m_createAction->setShortcut(QKeySequence::New);
    m_detachAction = sessionMenu->addAction(tr("分离当前会话"), this,
                                            [this] { m_controller->detachSession(currentSessionId()); });
    m_terminateAction =
        sessionMenu->addAction(tr("终止当前会话…"), this, [this] { confirmTerminate(currentSessionId()); });
    helpMenu->addAction(tr("关于 RADManager"), this,
                        [this]
                        {
                            QMessageBox::about(
                                this, tr("关于 RADManager"),
                                tr("远程 CLI 会话管理器\n\n当前为 FakeAgent "
                                   "模拟验收模式。\n不连接网络、不启动 CLI。终端将在下一阶段接入。"));
                        });

    auto *toolbar = addToolBar(tr("主机与会话"));
    toolbar->setMovable(false);
    auto *host = new QComboBox(toolbar);
    host->setObjectName(QStringLiteral("hostCombo"));
    host->addItem(tr("FakeAgent · 本机模拟"));
    host->setMinimumContentsLength(20);
    toolbar->addWidget(host);
    m_connect = new QPushButton(tr("连接"), toolbar);
    m_connect->setObjectName(QStringLiteral("connectButton"));
    toolbar->addWidget(m_connect);
    m_connection = new QLabel(toolbar);
    m_connection->setObjectName(QStringLiteral("connectionLabel"));
    toolbar->addWidget(m_connection);
    auto *spacer = new QWidget(toolbar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);
    m_create = new QPushButton(tr("新建会话"), toolbar);
    m_create->setObjectName(QStringLiteral("newSessionButton"));
    m_create->setProperty("role", "primary");
    toolbar->addWidget(m_create);
    connect(m_create, &QPushButton::clicked, this, &ClientWindow::createSession);
    connect(m_connect, &QPushButton::clicked, this,
            [this]
            {
                if (m_controller->isConnected())
                    m_controller->disconnectAgent();
                else
                    m_controller->connectAgent();
            });

    auto *central = new QWidget(this);
    auto *centralLayout = new QVBoxLayout(central);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);
    m_error = new QLabel(central);
    m_error->setObjectName(QStringLiteral("errorLabel"));
    m_error->setProperty("role", "error");
    m_error->setWordWrap(true);
    m_error->setTextFormat(Qt::PlainText);
    centralLayout->addWidget(m_error);
    m_splitter = new QSplitter(central);
    m_splitter->setObjectName(QStringLiteral("workspaceSplitter"));
    centralLayout->addWidget(m_splitter, 1);
    auto *navigation = new QWidget(m_splitter);
    auto *navigationLayout = new QVBoxLayout(navigation);
    navigationLayout->setContentsMargins(0, 0, 0, 0);
    auto *heading = new QLabel(tr("工作区"), navigation);
    heading->setProperty("role", "section");
    navigationLayout->addWidget(heading);
    m_tree = new QTreeView(navigation);
    m_tree->setObjectName(QStringLiteral("sessionTree"));
    m_model = new QStandardItemModel(this);
    m_tree->setModel(m_model);
    m_tree->setHeaderHidden(true);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setTextElideMode(Qt::ElideRight);
    m_tree->setIndentation(18);
    navigationLayout->addWidget(m_tree, 1);
    connect(m_tree, &QTreeView::clicked, this,
            [this](const QModelIndex &index)
            {
                const auto id = index.data(Qt::UserRole).toString();
                if (!id.isEmpty())
                    openSession(id);
            });
    auto *navigationAction = viewMenu->addAction(tr("工作区侧栏"));
    navigationAction->setCheckable(true);
    navigationAction->setChecked(true);
    connect(navigationAction, &QAction::toggled, navigation, &QWidget::setVisible);

    auto *content = new QWidget(m_splitter);
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);
    auto *contextBar = new QWidget(content);
    contextBar->setObjectName(QStringLiteral("contextBar"));
    auto *contextLayout = new QHBoxLayout(contextBar);
    contextLayout->setContentsMargins(18, 7, 14, 7);
    m_context = new QLabel(contextBar);
    m_context->setObjectName(QStringLiteral("sessionContextLabel"));
    m_context->setTextFormat(Qt::PlainText);
    m_context->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    contextLayout->addWidget(m_context, 1);
    m_detach = new QPushButton(tr("分离"), contextBar);
    m_detach->setObjectName(QStringLiteral("detachButton"));
    m_terminate = new QPushButton(tr("终止…"), contextBar);
    m_terminate->setObjectName(QStringLiteral("terminateButton"));
    m_terminate->setProperty("role", "danger");
    contextLayout->addWidget(m_detach);
    contextLayout->addWidget(m_terminate);
    connect(m_detach, &QPushButton::clicked, this,
            [this] { m_controller->detachSession(currentSessionId()); });
    connect(m_terminate, &QPushButton::clicked, this, [this] { confirmTerminate(currentSessionId()); });
    contentLayout->addWidget(contextBar);
    m_workspace = new QStackedWidget(content);
    contentLayout->addWidget(m_workspace, 1);
    auto *empty = new QWidget(m_workspace);
    empty->setObjectName(QStringLiteral("emptyPage"));
    auto *emptyLayout = new QVBoxLayout(empty);
    emptyLayout->setContentsMargins(40, 40, 40, 40);
    emptyLayout->addStretch();
    auto *emptyHeading = new QLabel(tr("从一个会话开始"), empty);
    emptyHeading->setProperty("role", "heading");
    emptyHeading->setAlignment(Qt::AlignCenter);
    emptyLayout->addWidget(emptyHeading);
    auto *emptyDescription = new QLabel(
        tr("连接 FakeAgent，再从工作区打开或新建会话。\n当前仅验证界面操作；不会连接真实 Agent 或启动 CLI。"),
        empty);
    emptyDescription->setProperty("role", "secondary");
    emptyDescription->setAlignment(Qt::AlignCenter);
    emptyDescription->setWordWrap(true);
    emptyLayout->addWidget(emptyDescription);
    emptyLayout->addStretch(2);
    m_workspace->addWidget(empty);
    m_tabs = new QTabWidget(m_workspace);
    m_tabs->setObjectName(QStringLiteral("sessionTabs"));
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    m_tabs->setDocumentMode(true);
    m_tabs->setElideMode(Qt::ElideRight);
    m_workspace->addWidget(m_tabs);
    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &ClientWindow::closeSession);
    connect(m_tabs, &QTabWidget::currentChanged, this, [this] { updateContext(); });
    m_splitter->setSizes({250, 1030});
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setCollapsible(1, false);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("sessionStatusLabel"));
    statusBar()->addWidget(m_status, 1);
    auto *simulation = new QLabel(tr("模拟模式  ·  终端未接入"), this);
    simulation->setProperty("role", "secondary");
    statusBar()->addPermanentWidget(simulation);
    setCentralWidget(central);
}

QString ClientWindow::currentSessionId() const
{
    const auto *page = m_tabs->currentWidget();
    return page ? page->property("sessionId").toString() : QString();
}
int ClientWindow::sessionTabCount() const
{
    return m_tabs->count();
}
void ClientWindow::openSession(const QString &id)
{
    if (m_controller->isConnected() && !m_controller->isBusy())
        m_controller->openSession(id);
}
void ClientWindow::showSession(const QString &id)
{
    for (int index = 0; index < m_tabs->count(); ++index)
    {
        if (m_tabs->widget(index)->property("sessionId").toString() == id)
        {
            m_tabs->setCurrentIndex(index);
            updateContext();
            return;
        }
    }
    const auto sessions = m_controller->sessions();
    for (const auto &session : sessions)
    {
        if (session.id != id)
            continue;
        auto *page = new QWidget(m_tabs);
        page->setObjectName(QStringLiteral("sessionPage"));
        page->setProperty("sessionId", id);
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(30, 30, 30, 30);
        auto *summary = new QLabel(page);
        summary->setObjectName(QStringLiteral("sessionSummary"));
        summary->setWordWrap(true);
        summary->setTextFormat(Qt::PlainText);
        layout->addWidget(summary);
        auto *notice = new QLabel(tr("终端将在下一阶段接入"), page);
        notice->setProperty("role", "heading");
        layout->addSpacing(40);
        layout->addWidget(notice);
        auto *description = new QLabel(tr("这里将显示此会话的真实终端。\n当前可验证新建、附加、分离与终止；Fa"
                                          "keAgent 不会运行任何进程。"),
                                       page);
        description->setProperty("role", "secondary");
        description->setWordWrap(true);
        layout->addWidget(description);
        layout->addStretch();
        const int index = m_tabs->addTab(page, session.name);
        auto *closeButton = new QToolButton(m_tabs->tabBar());
        closeButton->setObjectName(QStringLiteral("sessionTabCloseButton"));
        closeButton->setText(QStringLiteral("×"));
        closeButton->setAutoRaise(true);
        closeButton->setToolTip(tr("关闭会话标签"));
        closeButton->setAccessibleName(tr("关闭会话标签：%1").arg(session.name));
        connect(closeButton, &QToolButton::clicked, page,
                [this, page] { closeSession(m_tabs->indexOf(page)); });
        m_tabs->tabBar()->setTabButton(index, QTabBar::RightSide, closeButton);
        m_tabs->setTabToolTip(index, session.name);
        m_tabs->setCurrentIndex(index);
        m_workspace->setCurrentWidget(m_tabs);
        updateContext();
        break;
    }
}
void ClientWindow::removeSession(const QString &id)
{
    for (int index = 0; index < m_tabs->count(); ++index)
        if (m_tabs->widget(index)->property("sessionId").toString() == id)
        {
            auto *page = m_tabs->widget(index);
            m_tabs->removeTab(index);
            page->deleteLater();
            break;
        }
    m_workspace->setCurrentIndex(m_tabs->count() == 0 ? 0 : 1);
    updateContext();
}
void ClientWindow::refreshUi()
{
    const bool connected = m_controller->isConnected();
    const bool busy = m_controller->isBusy();
    m_connection->setText(busy ? tr("◌ 正在处理…") : connected ? tr("● 已连接 · 模拟") : tr("○ 未连接"));
    m_connect->setText(connected ? tr("断开") : tr("连接"));
    m_connect->setEnabled(!busy);
    m_create->setEnabled(connected && !busy);
    m_createAction->setEnabled(connected && !busy);
    m_error->setText(m_controller->error());
    m_error->setVisible(!m_controller->error().isEmpty());
    const auto selectedId = m_tree->currentIndex().data(Qt::UserRole).toString();
    m_model->clear();
    QHash<QString, QStandardItem *> groups;
    const auto sessions = m_controller->sessions();
    for (const auto &session : sessions)
    {
        auto *group = groups.value(session.workspace, nullptr);
        if (!group)
        {
            group = new QStandardItem(style()->standardIcon(QStyle::SP_DirIcon), session.workspace);
            group->setEditable(false);
            m_model->appendRow(group);
            groups.insert(session.workspace, group);
        }
        const auto text = session.name + QLatin1Char('\n') + processText(session, connected) +
                          QStringLiteral(" · ") + attachmentText(session);
        auto *item = new QStandardItem(style()->standardIcon(QStyle::SP_ComputerIcon), text);
        item->setData(session.id, Qt::UserRole);
        item->setToolTip(text);
        item->setEditable(false);
        group->appendRow(item);
        if (session.id == selectedId)
            m_tree->setCurrentIndex(item->index());
    }
    m_tree->expandAll();
    m_tree->setEnabled(connected && !busy);
    updateContext();
}
void ClientWindow::updateContext()
{
    const bool connected = m_controller->isConnected();
    const bool ready = connected && !m_controller->isBusy();
    const auto id = currentSessionId();
    m_context->setText(tr("会话工作区"));
    m_status->setText(connected ? tr("已连接 FakeAgent · 选择会话以查看控制权")
                                : tr("未连接 · 不发送会话操作"));
    bool detachable = false;
    bool terminable = false;
    const auto sessions = m_controller->sessions();
    for (const auto &session : sessions)
    {
        for (int index = 0; index < m_tabs->count(); ++index)
        {
            auto *page = m_tabs->widget(index);
            if (page->property("sessionId").toString() == session.id)
            {
                page->findChild<QLabel *>(QStringLiteral("sessionSummary"))
                    ->setText(tr("%1\n\n%2  ·  %3\n会话 ID：%4")
                                  .arg(session.name, processText(session, connected), attachmentText(session),
                                       session.id));
            }
        }
        if (session.id != id)
            continue;
        m_context->setText(tr("%1  /  %2  /  主工作区").arg(session.workspace, session.provider));
        m_context->setToolTip(m_context->text());
        m_status->setText(processText(session, connected) + QStringLiteral("  |  ") +
                          attachmentText(session));
        detachable = ready && session.attached;
        terminable = ready && session.running && session.attached && !session.readOnly;
    }
    m_detach->setEnabled(detachable);
    m_terminate->setEnabled(terminable);
    m_detachAction->setEnabled(detachable);
    m_terminateAction->setEnabled(terminable);
}
void ClientWindow::createSession()
{
    if (!m_controller->isConnected() || m_controller->isBusy())
        return;
    CreateSessionDialog dialog(m_controller, this);
    dialog.setObjectName(QStringLiteral("createSessionDialog"));
    dialog.setWindowTitle(tr("新建模拟会话"));
    dialog.resize(460, 260);
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *name = new QLineEdit(&dialog);
    name->setObjectName(QStringLiteral("sessionNameEdit"));
    name->setMaxLength(256);
    name->setPlaceholderText(tr("例如：接口联调"));
    form->addRow(tr("会话名称"), name);
    auto *workspace = new QComboBox(&dialog);
    workspace->setObjectName(QStringLiteral("workspaceCombo"));
    workspace->addItems(m_controller->workspaces());
    form->addRow(tr("工作区"), workspace);
    auto *provider = new QComboBox(&dialog);
    provider->setObjectName(QStringLiteral("providerCombo"));
    provider->addItem(QStringLiteral("FakeCli"));
    form->addRow(tr("Provider"), provider);
    layout->addLayout(form);
    auto *error = new QLabel(tr("仅创建模拟记录，不启动真实 CLI。"), &dialog);
    error->setObjectName(QStringLiteral("createErrorLabel"));
    error->setWordWrap(true);
    error->setTextFormat(Qt::PlainText);
    layout->addWidget(error);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    auto *confirm = buttons->button(QDialogButtonBox::Ok);
    confirm->setObjectName(QStringLiteral("createConfirmButton"));
    confirm->setText(tr("创建"));
    confirm->setProperty("role", "primary");
    confirm->setEnabled(false);
    auto *cancel = buttons->button(QDialogButtonBox::Cancel);
    cancel->setText(tr("取消"));
    layout->addWidget(buttons);
    connect(name, &QLineEdit::textChanged, &dialog,
            [this, name, confirm]
            {
                confirm->setEnabled(!name->text().trimmed().isEmpty() && !m_controller->isBusy() &&
                                    m_controller->isConnected());
            });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog,
            [this, name, workspace] { m_controller->createSession(name->text(), workspace->currentText()); });
    connect(m_controller, &ClientController::sessionCreated, &dialog, &QDialog::accept);
    connect(m_controller, &ClientController::changed, &dialog,
            [this, name, workspace, confirm, cancel, error]
            {
                const bool ready = !m_controller->isBusy() && m_controller->isConnected();
                name->setEnabled(ready);
                workspace->setEnabled(ready);
                confirm->setEnabled(ready && !name->text().trimmed().isEmpty());
                cancel->setEnabled(!m_controller->isBusy());
                error->setText(m_controller->isBusy() ? tr("正在创建模拟会话…") : m_controller->error());
            });
    name->setFocus();
    dialog.exec();
}
void ClientWindow::closeSession(int index)
{
    if (index < 0 || index >= m_tabs->count() || m_controller->isBusy())
        return;
    const auto id = m_tabs->widget(index)->property("sessionId").toString();
    if (!m_controller->isConnected())
    {
        removeSession(id);
        return;
    }
    QMessageBox choice(QMessageBox::Question, tr("关闭会话标签"),
                       tr("分离只关闭当前标签，模拟会话仍保持原状态。"), QMessageBox::NoButton, this);
    choice.setObjectName(QStringLiteral("closeSessionDialog"));
    auto *detach = choice.addButton(tr("分离"), QMessageBox::AcceptRole);
    auto *terminate = choice.addButton(tr("终止…"), QMessageBox::DestructiveRole);
    auto *cancel = choice.addButton(tr("取消"), QMessageBox::RejectRole);
    choice.setDefaultButton(detach);
    choice.setEscapeButton(cancel);
    const auto sessions = m_controller->sessions();
    for (const auto &session : sessions)
        if (session.id == id)
            terminate->setEnabled(session.running && session.attached && !session.readOnly);
    choice.exec();
    if (choice.clickedButton() == detach)
        m_controller->detachSession(id);
    else if (choice.clickedButton() == terminate)
        confirmTerminate(id);
}
void ClientWindow::confirmTerminate(const QString &id)
{
    if (id.isEmpty() || m_controller->isBusy() || !m_controller->isConnected())
        return;
    QString name;
    const auto sessions = m_controller->sessions();
    for (const auto &session : sessions)
        if (session.id == id)
            name = session.name;
    QMessageBox confirmation(QMessageBox::Warning, tr("确认终止会话"),
                             tr("确定终止“%1”？\n终止会结束该模拟会话，不能通过重新附加恢复运行。").arg(name),
                             QMessageBox::Yes | QMessageBox::No, this);
    confirmation.setObjectName(QStringLiteral("terminateSessionDialog"));
    confirmation.setTextFormat(Qt::PlainText);
    confirmation.button(QMessageBox::Yes)->setText(tr("终止"));
    confirmation.button(QMessageBox::No)->setText(tr("取消"));
    confirmation.setDefaultButton(QMessageBox::No);
    confirmation.setEscapeButton(QMessageBox::No);
    if (confirmation.exec() == QMessageBox::Yes)
        m_controller->terminateSession(id);
}
void ClientWindow::closeEvent(QCloseEvent *event)
{
    m_controller->disconnectAgent();
    event->accept();
}
}
