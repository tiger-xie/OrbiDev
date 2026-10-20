#include "SetupTheme.h"
#include "SetupWindow.h"

#include <QApplication>
#include <QCheckBox>
#include <QFile>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QtTest>

class SetupWindowTests final : public QObject
{
    Q_OBJECT
private slots:
    void homeStepsAndConfirmationFollowActualResults()
    {
        QTemporaryDir directory;
        sshsetup::SetupWindow window(true, directory.filePath(QStringLiteral("connection.json")));
        window.setPage(2);
        QCOMPARE(window.m_stepRows.size(), 4);
        for (auto *step : window.m_stepRows)
            QCOMPARE(step->property("state").toString(), QStringLiteral("pending"));
        QVERIFY(!window.m_applyConfirmed->isChecked());
        QVERIFY(!window.m_next->isEnabled());
        window.m_environmentReady = true;
        window.m_normalizedKey = QStringLiteral("公开测试占位");
        window.m_importFingerprint = QStringLiteral("SHA256:旧校验");
        window.m_applyConfirmed->setChecked(true);
        window.updateActions();
        QVERIFY(window.m_next->isEnabled());
        window.m_publicInput->setPlainText(QStringLiteral("新输入"));
        QVERIFY(window.m_normalizedKey.isEmpty());
        QVERIFY(window.m_importFingerprint.isEmpty());
        QVERIFY(!window.m_applyConfirmed->isChecked());
        QVERIFY(!window.m_next->isEnabled());
        QVERIFY(!window.m_applySummary->text().contains(QStringLiteral("旧校验")));
        window.m_applyConfirmed->setChecked(true);
        window.invalidateImportResults();
        QVERIFY(!window.m_applyConfirmed->isChecked());
        window.m_importFailed = true;
        window.updateActions();
        QCOMPARE(window.m_stepRows[1]->property("state").toString(), QStringLiteral("failed"));
        window.m_authorized = true;
        window.setPage(3);
        QVERIFY(!window.m_back->isEnabled());
        QVERIFY(window.m_next->isEnabled());
        QVERIFY(window.m_exportConnection->isEnabled());
        QVERIFY(!QFile::exists(directory.filePath(QStringLiteral("connection.json"))));
    }

    void homePreviewIsInertAndFitsNarrowWindow()
    {
        QTemporaryDir directory;
        sshsetup::SetupWindow window(true, directory.filePath(QStringLiteral("connection.json")), {}, true);
        window.resize(880, 620);
        window.show();
        for (int page = 0; page < 4; ++page)
        {
            window.setPreviewPage(page);
            QTest::qWait(30);
            QVERIFY(!window.m_next->isEnabled());
            QVERIFY(!window.m_pages->isEnabled());
            for (auto *step : window.m_stepRows)
                QCOMPARE(step->property("state").toString(), QStringLiteral("pending"));
            auto *scroll = qobject_cast<QScrollArea *>(window.m_pages->currentWidget());
            QVERIFY(scroll);
            QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
            QVERIFY(window.rect().contains(
                QRect(window.m_next->mapTo(&window, QPoint()), window.m_next->size())));
        }
        window.setPreviewPage(2);
        window.m_applySummary->setText(QString(500, QLatin1Char('A')));
        QTest::qWait(30);
        QCOMPARE(
            qobject_cast<QScrollArea *>(window.m_pages->currentWidget())->horizontalScrollBar()->maximum(),
            0);
        QVERIFY(!window.m_worker);
        QVERIFY(!window.m_authorized);
        QVERIFY(window.m_importFingerprint.isEmpty());
        QVERIFY(!QFile::exists(directory.filePath(QStringLiteral("connection.json"))));
    }

