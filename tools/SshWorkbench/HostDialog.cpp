#include "HostDialog.h"
#include "WindowFrame.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSpinBox>
#include <QStyle>
#include <QThread>
#include <QToolButton>
#include <QVBoxLayout>
#include <memory>
#include <utility>

namespace sshworkbench
{
HostDialog::HostDialog(const HostProfile &host, QWidget *parent) : QDialog(parent), m_original(host)
{
    setWindowTitle(host.id.isEmpty() ? tr("添加家庭主机") : tr("编辑家庭主机"));
    setObjectName(QStringLiteral("workbenchDialog"));
    setMinimumSize(480, 400);
    const QSize available = screen()->availableGeometry().size() - QSize(48, 48);
    resize(QSize(680, 600).boundedTo(available));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);
    layout->addWidget(new sshsetup::WindowFrame(this, true));
    auto *scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("dialogScroll"));
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_fields = new QWidget(scroll);
    m_fields->setObjectName(QStringLiteral("dialogBody"));
    auto *content = new QVBoxLayout(m_fields);
    content->setContentsMargins(24, 20, 24, 20);
    content->setSpacing(12);
    auto *form = new QFormLayout;
    form->setHorizontalSpacing(20);
    form->setVerticalSpacing(12);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    content->addLayout(form);
    const auto field = [this, form](const QString &label, const QString &value, const char *name)
    {
        auto *edit = new QLineEdit(value, m_fields);
        edit->setObjectName(QString::fromLatin1(name));
        form->addRow(label, edit);
        return edit;
    };
    m_name = field(tr("主机名称"), host.name, "hostNameEdit");
    m_name->setMaxLength(80);
    auto *addressRow = new QWidget(m_fields);
    auto *addressLayout = new QHBoxLayout(addressRow);
    addressLayout->setContentsMargins(0, 0, 0, 0);
    addressLayout->setSpacing(8);
    m_host = new QLineEdit(host.connection.host, addressRow);
    m_host->setObjectName(QStringLiteral("hostAddressEdit"));
    addressLayout->addWidget(m_host, 1);
    auto *portLabel = new QLabel(tr("端口"), addressRow);
    addressLayout->addWidget(portLabel);
    m_port = new QSpinBox(addressRow);
    m_port->setObjectName(QStringLiteral("hostPortSpin"));
    m_port->setRange(1, 65535);
    m_port->setValue(host.connection.port);
    m_port->setFixedWidth(88);
    portLabel->setBuddy(m_port);
    addressLayout->addWidget(m_port);
    form->addRow(tr("家庭地址"), addressRow);
    m_user = field(tr("登录用户"), host.connection.user, "hostUserEdit");
    auto *identityRow = new QWidget(m_fields);
    auto *identityLayout = new QHBoxLayout(identityRow);
    identityLayout->setContentsMargins(0, 0, 0, 0);
    m_identity = new QLineEdit(host.connection.identityFile, identityRow);
    m_identity->setObjectName(QStringLiteral("hostIdentityEdit"));
    m_identity->setPlaceholderText(tr("本机私钥文件路径，仅保存路径"));
    m_identity->setMinimumWidth(0);
    identityLayout->addWidget(m_identity, 1);
    auto *browse = new QPushButton(tr("浏览…"), identityRow);
    identityLayout->addWidget(browse);
    form->addRow(tr("SSH 身份"), identityRow);
    connect(browse, &QPushButton::clicked, this,
            [this]
            {
                const QString path =
                    QFileDialog::getOpenFileName(this, tr("选择本机 SSH 私钥文件"), m_identity->text());
                if (!path.isEmpty())
                    m_identity->setText(path);
            });
    m_fingerprintToggle = new QToolButton(m_fields);
    m_fingerprintToggle->setObjectName(QStringLiteral("hostFingerprintToggle"));
    m_fingerprintToggle->setText(tr("主机指纹（可选）"));
    m_fingerprintToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_fingerprintToggle->setCheckable(true);
    m_fingerprintToggle->setArrowType(Qt::RightArrow);
    content->addWidget(m_fingerprintToggle, 0, Qt::AlignLeft);
    m_fingerprintFields = new QWidget(m_fields);
    auto *fingerprintLayout = new QVBoxLayout(m_fingerprintFields);
    fingerprintLayout->setContentsMargins(0, 0, 0, 0);
    fingerprintLayout->setSpacing(8);
    m_fingerprint = new QLineEdit(host.connection.hostFingerprint, m_fingerprintFields);
    m_fingerprint->setObjectName(QStringLiteral("hostFingerprintEdit"));
    m_fingerprint->setPlaceholderText(tr("SHA256:…；留空使用已信任的 known_hosts"));
    fingerprintLayout->addWidget(m_fingerprint);
    m_confirmed = new QCheckBox(tr("已核对家庭电脑显示的主机指纹"), m_fingerprintFields);
    m_confirmed->setObjectName(QStringLiteral("hostFingerprintConfirmed"));
    fingerprintLayout->addWidget(m_confirmed);
    content->addWidget(m_fingerprintFields);
    connect(m_fingerprintToggle, &QToolButton::toggled, this,
            [this](bool expanded)
            {
                m_fingerprintFields->setVisible(expanded);
                m_fingerprintToggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
            });
    m_fingerprintFields->setVisible(false);
    m_fingerprintToggle->setChecked(!host.connection.hostFingerprint.isEmpty());
    auto *actions = new QHBoxLayout;
    auto *import = new QPushButton(tr("导入连接信息…"), m_fields);
    import->setObjectName(QStringLiteral("hostImportButton"));
    auto *test = new QPushButton(tr("测试连接"), m_fields);
    test->setObjectName(QStringLiteral("hostTestButton"));
    actions->addWidget(import);
    actions->addWidget(test);
    m_copyDiagnostic = new QPushButton(tr("复制脱敏诊断"), m_fields);
    m_copyDiagnostic->setObjectName(QStringLiteral("hostCopyDiagnosticButton"));
    m_copyDiagnostic->setEnabled(false);
    m_copyDiagnostic->hide();
    actions->addWidget(m_copyDiagnostic);
    connect(m_copyDiagnostic, &QPushButton::clicked, this,
            [this] { QApplication::clipboard()->setText(m_diagnosticReport); });
    actions->addStretch();
    content->addLayout(actions);
    connect(import, &QPushButton::clicked, this,
            [this]
            {
                const QString path =
                    QFileDialog::getOpenFileName(this, tr("导入家庭端连接信息"), {}, tr("连接信息 (*.json)"));
                if (!path.isEmpty())
                    importConnection(path);
            });
    connect(test, &QPushButton::clicked, this, &HostDialog::checkConnection);
    auto *hint = new QLabel(tr("测试不会启动 Claude 或确认工作区信任。保存档案不代表连接已通过。"), m_fields);
    hint->setObjectName(QStringLiteral("dialogHint"));
    hint->setWordWrap(true);
    hint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    hint->setProperty("role", "muted");
    content->addWidget(hint);
    m_feedback = new QLabel(m_fields);
    m_feedback->setObjectName(QStringLiteral("feedback"));
    m_feedback->setTextFormat(Qt::PlainText);
    m_feedback->setWordWrap(true);
    m_feedback->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    content->addWidget(m_feedback);
    content->addStretch();
    scroll->setWidget(m_fields);
    layout->addWidget(scroll, 1);
    auto *footer = new QWidget(this);
    footer->setObjectName(QStringLiteral("dialogFooter"));
    auto *footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(24, 16, 24, 16);
    m_buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, footer);
    auto *save = m_buttons->button(QDialogButtonBox::Save);
    save->setText(tr("保存主机"));
    save->setObjectName(QStringLiteral("hostSaveButton"));
    save->setProperty("role", "primary");
    // 标准按钮可能已在按钮盒布局时完成样式初始化，设置角色后需要刷新。
    save->style()->unpolish(save);
    save->style()->polish(save);
    m_buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
    footerLayout->addWidget(m_buttons);
    layout->addWidget(footer);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    for (auto *edit : {m_host, m_user, m_identity, m_fingerprint})
        connect(edit, &QLineEdit::textChanged, this, &HostDialog::invalidateResult);
    connect(m_port, qOverload<int>(&QSpinBox::valueChanged), this, &HostDialog::invalidateResult);
    showFeedback(tr("尚未测试连接。"));
}

