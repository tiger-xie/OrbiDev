#include <controllers/ClientController.h>
#include <controllers/FakeAgent.h>
#include <ui/ClientWindow.h>
#include <ui/Theme.h>

#include <QApplication>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QTabBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeView>
#include <QtTest>

using namespace rad::client;

namespace
{
struct WindowFixture
{
    FakeAgent agent;
    ClientController controller{&agent};
    ClientWindow window{&controller};

    WindowFixture()
    {
        window.show();
    }

    QPushButton *button(const char *name)
    {
        return window.findChild<QPushButton *>(QString::fromLatin1(name));
    }
};

// 持续检测模态窗口；异常时也关闭它，避免断言失败后测试卡在嵌套事件循环。
class DialogResponder final : public QObject
{
public:
    explicit DialogResponder(std::function<void(QDialog *)> respond) : m_respond(std::move(respond))
    {
        m_timer.setInterval(10);
        connect(&m_timer, &QTimer::timeout, this,
                [this]
                {
                    auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                    if (dialog && ++m_ticks > 300)
                        dialog->reject();
                    else if (dialog)
                        m_respond(dialog);
                });
        m_timer.start();
    }

private:
    QTimer m_timer;
    int m_ticks = 0;
    std::function<void(QDialog *)> m_respond;
};

QString firstRunningId(const ClientController &controller)
{
    for (const auto &session : controller.sessions())
        if (session.running && session.writerAvailable)
            return session.id;
    return {};
}

bool clickButtonText(QMessageBox *message, const QString &text)
{
    for (auto *button : message->buttons())
        if (button->text() == text)
        {
            button->click();
            return true;
        }
    message->reject();
    return false;
}

void clickTabClose(ClientWindow &window)
{
    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("sessionTabs"));
    auto *button = tabs->tabBar()->tabButton(0, QTabBar::RightSide);
    if (!button)
        button = tabs->tabBar()->tabButton(0, QTabBar::LeftSide);
    if (button)
        QTest::mouseClick(button, Qt::LeftButton);
}
}

