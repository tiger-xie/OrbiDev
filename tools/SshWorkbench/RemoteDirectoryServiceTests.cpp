#include "RemoteDirectoryService.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

namespace sshworkbench
{
namespace
{
QByteArray directoryJson()
{
    QJsonObject object{
        {QStringLiteral("success"), true},
        {QStringLiteral("currentPath"), QStringLiteral("D:\\项目")},
        {QStringLiteral("parentPath"), QStringLiteral("D:\\")},
        {QStringLiteral("homePath"), QStringLiteral("C:\\Users\\家")},
        {QStringLiteral("drives"), QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("D:")},
                                                          {QStringLiteral("path"), QStringLiteral("D:\\")}}}},
        {QStringLiteral("directories"), QJsonArray{}}};
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}
QString powershell()
{
    return qEnvironmentVariable("SystemRoot") +
           QStringLiteral("/System32/WindowsPowerShell/v1.0/powershell.exe");
}
QString decodeUnicode(const QString &base64)
{
    const auto bytes = QByteArray::fromBase64(base64.toLatin1());
    QString result;
    for (int index = 0; index + 1 < bytes.size(); index += 2)
        result.append(QChar(uchar(bytes[index]) | (ushort(uchar(bytes[index + 1])) << 8)));
    return result;
}
}
class RemoteDirectoryServiceTests : public QObject
{
    Q_OBJECT
private slots:
    void emptyDirectoryAndDrives()
    {
        const auto result = parseRemoteDirectoryResult(directoryJson());
        QVERIFY(result.success);
        QCOMPARE(result.currentPath, QStringLiteral("D:\\项目"));
        QCOMPARE(result.drives.size(), 1);
        QVERIFY(result.directories.isEmpty());
    }
    void rejectMalformedAndOversized()
    {
        QVERIFY(!parseRemoteDirectoryResult("not-json").success);
        QVERIFY(!parseRemoteDirectoryResult("{}").success);
        QVERIFY(!parseRemoteDirectoryResult(QByteArray(2 * 1024 * 1024 + 1, 'x')).success);
        auto object = QJsonDocument::fromJson(directoryJson()).object();
        object.remove(QStringLiteral("homePath"));
        QVERIFY(!parseRemoteDirectoryResult(QJsonDocument(object).toJson()).success);
        object = QJsonDocument::fromJson(directoryJson()).object();
        QJsonArray entries;
        for (int index = 0; index <= 5000; ++index)
            entries.append(QJsonObject{{QStringLiteral("name"), QStringLiteral("a")},
                                       {QStringLiteral("path"), QStringLiteral("D:\\a")}});
        object.insert(QStringLiteral("directories"), entries);
        QVERIFY(!parseRemoteDirectoryResult(QJsonDocument(object).toJson()).success);
    }
    void knownFailures_data()
    {
        QTest::addColumn<QString>("code");
        QTest::addColumn<QString>("message");
        QTest::newRow("permission") << QStringLiteral("ACCESS_DENIED") << QStringLiteral("无权");
        QTest::newRow("missing") << QStringLiteral("NOT_FOUND") << QStringLiteral("不存在");
        QTest::newRow("not-directory") << QStringLiteral("NOT_DIRECTORY") << QStringLiteral("不是");
        QTest::newRow("too-many") << QStringLiteral("ENTRY_LIMIT") << QStringLiteral("5000");
        QTest::newRow("too-large") << QStringLiteral("OUTPUT_LIMIT") << QStringLiteral("2 MiB");
    }
    void knownFailures()
    {
        QFETCH(QString, code);
        QFETCH(QString, message);
        const auto response =
            QJsonDocument(QJsonObject{{QStringLiteral("success"), false}, {QStringLiteral("error"), code}})
                .toJson();
        const auto result = parseRemoteDirectoryResult(response);
        QVERIFY(!result.success);
        QVERIFY(result.error.contains(message));
    }
    void encodedPathIsData()
    {
        const QString path = QStringLiteral("D:\\中文 [目录] ' ; $() & ` ‘ ”");
        QStringList arguments;
        QString error;
        QVERIFY(remoteDirectoryCommandArguments(path, arguments, error));
        const auto script = decodeUnicode(arguments.last());
        QVERIFY(!script.contains(path));
        QVERIFY(script.contains(QStringLiteral("-LiteralPath")));
        QVERIFY(!script.contains(QStringLiteral("claude"), Qt::CaseInsensitive));
        QVERIFY(script.contains(QStringLiteral("-First 5001")));
        QVERIFY(!remoteDirectoryCommandArguments(QString(4097, 'a'), arguments, error));
        QVERIFY(!remoteDirectoryCommandArguments(QStringLiteral("a\nb"), arguments, error));
    }
    void localReadOnlyScript()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const auto directory = temporary.filePath(QStringLiteral("中文 [x] ' ; $() & ` ‘ ”"));
        QVERIFY(QDir().mkpath(directory));
        QVERIFY(QDir().mkpath(directory + QStringLiteral("/子目录")));
        QFile file(directory + QStringLiteral("/普通文件.txt"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("unchanged");
        file.close();
        QStringList arguments;
        QString error;
        QVERIFY(remoteDirectoryCommandArguments(directory, arguments, error));
        arguments.removeFirst();
        QProcess process;
        process.start(powershell(), arguments);
        QVERIFY(process.waitForFinished(10000));
        QCOMPARE(process.exitCode(), 0);
        const auto result = parseRemoteDirectoryResult(process.readAllStandardOutput());
        QVERIFY2(result.success, qPrintable(result.error));
        QCOMPARE(QDir::fromNativeSeparators(result.currentPath), directory);
        QCOMPARE(result.directories.size(), 1);
        QCOMPARE(result.directories.first().name, QStringLiteral("子目录"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("unchanged"));
        QVERIFY(
            remoteDirectoryCommandArguments(temporary.filePath(QStringLiteral("missing")), arguments, error));
        arguments.removeFirst();
        process.start(powershell(), arguments);
        QVERIFY(process.waitForFinished(10000));
        QVERIFY(!parseRemoteDirectoryResult(process.readAllStandardOutput()).success);
    }
    void asynchronousTimeout()
    {
        RemoteDirectoryService service;
        QSignalSpy results(&service, &RemoteDirectoryService::finished);
        service.startLocalProcessForTest(powershell(),
                                         {QStringLiteral("-NoProfile"), QStringLiteral("-Command"),
                                          QStringLiteral("Start-Sleep -Seconds 10")},
                                         30);
        QVERIFY(service.isBusy());
        QTRY_COMPARE_WITH_TIMEOUT(results.count(), 1, 2000);
        const auto measured = qvariant_cast<RemoteDirectoryResult>(results.first().first());
        QVERIFY(measured.preparationMilliseconds >= 0);
        QVERIFY(measured.queryMilliseconds >= 0);
        QVERIFY(qvariant_cast<RemoteDirectoryResult>(results.first().first())
                    .error.contains(QStringLiteral("超时")));
        QTRY_VERIFY_WITH_TIMEOUT(!service.isBusy(), 2000);
        QTest::qWait(50);
        QCOMPARE(results.count(), 1);
    }
    void cancellationAndReuse()
    {
        RemoteDirectoryService service;
        QSignalSpy results(&service, &RemoteDirectoryService::finished);
        service.startLocalProcessForTest(powershell(),
                                         {QStringLiteral("-NoProfile"), QStringLiteral("-Command"),
                                          QStringLiteral("Start-Sleep -Seconds 10")},
                                         1000);
        service.cancel();
        QTRY_VERIFY_WITH_TIMEOUT(!service.isBusy(), 2000);
        QCOMPARE(results.count(), 0);
        service.startLocalProcessForTest(QStringLiteral("C:/nonexistent-orbidev-test.exe"), {}, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(results.count(), 1, 2000);
        QVERIFY(!service.isBusy());
        QVERIFY(!qvariant_cast<RemoteDirectoryResult>(results.first().first()).success);
    }
    void cancelledPreparationDoesNotLaunch()
    {
        RemoteDirectoryService service;
        QSignalSpy results(&service, &RemoteDirectoryService::finished);
        QSignalSpy busy(&service, &RemoteDirectoryService::busyChanged);
        sshsetup::ConnectionProfile profile;
        profile.host = QStringLiteral("-invalid");
        service.beginPreparation(profile, {}, 100);
        QVERIFY(service.isBusy());
        service.cancel();
        QVERIFY(service.isBusy());
        QTRY_VERIFY_WITH_TIMEOUT(!service.isBusy(), 2000);
        QCOMPARE(results.count(), 0);
        QCOMPARE(busy.count(), 2);
        service.start(profile, QString());
        QTRY_COMPARE_WITH_TIMEOUT(results.count(), 1, 2000);
    }
    void preparationTimeoutReportsOnceAndBlocksStacking()
    {
        RemoteDirectoryService service;
        QSignalSpy results(&service, &RemoteDirectoryService::finished);
        sshsetup::ConnectionProfile profile;
        profile.host = QStringLiteral("-invalid");
        service.beginPreparation(profile, {}, 300, 20);
        QTRY_COMPARE_WITH_TIMEOUT(results.count(), 1, 2000);
        QVERIFY(qvariant_cast<RemoteDirectoryResult>(results.first().first())
                    .error.contains(QStringLiteral("超时")));
        QVERIFY(service.isBusy());
        service.start(profile, QString());
        QCOMPARE(results.count(), 2);
        QVERIFY(qvariant_cast<RemoteDirectoryResult>(results.last().first())
                    .error.contains(QStringLiteral("收尾")));
        QTRY_VERIFY_WITH_TIMEOUT(!service.isBusy(), 2000);
        QCOMPARE(results.count(), 2);
    }
    void destructionDuringPreparationIsSafe()
    {
        auto *service = new RemoteDirectoryService;
        sshsetup::ConnectionProfile profile;
        profile.host = QStringLiteral("-invalid");
        service->beginPreparation(profile, {}, 100);
        delete service;
        QTest::qWait(200);
    }
    void processOutputLimit()
    {
        RemoteDirectoryService service;
        QSignalSpy results(&service, &RemoteDirectoryService::finished);
        service.startLocalProcessForTest(powershell(),
                                         {QStringLiteral("-NoProfile"), QStringLiteral("-Command"),
                                          QStringLiteral("[Console]::Out.Write(('x' * 3145728))")},
                                         10000);
        QTRY_COMPARE_WITH_TIMEOUT(results.count(), 1, 12000);
        QVERIFY(qvariant_cast<RemoteDirectoryResult>(results.first().first())
                    .error.contains(QStringLiteral("2 MiB")));
        QTRY_VERIFY_WITH_TIMEOUT(!service.isBusy(), 2000);
        QCOMPARE(results.count(), 1);
    }
    void shortBootstrapHandlesLongPathThroughCmd()
    {
        // 不需要真实长目录：即便路径不存在，也必须返回查询 JSON，而不是 cmd 长命令错误。
        const QString path = QStringLiteral("C:/") + QStringLiteral("中文long/").repeated(100);
        QStringList command;
        QString error;
        QVERIFY(remoteDirectoryCommandArguments(path, command, error));
        QVERIFY(command.join(QLatin1Char(' ')).size() > 8191);
        QByteArray input;
        const auto transport = RemoteDirectoryService::directoryTransportArguments(command, input);
        QVERIFY(transport.join(QLatin1Char(' ')).size() < 1024);
        QVERIFY(!input.isEmpty());
        QProcess process;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("PATH"), qEnvironmentVariable("SystemRoot") +
                                                       QStringLiteral("/System32/WindowsPowerShell/v1.0;") +
                                                       environment.value(QStringLiteral("PATH")));
        process.setProcessEnvironment(environment);
        process.setProgram(qEnvironmentVariable("ComSpec"));
        // cmd 自行解析命令尾部，不能让 QProcess 将整条远端命令当作单个参数加引号。
        process.setNativeArguments(QStringLiteral("/d /s /c ") + transport.join(QLatin1Char(' ')));
        process.start();
        QVERIFY(process.waitForStarted(3000));
        QCOMPARE(process.write(input), qint64(input.size()));
        process.closeWriteChannel();
        QVERIFY(process.waitForFinished(10000));
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QJsonParseError parse;
        const auto output = QJsonDocument::fromJson(process.readAllStandardOutput(), &parse);
        QVERIFY2(parse.error == QJsonParseError::NoError, process.readAllStandardError().constData());
        QVERIFY(output.isObject());
        QVERIFY(output.object().value(QStringLiteral("success")).isBool());
        QVERIFY(!output.object().value(QStringLiteral("success")).toBool());
    }
    void failedStartAfterCancellationReleasesBusy()
    {
        RemoteDirectoryService service;
        QSignalSpy results(&service, &RemoteDirectoryService::finished);
        service.startLocalProcessForTest(QStringLiteral("C:/nonexistent-orbidev-test.exe"), {}, 1000);
        // Windows 可以同步报告启动失败；取消只禁止尚未交付的结果，不撤回已交付的失败。
        const int beforeCancel = results.count();
        QVERIFY(beforeCancel <= 1);
        service.cancel();
        QTRY_VERIFY_WITH_TIMEOUT(!service.isBusy(), 2000);
        QCOMPARE(results.count(), beforeCancel);
        service.startLocalProcessForTest(QStringLiteral("C:/nonexistent-orbidev-test.exe"), {}, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(results.count(), beforeCancel + 1, 2000);
        QVERIFY(!service.isBusy());
    }
    void combinedOutputLimit()
    {
        RemoteDirectoryService service;
        QSignalSpy results(&service, &RemoteDirectoryService::finished);
        service.startLocalProcessForTest(
            powershell(),
            {QStringLiteral("-NoProfile"), QStringLiteral("-Command"),
             QStringLiteral(
                 "[Console]::Out.Write(('x' * 1500000)); [Console]::Error.Write(('y' * 1500000))")},
            10000);
        QTRY_COMPARE_WITH_TIMEOUT(results.count(), 1, 12000);
        QVERIFY(qvariant_cast<RemoteDirectoryResult>(results.first().first())
                    .error.contains(QStringLiteral("2 MiB")));
        QTRY_VERIFY_WITH_TIMEOUT(!service.isBusy(), 2000);
        QCOMPARE(results.count(), 1);
    }
    void invalidProfileDoesNotStartSsh()
    {
        RemoteDirectoryService service;
        QSignalSpy results(&service, &RemoteDirectoryService::finished);
        sshsetup::ConnectionProfile profile;
        profile.host = QStringLiteral("-invalid");
        service.start(profile, QString());
        QTRY_COMPARE_WITH_TIMEOUT(results.count(), 1, 3000);
        QVERIFY(!qvariant_cast<RemoteDirectoryResult>(results.first().first()).success);
        QVERIFY(!service.isBusy());
    }
};
}
QTEST_GUILESS_MAIN(sshworkbench::RemoteDirectoryServiceTests)
#include "RemoteDirectoryServiceTests.moc"