HostProfile HostDialog::host() const
{
    HostProfile candidate = m_original;
    candidate.name = m_name->text().trimmed();
    candidate.connection.host = m_host->text().trimmed();
    candidate.connection.user = m_user->text().trimmed();
    candidate.connection.port = m_port->value();
    candidate.connection.identityFile = m_identity->text().trimmed();
    candidate.connection.hostFingerprint = m_fingerprint->text().trimmed();
    return candidate;
}

void HostDialog::setSaveHandler(std::function<bool(const HostProfile &, QString &)> handler)
{
    m_saveHandler = std::move(handler);
}

bool HostDialog::validateFields()
{
    const auto candidate = host();
    QString error;
    if (candidate.name.isEmpty() || candidate.name.size() > 80)
        error = tr("请输入 1 至 80 个字符的主机名称。");
    for (const QChar character : candidate.name)
        if (character.category() == QChar::Other_Control)
            error = tr("主机名称不能包含控制字符。");
    if (error.isEmpty())
        sshsetup::validateProfile(candidate.connection, error);
    const auto &before = m_original.connection;
    const auto &after = candidate.connection;
    const bool changed = m_original.id.isEmpty() || before.host != after.host || before.port != after.port ||
                         before.hostFingerprint != after.hostFingerprint;
    if (error.isEmpty() && changed && !after.hostFingerprint.isEmpty() && !m_confirmed->isChecked())
        error = tr("请先核对家庭电脑显示的主机指纹，再勾选确认。");
    if (!error.isEmpty())
    {
        if (!after.hostFingerprint.isEmpty())
            m_fingerprintToggle->setChecked(true);
        showFeedback(error, true);
    }
    return error.isEmpty();
}

