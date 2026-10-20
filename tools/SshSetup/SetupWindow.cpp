#include "SetupWindow.h"
#include "WindowFrame.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScreen>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>
#include <memory>

#include <windows.h>

#include <shellapi.h>

namespace sshsetup
{
namespace
{
QLabel *label(const QString &text, QWidget *parent, const char *role = nullptr)
{
    auto *result = new QLabel(text, parent);
    result->setWordWrap(true);
    result->setMinimumWidth(0);
    result->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    result->setTextFormat(Qt::PlainText);
    result->setTextInteractionFlags(Qt::TextSelectableByMouse);
    if (role)
        result->setProperty("role", role);
    return result;
}

QWidget *card(QWidget *parent, QVBoxLayout *&layout)
{
    auto *result = new QWidget(parent);
    result->setProperty("role", "setupCard");
    result->setAttribute(Qt::WA_StyledBackground);
    layout = new QVBoxLayout(result);
    layout->setContentsMargins(22, 20, 22, 20);
    layout->setSpacing(16);
    return result;
}

void setIcon(QPushButton *button, const QString &name)
{
    button->setIcon(QIcon(QStringLiteral(":/resources/icon/setup/%1.svg").arg(name)));
    button->setIconSize(QSize(20, 20));
}

bool savePublicFile(const QString &path, const QByteArray &contents, QString &error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size() || !file.commit())
    {
        error = QObject::tr("保存失败：%1").arg(file.errorString());
        return false;
    }
    return true;
}
}

SetupWindow::SetupWindow(bool homeMode, const QString &profilePath, const QString &legacyConfigPath,
                         bool preview, QWidget *parent)
    : QWidget(parent), m_homeMode(homeMode), m_preview(preview), m_profilePath(profilePath),
      m_legacyConfigPath(legacyConfigPath)
{
    buildUi();
    setPage(0);
    if (!m_homeMode && !m_preview)
    {
        QString error;
        if (QFile::exists(m_profilePath))
        {
            if (!loadProfile(m_profilePath, m_profile, error))
                showFeedback(error, true);
        }
        else if (!m_legacyConfigPath.isEmpty() && QFile::exists(m_legacyConfigPath))
        {
            if (!importConnectionInfo(m_legacyConfigPath, m_profile, error))
                showFeedback(error, true);
            else
                showFeedback(tr("已读取旧启动器的主机配置。请在身份步骤选择已有密钥，以保留现有授权。"));
        }
        fillConnectionFields();
    }
}

SetupWindow::~SetupWindow()
{
    // 销毁时等待有限时长的服务调用退出，避免后台回调访问已销毁窗口。
    if (m_worker)
    {
        m_worker->wait();
        delete m_worker;
    }
}

void SetupWindow::buildUi()
{
    setWindowTitle(m_homeMode ? tr("OrbiDev · 家庭端配置工具") : tr("OrbiDev · 首次设置"));
    setMinimumSize(880, 620);
    const QSize available = QApplication::primaryScreen()->availableGeometry().size();
    resize(qMin(1080, available.width() - 48), qMin(740, available.height() - 64));
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(new WindowFrame(this));
    auto *body = new QHBoxLayout;
    body->setSpacing(0);
    auto *rail = new QWidget(this);
    rail->setObjectName(QStringLiteral("stepRail"));
    rail->setFixedWidth(m_homeMode ? 220 : 248);
    auto *railLayout = new QVBoxLayout(rail);
    railLayout->setContentsMargins(0, 30, 0, 24);
    const QStringList names =
        m_homeMode ? QStringList{tr("检查环境"), tr("导入公钥"), tr("应用配置"), tr("获取连接信息")}
                   : QStringList{tr("检查环境"), tr("创建本机身份"), tr("授权家庭电脑"), tr("测试连接")};
    railLayout->setSpacing(0);
    for (int index = 0; index < names.size(); ++index)
    {
        if (m_homeMode)
        {
            auto *step = label(QStringLiteral("%1   %2").arg(index + 1).arg(names.at(index)), rail, "step");
            step->setMinimumHeight(70);
            railLayout->addWidget(step);
            m_steps.append(step);
            continue;
        }
        auto *row = new QWidget(rail);
        row->setObjectName(QStringLiteral("setupStep%1").arg(index));
        row->setProperty("role", "setupStep");
        row->setAttribute(Qt::WA_StyledBackground);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(24, 18, 16, 18);
        rowLayout->setSpacing(16);
        auto *badge = label(QString::number(index + 1), row, "stepBadge");
        badge->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        badge->setTextInteractionFlags(Qt::NoTextInteraction);
        badge->setFixedSize(32, 32);
        badge->setAlignment(Qt::AlignCenter);
        rowLayout->addWidget(badge, 0, Qt::AlignTop);
        auto *texts = new QVBoxLayout;
        texts->setSpacing(6);
        texts->addWidget(label(names.at(index), row, "stepName"));
        auto *description = label({}, row, "muted");
        texts->addWidget(description);
        rowLayout->addLayout(texts, 1);
        railLayout->addWidget(row);
        m_stepRows.append(row);
        m_stepBadges.append(badge);
        m_stepDescriptions.append(description);
        if (index < names.size() - 1)
        {
            auto *lineRow = new QHBoxLayout;
            lineRow->setContentsMargins(40, 0, 0, 0);
            auto *line = new QWidget(rail);
            line->setProperty("role", "stepLine");
            line->setAttribute(Qt::WA_StyledBackground);
            line->setFixedSize(2, 18);
            lineRow->addWidget(line);
            lineRow->addStretch();
            railLayout->addLayout(lineRow);
            m_stepLines.append(line);
        }
    }
    railLayout->addStretch();
    if (m_homeMode)
        railLayout->addWidget(label(tr("OrbiDev\nSSH 工作台"), rail, "muted"));
    body->addWidget(rail);
    auto *content = new QWidget(this);
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(32, 28, 32, 20);
    m_pages = new QStackedWidget(content);
    m_pages->setObjectName(QStringLiteral("setupPages"));
    buildEnvironmentPage();
    if (m_homeMode)
    {
        buildImportPage();
        buildApplyPage();
        buildResultPage();
    }
    else
    {
        buildIdentityPage();
        buildAuthorizationPage();
        buildConnectionPage();
    }
    contentLayout->addWidget(m_pages, 1);
    m_feedback = label(tr("准备就绪。请先检查这台电脑的环境。"), content);
    m_feedback->setObjectName(QStringLiteral("feedback"));
    contentLayout->addWidget(m_feedback);
    body->addWidget(content, 1);
    root->addLayout(body, 1);
    auto *footerWidget = new QWidget(this);
    footerWidget->setObjectName(QStringLiteral("setupFooter"));
    footerWidget->setAttribute(Qt::WA_StyledBackground);
    auto *footer = new QHBoxLayout(footerWidget);
    footer->setContentsMargins(24, 16, 32, 16);
    m_back = new QPushButton(tr("上一步"), this);
    m_back->setObjectName(QStringLiteral("backButton"));
    m_next = new QPushButton(tr("下一步"), this);
    m_next->setObjectName(QStringLiteral("nextButton"));
    m_next->setProperty("role", "primary");
    footer->addWidget(m_back);
    footer->addStretch();
    footer->addWidget(m_next);
    root->addWidget(footerWidget);
    connect(m_pages, &QStackedWidget::currentChanged, this, [this] { updateActions(); });
    connect(m_back, &QPushButton::clicked, this, [this] { setPage(m_pages->currentIndex() - 1); });
    connect(m_next, &QPushButton::clicked, this, &SetupWindow::next);
}

