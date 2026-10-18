#include "IdentityService.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QTemporaryDir>
#include <QtTest>

#define WIN32_LEAN_AND_MEAN

#include <windows.h>

#include <aclapi.h>
#include <sddl.h>

class IdentityServiceTests : public QObject
{
    Q_OBJECT
private slots:
    void generatesAndReuses();
    void incompletePairIsNotOverwritten();
    void mismatchedPublicKeyIsRejected();
    void unsafeExistingDirectoryIsRejected();
    void concurrentOperationIsRejected();
    void relativePathIsRejected();
    void existingAdministratorAccessIsCompatible();
};

void IdentityServiceTests::generatesAndReuses()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString directory = temporary.filePath(QStringLiteral("identity"));
    sshsetup::Identity first;
    QString error;
    QVERIFY2(sshsetup::ensureIdentity(directory, first, error), qPrintable(error));
    QVERIFY(first.publicKey.startsWith(QStringLiteral("ssh-ed25519 ")));
    QVERIFY(first.fingerprint.startsWith(QStringLiteral("SHA256:")));
    const QDateTime written = QFileInfo(first.privateKeyPath).lastModified();
    sshsetup::Identity second;
    QVERIFY2(sshsetup::ensureIdentity(directory, second, error), qPrintable(error));
    QCOMPARE(first.publicKey, second.publicKey);
    QCOMPARE(first.fingerprint, second.fingerprint);
    QCOMPARE(written, QFileInfo(first.privateKeyPath).lastModified());
    QVERIFY(!QFileInfo::exists(temporary.filePath(QStringLiteral("home_dev"))));
}

void IdentityServiceTests::incompletePairIsNotOverwritten()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString directory = temporary.filePath(QStringLiteral("identity"));
    sshsetup::Identity identity;
    QString error;
    QVERIFY2(sshsetup::ensureIdentity(directory, identity, error), qPrintable(error));
    const QDateTime written = QFileInfo(identity.privateKeyPath).lastModified();
    QVERIFY(QFile::remove(identity.privateKeyPath + QStringLiteral(".pub")));
    QVERIFY(!sshsetup::ensureIdentity(directory, identity, error));
    QVERIFY(error.contains(QStringLiteral("不完整")));
    QCOMPARE(written, QFileInfo(identity.privateKeyPath).lastModified());
    QVERIFY(!QFileInfo::exists(identity.privateKeyPath + QStringLiteral(".pub")));
}

void IdentityServiceTests::mismatchedPublicKeyIsRejected()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    sshsetup::Identity first;
    sshsetup::Identity second;
    QString error;
    QVERIFY2(sshsetup::ensureIdentity(temporary.filePath(QStringLiteral("first")), first, error),
             qPrintable(error));
    QVERIFY2(sshsetup::ensureIdentity(temporary.filePath(QStringLiteral("second")), second, error),
             qPrintable(error));
    QFile publicFile(first.privateKeyPath + QStringLiteral(".pub"));
    QVERIFY(publicFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(publicFile.write(second.publicKey.toUtf8()),
             static_cast<qint64>(second.publicKey.toUtf8().size()));
    publicFile.close();
    sshsetup::Identity inspected;
    inspected.fingerprint = QStringLiteral("unchanged");
    QVERIFY(!sshsetup::inspectIdentity(first.privateKeyPath, inspected, error));
    QVERIFY(error.contains(QStringLiteral("不匹配")));
    QCOMPARE(inspected.fingerprint, QStringLiteral("unchanged"));
}

void IdentityServiceTests::unsafeExistingDirectoryIsRejected()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString directory = temporary.filePath(QStringLiteral("identity"));
    QVERIFY(QDir().mkpath(directory));
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    QVERIFY(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;FA;;;WD)", SDDL_REVISION_1,
                                                                 &descriptor, nullptr));
    const BOOL applied =
        SetFileSecurityW(reinterpret_cast<LPCWSTR>(directory.utf16()),
                         DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, descriptor);
    LocalFree(descriptor);
    QVERIFY(applied);
    sshsetup::Identity identity;
    QString error;
    // 明确设置过宽规则，验证拒绝过程不会生成私钥或静默修复权限。
    QVERIFY(!sshsetup::ensureIdentity(directory, identity, error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!QFileInfo::exists(QDir(directory).filePath(QStringLiteral("id_ed25519"))));
}

void IdentityServiceTests::concurrentOperationIsRejected()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString directory = temporary.filePath(QStringLiteral("identity"));
    sshsetup::Identity identity;
    QString error;
    QVERIFY2(sshsetup::ensureIdentity(directory, identity, error), qPrintable(error));
    QLockFile lock(QDir(directory).filePath(QStringLiteral("identity.lock")));
    QVERIFY(lock.tryLock());
    QVERIFY(!sshsetup::ensureIdentity(directory, identity, error));
    QVERIFY(error.contains(QStringLiteral("正在进行")));
}

void IdentityServiceTests::relativePathIsRejected()
{
    sshsetup::Identity identity;
    QString error;
    QVERIFY(!sshsetup::ensureIdentity(QStringLiteral("relative-identity"), identity, error));
    QVERIFY(error.contains(QStringLiteral("绝对路径")));
}

void IdentityServiceTests::existingAdministratorAccessIsCompatible()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString directory = temporary.filePath(QStringLiteral("identity"));
    sshsetup::Identity identity;
    QString error;
    QVERIFY2(sshsetup::ensureIdentity(directory, identity, error), qPrintable(error));
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR original = nullptr;
    QCOMPARE(GetNamedSecurityInfoW(
                 const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(identity.privateKeyPath.utf16())),
                 SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &original),
             DWORD(ERROR_SUCCESS));
    LPWSTR ownerText = nullptr;
    const BOOL converted = ConvertSidToStringSidW(owner, &ownerText);
    LocalFree(original);
    QVERIFY(converted);
    const QString descriptorText =
        QStringLiteral("D:P(A;;FA;;;%1)(A;;FA;;;SY)(A;;FA;;;BA)").arg(QString::fromWCharArray(ownerText));
    LocalFree(ownerText);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    QVERIFY(ConvertStringSecurityDescriptorToSecurityDescriptorW(
        reinterpret_cast<LPCWSTR>(descriptorText.utf16()), SDDL_REVISION_1, &descriptor, nullptr));
    const BOOL applied =
        SetFileSecurityW(reinterpret_cast<LPCWSTR>(identity.privateKeyPath.utf16()),
                         DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, descriptor);
    LocalFree(descriptor);
    QVERIFY(applied);
    sshsetup::Identity inspected;
    QVERIFY2(sshsetup::inspectIdentity(identity.privateKeyPath, inspected, error), qPrintable(error));
    QCOMPARE(inspected.fingerprint, identity.fingerprint);
    QVERIFY(!sshsetup::ensureIdentity(directory, inspected, error));
}

QTEST_GUILESS_MAIN(IdentityServiceTests)
#include "IdentityServiceTests.moc"
