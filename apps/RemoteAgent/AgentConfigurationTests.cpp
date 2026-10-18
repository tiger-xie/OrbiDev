#include "AgentConfiguration.h"
#include <FakeCli/FakeCliProvider.h>
#include <protocol/ProtocolCodec.h>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

class AgentConfigurationTests : public QObject
{
    Q_OBJECT
private slots:
    void validation()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString executable = directory.filePath(QStringLiteral("FakeCli.exe"));
        QFile fixture(executable);
        QVERIFY(fixture.open(QIODevice::WriteOnly));
        fixture.close();
        const QJsonObject client{{"id", "tester"}, {"token_sha256", QString(64, QLatin1Char('a'))}};
        const QJsonObject workspace{
            {"id", "project"}, {"path", directory.path()}, {"allowed_clients", QJsonArray{"tester"}}};
        const QJsonObject valid{{"clients", QJsonArray{client}},
                                {"workspaces", QJsonArray{workspace}},
                                {"fake_cli_path", executable}};
        const QString path = directory.filePath(QStringLiteral("agent.json"));
        const auto parse = [&](const QByteArray &bytes)
        {
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
                return rad::Result<rad::AgentConfiguration>::failure({rad::ErrorCode::SystemError, {}});
            file.close();
            return rad::loadAgentConfiguration(path);
        };
        const auto accepted = parse(QJsonDocument(valid).toJson());
        QVERIFY(accepted.isSuccess());
        QCOMPARE(accepted.value().server.port, quint16(17890));
        QCOMPARE(accepted.value().sessions.maxRetainedSessions, 32);
        // 非空控制字符每个编码为六字节，覆盖合法名称的最坏 JSON 膨胀。
        QJsonArray summaries;
        for (int index = 0; index < accepted.value().sessions.maxRetainedSessions; ++index)
            summaries.append(QJsonObject{{"session_id", rad::SessionId::create().toString()},
                                         {"name", QString(256, QChar(1))},
                                         {"provider_id", "fake"},
                                         {"workspace_id", QString(64, QLatin1Char('w'))},
                                         {"process_state", "creating"},
                                         {"attachable", false}});
        const rad::protocol::Envelope listed{rad::kProtocolVersion,
                                             "session.listed",
                                             rad::SessionId::create().toString(),
                                             {},
                                             {{"sessions", summaries}}};
        QByteArray encoded;
        rad::protocol::Error protocolError;
        QVERIFY(rad::protocol::encodeJson(listed, encoded, protocolError));
        QVERIFY(encoded.size() <= rad::protocol::kMaxFrameBytes);
        QCOMPARE(accepted.value().server.credentials.front().tokenSha256.size(), 32);
        QCOMPARE(accepted.value().sessions.workspaces.front().allowedClientIds, QStringList{"tester"});
        auto changed = valid;
        changed["port"] = 1.5;
        QVERIFY(!parse(QJsonDocument(changed).toJson()).isSuccess());
        changed = valid;
        changed["port"] = 0;
        QVERIFY(!parse(QJsonDocument(changed).toJson()).isSuccess());
        changed = valid;
        changed["unknown"] = true;
        QVERIFY(!parse(QJsonDocument(changed).toJson()).isSuccess());
        changed = valid;
        changed["clients"] = QJsonArray{client, client};
        QVERIFY(!parse(QJsonDocument(changed).toJson()).isSuccess());
        changed = valid;
        auto invalidClient = client;
        invalidClient["token_sha256"] = "secret-must-not-appear";
        changed["clients"] = QJsonArray{invalidClient};
        const auto failed = parse(QJsonDocument(changed).toJson());
        QVERIFY(!failed.isSuccess());
        QVERIFY(!failed.error().message.contains(QStringLiteral("secret-must-not-appear")));
        invalidClient = client;
        invalidClient["id"] = "tester\n";
        changed["clients"] = QJsonArray{invalidClient};
        QVERIFY(!parse(QJsonDocument(changed).toJson()).isSuccess());
        invalidClient = client;
        invalidClient["token_sha256"] = QString(64, QLatin1Char('a')) + QLatin1Char('\n');
        changed["clients"] = QJsonArray{invalidClient};
        QVERIFY(!parse(QJsonDocument(changed).toJson()).isSuccess());
        changed = valid;
        auto invalidWorkspace = workspace;
        invalidWorkspace["allowed_clients"] = QJsonArray{"unknown"};
        changed["workspaces"] = QJsonArray{invalidWorkspace};
        QVERIFY(!parse(QJsonDocument(changed).toJson()).isSuccess());
        changed = valid;
        changed["fake_cli_path"] = "relative.exe";
        QVERIFY(!parse(QJsonDocument(changed).toJson()).isSuccess());
        QByteArray duplicate = QJsonDocument(valid).toJson(QJsonDocument::Compact);
        duplicate.insert(1, "\"port\":17890,\"po\\u0072t\":17891,");
        QVERIFY(!parse(duplicate).isSuccess());
        QVERIFY(!parse(QByteArray(65537, ' ')).isSuccess());
        QVERIFY(!parse("{").isSuccess());
    }
    void fixedProvider()
    {
        rad::FakeCliProvider provider(QStringLiteral("C:/fixtures/FakeCli.exe"));
        rad::SessionRequest request;
        request.providerId = QStringLiteral("fake");
        request.workspaceId = QStringLiteral("project");
        rad::WorkspaceContext workspace{request.workspaceId, QStringLiteral("C:/workspace")};
        const auto launch = provider.makeLaunchSpec(request, workspace);
        QVERIFY(launch.isSuccess());
        QCOMPARE(launch.value().arguments, QStringList{QStringLiteral("--interactive")});
        QCOMPARE(launch.value().executablePath, QStringLiteral("C:/fixtures/FakeCli.exe"));
        request.worktreeName = QStringLiteral("untrusted");
        QVERIFY(!provider.makeLaunchSpec(request, workspace).isSuccess());
        request.worktreeName.clear();
        request.providerId = QStringLiteral("shell");
        QVERIFY(!provider.makeLaunchSpec(request, workspace).isSuccess());
    }
};
QTEST_GUILESS_MAIN(AgentConfigurationTests)
#include "AgentConfigurationTests.moc"