QWidget *SetupWindow::makePage(const QString &title, const QString &description, QVBoxLayout *&layout)
{
    auto *page = new QWidget(m_pages);
    if (!m_homeMode)
        page->setObjectName(QStringLiteral("clientSetupPage"));
    layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 12);
    layout->setSpacing(18);
    layout->addWidget(label(title, page, "title"));
    layout->addWidget(label(description, page, "muted"));
    auto *scroll = new QScrollArea(m_pages);
    scroll->setWidgetResizable(true);
    scroll->setWidget(page);
    m_pages->addWidget(scroll);
    return page;
}

void SetupWindow::buildEnvironmentPage()
{
    QVBoxLayout *layout = nullptr;
    auto *page =
        makePage(tr("先检查连接环境"),
                 m_homeMode ? tr("在运行 Claude 的家庭电脑上使用此工具。默认只配置当前 Windows 用户。")
                            : tr("检查 OpenSSH 客户端与 Tailscale。网络在线与密钥授权将分别验证。"),
                 layout);
    QVBoxLayout *environmentLayout = layout;
    QWidget *environmentCard = page;
    if (!m_homeMode)
    {
        environmentCard = card(page, environmentLayout);
        environmentLayout->addWidget(label(tr("本机连接组件"), environmentCard, "section"));
        layout->addWidget(environmentCard);
    }
    m_environmentText = label(m_homeMode ? tr("尚未检查\n\nOpenSSH：待检测\nTailscale：待检测")
                                         : tr("OpenSSH 客户端：待检测\n\nTailscale 网络：待检测"),
                              environmentCard);
    environmentLayout->addWidget(m_environmentText);
    auto *check = new QPushButton(tr("检查环境"), page);
    check->setObjectName(QStringLiteral("inspectButton"));
    if (!m_homeMode)
        check->setProperty("role", "primary");
    environmentLayout->addWidget(check, 0, Qt::AlignLeft);
    connect(check, &QPushButton::clicked, this, &SetupWindow::inspectEnvironment);
    if (m_homeMode)
    {
        auto *copyReport = new QPushButton(tr("复制脱敏诊断"), page);
        copyReport->setObjectName(QStringLiteral("copyEnvironmentButton"));
        layout->addWidget(copyReport, 0, Qt::AlignLeft);
        connect(copyReport, &QPushButton::clicked, this,
                [this]
                {
                    if (!m_preview)
                        QApplication::clipboard()->setText(
                            m_environmentReport.isEmpty() ? tr("家庭环境尚未检查。") : m_environmentReport);
                });
        auto *optional = new QPushButton(tr("打开 Windows 可选功能"), page);
        auto *startService = new QPushButton(tr("启动 SSH 服务…"), page);
        startService->setObjectName(QStringLiteral("startSshServiceButton"));
        layout->addWidget(startService, 0, Qt::AlignLeft);
        connect(
            startService, &QPushButton::clicked, this,
            [this]
            {
                if (m_preview)
                    return;
                const QString script =
                    QDir(QApplication::applicationDirPath()).filePath(QStringLiteral("Start-SshService.cmd"));
                if (!QFileInfo::exists(script) || !QDesktopServices::openUrl(QUrl::fromLocalFile(script)))
                    showFeedback(tr("无法打开启动脚本，请使用完整解压后的安装包。"), true);
                else
                    showFeedback(tr("请在脚本窗口确认启动；完成后点击“检查环境”刷新结果。"));
            });
        layout->addWidget(optional, 0, Qt::AlignLeft);
        connect(optional, &QPushButton::clicked, this,
                [this]
                {
                    if (!m_preview &&
                        !QDesktopServices::openUrl(QUrl(QStringLiteral("ms-settings:optionalfeatures"))))
                        showFeedback(tr("无法打开系统设置，请从 Windows 设置进入可选功能。"), true);
                });
        auto *elevate = new QPushButton(tr("以管理员权限重新打开"), page);
        layout->addWidget(elevate, 0, Qt::AlignLeft);
        connect(elevate, &QPushButton::clicked, this,
                [this]
                {
                    if (m_preview)
                        return;
                    if (QMessageBox::question(
                            this, tr("重新打开配置工具"),
                            tr("请使用同一个 Windows 账户确认 "
                               "UAC。新窗口不会自动应用授权，需要重新导入公钥。\n是否继续？")) !=
                        QMessageBox::Yes)
                        return;
                    const QString executable = QDir::toNativeSeparators(QApplication::applicationFilePath());
                    SHELLEXECUTEINFOW request{};
                    request.cbSize = sizeof(request);
                    request.lpVerb = L"runas";
                    request.lpFile = reinterpret_cast<LPCWSTR>(executable.utf16());
                    request.lpParameters = L"--home";
                    request.nShow = SW_SHOWNORMAL;
                    if (!ShellExecuteExW(&request))
                        showFeedback(
                            tr("未启动管理员窗口（取消或系统错误 %1）。当前配置未改变。").arg(GetLastError()),
                            true);
                    else
                        showFeedback(tr("已请求打开管理员窗口。请先核对新窗口中的当前用户。"));
                });
        layout->addWidget(
            label(tr("已有 SSH 服务只追加公钥，不更改默认 "
                     "Shell，不重启服务。\n未安装：在可选功能中添加 OpenSSH 服务器，安装失败保留系统错误。\n"
                     "已安装但未运行：点击“启动 SSH 服务…”并确认。\n"
                     "服务运行仍无法连接：检查监听地址与 Tailscale 访问规则，勿直接开放公网端口。"),
                  page, "muted"));
    }
    layout->addStretch();
}

