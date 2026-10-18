#include "IdentityService.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QProcess>
#include <QStringList>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>

#include <aclapi.h>
#include <sddl.h>

namespace sshsetup
{
namespace
{
struct LocalBuffer
{
    void *value = nullptr;
    ~LocalBuffer()
    {
        if (value)
            LocalFree(value);
    }
};

bool currentSid(QByteArray &storage, QString &error)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    {
        error = QStringLiteral("无法读取当前 Windows 身份。");
        return false;
    }
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    storage.resize(static_cast<int>(size));
    const BOOL ok = GetTokenInformation(token, TokenUser, storage.data(), size, &size);
    CloseHandle(token);
    if (!ok)
        error = QStringLiteral("无法取得当前用户 SID。");
    return ok != FALSE;
}

bool safePath(const QString &path, QString &error)
{
    if (!QDir::isAbsolutePath(path) || path.startsWith(QStringLiteral("\\\\")) ||
        path.startsWith(QStringLiteral("//")))
    {
        error = QStringLiteral("身份路径必须是本机绝对路径。");
        return false;
    }
    QString cursor = QDir::cleanPath(path);
    while (!cursor.isEmpty())
    {
        const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(cursor.utf16()));
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        {
            error = QStringLiteral("身份路径不能经过符号链接或重解析目录。");
            return false;
        }
        const QString parent = QFileInfo(cursor).absolutePath();
        if (parent == cursor)
            break;
        cursor = parent;
    }
    return true;
}

bool protectedAcl(const QString &path, QString &error, bool allowAdministrators = false)
{
    QByteArray userStorage;
    if (!currentSid(userStorage, error))
        return false;
    const PSID user = reinterpret_cast<TOKEN_USER *>(userStorage.data())->User.Sid;
    BYTE systemStorage[SECURITY_MAX_SID_SIZE] = {};
    DWORD systemSize = sizeof(systemStorage);
    if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, systemStorage, &systemSize))
    {
        error = QStringLiteral("无法取得系统权限标识。");
        return false;
    }
    PACL acl = nullptr;
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD status = GetNamedSecurityInfoW(
        const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(path.utf16())), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, &acl, nullptr, &descriptor);
    LocalBuffer holder;
    holder.value = descriptor;
    if (status != ERROR_SUCCESS || !acl || !owner || !EqualSid(owner, user))
    {
        error = QStringLiteral("身份权限检查失败：必须由当前用户拥有，且仅当前用户与 SYSTEM 可访问。");
        return false;
    }
    bool userAllowed = false;
    for (DWORD index = 0; index < acl->AceCount; ++index)
    {
        void *entry = nullptr;
        if (!GetAce(acl, index, &entry))
        {
            error = QStringLiteral("无法检查身份访问权限。");
            return false;
        }
        const auto *header = static_cast<ACE_HEADER *>(entry);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE)
        {
            error = QStringLiteral("身份权限包含非标准规则，请手动检查后重试。");
            return false;
        }
        const auto *ace = static_cast<ACCESS_ALLOWED_ACE *>(entry);
        PSID sid = const_cast<DWORD *>(&ace->SidStart);
        if (!EqualSid(sid, user) && !EqualSid(sid, systemStorage) &&
            !(allowAdministrators && IsWellKnownSid(sid, WinBuiltinAdministratorsSid)))
        {
            error = QStringLiteral("身份文件权限过宽：请限制为当前用户与 SYSTEM 后重试。");
            return false;
        }
        if (EqualSid(sid, user) && !(header->AceFlags & INHERIT_ONLY_ACE) &&
            (ace->Mask & (FILE_READ_DATA | GENERIC_READ | GENERIC_ALL)))
            userAllowed = true;
    }
    if (!userAllowed)
        error = QStringLiteral("当前用户没有身份文件读取权限。");
    return userAllowed;
}

bool createProtectedDirectory(const QString &path, QString &error)
{
    QByteArray storage;
    if (!currentSid(storage, error))
        return false;
    LPWSTR sidText = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(storage.data())->User.Sid, &sidText))
    {
        error = QStringLiteral("无法构建身份目录权限。");
        return false;
    }
    LocalBuffer sidHolder;
    sidHolder.value = sidText;
    const QString sid = QString::fromWCharArray(sidText);
    const QString sddl = QStringLiteral("O:%1D:P(A;OICI;FA;;;%1)(A;OICI;FA;;;SY)").arg(sid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(reinterpret_cast<LPCWSTR>(sddl.utf16()),
                                                              SDDL_REVISION_1, &descriptor, nullptr))
    {
        error = QStringLiteral("无法构建身份目录安全描述符。");
        return false;
    }
    LocalBuffer descriptorHolder;
    descriptorHolder.value = descriptor;
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    if (!CreateDirectoryW(reinterpret_cast<LPCWSTR>(path.utf16()), &attributes) &&
        GetLastError() != ERROR_ALREADY_EXISTS)
    {
        error = QStringLiteral("无法创建受保护的身份目录，请检查父目录是否存在及写入权限。");
        return false;
    }
    return protectedAcl(path, error);
}

