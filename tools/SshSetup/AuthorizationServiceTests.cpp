#include "AuthorizationService.h"
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <windows.h>

#include <sddl.h>

namespace
{
QString publicKey(char byte = 'a')
{
    QByteArray blob = QByteArray::fromHex("0000000b") + QByteArray("ssh-ed25519") +
                      QByteArray::fromHex("00000020") + QByteArray(32, byte);
    return QStringLiteral("ssh-ed25519 ") + QString::fromLatin1(blob.toBase64()) +
           QStringLiteral(" company-device");
}
QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}
}
class AuthorizationServiceTests final : public QObject
{
    Q_OBJECT
private slots:
    void explainsFingerprintInsteadOfPublicKey()
    {
        QString normalized, fingerprint, error;
        for (const QString &input :
             {QStringLiteral("SHA256:abc123"), QStringLiteral("256 SHA256:abc123 device (ED25519)")})
        {
            QVERIFY(!sshsetup::validatePublicKey(input, normalized, fingerprint, error));
            QVERIFY(error.contains(QStringLiteral("指纹")));
            QVERIFY(error.contains(QStringLiteral(".pub")));
            QVERIFY(error.contains(QStringLiteral("复制公钥")));
            QVERIFY(normalized.isEmpty());
            QVERIFY(fingerprint.isEmpty());
        }
    }
    void warnsOnlyForExplicitLoopbackConfiguration()
    {
        for (const QString &configuration :
             {QStringLiteral("ListenAddress 127.0.0.1\n"),
              QStringLiteral("ListenAddress ::1\nListenAddress 127.0.0.2:2222\n"),
              QStringLiteral("ListenAddress [::1]:22 # local\n")})
        {
            const QString warning = sshsetup::sshListenConfigurationWarning(configuration);
            QVERIFY(warning.contains(QStringLiteral("仅配置了回环")));
            QVERIFY(warning.contains(QStringLiteral("实际监听")));
        }
        for (const QString &configuration :
             {QStringLiteral("#ListenAddress 127.0.0.1\n"), QStringLiteral("ListenAddress 0.0.0.0\n"),
              QStringLiteral("ListenAddress 127.0.0.1\nListenAddress 100.64.0.1\n"),
              QStringLiteral("ListenAddress ::\n"), QStringLiteral("ListenAddress 127.999.0.1\n")})
            QVERIFY(!sshsetup::sshListenConfigurationWarning(configuration)
                         .contains(QStringLiteral("仅配置了回环")));
        QVERIFY(
            sshsetup::sshListenConfigurationWarning(QStringLiteral("ListenAddress 127.0.0.1\nInclude custom"))
                .contains(QStringLiteral("无法")));
    }
    void redactsHomeEnvironmentReport()
    {
        sshsetup::HomeEnvironment environment;
        environment.hostName = QStringLiteral("private-host");
        environment.userName = QStringLiteral("private-user");
        environment.host = QStringLiteral("100.64.0.99");
        environment.serviceExecutable = QStringLiteral("C:/private-install/sshd.exe");
        environment.authorizedKeysPath = QStringLiteral("C:/private-keys/authorized_keys");
        environment.error = QStringLiteral("secret-error-with-path");
        environment.listenWarning = QStringLiteral("secret-warning");
        QString report = sshsetup::homeEnvironmentReport(environment);
        QVERIFY(report.contains(QStringLiteral("未检测到安装")));
        environment.serviceInstalled = true;
        environment.serviceState = QStringLiteral("未运行");
        report = sshsetup::homeEnvironmentReport(environment);
        QVERIFY(report.contains(QStringLiteral("未运行")));
        environment.sshAvailable = true;
        report = sshsetup::homeEnvironmentReport(environment);
        QVERIFY(report.contains(QStringLiteral("运行中")));
        for (const QString &secret :
             {environment.hostName, environment.userName, environment.host, environment.serviceExecutable,
              environment.authorizedKeysPath, environment.error, environment.listenWarning})
            QVERIFY(!report.contains(secret));
    }
    void distinguishesLockedConfiguration()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("sshd_config"));
        HANDLE locked = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ | GENERIC_WRITE, 0,
                                    nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(locked != INVALID_HANDLE_VALUE);
        QString configuration, error;
        const bool read = sshsetup::readSshConfiguration(path, configuration, error);
        CloseHandle(locked);
        QVERIFY(!read);
        QVERIFY(error.contains(QStringLiteral("独占")));
    }
    void distinguishesDeniedConfiguration()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("sshd_config"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("Port 22\n");
        file.close();
        DWORD required = 0;
        GetFileSecurityW(reinterpret_cast<LPCWSTR>(path.utf16()), DACL_SECURITY_INFORMATION, nullptr, 0,
                         &required);
        QByteArray original(int(required), '\0');
        QVERIFY(GetFileSecurityW(reinterpret_cast<LPCWSTR>(path.utf16()), DACL_SECURITY_INFORMATION,
                                 original.data(), required, &required));
        PSECURITY_DESCRIPTOR denied = nullptr;
        QVERIFY(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(D;;FR;;;WD)(A;;FA;;;OW)",
                                                                     SDDL_REVISION_1, &denied, nullptr));
        const BOOL applied =
            SetFileSecurityW(reinterpret_cast<LPCWSTR>(path.utf16()),
                             DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, denied);
        LocalFree(denied);
        QVERIFY(applied);
        QString configuration, error;
        const bool read = sshsetup::readSshConfiguration(path, configuration, error);
        const BOOL restored = SetFileSecurityW(
            reinterpret_cast<LPCWSTR>(path.utf16()),
            DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION, original.data());
        QVERIFY(restored);
        QVERIFY(!read);
        QVERIFY(error.contains(QStringLiteral("没有权限")));
    }
    void distinguishesMissingConfiguration()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("ssh/sshd_config"));
        QString configuration = QStringLiteral("unchanged"), error;
        QVERIFY(!sshsetup::readSshConfiguration(path, configuration, error));
        QCOMPARE(configuration, QStringLiteral("unchanged"));
        QVERIFY(error.contains(QStringLiteral("不存在")));
        QVERIFY(error.contains(QStringLiteral("sshd_config")));
        QVERIFY(!QFile::exists(path));
    }
    void readsConfigurationWithoutChangingFile()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("sshd_config"));
        const QByteArray contents = QByteArray::fromHex("efbbbf") + "Port 2222\r\n";
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(contents), qint64(contents.size()));
        file.close();
        QString configuration, error;
        QVERIFY2(sshsetup::readSshConfiguration(path, configuration, error), qPrintable(error));
        QCOMPARE(configuration, QStringLiteral("Port 2222\r\n"));
        QCOMPARE(readFile(path), contents);
    }
    void rejectsChangedPreview()
    {
        sshsetup::HomeEnvironment expected;
        expected.userName = QStringLiteral("home-user");
        expected.host = QStringLiteral("100.64.0.1");
        expected.authorizedKeysPath = QStringLiteral("C:/Users/home-user/.ssh/authorized_keys");
        expected.hostFingerprint = QStringLiteral("SHA256:test");
        expected.sshAvailable = true;
        QString error;
        QVERIFY(sshsetup::matchesAuthorizationPreview(expected, expected, error));
        auto changed = expected;
        changed.authorizedKeysPath = QStringLiteral("C:/ProgramData/ssh/administrators_authorized_keys");
        QVERIFY(!sshsetup::matchesAuthorizationPreview(expected, changed, error));
        changed = expected;
        changed.userName = QStringLiteral("another-user");
        QVERIFY(!sshsetup::matchesAuthorizationPreview(expected, changed, error));
        changed = expected;
        changed.port = 2222;
        QVERIFY(!sshsetup::matchesAuthorizationPreview(expected, changed, error));
        changed = expected;
        changed.host = QStringLiteral("100.64.0.2");
        QVERIFY(!sshsetup::matchesAuthorizationPreview(expected, changed, error));
        changed = expected;
        changed.hostFingerprint = QStringLiteral("SHA256:changed");
        QVERIFY(!sshsetup::matchesAuthorizationPreview(expected, changed, error));
        changed = expected;
        changed.sshAvailable = false;
        QVERIFY(!sshsetup::matchesAuthorizationPreview(expected, changed, error));
        changed = expected;
        changed.serviceExecutable = QStringLiteral("C:/other/sshd.exe");
        QVERIFY(!sshsetup::matchesAuthorizationPreview(expected, changed, error));
        changed = expected;
        changed.hostName = QStringLiteral("another-host");
        QVERIFY(!sshsetup::matchesAuthorizationPreview(expected, changed, error));
    }
    void validatesPublicKey()
    {
        QString normalized, fingerprint, error;
        QVERIFY(sshsetup::validatePublicKey(publicKey(), normalized, fingerprint, error));
        QVERIFY(fingerprint.startsWith(QStringLiteral("SHA256:")));
        QCOMPARE(normalized, publicKey());
        for (const QString &invalid :
             {QStringLiteral("-----BEGIN OPENSSH PRIVATE KEY-----"),
              QStringLiteral("command=\"whoami\" ") + publicKey(),
              publicKey() + QLatin1Char('\n') + publicKey(), QStringLiteral("ssh-ed25519 AAAA"),
              publicKey().replace(QStringLiteral("ssh-ed25519"), QStringLiteral("ssh-rsa")), publicKey('\0')})
        {
            QVERIFY(!sshsetup::validatePublicKey(invalid, normalized, fingerprint, error));
            QVERIFY(!error.isEmpty());
        }
    }
    void resolvesDefaultPaths()
    {
        const QString config =
            QStringLiteral("Port 2222\nAuthorizedKeysFile .ssh/authorized_keys\nMatch Group administrators\n "
                           "AuthorizedKeysFile __PROGRAMDATA__/ssh/administrators_authorized_keys\n");
        QString path, error;
        int port = 0;
        QVERIFY(sshsetup::resolveAuthorizedKeys(config, false, QStringLiteral("C:/Users/test"),
                                                QStringLiteral("C:/ProgramData"), path, port, error));
        QCOMPARE(path, QStringLiteral("C:/Users/test/.ssh/authorized_keys"));
        QCOMPARE(port, 2222);
        QVERIFY(sshsetup::resolveAuthorizedKeys(config, true, QStringLiteral("C:/Users/test"),
                                                QStringLiteral("C:/ProgramData"), path, port, error));
        QCOMPARE(path, QStringLiteral("C:/ProgramData/ssh/administrators_authorized_keys"));
        for (const QString &bad : {QStringLiteral("Include other.conf"), QStringLiteral("Match User test"),
                                   QStringLiteral("AuthorizedKeysFile other"), QStringLiteral("Port 0"),
                                   QStringLiteral("AuthorizedKeysCommand cmd")})
            QVERIFY(!sshsetup::resolveAuthorizedKeys(bad, false, {}, {}, path, port, error));
    }
    void preservesAndDeduplicates()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("authorized_keys"));
        const QByteArray original = "# existing\r\n" + publicKey('b').toUtf8();
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(original), qint64(original.size()));
        file.close();
        QString error;
        const auto secured = [](QString &) { return true; };
        QVERIFY(sshsetup::appendAuthorizedKey(path, publicKey(), secured, error));
        const QByteArray appended = readFile(path);
        QVERIFY(appended.startsWith(original));
        QVERIFY(sshsetup::appendAuthorizedKey(path, publicKey() + QStringLiteral(" other-comment"), secured,
                                              error));
        QCOMPARE(readFile(path), appended);
        QCOMPARE(QDir(directory.path()).entryList({QStringLiteral("*.orbidev-backup-*")}, QDir::Files).size(),
                 1);
    }
    void preservesRestrictedExistingKey()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("authorized_keys"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray original = "restrict,command=\"some command\" " + publicKey().toUtf8() + "\r\n";
        file.write(original);
        file.close();
        QString error;
        QVERIFY(sshsetup::appendAuthorizedKey(path, publicKey(), [](QString &) { return true; }, error));
        QCOMPARE(readFile(path), original);
    }
    void rollsBackPermissionFailure()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("authorized_keys"));
        QString error;
        const auto denied = [](QString &failure)
        {
            failure = QStringLiteral("权限失败");
            return false;
        };
        QVERIFY(!sshsetup::appendAuthorizedKey(path, publicKey(), denied, error));
        QVERIFY(!QFile::exists(path));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray original("# original\r\n");
        file.write(original);
        file.close();
        QVERIFY(!sshsetup::appendAuthorizedKey(path, publicKey(), denied, error));
        QCOMPARE(readFile(path), original);
        QVERIFY(error.contains(QStringLiteral("恢复")));
    }
};
QTEST_GUILESS_MAIN(AuthorizationServiceTests)
#include "AuthorizationServiceTests.moc"