void SetupWindow::buildIdentityPage()
{
    QVBoxLayout *layout = nullptr;
    auto *page = makePage(tr("为这台电脑准备 SSH 身份"),
                          tr("使用专用 Ed25519 密钥。已有轻量版密钥也可以继续使用，无需重新授权。"), layout);
    QVBoxLayout *identityLayout = nullptr;
    auto *identityCard = card(page, identityLayout);
    identityLayout->addWidget(label(tr("本机 SSH 身份"), identityCard, "section"));
    m_identityText = label(tr("尚未准备身份。创建后将在这里显示公钥指纹与本机保存位置。"), identityCard);
    identityLayout->addWidget(m_identityText);
    layout->addWidget(identityCard);
    auto *create = new QPushButton(tr("创建或检查专用身份"), page);
    create->setObjectName(QStringLiteral("createIdentityButton"));
    auto *existing = new QPushButton(tr("使用已有密钥…"), page);
    create->setProperty("role", "primary");
    auto *identityButtons = new QHBoxLayout;
    identityButtons->addWidget(create);
    identityButtons->addWidget(existing);
    identityButtons->addStretch();
    identityLayout->addLayout(identityButtons);
    connect(create, &QPushButton::clicked, this, [this] { prepareIdentity(); });
    connect(existing, &QPushButton::clicked, this,
            [this]
            {
                const QString path =
                    QFileDialog::getOpenFileName(this, tr("选择已有 SSH 私钥（仅在本机读取）"),
                                                 QDir::homePath() + QStringLiteral("/.ssh"));
                if (!path.isEmpty())
                    prepareIdentity(path);
            });
    layout->addWidget(label(
        tr("专用密钥不设置口令，使用 Windows 文件权限保护。不会覆盖已有 home_dev；权限检查失败时停止。"),
        page, "muted"));
    layout->addStretch();
}