bool runKeygen(const QStringList &arguments, QByteArray &output, QString &error)
{
    wchar_t systemDirectory[MAX_PATH] = {};
    if (!GetSystemDirectoryW(systemDirectory, MAX_PATH))
    {
        error = QStringLiteral("无法定位 Windows 系统目录。");
        return false;
    }
    const QString executable =
        QDir(QString::fromWCharArray(systemDirectory)).filePath(QStringLiteral("OpenSSH/ssh-keygen.exe"));
    if (!QFileInfo::exists(executable))
    {
        error = QStringLiteral("未安装 Windows OpenSSH 客户端，无法管理 SSH 身份。");
        return false;
    }
    QProcess process;
    process.setProgram(executable);
    process.setArguments(arguments);
    process.start();
    if (!process.waitForStarted(5000))
    {
        error = QStringLiteral("无法启动系统 ssh-keygen。");
        return false;
    }
    process.closeWriteChannel();
    if (!process.waitForFinished(15000))
    {
        process.kill();
        process.waitForFinished(5000);
        error = QStringLiteral("SSH 身份操作超时，未完成的密钥对不会自动覆盖。");
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
    {
        error = QStringLiteral("SSH 密钥检查或生成失败；请检查密钥是否完整、无口令且权限正确。");
        return false;
    }
    output = process.readAllStandardOutput();
    return true;
}

bool secureNewPrivateKey(const QString &directory, const QString &keyPath, QString &error)
{
    if (!safePath(keyPath, error))
        return false;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD status = GetNamedSecurityInfoW(
        const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(directory.utf16())), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, nullptr, &descriptor);
    LocalBuffer holder;
    holder.value = descriptor;
    // Windows OpenSSH 会自行添加管理员规则，仅收紧本次新建文件，不触碰用户已有密钥。
    if (status != ERROR_SUCCESS ||
        !SetFileSecurityW(reinterpret_cast<LPCWSTR>(keyPath.utf16()),
                          DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, descriptor))
    {
        error = QStringLiteral("新密钥已生成，但无法收紧私钥权限；身份尚未准备好，请手动检查。");
        return false;
    }
    return protectedAcl(keyPath, error);
}

QString keyMaterial(const QString &text)
{
    const QStringList parts = text.simplified().split(QLatin1Char(' '));
    if (text.contains(QLatin1Char('\n')) || text.contains(QLatin1Char('\r')) || parts.size() < 2 ||
        !parts.first().startsWith(QStringLiteral("ssh-")))
        return {};
    return parts.at(0) + QLatin1Char(' ') + parts.at(1);
}
}

bool inspectIdentity(const QString &privateKeyPath, Identity &identity, QString &error)
{
    const QString publicPath = privateKeyPath + QStringLiteral(".pub");
    if (!safePath(privateKeyPath, error) || !safePath(publicPath, error))
        return false;
    if (!QFileInfo(privateKeyPath).isFile() || !QFileInfo(publicPath).isFile())
    {
        error = QStringLiteral("密钥对不完整：需要私钥及对应的 .pub 文件，不会覆盖已有文件。");
        return false;
    }
    if (!protectedAcl(privateKeyPath, error, true))
        return false;
    QFile publicFile(publicPath);
    if (!publicFile.open(QIODevice::ReadOnly) || publicFile.size() > 16384)
    {
        error = QStringLiteral("无法读取公钥，或公钥文件过大。");
        return false;
    }
    const QString publicKey = QString::fromUtf8(publicFile.readAll()).trimmed();
    QByteArray derived;
    if (!runKeygen(
            {QStringLiteral("-y"), QStringLiteral("-P"), QString(), QStringLiteral("-f"), privateKeyPath},
            derived, error))
        return false;
    if (keyMaterial(publicKey).isEmpty() ||
        keyMaterial(publicKey) != keyMaterial(QString::fromUtf8(derived).trimmed()))
    {
        error = QStringLiteral("公钥与私钥不匹配，不会自动覆盖。");
        return false;
    }
    QByteArray fingerprint;
    if (!runKeygen({QStringLiteral("-l"), QStringLiteral("-E"), QStringLiteral("sha256"),
                    QStringLiteral("-f"), publicPath},
                   fingerprint, error))
        return false;
    const QStringList fields = QString::fromUtf8(fingerprint).simplified().split(QLatin1Char(' '));
    if (fields.size() < 2 || !fields.at(1).startsWith(QStringLiteral("SHA256:")))
    {
        error = QStringLiteral("未取得有效公钥指纹。");
        return false;
    }
    identity = {QDir::cleanPath(privateKeyPath), publicKey, fields.at(1)};
    error.clear();
    return true;
}

bool ensureIdentity(const QString &directory, Identity &identity, QString &error)
{
    if (!safePath(directory, error))
        return false;
    const QString normalized = QDir::cleanPath(directory);
    if (!createProtectedDirectory(normalized, error))
        return false;
    QLockFile lock(QDir(normalized).filePath(QStringLiteral("identity.lock")));
    if (!lock.tryLock(0))
    {
        error = QStringLiteral("另一项身份操作正在进行，请稍后重试。");
        return false;
    }
    const QString keyPath = QDir(normalized).filePath(QStringLiteral("id_ed25519"));
    if (QFileInfo::exists(keyPath) || QFileInfo::exists(keyPath + QStringLiteral(".pub")))
    {
        Identity inspected;
        if (!inspectIdentity(keyPath, inspected, error) || !protectedAcl(keyPath, error))
            return false;
        identity = inspected;
        return true;
    }
    QByteArray ignored;
    if (!runKeygen({QStringLiteral("-q"), QStringLiteral("-t"), QStringLiteral("ed25519"),
                    QStringLiteral("-N"), QString(), QStringLiteral("-C"), QStringLiteral("OrbiDev"),
                    QStringLiteral("-f"), keyPath},
                   ignored, error))
        return false;
    if (!secureNewPrivateKey(normalized, keyPath, error))
        return false;
    return inspectIdentity(keyPath, identity, error);
}
}
