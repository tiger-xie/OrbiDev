#include "SshSession.h"
#include <terminal/TerminalView.h>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <QWebEnginePage>
#include <QWebEngineView>
#include <QtTest>
#include <atomic>

namespace sshworkbench
{
namespace
{
QVariant evaluate(rad::client::TerminalView &view, const QString &script)
{
    struct Evaluation
    {
        QVariant value;
        bool completed = false;
    };
    const auto evaluation = std::make_shared<Evaluation>();
    view.webView()->page()->runJavaScript(script,
                                          [evaluation](const QVariant &value)
                                          {
                                              evaluation->value = value;
                                              evaluation->completed = true;
                                          });
    QElapsedTimer timer;
    timer.start();
    while (!evaluation->completed && timer.elapsed() < 5000)
        QTest::qWait(10);
    return evaluation->value;
}
}
class SshSessionTests : public QObject
{
    Q_OBJECT
private:
    QString fakeCli() const
    {
        return QCoreApplication::applicationDirPath() + QStringLiteral("/FakeCli.exe");
    }
    QString powershell() const
    {
        return qEnvironmentVariable("SystemRoot") +
               QStringLiteral("/System32/WindowsPowerShell/v1.0/powershell.exe");
    }
private slots:
    void embeddedTerminalRoundTrip()
    {
        rad::client::TerminalView terminal;
        SshSession session;
        QSignalSpy ready(&terminal, &rad::client::TerminalView::ready);
        QSignalSpy failed(&terminal, &rad::client::TerminalView::failed);
        QSignalSpy errors(&session, &SshSession::errorOccurred);
        QSignalSpy exits(&session, &SshSession::exited);
        connect(&session, &SshSession::outputReady, &terminal,
                [&terminal](const QByteArray &bytes) { terminal.appendOutput(bytes); });
        connect(&terminal, &rad::client::TerminalView::inputReady, &session, &SshSession::writeInput);
        connect(&terminal, &rad::client::TerminalView::terminalResized, &session, &SshSession::resize);
        terminal.resize(800, 480);
        terminal.show();
        QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty() || !failed.isEmpty(), 20000);
        QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
        QCOMPARE(ready.count(), 1);
        session.startProcess({}, {}, 100, 30, fakeCli());
        terminal.setInputEnabled(true);
        // 通过 xterm 的公开输入入口驱动完整链路，不改系统剪贴板，也不依赖窗口焦点。
        const QString screen = QStringLiteral(
            "(function(){let b=clientTerminal.term.buffer.active,s='';"
            "for(let i=0;i<b.length;i++)s+=b.getLine(i).translateToString(true)+'\\n';return s;})()");
        QTRY_VERIFY_WITH_TIMEOUT(evaluate(terminal, screen).toString().contains(QStringLiteral("[READY]")),
                                 10000);
        QTRY_VERIFY(!evaluate(terminal, QStringLiteral("clientTerminal.term.options.disableStdin")).toBool());
        evaluate(terminal, QStringLiteral("clientTerminal.term.paste('echo 中文\\r')"));
        QTRY_VERIFY_WITH_TIMEOUT(evaluate(terminal, screen).toString().contains(QStringLiteral("[ECHO]中文")),
                                 10000);
        evaluate(terminal, QStringLiteral("clientTerminal.term.paste('exit\\r')"));
        QTRY_COMPARE_WITH_TIMEOUT(exits.count(), 1, 10000);
        QCOMPARE(exits.first().first().toInt(), 0);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(errors.count(), 0);
    }
    void rejectsInvalidProfileAsynchronously()
    {
        SshSession session;
        QSignalSpy errors(&session, &SshSession::errorOccurred);
        QSignalSpy exits(&session, &SshSession::exited);
        sshsetup::ConnectionProfile profile;
        profile.host = QStringLiteral("-oProxyCommand=bad");
        session.start(profile, {});
        QTRY_COMPARE_WITH_TIMEOUT(exits.count(), 1, 3000);
        QCOMPARE(errors.count(), 1);
        QVERIFY(!session.isActive());
    }
    void hostVerificationIsAsynchronousAndPrecedesFailure()
    {
        SshSession session;
        QSignalSpy verification(&session, &SshSession::hostVerificationStarted);
        QSignalSpy starts(&session, &SshSession::started);
        QSignalSpy errors(&session, &SshSession::errorOccurred);
        QSignalSpy exits(&session, &SshSession::exited);
        QStringList events;
        std::atomic_bool verificationOnOwningThread{false};
        connect(
            &session, &SshSession::hostVerificationStarted, &session, [&session, &verificationOnOwningThread]
            { verificationOnOwningThread = QThread::currentThread() == session.thread(); },
            Qt::DirectConnection);
        connect(&session, &SshSession::hostVerificationStarted, &session,
                [&events] { events.append(QStringLiteral("verification")); });
        connect(&session, &SshSession::errorOccurred, &session,
                [&events] { events.append(QStringLiteral("error")); });
        connect(&session, &SshSession::exited, &session,
                [&events] { events.append(QStringLiteral("exit")); });
        sshsetup::ConnectionProfile profile;
        profile.host = QStringLiteral("-oProxyCommand=bad");
        session.start(profile, {});
        QCOMPARE(verification.count(), 0);
        QCOMPARE(errors.count(), 0);
        QCOMPARE(exits.count(), 0);
        QVERIFY(session.isActive());
        QTRY_COMPARE_WITH_TIMEOUT(exits.count(), 1, 3000);
        QCOMPARE(verification.count(), 1);
        QVERIFY(verificationOnOwningThread.load());
        QCOMPARE(starts.count(), 0);
        QCOMPARE(errors.count(), 1);
        QCOMPARE(events, QStringList({QStringLiteral("verification"), QStringLiteral("error"),
                                      QStringLiteral("exit")}));
        QTest::qWait(40);
        QCOMPARE(verification.count(), 1);
        QCOMPARE(exits.count(), 1);
    }
    void invalidDirectoryDoesNotStartHostVerification()
    {
        SshSession session;
        QSignalSpy verification(&session, &SshSession::hostVerificationStarted);
        QSignalSpy starts(&session, &SshSession::started);
        QSignalSpy errors(&session, &SshSession::errorOccurred);
        QSignalSpy exits(&session, &SshSession::exited);
        session.start({}, QStringLiteral("D:/a\nb"));
        QTRY_COMPARE_WITH_TIMEOUT(exits.count(), 1, 3000);
        QCOMPARE(verification.count(), 0);
        QCOMPARE(starts.count(), 0);
        QCOMPARE(errors.count(), 1);
        QVERIFY(errors.first().first().toString().contains(QStringLiteral("远程目录")));
    }
    void rejectsInvalidDirectory()
    {
        QStringList arguments;
        QString error;
        QVERIFY(!remoteCommandArguments(QStringLiteral("D:/a\nb"), arguments, error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!remoteCommandArguments(QString(4097, QLatin1Char('a')), arguments, error));
    }
    void interactiveArgumentsKeepStrictHostVerification()
    {
        sshsetup::ConnectionProfile profile;
        profile.host = QStringLiteral("127.0.0.1");
        profile.user = QStringLiteral("test-user");
        QTemporaryDir temporary;
        QStringList arguments;
        QString error;
        QVERIFY2(sshsetup::prepareSshArguments(profile, temporary.path(), arguments, error, true),
                 qPrintable(error));
        QVERIFY(arguments.contains(QStringLiteral("StrictHostKeyChecking=yes")));
        QVERIFY(arguments.contains(QStringLiteral("BatchMode=no")));
        QVERIFY(arguments.contains(QStringLiteral("ClearAllForwardings=yes")));
        QVERIFY(arguments.contains(QStringLiteral("-tt")));
        QCOMPARE(arguments.last(), QStringLiteral("test-user@127.0.0.1"));
    }
    void literalDirectoryAndFailureDoNotExecuteCommands()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString directory = temporary.filePath(QStringLiteral("中文’;$();[目录]"));
        QVERIFY(QDir().mkpath(directory));
        QFile sentinel(QDir(directory).filePath(QStringLiteral("expected-working-directory.txt")));
        QVERIFY(sentinel.open(QIODevice::WriteOnly));
        sentinel.close();
        const QString marker = temporary.filePath(QStringLiteral("started.txt"));
        QFile fake(temporary.filePath(QStringLiteral("claude.cmd")));
        QVERIFY(fake.open(QIODevice::WriteOnly));
        fake.write(
            (QStringLiteral(
                 "@echo off\r\nif not exist expected-working-directory.txt exit /b 42\r\necho invoked > \"") +
             marker + QStringLiteral("\"\r\nexit /b 0\r\n"))
                .toLocal8Bit());
        fake.close();
        auto environment = QProcessEnvironment::systemEnvironment();
        // 将测试替身放到 PATH 首位；不会启动用户安装的 Claude。
        environment.insert(QStringLiteral("PATH"), temporary.path());
        QStringList arguments;
        QString error;
        QVERIFY(remoteCommandArguments(directory, arguments, error));
        arguments.removeFirst();
        QProcess process;
        process.setProcessEnvironment(environment);
        process.start(powershell(), arguments);
        QVERIFY(process.waitForFinished(10000));
        QCOMPARE(process.exitCode(), 0);
        QVERIFY(QFile::exists(marker));
        QVERIFY(QFile::remove(marker));
        QVERIFY(remoteCommandArguments(directory + QStringLiteral("/missing"), arguments, error));
        arguments.removeFirst();
        process.start(powershell(), arguments);
        QVERIFY(process.waitForFinished(10000));
        QVERIFY(process.exitCode() != 0);
        QVERIFY(!QFile::exists(marker));
        for (const QString &invalid : {sentinel.fileName(), QStringLiteral("HKCU:\\Software")})
        {
            QVERIFY(remoteCommandArguments(invalid, arguments, error));
            arguments.removeFirst();
            process.start(powershell(), arguments);
            QVERIFY(process.waitForFinished(10000));
            QVERIFY(process.exitCode() != 0);
            QVERIFY(!QFile::exists(marker));
        }
    }
    void fakeCliRoundTripResizeAndExit()
    {
        QVERIFY(QFile::exists(fakeCli()));
        SshSession session;
        QByteArray output;
        connect(&session, &SshSession::outputReady, &session,
                [&output](const QByteArray &bytes) { output += bytes; });
        QSignalSpy exits(&session, &SshSession::exited);
        QSignalSpy errors(&session, &SshSession::errorOccurred);
        session.startProcess({}, {}, 100, 30, fakeCli());
        QTRY_VERIFY_WITH_TIMEOUT(output.contains("[READY]"), 10000);
        session.writeInput(QStringLiteral("echo 你好\r").toUtf8());
        QTRY_VERIFY_WITH_TIMEOUT(output.contains(QStringLiteral("[ECHO]你好").toUtf8()), 5000);
        session.resize(91, 27);
        session.writeInput("size\r");
        QTRY_VERIFY_WITH_TIMEOUT(output.contains("[SIZE]91x27"), 5000);
        session.writeInput("exit\r");
        QTRY_COMPARE_WITH_TIMEOUT(exits.count(), 1, 10000);
        QCOMPARE(exits.first().first().toInt(), 0);
        QVERIFY(output.contains("[BYE]"));
        QCOMPARE(errors.count(), 0);
    }
    void explicitStopIsIdempotent()
    {
        SshSession session;
        QSignalSpy verification(&session, &SshSession::hostVerificationStarted);
        QSignalSpy starts(&session, &SshSession::started);
        QSignalSpy exits(&session, &SshSession::exited);
        session.startProcess({}, {}, 100, 30, fakeCli());
        QTRY_COMPARE_WITH_TIMEOUT(starts.count(), 1, 10000);
        session.stop();
        session.stop();
        QTRY_COMPARE_WITH_TIMEOUT(exits.count(), 1, 10000);
        QCOMPARE(verification.count(), 0);
        QVERIFY(!session.isActive());
    }
    void sessionsKeepIndependentInputAndExit()
    {
        SshSession first;
        SshSession second;
        QByteArray firstOutput, secondOutput;
        connect(&first, &SshSession::outputReady, &first,
                [&firstOutput](const QByteArray &bytes) { firstOutput += bytes; });
        connect(&second, &SshSession::outputReady, &second,
                [&secondOutput](const QByteArray &bytes) { secondOutput += bytes; });
        QSignalSpy exits(&first, &SshSession::exited);
        first.startProcess({}, {}, 100, 30, fakeCli());
        second.startProcess({}, {}, 100, 30, fakeCli());
        QTRY_VERIFY_WITH_TIMEOUT(firstOutput.contains("[READY]") && secondOutput.contains("[READY]"), 10000);
        first.writeInput("echo project-one\r");
        second.writeInput("echo project-two\r");
        QTRY_VERIFY_WITH_TIMEOUT(
            firstOutput.contains("[ECHO]project-one") && secondOutput.contains("[ECHO]project-two"), 5000);
        QVERIFY(!firstOutput.contains("project-two"));
        QVERIFY(!secondOutput.contains("project-one"));
        first.stop();
        QTRY_COMPARE_WITH_TIMEOUT(exits.count(), 1, 10000);
        second.writeInput("echo still-running\r");
        QTRY_VERIFY_WITH_TIMEOUT(secondOutput.contains("[ECHO]still-running"), 5000);
        second.stop();
    }
    void inputOverflowIsReported()
    {
        SshSession session;
        QSignalSpy exits(&session, &SshSession::exited);
        QSignalSpy errors(&session, &SshSession::errorOccurred);
        session.startProcess({}, {}, 100, 30, fakeCli());
        session.writeInput(QByteArray(256 * 1024 + 1, 'x'));
        QTRY_COMPARE_WITH_TIMEOUT(exits.count(), 1, 10000);
        QVERIFY(!errors.isEmpty());
        QVERIFY(errors.first().first().toString().contains(QStringLiteral("缓冲上限")));
    }
};
}
int main(int argc, char **argv)
{
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS",
            qgetenv("QTWEBENGINE_CHROMIUM_FLAGS") + " --disable-background-timer-throttling");
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    sshworkbench::SshSessionTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "SshSessionTests.moc"