void SetupWindow::buildAuthorizationPage()
{
    QVBoxLayout *layout = nullptr;
    auto *page =
        makePage(tr("将这台电脑授权给家庭电脑"), tr("请在家庭电脑的配置工具中导入这里的公钥。"), layout);
    QVBoxLayout *summaryLayout = nullptr;
    auto *summaryCard = card(page, summaryLayout);
    m_publicSummary = label(tr("本机设备：准备身份后显示\n\n公钥指纹：尚未准备"), summaryCard);
    summaryLayout->addWidget(m_publicSummary);
    layout->addWidget(summaryCard);
    m_publicKeyPreview = new QPlainTextEdit(page);
    m_publicKeyPreview->setObjectName(QStringLiteral("publicKeyPreview"));
    m_publicKeyPreview->setReadOnly(true);
    m_publicKeyPreview->setMaximumHeight(90);
    m_publicKeyPreview->setPlaceholderText(
        tr("准备身份后显示 ssh-ed25519 开头的公钥；SHA256 指纹不能用于授权。"));
    auto *showKey = new QPushButton(tr("查看公钥正文"), summaryCard);
    showKey->setObjectName(QStringLiteral("showPublicKeyButton"));
    showKey->setCheckable(true);
    m_publicKeyPreview->setVisible(false);
    connect(showKey, &QPushButton::toggled, this,
            [this, showKey](bool visible)
            {
                m_publicKeyPreview->setVisible(visible);
                showKey->setText(visible ? tr("收起公钥正文") : tr("查看公钥正文"));
            });
    summaryLayout->addWidget(m_publicKeyPreview);
    auto *buttons = new QHBoxLayout;
    auto *copy = new QPushButton(tr("复制公钥"), page);
    auto *exportKey = new QPushButton(tr("导出公钥"), page);
    copy->setObjectName(QStringLiteral("copyPublicKeyButton"));
    exportKey->setObjectName(QStringLiteral("exportPublicKeyButton"));
    setIcon(copy, QStringLiteral("copy"));
    setIcon(exportKey, QStringLiteral("export"));
    buttons->addWidget(copy);
    buttons->addWidget(exportKey);
    buttons->addWidget(showKey);
    buttons->addStretch();
    summaryLayout->addLayout(buttons);
    connect(copy, &QPushButton::clicked, this,
            [this]
            {
                if (m_identity.publicKey.isEmpty())
                    return;
                QApplication::clipboard()->setText(m_identity.publicKey);
                showFeedback(tr("公钥已复制。请到家庭端配置工具中粘贴。"));
            });
    connect(exportKey, &QPushButton::clicked, this, &SetupWindow::exportPublicKey);
    QVBoxLayout *instructions = nullptr;
    auto *instructionCard = card(page, instructions);
    instructions->addWidget(label(tr("接下来，请在家庭电脑上完成以下操作"), instructionCard, "section"));
    const QStringList actions{tr("打开 OrbiDev 家庭端配置工具"), tr("粘贴或导入公钥，核对指纹后应用授权"),
                              tr("导出连接信息，返回这里测试连接")};
    for (int index = 0; index < actions.size(); ++index)
    {
        auto *row = new QHBoxLayout;
        row->setSpacing(16);
        auto *number = label(QString::number(index + 1), instructionCard, "instructionNumber");
        number->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        number->setTextInteractionFlags(Qt::NoTextInteraction);
        number->setFixedSize(28, 28);
        number->setAlignment(Qt::AlignCenter);
        row->addWidget(number);
        row->addWidget(label(actions.at(index), instructionCard), 1);
        instructions->addLayout(row);
    }
    layout->addWidget(instructionCard);
    layout->addStretch();
    layout->addWidget(
        label(tr("私钥仅保存在本机，不会导出或传输。家庭端无需保持配置工具运行。"), page, "muted"));
}

void SetupWindow::buildConnectionPage()
{
    QVBoxLayout *layout = nullptr;
    auto *page = makePage(
        tr("测试家庭电脑连接"),
        tr("分别检查网络、SSH 服务、密钥认证与 Claude。首次连接请核对家庭端显示的主机指纹。"), layout);
    QVBoxLayout *connectionLayout = nullptr;
    auto *connectionCard = card(page, connectionLayout);
    layout->addWidget(connectionCard);
    auto *form = new QFormLayout;
    form->setSpacing(14);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_host = new QLineEdit(page);
    m_host->setObjectName(QStringLiteral("hostEdit"));
    m_host->setPlaceholderText(tr("Tailscale 地址或 SSH 主机名"));
    m_user = new QLineEdit(page);
    m_port = new QSpinBox(page);
    m_port->setRange(1, 65535);
    m_port->setValue(22);
    m_fingerprint = new QLineEdit(page);
    m_fingerprint->setPlaceholderText(tr("SHA256:…（已信任的 known_hosts 可留空）"));
    form->addRow(tr("家庭主机"), m_host);
    form->addRow(tr("登录用户"), m_user);
    form->addRow(tr("SSH 端口"), m_port);
    form->addRow(tr("预期主机指纹"), m_fingerprint);
    connectionLayout->addLayout(form);
    m_fingerprintConfirmed = new QCheckBox(tr("已核对家庭电脑显示的主机指纹"), page);
    connectionLayout->addWidget(m_fingerprintConfirmed);
    auto *buttons = new QHBoxLayout;
    auto *import = new QPushButton(tr("导入连接信息…"), page);
    auto *test = new QPushButton(tr("测试连接"), page);
    test->setProperty("role", "primary");
    test->setObjectName(QStringLiteral("testConnectionButton"));
    m_copyConnectionReport = new QPushButton(tr("复制脱敏诊断"), page);
    m_copyConnectionReport->setObjectName(QStringLiteral("copyConnectionReportButton"));
    m_copyConnectionReport->setEnabled(false);
    buttons->addWidget(import);
    buttons->addWidget(test);
    buttons->addWidget(m_copyConnectionReport);
    buttons->addStretch();
    connectionLayout->addLayout(buttons);
    connect(import, &QPushButton::clicked, this, &SetupWindow::importConnection);
    connect(test, &QPushButton::clicked, this, &SetupWindow::checkConnection);
    connect(m_copyConnectionReport, &QPushButton::clicked, this,
            [this] { QApplication::clipboard()->setText(m_connectionReport); });
    const auto invalidate = [this]
    {
        m_connectionVerified = false;
        m_connectionFailed = false;
        m_connectionReport.clear();
        m_copyConnectionReport->setEnabled(false);
        if (m_pages->currentIndex() == 3)
            showFeedback(tr("连接信息已修改，请重新测试。"));
        updateActions();
    };
    connect(m_host, &QLineEdit::textChanged, this, invalidate);
    connect(m_user, &QLineEdit::textChanged, this, invalidate);
    connect(m_port, QOverload<int>::of(&QSpinBox::valueChanged), this, invalidate);
    connect(m_fingerprint, &QLineEdit::textChanged, this,
            [this, invalidate]
            {
                m_fingerprintConfirmed->setChecked(false);
                invalidate();
            });
    connect(m_fingerprintConfirmed, &QCheckBox::toggled, this, invalidate);
    layout->addStretch();
    layout->addWidget(label(
        tr("测试不会启动 Claude 会话，也不会替你确认工作区信任。主机指纹变化时会停止连接。"), page, "muted"));
}

