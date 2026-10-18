#include "SetupTheme.h"
#include "SetupWindow.h"

#include <QApplication>
#include <QFile>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QtTest>

class SetupWindowTests final : public QObject
{
    Q_OBJECT
private slots:
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
};

QTEST_MAIN(SetupWindowTests)
#include "SetupWindowTests.moc"