    void publicKeyValidationDoesNotApplyAuthorization()
    {
        QTemporaryDir directory;
        sshsetup::SetupWindow window(true, directory.filePath(QStringLiteral("connection.json")));
        window.m_homeEnvironment.userName = QStringLiteral("test-user");
        window.m_homeEnvironment.authorizedKeysPath = directory.filePath(QStringLiteral("authorized_keys"));
        window.m_environmentReady = true;
        window.setPage(1);
        const QByteArray blob = QByteArray::fromHex("0000000b") + QByteArray("ssh-ed25519") +
                                QByteArray::fromHex("00000020") + QByteArray(32, 'a');
        const QString key = QStringLiteral("ssh-ed25519 ") + QString::fromLatin1(blob.toBase64()) +
                            QStringLiteral(" company-device");
        window.m_publicInput->setPlainText(key);
        window.next();
        QTRY_VERIFY_WITH_TIMEOUT(!window.m_busy, 15000);
        QCOMPARE(window.m_pages->currentIndex(), 2);
        QVERIFY(window.m_importFingerprint.startsWith(QStringLiteral("SHA256:")));
        QVERIFY(window.m_importDetails->text().contains(window.m_importFingerprint));
        QVERIFY(!window.m_applyConfirmed->isChecked());
        QVERIFY(!window.m_authorized);
        QVERIFY(!QFile::exists(window.m_homeEnvironment.authorizedKeysPath));
        window.setPage(1);
        window.m_publicInput->setPlainText(QStringLiteral("command=\"echo bad\" ") + key);
        window.next();
        QTRY_VERIFY_WITH_TIMEOUT(!window.m_busy, 15000);
        QCOMPARE(window.m_pages->currentIndex(), 1);
        QVERIFY(window.m_importFailed);
        QVERIFY(window.m_importFingerprint.isEmpty());
        QVERIFY(!QFile::exists(window.m_homeEnvironment.authorizedKeysPath));
    }

    void initTestCase()
    {
        sshsetup::applySetupTheme(*qApp);
    }

    void sharedThemeResourcesAreAvailable()
    {
        for (const auto *name : {"base", "window", "setup", "workbench"})
        {
            QFile file(QStringLiteral(":/resources/style/%1.qss").arg(QString::fromLatin1(name)));
            QVERIFY(file.open(QIODevice::ReadOnly));
            const auto style = QString::fromUtf8(file.readAll());
            QVERIFY(!style.trimmed().isEmpty());
            QVERIFY(qApp->styleSheet().contains(style));
        }
        for (const auto *name : {"app", "check", "chevron-down"})
        {
            const QIcon icon(QStringLiteral(":/resources/icon/common/%1.svg").arg(QString::fromLatin1(name)));
            QVERIFY(!icon.pixmap(24, 24).isNull());
        }
        for (const auto *name : {"copy", "export", "error", "user", "folder"})
        {
            const QIcon icon(QStringLiteral(":/resources/icon/setup/%1.svg").arg(QString::fromLatin1(name)));
            QVERIFY(!icon.pixmap(24, 24).isNull());
        }
        QVERIFY(!qApp->windowIcon().isNull());
    }

    void clientStepsFollowResultsRatherThanPageIndex()
    {
        QTemporaryDir directory;
        sshsetup::SetupWindow window(false, directory.filePath(QStringLiteral("connection.json")));
        window.setPage(3);
        for (int index = 0; index < 4; ++index)
            QCOMPARE(window.m_stepRows[index]->property("state").toString(), QStringLiteral("pending"));
        window.m_environmentReady = true;
        window.m_identity.publicKey = QStringLiteral("仅用于状态测试的占位内容");
        window.setPage(2);
        QCOMPARE(window.m_stepRows[0]->property("state").toString(), QStringLiteral("complete"));
        QCOMPARE(window.m_stepRows[1]->property("state").toString(), QStringLiteral("complete"));
        QCOMPARE(window.m_stepRows[2]->property("state").toString(), QStringLiteral("pending"));
        window.next();
        QVERIFY(window.m_authorizationAcknowledged);
        QVERIFY(!window.m_connectionVerified);
        QVERIFY(window.m_stepDescriptions[2]->text().contains(QStringLiteral("待验证")));
        window.m_connectionFailed = true;
        window.updateActions();
        QCOMPARE(window.m_stepRows[3]->property("state").toString(), QStringLiteral("failed"));
        window.m_connectionVerified = true;
        window.m_connectionFailed = false;
        window.updateActions();
        window.showFeedback(QStringLiteral("导出失败"), true);
        QCOMPARE(window.m_stepRows[3]->property("state").toString(), QStringLiteral("complete"));
        window.invalidateIdentityResults();
        window.updateActions();
        QVERIFY(!window.m_authorizationAcknowledged);
        QVERIFY(!window.m_connectionVerified);
        QCOMPARE(window.m_stepRows[2]->property("state").toString(), QStringLiteral("pending"));
        QVERIFY(!QFile::exists(directory.filePath(QStringLiteral("connection.json"))));
    }