void SetupWindow::buildImportPage()
{
    QVBoxLayout *layout = nullptr;
    auto *page = makePage(tr("授权一台公司电脑"), tr("只需导入公钥，无需保持此工具运行。"), layout);
    m_importSummary = label(tr("请先检查当前家庭用户"), page);
    layout->addWidget(m_importSummary);
    layout->addWidget(label(tr("公司电脑公钥"), page, "section"));
    m_publicInput = new QPlainTextEdit(page);
    m_publicInput->setObjectName(QStringLiteral("publicKeyInput"));
    m_publicInput->setPlaceholderText(tr("粘贴以 ssh-ed25519 开头的公钥。不要粘贴私钥。"));
    m_publicInput->setMaximumHeight(160);
    layout->addWidget(m_publicInput);
    auto *import = new QPushButton(tr("从 .pub 文件导入…"), page);
    layout->addWidget(import, 0, Qt::AlignRight);
    connect(import, &QPushButton::clicked, this,
            [this]
            {
                const QString path =
                    QFileDialog::getOpenFileName(this, tr("导入公钥"), {}, tr("公钥 (*.pub)"));
                if (path.isEmpty())
                    return;
                QFile file(path);
                if (!file.open(QIODevice::ReadOnly) || file.size() > 16384)
                {
                    showFeedback(tr("无法读取公钥文件，或文件超过 16 KiB。"), true);
                    return;
                }
                m_publicInput->setPlainText(QString::fromUtf8(file.readAll()));
            });
    connect(m_publicInput, &QPlainTextEdit::textChanged, this,
            [this]
            {
                m_normalizedKey.clear();
                m_importFingerprint.clear();
                m_next->setEnabled(!m_busy && !m_publicInput->toPlainText().trimmed().isEmpty());
            });
    layout->addWidget(
        label(tr("仅接受公钥，不接受私钥或附加启动命令。下一步会校验并显示公钥指纹。"), page, "muted"));
    layout->addStretch();
    layout->addWidget(
        label(tr("将追加授权，保留已有密钥；不会更改默认终端或重启现有 SSH 服务。"), page, "muted"));
}

void SetupWindow::buildApplyPage()
{
    QVBoxLayout *layout = nullptr;
    auto *page = makePage(tr("核对授权变更"), tr("请核对目标用户、授权位置和公钥指纹后应用。"), layout);
    m_applySummary = label({}, page);
    layout->addWidget(m_applySummary);
    m_applyConfirmed = new QCheckBox(tr("我已核对以上用户和公钥指纹"), page);
    connect(m_applyConfirmed, &QCheckBox::toggled, this, [this] { updateActions(); });
    layout->addWidget(m_applyConfirmed);
    layout->addStretch();
    layout->addWidget(
        label(tr("重复导入按指纹去重。只有点击“应用授权”才写入文件；写入或权限检查失败会返回错误。"), page,
              "muted"));
}

void SetupWindow::buildResultPage()
{
    QVBoxLayout *layout = nullptr;
    auto *page = makePage(tr("获取家庭电脑连接信息"),
                          tr("将这些公开信息带回公司电脑，在首次设置的最后一步导入。"), layout);
    m_resultSummary = label({}, page);
    layout->addWidget(m_resultSummary);
    auto *exportInfo = new QPushButton(tr("导出连接信息…"), page);
    layout->addWidget(exportInfo, 0, Qt::AlignLeft);
    connect(exportInfo, &QPushButton::clicked, this, &SetupWindow::exportConnection);
    layout->addStretch();
    layout->addWidget(
        label(tr("连接信息不包含私钥。请通过可信方式核对主机指纹。完成后可以关闭此工具。"), page, "muted"));
}

void SetupWindow::setPage(int page)
{
    m_pages->setCurrentIndex(qBound(0, page, 3));
    for (int index = 0; index < m_steps.size(); ++index)
    {
        m_steps[index]->setProperty("role", index == m_pages->currentIndex() ? "currentStep" : "step");
        m_steps[index]->style()->unpolish(m_steps[index]);
        m_steps[index]->style()->polish(m_steps[index]);
    }
    updateActions();
}

void SetupWindow::updateActions()
{
    const int page = m_pages->currentIndex();
    m_back->setEnabled(page > 0 && !m_busy && !m_authorized);
    m_next->setText(page == 3 ? (m_homeMode ? tr("完成") : tr("保存配置并完成"))
                              : (m_homeMode && page == 2 ? tr("应用授权") : tr("下一步")));
    bool enabled = false;
    if (page == 0)
        enabled = m_environmentReady;
    else if (m_homeMode)
        enabled = page == 1 ? !m_publicInput->toPlainText().trimmed().isEmpty()
                            : (page == 2 ? m_applyConfirmed->isChecked() && !m_normalizedKey.isEmpty()
                                         : m_authorized);
    else
        enabled = page == 3 ? m_connectionVerified : !m_identity.publicKey.isEmpty();
    m_next->setEnabled(enabled && !m_busy && !m_preview);
    if (!m_homeMode && page == 2)
        m_next->setText(tr("已完成授权，下一步"));
    updateSteps();
}

