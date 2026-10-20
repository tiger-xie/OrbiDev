#include "HostDialog.h"
#include "SetupTheme.h"

#include <QCheckBox>
#include <QClipboard>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTemporaryDir>
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
    void diagnosticCopyIsInvalidatedByTargetChanges()
    {
        HostDialog dialog(sample());
        QVERIFY(!dialog.m_copyDiagnostic->isEnabled());
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
        dialog.m_fingerprint->setText(fingerprint());
        QVERIFY(!dialog.validateFields());
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
