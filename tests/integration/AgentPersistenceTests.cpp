#include <QCryptographicHash>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QtTest>
#include <transport/WebSocketAgentClient.h>

class AgentPersistenceTests : public QObject
{
    Q_OBJECT
private slots:
    void crashAndRestart()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QTcpServer reservation;
        QVERIFY(reservation.listen(QHostAddress::LocalHost, 0));
        const auto port = reservation.serverPort();
        reservation.close();
        // 固定测试凭据只用于本机临时服务，禁止使用真实用户配置。
        const QByteArray token(64, 'b');
        const QJsonObject config{
            {"port", port},
            {"clients", QJsonArray{QJsonObject{
                            {"id", "tester"},
                            {"token_sha256",
                             QString::fromLatin1(
                                 QCryptographicHash::hash(token, QCryptographicHash::Sha256).toHex())}}}},
            {"workspaces", QJsonArray{QJsonObject{{"id", "workspace"},
                                                  {"path", directory.path()},
                                                  {"allowed_clients", QJsonArray{"tester"}}}}},
            {"fake_cli_path", qEnvironmentVariable("RAD_FAKECLI_PATH")}};
        const QString configPath = directory.filePath("agent.json");
        QFile configuration(configPath);
        QVERIFY(configuration.open(QIODevice::WriteOnly));
        const auto bytes = QJsonDocument(config).toJson();
        QCOMPARE(configuration.write(bytes), qint64(bytes.size()));
        configuration.close();
        QProcess agent;
        agent.setProcessChannelMode(QProcess::MergedChannels);
        const auto start = [&]()
        {
            agent.start(qEnvironmentVariable("RAD_AGENT_PATH"), {"--config", configPath});
            if (!agent.waitForStarted(10000))
                return false;
            QByteArray output;
            for (int i = 0; i < 100 && agent.state() != QProcess::NotRunning; ++i)
            {
                agent.waitForReadyRead(100);
                output += agent.readAll();
                if (output.contains("RemoteAgent "))
                    return true;
            }
            return false;
        };
        QVERIFY(start());
        rad::client::WebSocketAgentClient client(
            {QUrl(QString("ws://127.0.0.1:%1/").arg(port)), token, {"workspace"}});
        bool complete = false;
        QString failure;
        const auto completion = [&](const QString &error, const QString &)
        {
            failure = error;
            complete = true;
        };
        client.connectAgent(completion);
        QTRY_VERIFY_WITH_TIMEOUT(complete, 10000);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        complete = false;
        QString id;
        client.createSession("恢复验证", "workspace",
                             [&](const QString &error, const QString &created)
                             {
                                 failure = error;
                                 id = created;
                                 complete = true;
                             });
        QTRY_VERIFY_WITH_TIMEOUT(complete, 10000);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QVERIFY(!id.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(
            !client.sessions().isEmpty() && client.sessions().first().processState == "running", 10000);
        // 同目录第二实例必须在恢复数据库之前拒绝，不能影响原进程。
        QProcess duplicate;
        duplicate.start(qEnvironmentVariable("RAD_AGENT_PATH"), {"--config", configPath});
        QVERIFY(duplicate.waitForFinished(10000));
        QVERIFY(duplicate.exitCode() != 0);
        agent.kill();
        QVERIFY(agent.waitForFinished(10000));
        QTRY_VERIFY_WITH_TIMEOUT(!client.isConnected(), 10000);
        QVERIFY(start());
        complete = false;
        client.connectAgent(completion);
        QTRY_VERIFY_WITH_TIMEOUT(complete, 10000);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QCOMPARE(client.sessions().size(), 1);
        QCOMPARE(client.sessions().first().id, id);
        QCOMPARE(client.sessions().first().processState, QString("failed"));
        QVERIFY(!client.sessions().first().running);
        QVERIFY(!client.sessions().first().exitCodeKnown);
        complete = false;
        client.attachSession(id, completion);
        QTRY_VERIFY_WITH_TIMEOUT(complete, 10000);
        QVERIFY(!failure.isEmpty());
        client.disconnectAgent();
        agent.kill();
        QVERIFY(agent.waitForFinished(10000));
        QDirIterator stored(directory.filePath("agent-data"), QDir::Files, QDirIterator::Subdirectories);
        int fileCount = 0;
        while (stored.hasNext())
        {
            QFile file(stored.next());
            QVERIFY(file.open(QIODevice::ReadOnly));
            QVERIFY(!file.readAll().contains(token));
            ++fileCount;
        }
        QVERIFY(fileCount >= 2);
    }
};
QTEST_GUILESS_MAIN(AgentPersistenceTests)
#include "AgentPersistenceTests.moc"