void SetupWindow::updateSteps()
{
    if (m_homeMode)
        return;
    const bool completed[] = {m_environmentReady, !m_identity.publicKey.isEmpty(),
                              m_authorizationAcknowledged, m_connectionVerified};
    const bool failed[] = {m_environmentFailed, m_identityFailed, false, m_connectionFailed};
    const QStringList pending{tr("检查连接组件"), tr("准备专用密钥"), tr("导入本机公钥"), tr("验证远程访问")};
    const QStringList done{tr("客户端可用"), tr("身份已准备好"),
                           m_connectionVerified ? tr("授权已通过连接验证") : tr("已声明授权，待验证"),
                           tr("连接测试通过")};
    for (int index = 0; index < m_stepRows.size(); ++index)
    {
        const bool current = index == m_pages->currentIndex();
        const QString state = failed[index]      ? QStringLiteral("failed")
                              : completed[index] ? QStringLiteral("complete")
                                                 : QStringLiteral("pending");
        auto *row = m_stepRows[index];
        row->setProperty("current", current);
        row->setProperty("state", state);
        auto *badge = m_stepBadges[index];
        badge->setProperty("state", state);
        badge->setProperty("current", current);
        badge->setText(QString::number(index + 1));
        if (completed[index] && !failed[index])
            badge->setPixmap(QIcon(QStringLiteral(":/resources/icon/common/check.svg")).pixmap(20, 20));
        if (failed[index])
            badge->setPixmap(QIcon(QStringLiteral(":/resources/icon/setup/error.svg")).pixmap(20, 20));
        const QString description = m_busy && current  ? tr("正在处理…")
                                    : failed[index]    ? tr("未通过，请重试")
                                    : completed[index] ? done.at(index)
                                                       : pending.at(index);
        m_stepDescriptions[index]->setText(description);
        row->setAccessibleName(tr("步骤 %1：%2").arg(index + 1).arg(description));
        for (auto *widget : {row, static_cast<QWidget *>(badge)})
        {
            widget->style()->unpolish(widget);
            widget->style()->polish(widget);
            widget->update();
        }
        if (index < m_stepLines.size())
        {
            auto *line = m_stepLines[index];
            line->setProperty("complete", completed[index] && !failed[index]);
            line->style()->unpolish(line);
            line->style()->polish(line);
            line->update();
        }
    }
}

void SetupWindow::invalidateIdentityResults()
{
    m_authorizationAcknowledged = false;
    m_connectionVerified = false;
    m_connectionFailed = false;
    m_connectionReport.clear();
    m_copyConnectionReport->setEnabled(false);
}

void SetupWindow::showFeedback(const QString &message, bool error)
{
    m_feedback->setText(message);
    m_feedback->setProperty("error", error);
    m_feedback->style()->unpolish(m_feedback);
    m_feedback->style()->polish(m_feedback);
}

void SetupWindow::runOperation(std::function<void()> work, std::function<void()> finished)
{
    if (m_busy || m_preview)
        return;
    m_busy = true;
    m_pages->setEnabled(false);
    updateActions();
    showFeedback(tr("正在处理，请稍候…"));
    m_worker = QThread::create(std::move(work));
    auto *thread = m_worker;
    connect(thread, &QThread::finished, this,
            [this, thread, finished = std::move(finished)]
            {
                m_worker = nullptr;
                m_busy = false;
                m_pages->setEnabled(true);
                finished();
                updateActions();
                thread->deleteLater();
            });
    thread->start();
}

void SetupWindow::inspectEnvironment()
{
    if (m_homeMode)
    {
        auto result = std::make_shared<HomeEnvironment>();
        runOperation([result] { *result = inspectHomeEnvironment(); },
                     [this, result]
                     {
                         m_homeEnvironment = *result;
                         m_environmentReport = homeEnvironmentReport(*result);
                         m_environmentReady = result->sshAvailable && result->error.isEmpty();
                         m_environmentText->setText(
                             tr("家庭主机：%7\n当前用户：%1\nSSH 服务：%2\n配置文件：%3\n"
                                "服务程序：%8\n\n家庭地址：%4\nSSH 端口：%5\n授权文件：%6\n%9")
                                 .arg(result->userName, result->serviceState,
                                      QDir::toNativeSeparators(result->configurationPath), result->host)
                                 .arg(result->port)
                                 .arg(result->authorizedKeysPath, result->hostName,
                                      QDir::toNativeSeparators(result->serviceExecutable),
                                      result->listenWarning));
                         m_importSummary->setText(
                             tr("家庭主机：%1\n当前家庭用户：%2").arg(result->hostName, result->userName));
                         showFeedback(m_environmentReady ? tr("环境检查完成，可以导入公钥。") : result->error,
                                      !m_environmentReady);
                     });
        return;
    }
    auto result = std::make_shared<EnvironmentStatus>();
    runOperation(
        [result] { *result = inspectClientEnvironment(); },
        [this, result]
        {
            m_environmentReady = result->sshAvailable;
            m_environmentFailed = !m_environmentReady;
            m_environmentText->setText(tr("OpenSSH 客户端：%1\n\nTailscale：%2\n\n%3")
                                           .arg(result->sshAvailable ? tr("可用") : tr("未找到"),
                                                result->tailscaleOnline ? tr("在线") : tr("尚未确认在线"),
                                                result->details));
            showFeedback(m_environmentReady ? tr("可以准备本机身份。连接测试仍会单独验证远端可达性。")
                                            : tr("请先安装 Windows OpenSSH 客户端。"),
                         !m_environmentReady);
        });
}

