#include "RemoteDirectoryDialog.h"
#include "WindowFrame.h"
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScreen>
#include <QShowEvent>
#include <QSplitter>
#include <QStyle>
#include <QVBoxLayout>

namespace sshworkbench
{
namespace
{
QString directoryKey(QString path)
{
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (path.size() > 3 && path.endsWith(QLatin1Char('/')))
        path.chop(1);
    return path;
}
int cacheCost(const RemoteDirectoryResult &result)
{
    qint64 bytes =
        512 + 2LL * (result.currentPath.size() + result.parentPath.size() + result.homePath.size());
    for (const auto *entries : {&result.drives, &result.directories})
        for (const auto &entry : *entries)
            bytes += 128 + 2LL * (entry.name.size() + entry.path.size());
    // 每项至少占 256 KiB 配额，令总量上限同时约束为最多 32 个目录。
    return qMax(256, int((bytes + 1023) / 1024));
}
}
RemoteDirectoryDialog::RemoteDirectoryDialog(const HostProfile &host, const QString &initialPath,
                                             QWidget *parent)
    : QDialog(parent), m_host(host), m_initialPath(initialPath)
{
    setObjectName(QStringLiteral("remoteDirectoryDialog"));
    setProperty("workbenchDialog", true);
    setWindowTitle(tr("选择远程目录"));
    setMinimumSize(480, 380);
    resize(QSize(760, 580).boundedTo(screen()->availableGeometry().size() - QSize(48, 48)));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);
    layout->addWidget(new sshsetup::WindowFrame(this, true));
    auto *body = new QWidget(this);
    body->setObjectName(QStringLiteral("dialogBody"));
    auto *content = new QVBoxLayout(body);
    content->setContentsMargins(20, 16, 20, 16);
    content->setSpacing(12);
    auto *hostLabel = new QLabel(tr("家庭主机：%1").arg(host.name), body);
    hostLabel->setTextFormat(Qt::PlainText);
    hostLabel->setWordWrap(true);
    content->addWidget(hostLabel);
    m_location = new QLineEdit(initialPath, body);
    m_location->setObjectName(QStringLiteral("remoteLocation"));
    m_location->setAccessibleName(tr("远程当前位置，回车跳转"));
    m_location->setPlaceholderText(tr("用户主目录；也可输入远程路径并按回车"));
    m_location->setMaxLength(1024);
    content->addWidget(m_location);
    auto *navigation = new QHBoxLayout;
    m_up = new QPushButton(tr("上一级"), body);
    m_home = new QPushButton(tr("用户主目录"), body);
    m_refresh = new QPushButton(tr("刷新"), body);
    m_up->setObjectName(QStringLiteral("remoteUp"));
    m_home->setObjectName(QStringLiteral("remoteHome"));
    m_refresh->setObjectName(QStringLiteral("remoteRefresh"));
    for (auto *button : {m_up, m_home, m_refresh})
    {
        button->setAutoDefault(false);
        navigation->addWidget(button);
    }
    navigation->addStretch();
    content->addLayout(navigation);
    auto *splitter = new QSplitter(body);
    m_drives = new QListWidget(splitter);
    m_drives->setObjectName(QStringLiteral("remoteDrives"));
    m_drives->setAccessibleName(tr("家庭电脑磁盘"));
    m_directories = new QListWidget(splitter);
    m_directories->setObjectName(QStringLiteral("remoteDirectories"));
    m_directories->setAccessibleName(tr("当前目录中的文件夹，双击进入"));
    for (auto *list : {m_drives, m_directories})
    {
        list->setMinimumWidth(80);
        list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        list->setTextElideMode(Qt::ElideMiddle);
    }
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({160, 480});
    content->addWidget(splitter, 1);
    m_feedback = new QLabel(tr("准备读取远程目录…"), body);
    m_feedback->setObjectName(QStringLiteral("remoteDirectoryFeedback"));
    m_feedback->setTextFormat(Qt::PlainText);
    m_feedback->setWordWrap(true);
    content->addWidget(m_feedback);
    layout->addWidget(body, 1);
    auto *footer = new QWidget(this);
    footer->setObjectName(QStringLiteral("dialogFooter"));
    auto *footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(20, 14, 20, 16);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, footer);
    m_select = buttons->button(QDialogButtonBox::Ok);
    m_select->setText(tr("选择此目录"));
    m_select->setObjectName(QStringLiteral("selectRemoteDirectory"));
    m_select->setProperty("role", "primary");
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
    footerLayout->addWidget(buttons);
    layout->addWidget(footer);
    m_service = new RemoteDirectoryService(this);
    connect(m_service, &RemoteDirectoryService::finished, this, &RemoteDirectoryDialog::applyResult);
    connect(m_service, &RemoteDirectoryService::busyChanged, this, &RemoteDirectoryDialog::setBusy);
    connect(this, &QDialog::finished, m_service, &RemoteDirectoryService::cancel);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this,
            [this]
            {
                if (m_result.success && !m_service->isBusy())
                    accept();
            });
    connect(m_location, &QLineEdit::returnPressed, this,
            [this] { requestDirectory(m_location->text().trimmed()); });
    connect(m_location, &QLineEdit::textEdited, this,
            [this]
            {
                m_result.success = false;
                m_select->setEnabled(false);
                m_feedback->setText(tr("按回车或刷新读取输入的目录，再确认选择。"));
            });
    connect(m_home, &QPushButton::clicked, this, [this] { requestDirectory({}); });
    connect(m_up, &QPushButton::clicked, this, [this] { requestDirectory(m_result.parentPath); });
    connect(m_refresh, &QPushButton::clicked, this,
            [this] { requestDirectory(m_location->text().trimmed(), true); });
    connect(m_drives, &QListWidget::itemClicked, this,
            [this](QListWidgetItem *item) { requestDirectory(item->data(Qt::UserRole).toString()); });
    connect(m_drives, &QListWidget::itemActivated, this,
            [this](QListWidgetItem *item) { requestDirectory(item->data(Qt::UserRole).toString()); });
    connect(m_directories, &QListWidget::itemActivated, this,
            [this](QListWidgetItem *item) { requestDirectory(item->data(Qt::UserRole).toString()); });
    setBusy(true);
}

void RemoteDirectoryDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    if (!m_started)
    {
        m_started = true;
        requestDirectory(m_initialPath);
    }
}

QString RemoteDirectoryDialog::selectedPath() const
{
    return result() == QDialog::Accepted && m_result.success ? m_result.currentPath : QString();
}

void RemoteDirectoryDialog::setBusy(bool busy)
{
    m_location->setEnabled(!busy);
    m_drives->setEnabled(!busy);
    m_directories->setEnabled(!busy);
    m_home->setEnabled(!busy);
    m_refresh->setEnabled(!busy);
    m_up->setEnabled(!busy && m_result.success && !m_result.parentPath.isEmpty());
    m_select->setEnabled(!busy && m_result.success);
}

void RemoteDirectoryDialog::requestDirectory(const QString &path, bool refresh)
{
    if (m_service->isBusy())
        return;
    const QString key = directoryKey(path.isEmpty() ? m_homePath : path);
    if (refresh)
        m_cache.remove(key);
    else if (const auto *cached = m_cache.object(key))
    {
        // applyResult 会更新缓存，先复制，避免缓存替换使引用失效。
        const auto snapshot = *cached;
        applyResult(snapshot);
        m_feedback->setText(tr("已显示缓存目录；如家庭端目录有变化，请点击“刷新”。"));
        return;
    }
    m_result.success = false;
    m_location->setText(path);
    m_directories->clear();
    m_feedback->setText(tr("正在读取远程目录…可取消关闭，不影响已有终端。"));
    setBusy(true);
    m_service->start(m_host.connection, path);
}

void RemoteDirectoryDialog::applyResult(const RemoteDirectoryResult &result)
{
    m_result = result;
    m_feedback->setToolTip(result.preparationMilliseconds < 0
                               ? QString()
                               : tr("上次查询：准备与主机校验 %1 ms；SSH 连接与远端查询 %2。")
                                     .arg(result.preparationMilliseconds)
                                     .arg(result.queryMilliseconds < 0
                                              ? tr("未执行")
                                              : tr("%1 ms").arg(result.queryMilliseconds)));
    m_directories->clear();
    if (result.success)
    {
        m_homePath = result.homePath;
        m_cache.insert(directoryKey(result.currentPath), new RemoteDirectoryResult(result),
                       cacheCost(result));
        m_location->setText(result.currentPath);
        m_location->setToolTip(result.currentPath);
        m_drives->clear();
        const QIcon folder(QStringLiteral(":/resources/icon/setup/folder.svg"));
        const auto fill = [&folder](QListWidget *list, const QVector<RemoteDirectoryEntry> &entries)
        {
            for (const auto &entry : entries)
            {
                auto *item = new QListWidgetItem(folder, entry.name, list);
                item->setData(Qt::UserRole, entry.path);
                item->setToolTip(entry.path);
            }
        };
        fill(m_drives, result.drives);
        fill(m_directories, result.directories);
        m_feedback->setText(result.directories.isEmpty() ? tr("此目录没有子文件夹，可以直接选择此目录。")
                                                         : tr("双击文件夹进入；确认将选择上方当前目录。"));
    }
    else
        m_feedback->setText(result.error + tr("\n可刷新重试或返回用户主目录；认证问题请检查连接设置。"));
    setBusy(m_service->isBusy());
}
}