void HostDialog::done(int result)
{
    if (m_busy)
    {
        showFeedback(tr("连接测试正在进行，请等待测试结束后关闭。"));
        return;
    }
    if (result == QDialog::Accepted)
    {
        if (!validateFields())
            return;
        QString error;
        if (m_saveHandler && !m_saveHandler(host(), error))
        {
            showFeedback(error.isEmpty() ? tr("主机保存失败，请检查配置目录后重试。") : error, true);
            return;
        }
    }
    QDialog::done(result);
}

void HostDialog::invalidateResult()
{
    m_diagnosticReport.clear();
    m_copyDiagnostic->setEnabled(false);
    m_copyDiagnostic->hide();
    m_confirmed->setChecked(false);
    if (!m_fingerprint->text().isEmpty())
        m_fingerprintToggle->setChecked(true);
    showFeedback(tr("连接信息已修改，请重新测试。"));
}

bool HostDialog::importConnection(const QString &path)
{
    auto candidate = host().connection;
    QString error;
    if (!sshsetup::importConnectionInfo(path, candidate, error))
    {
        showFeedback(error, true);
        return false;
    }
    m_host->setText(candidate.host);
    m_user->setText(candidate.user);
    m_port->setValue(candidate.port);
    m_fingerprint->setText(candidate.hostFingerprint);
    if (!candidate.hostFingerprint.isEmpty())
        m_fingerprintToggle->setChecked(true);
    m_confirmed->setChecked(false);
    showFeedback(tr("已导入公开连接信息，本机身份未改变。请核对家庭端主机指纹。"));
    return true;
}

void HostDialog::checkConnection()
{
    if (m_busy || !validateFields())
        return;
    const auto profile = host().connection;
    auto result = std::make_shared<sshsetup::ConnectionResult>();
    m_busy = true;
    m_diagnosticReport.clear();
    m_copyDiagnostic->setEnabled(false);
    m_copyDiagnostic->hide();
    m_fields->setEnabled(false);
    m_buttons->setEnabled(false);
    showFeedback(tr("正在检查网络、SSH 服务、密钥认证与 Claude，请稍候…"));
    auto *thread = QThread::create([profile, result] { *result = sshsetup::testConnection(profile); });
    // 工作线程只捕获值；窗口意外销毁时也不会访问 UI，线程完成后独立回收。
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    connect(thread, &QThread::finished, this,
            [this, result]
            {
                m_busy = false;
                m_fields->setEnabled(true);
                m_buttons->setEnabled(true);
                m_diagnosticReport = sshsetup::formatConnectionReport(*result);
                m_copyDiagnostic->setEnabled(true);
                m_copyDiagnostic->show();
                showFeedback(m_diagnosticReport, !result->claudeAvailable);
            });
    thread->start();
}

void HostDialog::showFeedback(const QString &text, bool error)
{
    m_feedback->setText(text);
    m_feedback->setProperty("error", error);
    m_feedback->style()->unpolish(m_feedback);
    m_feedback->style()->polish(m_feedback);
    if (error)
    {
        auto *scroll = findChild<QScrollArea *>(QStringLiteral("dialogScroll"));
        scroll->ensureWidgetVisible(m_feedback);
    }
}
}
