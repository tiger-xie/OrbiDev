#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
#include <diagnostics/DiagnosticLog.h>

using namespace rad;
class DiagnosticLogTests final : public QObject
{
    Q_OBJECT
private slots:
    void rejectsUnstructuredSecrets()
    {
        QTemporaryDir directory;
        DiagnosticLog log;
        QVERIFY(log.open(directory.path()).isSuccess());
        QVERIFY(!log.record(QStringLiteral("token=secret")).isSuccess());
        QVERIFY(!log.record(QStringLiteral("session_error"), {}, static_cast<ErrorCode>(-1)).isSuccess());
        QVERIFY(log.record(QStringLiteral("session_error"), SessionId::create(), ErrorCode::InputFailed)
                    .isSuccess());
        QFile file(directory.filePath(QStringLiteral("agent.log")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto bytes = file.readAll();
        QVERIFY(!bytes.contains("secret"));
        QVERIFY(bytes.contains("InputFailed"));
        QCOMPARE(QJsonDocument::fromJson(bytes).object().size(), 4);
    }
    void sizeAndDailyRetention()
    {
        QTemporaryDir directory;
        QDateTime now = QDateTime::currentDateTimeUtc();
        DiagnosticLog::Options options;
        options.maxBytes = 256;
        options.maxArchives = 2;
        options.retentionDays = 2;
        options.clock = [&now] { return now; };
        DiagnosticLog log(options);
        QVERIFY(log.open(directory.path()).isSuccess());
        for (int i = 0; i < 30; ++i)
            QVERIFY(log.record(QStringLiteral("agent_started")).isSuccess());
        QCOMPARE(QDir(directory.path()).entryList({QStringLiteral("*.log")}, QDir::Files).size(), 3);
        for (const auto &entry : QDir(directory.path()).entryInfoList({QStringLiteral("*.log")}, QDir::Files))
            QVERIFY(entry.size() <= options.maxBytes);
        now = now.addDays(3);
        QVERIFY(log.record(QStringLiteral("agent_stopped")).isSuccess());
        QCOMPARE(QDir(directory.path()).entryList({QStringLiteral("*.log")}, QDir::Files).size(), 1);
        QFile file(directory.filePath(QStringLiteral("agent.log")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(!file.readAll().contains("agent_started"));
    }
    void directoryAndRotationFailure()
    {
        QTemporaryDir directory;
        QFile blocker(directory.filePath(QStringLiteral("blocked")));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        DiagnosticLog bad;
        QVERIFY(!bad.open(blocker.fileName()).isSuccess());
        DiagnosticLog::Options options;
        options.maxBytes = 256;
        options.maxArchives = 1;
        DiagnosticLog log(options);
        QVERIFY(log.open(directory.path()).isSuccess());
        QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("agent.1.log")));
        bool failed = false;
        for (int i = 0; i < 10; ++i)
        {
            const auto result = log.record(QStringLiteral("agent_started"));
            if (!result.isSuccess())
            {
                QVERIFY(!result.error().message.contains(directory.path()));
                failed = true;
                break;
            }
        }
        QVERIFY(failed);
    }
    void exclusiveDirectory()
    {
        QTemporaryDir directory;
        DiagnosticLog first;
        DiagnosticLog second;
        QVERIFY(first.open(directory.path()).isSuccess());
        QVERIFY(!second.open(directory.path()).isSuccess());
    }
};
QTEST_GUILESS_MAIN(DiagnosticLogTests)
#include "DiagnosticLogTests.moc"
