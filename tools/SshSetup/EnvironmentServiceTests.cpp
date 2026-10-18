#include "AuthorizationService.h"
#include "EnvironmentService.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
using namespace sshsetup;
class EnvironmentServiceTests : public QObject
{
    Q_OBJECT
private slots:
    void optionalLiveConnection()
    {
        const QString path = qEnvironmentVariable("ORBIDEV_LIVE_SETUP_CONFIG");
        if (path.isEmpty())
            QSKIP("未指定真实双端只读验证配置。");
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto object = QJsonDocument::fromJson(file.readAll()).object();
        ConnectionProfile profile;
        QString error;
        QVERIFY2(importConnectionInfo(path, profile, error), qPrintable(error));
        profile.identityFile = object.value(QStringLiteral("identity_file")).toString();
        QVERIFY(!profile.identityFile.isEmpty());
        const ConnectionResult result = testConnection(profile);
        QVERIFY2(result.networkReachable, qPrintable(result.details));
        QVERIFY2(result.sshReachable, qPrintable(result.details));
        QVERIFY2(result.authenticated, qPrintable(result.details));
        QVERIFY2(result.claudeAvailable, qPrintable(result.details));
        // 通过已有严格主机信任的连接读取主机公钥，不依赖 known_hosts 当前保存的算法。
        const QString script = QStringLiteral(
            "Get-Content -LiteralPath (Join-Path $env:ProgramData 'ssh/ssh_host_ed25519_key.pub')");
        QByteArray encoded;
        for (const auto character : script)
        {
            encoded.append(char(character.unicode() & 255));
            encoded.append(char(character.unicode() >> 8));
        }
        QProcess lookup;
        lookup.start(qEnvironmentVariable("SystemRoot") + QStringLiteral("/System32/OpenSSH/ssh.exe"),
                     {QStringLiteral("-F"),
                      QStringLiteral("NUL"),
                      QStringLiteral("-o"),
                      QStringLiteral("BatchMode=yes"),
                      QStringLiteral("-o"),
                      QStringLiteral("StrictHostKeyChecking=yes"),
                      QStringLiteral("-o"),
                      QStringLiteral("IdentitiesOnly=yes"),
                      QStringLiteral("-o"),
                      QStringLiteral("IdentityAgent=none"),
                      QStringLiteral("-o"),
                      QStringLiteral("ConnectTimeout=8"),
                      QStringLiteral("-i"),
                      profile.identityFile,
                      QStringLiteral("-p"),
                      QString::number(profile.port),
                      profile.user + QLatin1Char('@') + profile.host,
                      QStringLiteral("powershell.exe"),
                      QStringLiteral("-NoProfile"),
                      QStringLiteral("-NonInteractive"),
                      QStringLiteral("-EncodedCommand"),
                      QString::fromLatin1(encoded.toBase64())});
        QVERIFY(lookup.waitForStarted(3000));
        QVERIFY(lookup.waitForFinished(15000));
        QString expectedFingerprint;
        for (const auto &line : lookup.readAllStandardOutput().split('\n'))
        {
            const int offset = line.indexOf("ssh-ed25519 ");
            QString normalized;
            if (offset >= 0)
                validatePublicKey(QString::fromUtf8(line.mid(offset)).trimmed(), normalized,
                                  expectedFingerprint, error);
        }
        QVERIFY2(!expectedFingerprint.isEmpty(), "需通过已信任的连接读取家庭 Ed25519 主机公钥。");
        profile.hostFingerprint = expectedFingerprint;
        const auto pinned = testConnection(profile);
        QVERIFY2(pinned.authenticated && pinned.claudeAvailable, qPrintable(pinned.details));
        profile.hostFingerprint[7] =
            profile.hostFingerprint[7] == QLatin1Char('A') ? QLatin1Char('B') : QLatin1Char('A');
        const auto mismatch = testConnection(profile);
        QVERIFY(mismatch.sshReachable);
        QVERIFY(!mismatch.authenticated);
        QVERIFY(mismatch.details.contains(QStringLiteral("指纹")));
    }
    void rejectsCommandArguments()
    {
        QString error;
        ConnectionProfile profile{QStringLiteral("-oProxyCommand=bad"), QStringLiteral("homeuser")};
        QVERIFY(!validateProfile(profile, error));
        profile.host = QStringLiteral("home");
        profile.port = 0;
        QVERIFY(!validateProfile(profile, error));
        profile.port = 22;
        profile.user = QStringLiteral("user;whoami");
        QVERIFY(!validateProfile(profile, error));
    }
    void roundTrip()
    {
        QTemporaryDir directory;
        QString error;
        ConnectionProfile expected;
        expected.host = QStringLiteral("home");
        expected.user = QStringLiteral("homeuser");
        const QString path = directory.filePath(QStringLiteral("connection.json"));
        QVERIFY2(saveProfile(path, expected, error), qPrintable(error));
        ConnectionProfile actual;
        QVERIFY2(loadProfile(path, actual, error), qPrintable(error));
        QCOMPARE(actual.host, expected.host);
        QCOMPARE(actual.port, 22);
    }
    void importDoesNotReplaceIdentity()
    {
        QTemporaryDir directory;
        QFile file(directory.filePath(QStringLiteral("connection.json")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{\"host\":\"home\",\"user\":\"test\",\"port\":22,\"identityFile\":\"evil\"}");
        file.close();
        ConnectionProfile profile;
        profile.identityFile = QStringLiteral("C:/local/key");
        QString error;
        QVERIFY(importConnectionInfo(file.fileName(), profile, error));
        QCOMPARE(profile.identityFile, QStringLiteral("C:/local/key"));
        QFile broken(directory.filePath(QStringLiteral("bad.json")));
        QVERIFY(broken.open(QIODevice::WriteOnly));
        broken.write("{\"host\":\"-bad\",\"port\":22}");
        broken.close();
        QVERIFY(!importConnectionInfo(broken.fileName(), profile, error));
        QCOMPARE(profile.host, QStringLiteral("home"));
    }
};
QTEST_GUILESS_MAIN(EnvironmentServiceTests)
#include "EnvironmentServiceTests.moc"
