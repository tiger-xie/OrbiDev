#include "HostDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStyle>
#include <QThread>
#include <QVBoxLayout>
#include <memory>

namespace sshworkbench
{
HostDialog::HostDialog(const HostProfile &host, QWidget *parent) : QDialog(parent), m_original(host)
{
    setWindowTitle(host.id.isEmpty() ? tr("添加家庭主机") : tr("编辑家庭主机"));
    resize(680, 660);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 20, 24, 20);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    m_fields = new QWidget(scroll);
    auto *content = new QVBoxLayout(m_fields);
    auto *description = new QLabel(tr("保存每台电脑的连接信息。已有终端保持创建时的目标。"), m_fields);
    description->setWordWrap(true);
    description->setProperty("role", "muted");
    content->addWidget(description);
    auto *form = new QFormLayout;
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
    m_host = field(tr("家庭地址"), host.connection.host, "hostAddressEdit");
    m_user = field(tr("登录用户"), host.connection.user, "hostUserEdit");
    m_port = new QSpinBox(m_fields);
    m_port->setObjectName(QStringLiteral("hostPortSpin"));
    m_port->setRange(1, 65535);
    m_port->setValue(host.connection.port);
    form->addRow(tr("SSH 端口"), m_port);
    auto *identityRow = new QWidget(m_fields);
    auto *identityLayout = new QHBoxLayout(identityRow);
    identityLayout->setContentsMargins(0, 0, 0, 0);
    m_identity = new QLineEdit(host.connection.identityFile, identityRow);
    m_identity->setObjectName(QStringLiteral("hostIdentityEdit"));
    m_identity->setPlaceholderText(tr("本机私钥文件路径，仅保存路径"));
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
    m_fingerprint = field(tr("主机指纹"), host.connection.hostFingerprint, "hostFingerprintEdit");
    m_fingerprint->setPlaceholderText(tr("SHA256:…；留空使用已信任的 known_hosts"));
    m_confirmed = new QCheckBox(tr("已核对家庭电脑显示的主机指纹"), m_fields);
    m_confirmed->setObjectName(QStringLiteral("hostFingerprintConfirmed"));
    content->addWidget(m_confirmed);
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
    hint->setWordWrap(true);
    hint->setProperty("role", "muted");
    content->addWidget(hint);
    m_feedback = new QLabel(m_fields);
    m_feedback->setObjectName(QStringLiteral("feedback"));
    m_feedback->setTextFormat(Qt::PlainText);
    m_feedback->setWordWrap(true);
    content->addWidget(m_feedback);
    content->addStretch();
    scroll->setWidget(m_fields);
    layout->addWidget(scroll, 1);
    m_buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    m_buttons->button(QDialogButtonBox::Save)->setText(tr("保存主机"));
    m_buttons->button(QDialogButtonBox::Save)->setObjectName(QStringLiteral("hostSaveButton"));
    m_buttons->button(QDialogButtonBox::Save)->setProperty("role", "primary");
    m_buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
    layout->addWidget(m_buttons);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    for (auto *edit : {m_host, m_user, m_identity, m_fingerprint})
        connect(edit, &QLineEdit::textChanged, this, &HostDialog::invalidateResult);
    connect(m_port, qOverload<int>(&QSpinBox::valueChanged), this, &HostDialog::invalidateResult);
    showFeedback(tr("网络：未测试　SSH 服务：未测试\n密钥认证：未测试　Claude：未测试"));
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
        showFeedback(error, true);
    return error.isEmpty();
}

void HostDialog::done(int result)
{
    if (m_busy)
    {
        showFeedback(tr("连接测试正在进行，请等待测试结束后关闭。"));
        return;
    }
    if (result == QDialog::Accepted && !validateFields())
        return;
    QDialog::done(result);
}

void HostDialog::invalidateResult()
{
    m_diagnosticReport.clear();
    m_copyDiagnostic->setEnabled(false);
    m_confirmed->setChecked(false);
    showFeedback(
        tr("连接信息已修改，请重新测试。\n网络：未测试　SSH 服务：未测试\n密钥认证：未测试　Claude：未测试"));
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
}
}