void SetupWindow::prepareIdentity(const QString &existingPath)
{
    struct Result
    {
        Identity identity;
        QString error;
        bool success = false;
    };
    auto result = std::make_shared<Result>();
    const QString directory =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/identity");
    runOperation(
        [result, directory, existingPath]
        {
            if (existingPath.isEmpty() && !QDir().mkpath(QFileInfo(directory).absolutePath()))
            {
                result->error = QObject::tr("无法创建本机身份的父目录。");
                return;
            }
            result->success = existingPath.isEmpty()
                                  ? ensureIdentity(directory, result->identity, result->error)
                                  : inspectIdentity(existingPath, result->identity, result->error);
        },
        [this, result]
        {
            invalidateIdentityResults();
            m_identityFailed = !result->success;
            if (!result->success)
            {
                m_identity = {};
                m_publicKeyPreview->clear();
                m_publicSummary->setText(tr("尚未准备公钥"));

                m_identityText->setText(tr("身份尚未准备好。请解决下方问题后重试。"));
                showFeedback(result->error, true);
                return;
            }
            m_identity = result->identity;
            m_publicKeyPreview->setPlainText(m_identity.publicKey);
            m_profile.identityFile = m_identity.privateKeyPath;

            m_identityText->setText(tr("本机身份已准备好\n\n公钥指纹：%1\n\n私钥文件：%2")
                                        .arg(m_identity.fingerprint, m_identity.privateKeyPath));
            m_publicSummary->setText(tr("本机设备：%1\n\n公钥指纹：%2")
                                         .arg(qEnvironmentVariable("COMPUTERNAME"), m_identity.fingerprint));
            showFeedback(tr("密钥检查完成，私钥不会展示或传输。"));
        });
}

void SetupWindow::validateImport()
{
    struct Result
    {
        QString key, fingerprint, error;
        bool success = false;
    };
    auto result = std::make_shared<Result>();
    const QString input = m_publicInput->toPlainText();
    runOperation(
        [result, input]
        { result->success = validatePublicKey(input, result->key, result->fingerprint, result->error); },
        [this, result]
        {
            if (!result->success)
            {
                showFeedback(result->error, true);
                return;
            }
            m_normalizedKey = result->key;
            m_importFingerprint = result->fingerprint;
            m_applyConfirmed->setChecked(false);
            m_applySummary->setText(
                tr("目标用户：%1\n\n授权文件：%2\n\n公钥指纹：%"
                   "3\n\n变更：追加此公钥并检查文件权限，保留已有授权。%4")
                    .arg(m_homeEnvironment.userName, m_homeEnvironment.authorizedKeysPath,
                         m_importFingerprint,
                         m_homeEnvironment.administrator
                             ? tr("\n\n注意：管理员账户使用共享授权文件，可能对其他管理员账户同样生效。")
                             : QString()));
            setPage(2);
            showFeedback(tr("公钥校验通过。尚未写入任何授权文件。"));
        });
}

void SetupWindow::applyAuthorization()
{
    struct Result
    {
        bool success = false;
        QString error;
    };
    auto result = std::make_shared<Result>();
    const QString publicKey = m_normalizedKey;
    const HomeEnvironment expected = m_homeEnvironment;
    runOperation([result, publicKey, expected]
                 { result->success = authorizeCurrentUser(publicKey, expected, result->error); },
                 [this, result]
                 {
                     if (!result->success)
                     {
                         showFeedback(result->error, true);
                         return;
                     }
                     m_authorized = true;
                     m_resultSummary->setText(
                         tr("授权已应用\n\n家庭地址：%1\n\n登录用户：%2\n\nSSH 端口：%3\n\n主机指纹：%4")
                             .arg(m_homeEnvironment.host, m_homeEnvironment.userName)
                             .arg(m_homeEnvironment.port)
                             .arg(m_homeEnvironment.hostFingerprint));
                     setPage(3);
                     showFeedback(tr("公钥已授权。实际远程登录请在公司客户端进行测试。"));
                 });
}

void SetupWindow::fillConnectionFields()
{
    m_host->setText(m_profile.host);
    m_user->setText(m_profile.user);
    m_port->setValue(m_profile.port);
    m_fingerprint->setText(m_profile.hostFingerprint);
}

void SetupWindow::checkConnection()
{
    if (!m_fingerprint->text().trimmed().isEmpty() && !m_fingerprintConfirmed->isChecked())
    {
        showFeedback(tr("请先核对并确认家庭电脑显示的主机指纹。"), true);
        return;
    }
    m_profile.host = m_host->text().trimmed();
    m_profile.user = m_user->text().trimmed();
    m_profile.port = m_port->value();
    m_profile.hostFingerprint = m_fingerprint->text().trimmed();
    auto result = std::make_shared<ConnectionResult>();
    const auto profile = m_profile;
    m_connectionVerified = false;
    m_connectionReport.clear();
    m_copyConnectionReport->setEnabled(false);
    runOperation([result, profile] { *result = testConnection(profile); },
                 [this, result]
                 {
                     m_connectionVerified = result->authenticated && result->claudeAvailable;
                     m_connectionFailed = !m_connectionVerified;
                     m_connectionReport = formatConnectionReport(*result);
                     m_copyConnectionReport->setEnabled(true);
                     showFeedback(m_connectionReport, !m_connectionVerified);
                 });
}