    void clientPreviewIsInertAndFitsNarrowWindow()
    {
        QTemporaryDir directory;
        sshsetup::SetupWindow window(false, directory.filePath(QStringLiteral("connection.json")), {}, true);
        window.resize(880, 620);
        window.show();
        for (int page = 0; page < 4; ++page)
        {
            window.setPreviewPage(page);
            QTest::qWait(30);
            QVERIFY(!window.m_pages->isEnabled());
            QVERIFY(!window.m_next->isEnabled());
            QCOMPARE(window.m_pages->currentIndex(), page);
            for (auto *step : window.m_stepRows)
                QCOMPARE(step->property("state").toString(), QStringLiteral("pending"));
            for (int index = 0; index < window.m_stepRows.size(); ++index)
            {
                const auto *badge = window.m_stepBadges[index];
                const auto *description = window.m_stepDescriptions[index];
                QVERIFY(badge->geometry().right() < description->geometry().left());
            }
            auto *scroll = qobject_cast<QScrollArea *>(window.m_pages->currentWidget());
            QVERIFY(scroll);
            QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
            const QRect nextRect(window.m_next->mapTo(&window, QPoint()), window.m_next->size());
            const QRect backRect(window.m_back->mapTo(&window, QPoint()), window.m_back->size());
            QVERIFY(window.rect().contains(nextRect));
            QVERIFY(window.rect().contains(backRect));
            QVERIFY(!nextRect.intersects(backRect));
        }
        window.m_publicSummary->setText(QString(300, QLatin1Char('A')));
        window.setPreviewPage(2);
        window.m_publicSummary->setText(QString(300, QLatin1Char('A')));
        QTest::qWait(30);
        QCOMPARE(
            qobject_cast<QScrollArea *>(window.m_pages->currentWidget())->horizontalScrollBar()->maximum(),
            0);
        QVERIFY(window.m_identity.publicKey.isEmpty());
        QVERIFY(!QFile::exists(directory.filePath(QStringLiteral("connection.json"))));
    }

