#include "HostDialog.h"
#include "SetupTheme.h"

#include <QCheckBox>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest>

namespace sshworkbench
{
class HostDialogTests : public QObject
{
    Q_OBJECT
private:
    static HostProfile sample()
    {
        HostProfile profile;
        profile.id = QStringLiteral("host-one");
        profile.name = QStringLiteral("家庭测试机");
        profile.connection.host = QStringLiteral("home.example");
        profile.connection.user = QStringLiteral("developer");
        profile.connection.identityFile = QStringLiteral("C:/keys/identity");
        return profile;
    }
    static QString fingerprint()
    {
        return QStringLiteral("SHA256:") + QString(43, QLatin1Char('A'));
    }

private slots:
    void longFieldsFitAndFooterRemainsVisible()
    {
        sshsetup::applySetupTheme(*qApp);
        auto profile = sample();
        profile.name = QStringLiteral("家庭开发电脑与长期项目验证环境");
        profile.connection.identityFile = QStringLiteral("C:/Users/example/") +
                                          QStringLiteral("very-long-folder/").repeated(12) +
                                          QStringLiteral("identity");
        HostDialog dialog(profile);
        dialog.show();
        QTest::qWait(100);
        auto *scroll = dialog.findChild<QScrollArea *>(QStringLiteral("dialogScroll"));
        auto *footer = dialog.findChild<QWidget *>(QStringLiteral("dialogFooter"));
        QVERIFY(scroll && footer);
        QVERIFY(dialog.m_fingerprintFields->isHidden());
        QVERIFY(dialog.m_copyDiagnostic->isHidden());
        QCOMPARE(scroll->verticalScrollBar()->maximum(), 0);
        QVERIFY(scroll->viewport()->rect().contains(
            QRect(dialog.m_feedback->mapTo(scroll->viewport(), QPoint()), dialog.m_feedback->size())));
        QCOMPARE(dialog.m_feedback->text(), QStringLiteral("尚未测试连接。"));
        const auto directory = qEnvironmentVariable("ORBIDEV_UI06_HOST_SCREENSHOT");
        if (!directory.isEmpty())
        {
            QVERIFY(QDir().mkpath(directory));
            QTest::qWait(100);
            QVERIFY(dialog.grab().save(directory + QStringLiteral("/host.png")));
        }
        dialog.resize(480, 440);
        QTest::qWait(100);
        QCOMPARE(dialog.width(), 480);
        QVERIFY(dialog.rect().contains(QRect(footer->mapTo(&dialog, QPoint()), footer->size())));
        QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
        QVERIFY(scroll->verticalScrollBar()->maximum() > 0);
        QVERIFY(dialog.m_buttons->button(QDialogButtonBox::Save)->isVisible());
        if (!directory.isEmpty())
        {
            QVERIFY(dialog.grab().save(directory + QStringLiteral("/host-narrow.png")));
            dialog.showFeedback(QStringLiteral("主机保存失败。请检查配置目录权限后重试。"), true);
            QTest::qWait(100);
            QVERIFY(dialog.grab().save(directory + QStringLiteral("/host-save-error.png")));
        }
    }
    void saveFailureKeepsFieldsAndDialogOpen()
    {
        HostDialog dialog(sample());
        int attempts = 0;
        dialog.setSaveHandler(
            [&attempts](const HostProfile &, QString &error)
            {
                ++attempts;
                error = QStringLiteral("测试目录不可写，请选择其他位置。");
                return attempts > 1;
            });
        dialog.show();
        dialog.m_name->setText(QStringLiteral("保留修改"));
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        dialog.accept();
        QCOMPARE(attempts, 1);
        QCOMPARE(accepted.count(), 0);
        QVERIFY(dialog.isVisible());
        QCOMPARE(dialog.m_name->text(), QStringLiteral("保留修改"));
        QVERIFY(dialog.m_feedback->text().contains(QStringLiteral("不可写")));
        dialog.accept();
        QCOMPARE(attempts, 2);
        QCOMPARE(accepted.count(), 1);
    }
    void invalidFieldsAndCancelDoNotSave()
    {
        HostDialog dialog(sample());
        int attempts = 0;
        dialog.setSaveHandler(
            [&attempts](const HostProfile &, QString &)
            {
                ++attempts;
                return true;
            });
        dialog.m_name->clear();
        dialog.accept();
        QCOMPARE(attempts, 0);
        dialog.reject();
        QCOMPARE(attempts, 0);
    }
    void diagnosticCopyIsInvalidatedByTargetChanges()
    {
        HostDialog dialog(sample());
        QVERIFY(!dialog.m_copyDiagnostic->isEnabled());
        QVERIFY(dialog.m_copyDiagnostic->isHidden());
        dialog.m_diagnosticReport = QStringLiteral("网络：失败\n后续：未执行");
        dialog.m_copyDiagnostic->setEnabled(true);
        // 系统剪贴板可能短暂被其他应用占用，重试真实按钮操作但不跳过失败。
        QElapsedTimer deadline;
        deadline.start();
        bool copied = false;
        do
        {
            dialog.m_copyDiagnostic->click();
            copied = QApplication::clipboard()->text() == dialog.m_diagnosticReport;
            if (!copied)
                QTest::qWait(100);
        } while (!copied && deadline.elapsed() < 5000);
        QVERIFY2(copied, "系统剪贴板不可用，未验证复制诊断。");
        dialog.m_host->setText(QStringLiteral("another.example"));
        QVERIFY(dialog.m_diagnosticReport.isEmpty());
        QVERIFY(!dialog.m_copyDiagnostic->isEnabled());
        QVERIFY(dialog.m_copyDiagnostic->isHidden());
        QCOMPARE(dialog.m_feedback->text(), QStringLiteral("连接信息已修改，请重新测试。"));
    }
    void fieldsPreserveStableIdentity()
    {
        const auto original = sample();
        HostDialog dialog(original);
        const auto screenshot = qEnvironmentVariable("ORBIDEV_HOST_SCREENSHOT");
        if (!screenshot.isEmpty())
        {
            sshsetup::applySetupTheme(*qApp);
            dialog.show();
            QTest::qWait(100);
            QVERIFY(dialog.grab().save(screenshot));
        }
        dialog.m_name->setText(QStringLiteral("重新命名"));
        dialog.m_port->setValue(2222);
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        dialog.accept();
        QCOMPARE(accepted.count(), 1);
        QCOMPARE(dialog.host().id, original.id);
        QCOMPARE(dialog.host().name, QStringLiteral("重新命名"));
        QCOMPARE(dialog.host().connection.port, 2222);
        QCOMPARE(dialog.host().connection.identityFile, original.connection.identityFile);
    }
    void invalidFieldsCannotSave()
    {
        HostDialog dialog(sample());
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        dialog.m_name->clear();
        dialog.accept();
        QCOMPARE(accepted.count(), 0);
        QVERIFY(dialog.m_feedback->property("error").toBool());
        dialog.m_name->setText(QStringLiteral("主机"));
        dialog.m_host->setText(QStringLiteral("host -o option"));
        dialog.accept();
        QCOMPARE(accepted.count(), 0);
    }
    void changedFingerprintRequiresConfirmation()
    {
        HostDialog dialog(sample());
        QVERIFY(!dialog.m_fingerprintToggle->isChecked());
        dialog.m_fingerprint->setText(fingerprint());
        QVERIFY(dialog.m_fingerprintToggle->isChecked());
        dialog.m_fingerprintToggle->setChecked(false);
        QVERIFY(!dialog.validateFields());
        QVERIFY(dialog.m_fingerprintToggle->isChecked());
        QVERIFY(!dialog.m_confirmed->isChecked());
        dialog.m_confirmed->setChecked(true);
        QVERIFY(dialog.validateFields());
        dialog.m_host->setText(QStringLiteral("other.example"));
        QVERIFY(!dialog.m_confirmed->isChecked());
        QVERIFY(!dialog.validateFields());
    }
    void existingFingerprintRemainsTrustedButNewHostRequiresConfirmation()
    {
        auto profile = sample();
        profile.connection.hostFingerprint = fingerprint();
        HostDialog existing(profile);
        QVERIFY(existing.m_fingerprintToggle->isChecked());
        QVERIFY(!existing.m_confirmed->isChecked());
        QVERIFY(existing.validateFields());
        profile.id.clear();
        HostDialog added(profile);
        QVERIFY(!added.validateFields());
    }
    void importRetainsPrivateKeyPathAndRequiresFingerprintReview()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QFile file(directory.filePath(QStringLiteral("connection.json")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QJsonObject object{{QStringLiteral("version"), 1},
                                 {QStringLiteral("host"), QStringLiteral("new.example")},
                                 {QStringLiteral("user"), QStringLiteral("family")},
                                 {QStringLiteral("port"), 2222},
                                 {QStringLiteral("hostFingerprint"), fingerprint()}};
        file.write(QJsonDocument(object).toJson());
        file.close();
        const auto original = sample();
        HostDialog dialog(original);
        QVERIFY(dialog.importConnection(file.fileName()));
        QVERIFY(dialog.m_fingerprintToggle->isChecked());
        QCOMPARE(dialog.host().id, original.id);
        QCOMPARE(dialog.host().connection.host, QStringLiteral("new.example"));
        QCOMPARE(dialog.host().connection.identityFile, original.connection.identityFile);
        QCOMPARE(dialog.host().connection.port, 2222);
        QVERIFY(!dialog.m_confirmed->isChecked());
        QVERIFY(!dialog.validateFields());
        const auto before = dialog.host();
        QVERIFY(!dialog.importConnection(directory.filePath(QStringLiteral("missing.json"))));
        QCOMPARE(dialog.host().connection.host, before.connection.host);
    }
    void busyDialogDoesNotCloseOrAccept()
    {
        HostDialog dialog(sample());
        dialog.m_busy = true;
        QSignalSpy finished(&dialog, &QDialog::finished);
        dialog.reject();
        dialog.accept();
        QCOMPARE(finished.count(), 0);
        dialog.m_busy = false;
        dialog.reject();
        QCOMPARE(finished.count(), 1);
    }
};
}
QTEST_MAIN(sshworkbench::HostDialogTests)
#include "HostDialogTests.moc"