void SetupWindow::importConnection()
{
    const QString path =
        QFileDialog::getOpenFileName(this, tr("导入家庭连接信息"), {}, tr("连接信息 (*.json)"));
    if (path.isEmpty())
        return;
    QString error;
    if (!importConnectionInfo(path, m_profile, error))
    {
        showFeedback(error, true);
        return;
    }
    fillConnectionFields();
    showFeedback(tr("已导入连接信息，请核对家庭端主机指纹后测试。"));
}

void SetupWindow::exportPublicKey()
{
    if (m_identity.publicKey.isEmpty() || m_preview)
        return;
    const QString path =
        QFileDialog::getSaveFileName(this, tr("导出公钥"), QStringLiteral("orbidev.pub"), tr("公钥 (*.pub)"));
    if (path.isEmpty())
        return;
    const QFileInfo destination(path);
    const QFileInfo privateFile(m_identity.privateKeyPath);
    const QString destinationPath =
        destination.exists() ? destination.canonicalFilePath() : destination.absoluteFilePath();
    const QString privatePath =
        privateFile.exists() ? privateFile.canonicalFilePath() : privateFile.absoluteFilePath();
    if (destinationPath.compare(privatePath, Qt::CaseInsensitive) == 0)
    {
        showFeedback(tr("公钥不能覆盖本机私钥，请选择其他文件名。"), true);
        return;
    }
    QString error;
    const bool saved = savePublicFile(path, m_identity.publicKey.toUtf8() + '\n', error);
    showFeedback(saved ? tr("公钥已导出。") : error, !saved);
}

void SetupWindow::exportConnection()
{
    if (!m_authorized || m_preview)
        return;
    const QString path = QFileDialog::getSaveFileName(
        this, tr("导出连接信息"), QStringLiteral("orbidev-home.json"), tr("连接信息 (*.json)"));
    if (path.isEmpty())
        return;
    const QJsonObject object{{QStringLiteral("version"), 1},
                             {QStringLiteral("host"), m_homeEnvironment.host},
                             {QStringLiteral("user"), m_homeEnvironment.userName},
                             {QStringLiteral("port"), m_homeEnvironment.port},
                             {QStringLiteral("hostFingerprint"), m_homeEnvironment.hostFingerprint}};
    QString error;
    const bool saved = savePublicFile(path, QJsonDocument(object).toJson(), error);
    showFeedback(saved ? tr("连接信息已导出，不含任何私钥。") : error, !saved);
}

void SetupWindow::next()
{
    if (m_busy || m_preview || !m_next->isEnabled())
        return;
    const int page = m_pages->currentIndex();
    if (m_homeMode && page == 1)
    {
        validateImport();
        return;
    }
    if (m_homeMode && page == 2)
    {
        applyAuthorization();
        return;
    }
    if (page == 3)
    {
        QString error;
        if (!m_homeMode && !saveProfile(m_profilePath, m_profile, error))
        {
            showFeedback(error, true);
            return;
        }
        close();
        return;
    }
    if (!m_homeMode && page == 2)
        m_authorizationAcknowledged = true;
    setPage(page + 1);
    if (!m_homeMode && page == 0 && m_identity.publicKey.isEmpty() &&
        (m_legacyConfigPath.isEmpty() || !QFile::exists(m_legacyConfigPath)))
        prepareIdentity(m_profile.identityFile);
}

void SetupWindow::closeEvent(QCloseEvent *event)
{
    if (m_busy)
    {
        showFeedback(tr("正在完成当前操作，请稍候再关闭窗口。"));
        event->ignore();
        return;
    }
    QWidget::closeEvent(event);
}

void SetupWindow::setPreviewPage(int page)
{
    if (!m_preview)
        return;
    if (m_homeMode)
    {
        m_environmentText->setText(
            tr("家庭主机：家庭电脑（界面预览）\n当前用户：homeuser\nSSH 服务：运行中（示例）\n"
               "配置文件：系统 SSH 配置目录\n服务程序：当前服务安装位置\n\n"
               "监听提示：配置仅含回环地址，其他电脑不能通过该地址连接。\n"
               "配置不等于实际监听；修改后请由管理员核对生效情况。"));
        m_importSummary->setText(tr("家庭主机：家庭电脑（界面预览）\n当前家庭用户：homeuser"));
        m_applySummary->setText(
            tr("目标用户：homeuser（示例）\n\n授权文件：用户目录中的 "
               ".ssh/authorized_keys\n\n公钥指纹：校验后显示\n\n变更：追加公钥并检查权限，保留已有授权。"));
        m_resultSummary->setText(tr("家庭地址、登录用户、端口和主机指纹将在实际配置完成后显示。"));
    }
    else
    {
        m_publicSummary->setText(tr("本机设备：公司电脑（界面预览）\n\n公钥指纹：生成并检查身份后显示"));
    }
    setPage(page);
    m_pages->setEnabled(false);
    m_back->setEnabled(false);
    showFeedback(tr("界面预览：未运行环境检测、生成密钥或修改 SSH 配置。"));
}
}