class ClientWindowTests final : public QObject
{
    Q_OBJECT
private slots:
    void restartedSessionIsClearlyInvalid()
    {
        ClientSession session;
        session.id = QStringLiteral("historical-session");
        session.name = QStringLiteral("重启前的终端");
        session.workspace = QStringLiteral("OrbiDev");
        session.running = false;
        session.exitCodeKnown = false;
        session.processState = QStringLiteral("failed");
        session.failureCode = QStringLiteral("agent_restarted");
        session.metadataOnly = true;
        FakeAgent agent(QVector<ClientSession>{session});
        ClientController controller(&agent);
        ClientWindow window(&controller);
        window.show();
        controller.connectAgent();
        QTRY_VERIFY(controller.isConnected() && !controller.isBusy());
        auto *tree = window.findChild<QTreeView *>(QStringLiteral("sessionTree"));
        QVERIFY(tree && tree->model());
        const auto workspace = tree->model()->index(0, 0);
        const auto terminal = tree->model()->index(0, 0, workspace);
        QVERIFY(terminal.isValid());
        const auto text = terminal.data().toString();
        QVERIFY(text.contains(QStringLiteral("Agent 重启")));
        QVERIFY(text.contains(QStringLiteral("不可恢复")));
        QVERIFY(!text.contains(QStringLiteral("运行中")));
        const auto screenshot = qEnvironmentVariable("RAD_M52_UI_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(window.grab().save(screenshot));
    }
    void connectionAndEmptyState()
    {
        WindowFixture fixture;
        QVERIFY(fixture.button("connectButton"));
        QVERIFY(fixture.button("newSessionButton"));
        QVERIFY(!fixture.controller.isConnected());
        QCOMPARE(fixture.window.sessionTabCount(), 0);
        QVERIFY(!fixture.button("newSessionButton")->isEnabled());
        QTest::mouseClick(fixture.button("connectButton"), Qt::LeftButton);
        QTRY_VERIFY(fixture.controller.isConnected() && !fixture.controller.isBusy());
        QVERIFY(fixture.button("newSessionButton")->isEnabled());
        QVERIFY(!fixture.controller.sessions().isEmpty());
        auto *tree = fixture.window.findChild<QTreeView *>(QStringLiteral("sessionTree"));
        QVERIFY(tree && tree->model());
        QVERIFY(tree->model()->rowCount() > 0);
        QCOMPARE(fixture.window.sessionTabCount(), 0);
    }

    void createDialogRejectsEmptyName()
    {
        WindowFixture fixture;
        fixture.controller.connectAgent();
        QTRY_VERIFY(fixture.controller.isConnected() && !fixture.controller.isBusy());
        const auto previousCount = fixture.agent.sessions().size();
        bool dialogVisited = false;
        bool rejectedEmpty = false;
        DialogResponder responder(
            [&](QDialog *dialog)
            {
                auto *name = dialog->findChild<QLineEdit *>(QStringLiteral("sessionNameEdit"));
                auto *confirm = dialog->findChild<QPushButton *>(QStringLiteral("createConfirmButton"));
                if (name && confirm)
                {
                    dialogVisited = true;
                    name->clear();
                    QTest::mouseClick(confirm, Qt::LeftButton);
                    auto *error = dialog->findChild<QLabel *>(QStringLiteral("createErrorLabel"));
                    rejectedEmpty = !confirm->isEnabled() || (error && !error->text().isEmpty());
                }
                dialog->reject();
            });
        QTest::mouseClick(fixture.button("newSessionButton"), Qt::LeftButton);
        QVERIFY(dialogVisited);
        QVERIFY(rejectedEmpty);
        QCOMPARE(fixture.agent.sessions().size(), previousCount);
    }

    void createFailureAndRetry()
    {
        WindowFixture fixture;
        fixture.controller.connectAgent();
        QTRY_VERIFY(fixture.controller.isConnected() && !fixture.controller.isBusy());
        const auto previousCount = fixture.agent.sessions().size();
        fixture.agent.failNext(FakeAgent::Operation::Create, QStringLiteral("模拟创建失败"));
        int submission = 0;
        bool failureVisible = false;
        bool failurePreservedCount = false;
        bool pendingCancelIgnored = false;
        DialogResponder responder(
            [&](QDialog *dialog)
            {
                auto *name = dialog->findChild<QLineEdit *>(QStringLiteral("sessionNameEdit"));
                auto *confirm = dialog->findChild<QPushButton *>(QStringLiteral("createConfirmButton"));
                auto *error = dialog->findChild<QLabel *>(QStringLiteral("createErrorLabel"));
                if (!name || !confirm || !error)
                {
                    dialog->reject();
                    return;
                }
                if (submission == 0)
                {
                    name->setText(QStringLiteral("中文会话"));
                    ++submission;
                    QTest::mouseClick(confirm, Qt::LeftButton);
                    dialog->reject();
                    pendingCancelIgnored = dialog->isVisible();
                }
                else if (submission == 1 && !fixture.controller.isBusy() &&
                         error->text().contains(QStringLiteral("模拟创建失败")))
                {
                    failureVisible = true;
                    failurePreservedCount = fixture.agent.sessions().size() == previousCount;
                    ++submission;
                    QTest::mouseClick(confirm, Qt::LeftButton);
                }
            });
        QTest::mouseClick(fixture.button("newSessionButton"), Qt::LeftButton);
        QVERIFY(failureVisible);
        QVERIFY(failurePreservedCount);
        QVERIFY(pendingCancelIgnored);
        QCOMPARE(submission, 2);
        QTRY_VERIFY(!fixture.controller.isBusy());
        QCOMPARE(fixture.agent.sessions().size(), previousCount + 1);
        QTRY_COMPARE(fixture.window.sessionTabCount(), 1);
    }

    void openingTwiceDoesNotDuplicateTab()
    {
        WindowFixture fixture;
        fixture.controller.connectAgent();
        QTRY_VERIFY(fixture.controller.isConnected() && !fixture.controller.isBusy());
        const auto id = firstRunningId(fixture.controller);
        QVERIFY(!id.isEmpty());
        fixture.agent.failNext(FakeAgent::Operation::Attach, QStringLiteral("模拟附加失败"));
        fixture.window.openSession(id);
        QTRY_VERIFY(!fixture.controller.isBusy());
        QCOMPARE(fixture.window.sessionTabCount(), 0);
        QVERIFY(fixture.controller.error().contains(QStringLiteral("模拟附加失败")));
        fixture.window.openSession(id);
        QTRY_COMPARE(fixture.window.sessionTabCount(), 1);
        fixture.window.openSession(id);
        QTRY_VERIFY(!fixture.controller.isBusy());
        QCOMPARE(fixture.window.sessionTabCount(), 1);
        QCOMPARE(fixture.window.currentSessionId(), id);
    }

    void detachPreservesProcessAndReattachesSameId()
    {
        WindowFixture fixture;
        fixture.controller.connectAgent();
        QTRY_VERIFY(fixture.controller.isConnected() && !fixture.controller.isBusy());
        const auto id = firstRunningId(fixture.controller);
        fixture.window.openSession(id);
        QTRY_COMPARE(fixture.window.sessionTabCount(), 1);
        bool offeredDetach = false;
        DialogResponder responder(
            [&](QDialog *dialog)
            {
                auto *message = qobject_cast<QMessageBox *>(dialog);
                if (message)
                    offeredDetach = clickButtonText(message, QStringLiteral("分离"));
                else
                    dialog->reject();
            });
        clickTabClose(fixture.window);
        QVERIFY(offeredDetach);
        QTRY_COMPARE(fixture.window.sessionTabCount(), 0);
        QCOMPARE(fixture.agent.terminateCount(), 0);
        bool retained = false;
        for (const auto &session : fixture.agent.sessions())
            if (session.id == id)
                retained = session.running && !session.attached;
        QVERIFY(retained);
        fixture.window.openSession(id);
        QTRY_COMPARE(fixture.window.sessionTabCount(), 1);
        QCOMPARE(fixture.window.currentSessionId(), id);
    }

    void terminationRequiresSecondConfirmation()
    {
        WindowFixture fixture;
        fixture.controller.connectAgent();
        QTRY_VERIFY(fixture.controller.isConnected() && !fixture.controller.isBusy());
        fixture.window.openSession(firstRunningId(fixture.controller));
        QTRY_COMPARE(fixture.window.sessionTabCount(), 1);
        int confirmations = 0;
        bool cancelWasDefault = false;
        bool proceed = false;
        DialogResponder responder(
            [&](QDialog *dialog)
            {
                auto *message = qobject_cast<QMessageBox *>(dialog);
                if (!message)
                {
                    dialog->reject();
                    return;
                }
                ++confirmations;
                if (message->objectName() == QStringLiteral("closeSessionDialog"))
                    clickButtonText(message, QStringLiteral("终止…"));
                else
                {
                    cancelWasDefault = message->defaultButton() == message->button(QMessageBox::No);
                    auto *answer = message->button(proceed ? QMessageBox::Yes : QMessageBox::No);
                    if (answer)
                        answer->click();
                    else
                        message->reject();
                }
            });
        clickTabClose(fixture.window);
        QCOMPARE(confirmations, 2);
        QVERIFY(cancelWasDefault);
        QCOMPARE(fixture.agent.terminateCount(), 0);
        QCOMPARE(fixture.window.sessionTabCount(), 1);
        proceed = true;
        clickTabClose(fixture.window);
        QCOMPARE(confirmations, 4);
        QTRY_COMPARE(fixture.agent.terminateCount(), 1);
    }

    void closingWindowDoesNotTerminate()
    {
        WindowFixture fixture;
        fixture.controller.connectAgent();
        QTRY_VERIFY(fixture.controller.isConnected() && !fixture.controller.isBusy());
        const auto id = firstRunningId(fixture.controller);
        fixture.window.openSession(id);
        QTRY_COMPARE(fixture.window.sessionTabCount(), 1);
        fixture.window.close();
        QTRY_VERIFY(!fixture.agent.isConnected());
        QCOMPARE(fixture.agent.terminateCount(), 0);
        for (const auto &session : fixture.agent.sessions())
            if (session.id == id)
                QVERIFY(session.running);
    }

    void disconnectKeepsLastKnownSessionAndDisablesActions()
    {
        WindowFixture fixture;
        fixture.controller.connectAgent();
        QTRY_VERIFY(fixture.controller.isConnected() && !fixture.controller.isBusy());
        const auto id = firstRunningId(fixture.controller);
        fixture.window.openSession(id);
        QTRY_COMPARE(fixture.window.sessionTabCount(), 1);
        fixture.agent.disconnectAgent();
        QTRY_VERIFY(!fixture.controller.isConnected());
        QCOMPARE(fixture.window.sessionTabCount(), 1);
        QVERIFY(!fixture.button("newSessionButton")->isEnabled());
        QVERIFY(!fixture.button("terminateButton")->isEnabled());
        QCOMPARE(fixture.agent.terminateCount(), 0);
        bool retained = false;
        for (const auto &session : fixture.controller.sessions())
            if (session.id == id)
                retained = session.running;
        QVERIFY(retained);
    }

    void readOnlySessionCannotTerminate()
    {
        WindowFixture fixture;
        fixture.controller.connectAgent();
        QTRY_VERIFY(fixture.controller.isConnected() && !fixture.controller.isBusy());
        QString readOnlyId;
        for (const auto &session : fixture.controller.sessions())
            if (session.running && !session.writerAvailable)
                readOnlyId = session.id;
        QVERIFY(!readOnlyId.isEmpty());
        fixture.window.openSession(readOnlyId);
        QTRY_COMPARE(fixture.window.sessionTabCount(), 1);
        QVERIFY(!fixture.button("terminateButton")->isEnabled());
        QVERIFY(fixture.button("detachButton")->isEnabled());
        QCOMPARE(fixture.agent.terminateCount(), 0);
    }

    void longSessionNameFitsWindowAndDistinctStatesKeepOwnTabs()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QSettings settings(directory.filePath(QStringLiteral("layout.ini")), QSettings::IniFormat);
        auto *application = qobject_cast<QApplication *>(QApplication::instance());
        QVERIFY(application);
        QCOMPARE(applyTheme(*application, settings), QStringLiteral("dark"));
        WindowFixture fixture;
        fixture.controller.connectAgent();
        QTRY_VERIFY(fixture.controller.isConnected() && !fixture.controller.isBusy());
        const QString longName(256, QChar(0x957F));
        fixture.controller.createSession(longName, fixture.controller.workspaces().first());
        QTRY_COMPARE(fixture.window.sessionTabCount(), 1);
        const auto longNameId = fixture.window.currentSessionId();
        QVERIFY(!longNameId.isEmpty());
        auto *tabs = fixture.window.findChild<QTabWidget *>(QStringLiteral("sessionTabs"));
        auto *tree = fixture.window.findChild<QTreeView *>(QStringLiteral("sessionTree"));
        QVERIFY(tabs && tree);
        QCOMPARE(tabs->tabToolTip(0), longName);

        QString readOnlyId;
        QString exitedId;
        for (const auto &session : fixture.controller.sessions())
        {
            if (session.running && !session.writerAvailable)
                readOnlyId = session.id;
            if (!session.running)
                exitedId = session.id;
        }
        QVERIFY(!readOnlyId.isEmpty() && !exitedId.isEmpty());
        fixture.window.openSession(readOnlyId);
        QTRY_COMPARE(fixture.window.sessionTabCount(), 2);
        QCOMPARE(fixture.window.currentSessionId(), readOnlyId);
        QVERIFY(!fixture.button("terminateButton")->isEnabled());
        fixture.window.openSession(exitedId);
        QTRY_COMPARE(fixture.window.sessionTabCount(), 3);
        QCOMPARE(fixture.window.currentSessionId(), exitedId);
        QVERIFY(!fixture.button("terminateButton")->isEnabled());

        const QVector<QSize> sizes{QSize(1280, 800), QSize(1440, 1024)};
        for (const auto &size : sizes)
        {
            fixture.window.resize(size);
            QTRY_COMPARE(fixture.window.size(), size);
            QVERIFY(fixture.window.minimumSizeHint().width() <= size.width());
            QVERIFY(tree->width() < size.width());
            QVERIFY(tabs->width() < size.width());
            QCOMPARE(tabs->tabToolTip(0), longName);
        }
        fixture.window.openSession(longNameId);
        QTRY_VERIFY(!fixture.controller.isBusy());
        QCOMPARE(fixture.window.currentSessionId(), longNameId);
        QCOMPARE(fixture.window.sessionTabCount(), 3);
        QSet<QString> openedIds;
        for (int index = 0; index < tabs->count(); ++index)
            openedIds.insert(tabs->widget(index)->property("sessionId").toString());
        QCOMPARE(openedIds.size(), 3);
        QVERIFY(openedIds.contains(longNameId) && openedIds.contains(readOnlyId) &&
                openedIds.contains(exitedId));
    }

    void unknownThemeFallsBackToBuiltIn()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QSettings settings(directory.filePath(QStringLiteral("theme.ini")), QSettings::IniFormat);
        settings.setValue(QStringLiteral("appearance/theme"), QStringLiteral("unknown-theme"));
        auto *application = qobject_cast<QApplication *>(QApplication::instance());
        QVERIFY(application);
        const QString originalStyle = application->styleSheet();
        QTest::ignoreMessage(QtWarningMsg, "Unknown client theme; using built-in dark theme.");
        const QString theme = applyTheme(*application, settings);
        const bool hasStyle = !application->styleSheet().isEmpty();
        application->setStyleSheet(originalStyle);
        QCOMPARE(theme, QStringLiteral("dark"));
        QCOMPARE(settings.value(QStringLiteral("appearance/theme")).toString(), QStringLiteral("dark"));
        QVERIFY(hasStyle);
    }
};

QTEST_MAIN(ClientWindowTests)
#include "ClientWindowTests.moc"