    void initialStateCannotSkipChecks()
    {
        QTemporaryDir directory;
        sshsetup::SetupWindow window(false, directory.filePath(QStringLiteral("connection.json")));
        auto *next = window.findChild<QPushButton *>(QStringLiteral("nextButton"));
        QVERIFY(next);
        QVERIFY(!next->isEnabled());
        QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("backButton"))->isEnabled());
        QVERIFY(!QFile::exists(directory.filePath(QStringLiteral("connection.json"))));
    }

    void previewCannotApplyOrGenerateIdentity()
    {
        QTemporaryDir directory;
        sshsetup::SetupWindow window(true, directory.filePath(QStringLiteral("connection.json")), {}, true);
        for (int page = 0; page < 4; ++page)
        {
            window.setPreviewPage(page);
            QCOMPARE(window.findChild<QStackedWidget *>(QStringLiteral("setupPages"))->currentIndex(), page);
            QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("nextButton"))->isEnabled());
            QVERIFY(!window.findChild<QStackedWidget *>(QStringLiteral("setupPages"))->isEnabled());
        }
        QVERIFY(!QFile::exists(directory.filePath(QStringLiteral("connection.json"))));
    }

    void invalidPublicKeyRemainsOnImportStep()
    {
        QTemporaryDir directory;
        sshsetup::SetupWindow window(true, directory.filePath(QStringLiteral("connection.json")));
        auto *pages = window.findChild<QStackedWidget *>(QStringLiteral("setupPages"));
        pages->setCurrentIndex(1);
        auto *input = window.findChild<QPlainTextEdit *>(QStringLiteral("publicKeyInput"));
        input->setPlainText(QStringLiteral("-----BEGIN OPENSSH PRIVATE KEY-----\nnot-a-key"));
        auto *next = window.findChild<QPushButton *>(QStringLiteral("nextButton"));
        QVERIFY(next->isEnabled());
        next->click();
        QTRY_VERIFY_WITH_TIMEOUT(pages->isEnabled(), 15000);
        QCOMPARE(pages->currentIndex(), 1);
        QVERIFY(window.findChild<QLabel *>(QStringLiteral("feedback"))->property("error").toBool());
        input->clear();
        QVERIFY(!next->isEnabled());
    }
    void fingerprintIsNotAPublicKey()
    {
        QTemporaryDir directory;
        sshsetup::SetupWindow window(true, directory.filePath(QStringLiteral("connection.json")));
        auto *pages = window.findChild<QStackedWidget *>(QStringLiteral("setupPages"));
        pages->setCurrentIndex(1);
        window.findChild<QPlainTextEdit *>(QStringLiteral("publicKeyInput"))
            ->setPlainText(QStringLiteral("SHA256:") + QString(43, QLatin1Char('A')));
        window.findChild<QPushButton *>(QStringLiteral("nextButton"))->click();
        QTRY_VERIFY_WITH_TIMEOUT(pages->isEnabled(), 15000);
        QCOMPARE(pages->currentIndex(), 1);
        auto *feedback = window.findChild<QLabel *>(QStringLiteral("feedback"));
        QVERIFY(feedback->text().contains(QStringLiteral("指纹")));
        QVERIFY(feedback->text().contains(QStringLiteral("公钥")));
        QVERIFY(feedback->property("error").toBool());
        QVERIFY(!QFile::exists(directory.filePath(QStringLiteral("connection.json"))));
    }
    void publicKeyAndDiagnosticHaveSeparateControls()
    {
        QTemporaryDir directory;
        sshsetup::SetupWindow window(false, directory.filePath(QStringLiteral("connection.json")));
        auto *key = window.findChild<QPlainTextEdit *>(QStringLiteral("publicKeyPreview"));
        QVERIFY(key && key->isReadOnly());
        QVERIFY(key->toPlainText().isEmpty());
        auto *copy = window.findChild<QPushButton *>(QStringLiteral("copyConnectionReportButton"));
        QVERIFY(copy && !copy->isEnabled());
    }
    void changingTargetClearsVisibleDiagnostic()
    {
        QTemporaryDir directory;
        sshsetup::SetupWindow window(false, directory.filePath(QStringLiteral("connection.json")));
        window.findChild<QStackedWidget *>(QStringLiteral("setupPages"))->setCurrentIndex(3);
        auto *feedback = window.findChild<QLabel *>(QStringLiteral("feedback"));
        feedback->setText(QStringLiteral("旧目标认证：通过"));
        window.findChild<QLineEdit *>(QStringLiteral("hostEdit"))->setText(QStringLiteral("new-host"));
        QVERIFY(feedback->text().contains(QStringLiteral("重新测试")));
        QVERIFY(!feedback->text().contains(QStringLiteral("通过")));
        QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("copyConnectionReportButton"))->isEnabled());
    }
};

QTEST_MAIN(SetupWindowTests)
#include "SetupWindowTests.moc"
