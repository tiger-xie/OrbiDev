#include "AuthorizationService.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>
#include <QUuid>
#include <QtEndian>
#include <memory>

#include <windows.h>

#include <sddl.h>

namespace sshsetup
{
namespace
{
bool saveBytes(const QString &path, const QByteArray &bytes, QString &error)
{
    // 原位更新保留现有所有者与 DACL；写入前的独立备份用于失败恢复。
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) != bytes.size() ||
        !file.flush())
    {
        error = QStringLiteral("无法保存授权文件：%1").arg(file.errorString());
        return false;
    }
    return true;
}
QString run(const QString &executable, const QStringList &arguments)
{
    if (executable.isEmpty())
        return {};
    QProcess process;
    process.start(executable, arguments);
    if (!process.waitForStarted(3000) || !process.waitForFinished(8000))
    {
        process.kill();
        process.waitForFinished(2000);
        return {};
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return {};
    return QString::fromUtf8(process.readAllStandardOutput()).trimmed();
}
bool currentIdentity(QString &sid, bool &administrator, QString &error)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    {
        error = QStringLiteral("无法读取当前 Windows 身份。");
        return false;
    }
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    QByteArray user(static_cast<int>(size), 0);
    if (!GetTokenInformation(token, TokenUser, user.data(), size, &size))
    {
        CloseHandle(token);
        error = QStringLiteral("无法读取用户 SID。");
        return false;
    }
    LPWSTR sidText = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(user.data())->User.Sid, &sidText))
    {
        CloseHandle(token);
        error = QStringLiteral("无法转换用户 SID。");
        return false;
    }
    sid = QString::fromWCharArray(sidText);
    LocalFree(sidText);
    GetTokenInformation(token, TokenGroups, nullptr, 0, &size);
    QByteArray groups(static_cast<int>(size), 0);
    if (!GetTokenInformation(token, TokenGroups, groups.data(), size, &size))
    {
        CloseHandle(token);
        error = QStringLiteral("无法读取账户组。");
        return false;
    }
    const auto *tokenGroups = reinterpret_cast<TOKEN_GROUPS *>(groups.data());
    for (DWORD index = 0; index < tokenGroups->GroupCount; ++index)
        if (IsWellKnownSid(tokenGroups->Groups[index].Sid, WinBuiltinAdministratorsSid))
            administrator = true;
    CloseHandle(token);
    return true;
}
bool secureAuthorizationFile(const QString &path, const QString &sid, bool administrator, QString &error)
{
    const QString descriptor = administrator ? QStringLiteral("D:P(A;;FA;;;SY)(A;;FA;;;BA)")
                                             : QStringLiteral("D:P(A;;FA;;;SY)(A;;FA;;;%1)").arg(sid);
    PSECURITY_DESCRIPTOR security = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(reinterpret_cast<LPCWSTR>(descriptor.utf16()),
                                                              SDDL_REVISION_1, &security, nullptr))
    {
        error = QStringLiteral("无法创建授权文件权限。");
        return false;
    }
    const bool applied =
        SetFileSecurityW(reinterpret_cast<LPCWSTR>(path.utf16()),
                         DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, security) != FALSE;
    const DWORD failure = GetLastError();
    LocalFree(security);
    if (!applied)
        error = QStringLiteral("设置授权文件权限失败（Windows %1）；管理员账户请使用当前账户提升权限重试。")
                    .arg(failure);
    return applied;
}
}

bool validatePublicKey(const QString &input, QString &normalized, QString &fingerprint, QString &error)
{
    error.clear();
    normalized.clear();
    fingerprint.clear();
    const QString line = input.trimmed();
    if (line.size() > 8192 || line.contains(QLatin1Char('\n')) || line.contains(QLatin1Char('\r')) ||
        line.contains(QStringLiteral("PRIVATE KEY")) || line.contains(QChar::Null))
    {
        error = QStringLiteral("请提供一行 Ed25519 公钥，不能导入私钥或多行内容。");
        return false;
    }
    if (QRegularExpression(QStringLiteral("^(?:\\d+[ \\t]+)?SHA256:")).match(line).hasMatch())
    {
        error = QStringLiteral("这是 SSH 指纹，不是公钥。请在公司端点击“复制公钥”，"
                               "或导入对应的 .pub 文件；不要提供私钥。");
        return false;
    }
    const auto match =
        QRegularExpression(
            QStringLiteral("^ssh-ed25519[ \\t]+([A-Za-z0-9+/]+={0,2})(?:[ \\t]+([^\\x00-\\x1f\\x7f]*))?$"))
            .match(line);
    if (!match.hasMatch())
    {
        error = QStringLiteral("仅接受不带 command 等授权选项的 Ed25519 公钥。");
        return false;
    }
    const QByteArray encoded = match.captured(1).toLatin1();
    const QByteArray bytes = QByteArray::fromBase64(encoded);
    const QByteArray canonical = bytes.toBase64();
    if (canonical != encoded || bytes.size() != 51 ||
        qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(bytes.constData())) != 11 ||
        bytes.mid(4, 11) != "ssh-ed25519" ||
        qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(bytes.constData() + 15)) != 32 ||
        bytes.mid(19) == QByteArray(32, '\0'))
    {
        error = QStringLiteral("公钥编码或 Ed25519 数据长度无效。");
        return false;
    }
    normalized = QStringLiteral("ssh-ed25519 ") + QString::fromLatin1(canonical);
    if (!match.captured(2).isEmpty())
        normalized += QLatin1Char(' ') + match.captured(2).trimmed();
    fingerprint = QStringLiteral("SHA256:") +
                  QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)
                                          .toBase64(QByteArray::OmitTrailingEquals));
    return true;
}

bool resolveAuthorizedKeys(const QString &configuration, bool administrator, const QString &home,
                           const QString &programData, QString &path, int &port, QString &error)
{
    error.clear();
    path.clear();
    port = 22;
    bool adminSection = false;
    bool adminOverride = false;
    bool globalKeysSeen = false;
    bool portSeen = false;
    for (QString line : configuration.split(QLatin1Char('\n')))
    {
        line = line.section(QLatin1Char('#'), 0, 0).trimmed();
        if (line.isEmpty())
            continue;
        const QStringList words = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        const QString name = words.first().toLower();
        if (name == QStringLiteral("match"))
        {
            if (words.size() != 3 || words[1].compare(QStringLiteral("Group"), Qt::CaseInsensitive) != 0 ||
                words[2].compare(QStringLiteral("administrators"), Qt::CaseInsensitive) != 0 || adminSection)
            {
                error = QStringLiteral("发现自定义 Match 规则，请人工核对 sshd_config 后授权。");
                return false;
            }
            adminSection = true;
        }
        else if (name == QStringLiteral("include") || name == QStringLiteral("authorizedkeyscommand") ||
                 name == QStringLiteral("hostkey") || name == QStringLiteral("authorizedkeyscommanduser") ||
                 name == QStringLiteral("pubkeyauthentication") ||
                 name == QStringLiteral("authenticationmethods"))
        {
            error = QStringLiteral("发现自定义认证规则 %1，已停止自动写入。").arg(name);
            return false;
        }
        else if (name == QStringLiteral("authorizedkeysfile"))
        {
            const QString expected =
                adminSection ? QStringLiteral("__PROGRAMDATA__/ssh/administrators_authorized_keys")
                             : QStringLiteral(".ssh/authorized_keys");
            if (words.size() != 2 || words[1].compare(expected, Qt::CaseInsensitive) != 0 ||
                (adminSection ? adminOverride : globalKeysSeen))
            {
                error = QStringLiteral("无法安全识别自定义 AuthorizedKeysFile，已停止自动写入。");
                return false;
            }
            if (adminSection)
                adminOverride = true;
            else
                globalKeysSeen = true;
        }
        else if (name == QStringLiteral("port"))
        {
            bool valid = false;
            const int parsed = words.size() == 2 ? words[1].toInt(&valid) : 0;
            if (!valid || parsed < 1 || parsed > 65535 || adminSection || portSeen)
            {
                error = QStringLiteral("SSH 端口配置无法自动识别。");
                return false;
            }
            port = parsed;
            portSeen = true;
        }
    }
    path = administrator && adminOverride
               ? QDir(programData).filePath(QStringLiteral("ssh/administrators_authorized_keys"))
               : QDir(home).filePath(QStringLiteral(".ssh/authorized_keys"));
    return true;
}

bool appendAuthorizedKey(const QString &path, const QString &publicKey,
                         const std::function<bool(QString &)> &secureFile, QString &error)
{
    QString normalized, fingerprint;
    if (!validatePublicKey(publicKey, normalized, fingerprint, error))
        return false;
    QString ancestor = QFileInfo(path).absoluteFilePath();
    while (!ancestor.isEmpty())
    {
        const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(ancestor.utf16()));
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        {
            error = QStringLiteral("授权路径包含重解析点，请人工核对目标后配置。");
            return false;
        }
        const QString parent = QFileInfo(ancestor).absolutePath();
        if (parent == ancestor)
            break;
        ancestor = parent;
    }
    if (QFileInfo(path).isSymLink() || !QDir().mkpath(QFileInfo(path).absolutePath()))
    {
        error = QStringLiteral("授权文件目录不可用或文件是符号链接。");
        return false;
    }
    QLockFile lock(path + QStringLiteral(".orbidev.lock"));
    if (!lock.tryLock(1000))
    {
        error = QStringLiteral("授权文件正在被其他配置操作使用。");
        return false;
    }
    const bool existed = QFile::exists(path);
    QByteArray previous;
    QByteArray previousSecurity;
    if (existed)
    {
        DWORD required = 0;
        GetFileSecurityW(reinterpret_cast<LPCWSTR>(path.utf16()), DACL_SECURITY_INFORMATION, nullptr, 0,
                         &required);
        previousSecurity.resize(static_cast<int>(required));
        if (required == 0 ||
            !GetFileSecurityW(reinterpret_cast<LPCWSTR>(path.utf16()), DACL_SECURITY_INFORMATION,
                              previousSecurity.data(), required, &required))
        {
            error = QStringLiteral("无法备份授权文件权限，未修改原文件。");
            return false;
        }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024)
        {
            error = QStringLiteral("无法读取授权文件，或文件超过安全处理大小。");
            return false;
        }
        previous = file.readAll();
        if (file.error() != QFile::NoError)
        {
            error = QStringLiteral("读取授权文件失败。");
            return false;
        }
    }
    const QString keyBlob = normalized.section(QLatin1Char(' '), 1, 1);
    bool present = false;
    const QRegularExpression existingKey(QStringLiteral(
        "^(?:(?:[^\\s\"]|\"[^\"]*\")+[ \\t]+)?ssh-ed25519[ \\t]+([A-Za-z0-9+/]+={0,2})(?:[ \\t]|$)"));
    for (const QByteArray &existingLine : previous.split('\n'))
    {
        const QString line = QString::fromUtf8(existingLine).trimmed();
        if (line.startsWith(QLatin1Char('#')))
            continue;
        const auto match = existingKey.match(line);
        if (match.hasMatch() && match.captured(1) == keyBlob)
            present = true;
    }
    const auto restorePermissions = [&]()
    {
        if (!existed)
            return true;
        SECURITY_DESCRIPTOR_CONTROL control = 0;
        DWORD revision = 0;
        if (!GetSecurityDescriptorControl(previousSecurity.data(), &control, &revision))
            return false;
        const SECURITY_INFORMATION flags =
            DACL_SECURITY_INFORMATION |
            ((control & SE_DACL_PROTECTED) ? PROTECTED_DACL_SECURITY_INFORMATION
                                           : UNPROTECTED_DACL_SECURITY_INFORMATION);
        return SetFileSecurityW(reinterpret_cast<LPCWSTR>(path.utf16()), flags, previousSecurity.data()) !=
               FALSE;
    };
    if (present)
    {
        if (secureFile(error))
            return true;
        if (!restorePermissions())
            error += QStringLiteral(" 原权限恢复失败，请人工检查。");
        return false;
    }
    const QString backup =
        path + QStringLiteral(".orbidev-backup-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (existed && !QFile::copy(path, backup))
    {
        error = QStringLiteral("备份授权文件失败，未修改原文件。");
        return false;
    }
    QByteArray updated = previous;
    if (!updated.isEmpty() && !updated.endsWith('\n'))
        updated += "\r\n";
    updated += normalized.toUtf8() + "\r\n";
    if (saveBytes(path, updated, error) && secureFile(error))
        return true;
    const QString originalError = error;
    QString rollbackError;
    bool restored = existed ? saveBytes(path, previous, rollbackError) : QFile::remove(path);
    if (restored && existed)
        restored = restorePermissions();
    error = originalError + (restored ? QStringLiteral(" 原授权内容已恢复。")
                                      : QStringLiteral(" 回滚失败，请从备份恢复：%1").arg(backup));
    return false;
}

bool readSshConfiguration(const QString &path, QString &configuration, QString &error)
{
    error.clear();
    const QString displayPath = QDir::toNativeSeparators(path);
    HANDLE file = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
            error = QStringLiteral("SSH 配置文件不存在：%1\n已检测到服务安装记录，但配置尚未生成或已被移走。"
                                   "请检查 sshd 服务状态与安装；不要用默认文件覆盖已有配置。（Windows %2）")
                        .arg(displayPath)
                        .arg(code);
        else if (code == ERROR_ACCESS_DENIED)
            error = QStringLiteral("没有权限读取 SSH 配置：%1\n请用同一账户提升权限后重试；"
                                   "若仍失败，请检查该文件及父目录 ACL。（Windows %2）")
                        .arg(displayPath)
                        .arg(code);
        else if (code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION)
            error = QStringLiteral("SSH 配置被其他程序独占：%1\n请关闭占用程序后重新检测。（Windows %2）")
                        .arg(displayPath)
                        .arg(code);
        else
            error = QStringLiteral("无法打开 SSH 配置：%1（Windows %2）。未修改配置或授权文件。")
                        .arg(displayPath)
                        .arg(code);
        return false;
    }
    const std::unique_ptr<void, decltype(&CloseHandle)> handle(file, &CloseHandle);
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart > 1024 * 1024 || size.QuadPart < 0)
    {
        error = QStringLiteral("无法取得 SSH 配置大小或文件超过 1 MiB：%1").arg(displayPath);
        return false;
    }
    QByteArray contents(static_cast<int>(size.QuadPart), '\0');
    DWORD bytesRead = 0;
    const BOOL success =
        ReadFile(file, contents.data(), static_cast<DWORD>(contents.size()), &bytesRead, nullptr);
    const DWORD code = success ? ERROR_SUCCESS : GetLastError();
    if (!success || bytesRead != static_cast<DWORD>(contents.size()))
    {
        error = QStringLiteral("读取 SSH 配置失败或读取期间文件发生变化：%1（Windows %2）。请重新检测。")
                    .arg(displayPath)
                    .arg(code);
        return false;
    }
    if (contents.startsWith(QByteArray::fromHex("efbbbf")))
        contents.remove(0, 3);
    if (contents.contains('\0'))
    {
        error = QStringLiteral("SSH 配置包含不支持的编码或空字符：%1。请核对文件编码，未自动改写。")
                    .arg(displayPath);
        return false;
    }
    configuration = QString::fromUtf8(contents);
    return true;
}

QString sshListenConfigurationWarning(const QString &configuration)
{
    bool hasAddress = false;
    bool allLoopback = true;
    bool uncertain = false;
    bool inMatch = false;
    for (QString line : configuration.split(QLatin1Char('\n')))
    {
        line = line.section(QLatin1Char('#'), 0, 0).trimmed();
        const QStringList words = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (words.isEmpty())
            continue;
        const QString directive = words.first().toLower();
        if (directive == QStringLiteral("include"))
            uncertain = true;
        if (directive == QStringLiteral("match"))
            inMatch = true;
        if (directive != QStringLiteral("listenaddress"))
            continue;
        if (words.size() != 2 || inMatch)
        {
            uncertain = true;
            continue;
        }
        hasAddress = true;
        const QString address = words[1];
        bool loopback =
            address == QStringLiteral("::1") ||
            QRegularExpression(QStringLiteral("^\\[::1\\](?::[0-9]+)?$")).match(address).hasMatch();
        const auto ipv4 =
            QRegularExpression(
                QStringLiteral("^127\\.([0-9]{1,3})\\.([0-9]{1,3})\\.([0-9]{1,3})(?::[0-9]+)?$"))
                .match(address);
        if (ipv4.hasMatch())
            loopback = ipv4.captured(1).toUInt() <= 255 && ipv4.captured(2).toUInt() <= 255 &&
                       ipv4.captured(3).toUInt() <= 255;
        allLoopback = allLoopback && loopback;
    }
    if (uncertain)
        return QStringLiteral("配置含扩展或无法识别的监听规则，请人工核对；配置不代表服务当前实际监听。");
    if (hasAddress && allLoopback)
        return QStringLiteral("sshd_config 仅配置了回环监听，其他电脑无法使用这些地址连接。"
                              "请人工核对 Tailscale 监听配置与服务实际监听；此工具不会改配置或重启服务。");
    return {};
}

QString homeEnvironmentReport(const HomeEnvironment &environment)
{
    QString service = QStringLiteral("未检测到安装");
    if (environment.serviceInstalled)
        service = environment.sshAvailable                               ? QStringLiteral("运行中")
                  : environment.serviceState == QStringLiteral("未运行") ? QStringLiteral("未运行")
                                                                         : QStringLiteral("状态未确认");
    return QStringLiteral("OrbiDev 家庭端诊断（已脱敏）\nOpenSSH 服务：%1\n账户类型：%2\n"
                          "配置监听提示：%3\n环境检查：%4\n"
                          "此报告不确认实际端口可达、密钥认证或远程 Claude 状态；请从公司端测试连接。")
        .arg(service, environment.administrator ? QStringLiteral("管理员组账户") : QStringLiteral("普通账户"),
             environment.listenWarning.isEmpty() ? QStringLiteral("无专项警告／尚未检查")
                                                 : QStringLiteral("需人工核对配置与实际监听"),
             environment.error.isEmpty() && environment.sshAvailable ? QStringLiteral("通过")
                                                                     : QStringLiteral("未通过／未完成"));
}

HomeEnvironment inspectHomeEnvironment()
{
    HomeEnvironment result;
    result.hostName = QSysInfo::machineHostName();
    wchar_t userName[257]{};
    DWORD userNameLength = 257;
    if (!GetUserNameW(userName, &userNameLength))
    {
        result.error = QStringLiteral("无法读取当前 Windows 账户名称。");
        return result;
    }
    result.userName = QString::fromWCharArray(userName);
    QString sid;
    if (!currentIdentity(sid, result.administrator, result.error))
        return result;
    QSettings service(QStringLiteral("HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Services\\sshd"),
                      QSettings::NativeFormat);
    const QString imagePath = service.value(QStringLiteral("ImagePath")).toString().trimmed();
    if (imagePath.isEmpty())
    {
        result.error = QStringLiteral(
            "未安装 OpenSSH Server。请在 Windows 可选功能安装 OpenSSH 服务器，再配置仅允许 "
            "Tailscale／回环访问的防火墙规则并启动 sshd。此工具不会更改现有服务或默认 Shell。");
        return result;
    }
    result.serviceInstalled = true;
    const auto command = QRegularExpression(QStringLiteral("^(?:\"([^\"]+sshd\\.exe)\"|([^\"]+sshd\\.exe))$"),
                                            QRegularExpression::CaseInsensitiveOption)
                             .match(imagePath);
    if (!command.hasMatch())
    {
        result.error =
            QStringLiteral("sshd 服务使用了自定义启动参数，无法确定实际配置文件，已停止自动授权。");
        return result;
    }
    QString executable = command.captured(1).isEmpty() ? command.captured(2) : command.captured(1);
    executable.replace(QStringLiteral("%SystemRoot%"), qEnvironmentVariable("SystemRoot"),
                       Qt::CaseInsensitive);
    result.serviceExecutable = executable;
    if (!QFileInfo(executable).isFile())
    {
        result.error = QStringLiteral("sshd 服务程序不存在，请修复 OpenSSH Server 安装。");
        return result;
    }
    const QString programData = qEnvironmentVariable("ProgramData");
    if (programData.isEmpty() || !QDir::isAbsolutePath(programData))
    {
        result.error =
            QStringLiteral("Windows ProgramData 路径无效，无法确定 SSH 配置位置。未使用相对路径查找。");
        return result;
    }
    const QString configPath = QDir(programData).filePath(QStringLiteral("ssh/sshd_config"));
    result.configurationPath = configPath;
    // 仅追加授权，不改变服务配置；普通账户不能读取主机私钥以运行 sshd -t。
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    SC_HANDLE sshService = manager ? OpenServiceW(manager, L"sshd", SERVICE_QUERY_STATUS) : nullptr;
    SERVICE_STATUS status{};
    const bool serviceStatusRead = sshService && QueryServiceStatus(sshService, &status);
    result.sshAvailable = serviceStatusRead && status.dwCurrentState == SERVICE_RUNNING;
    if (sshService)
        CloseServiceHandle(sshService);
    if (manager)
        CloseServiceHandle(manager);
    result.serviceState = result.sshAvailable ? QStringLiteral("运行中")
                                              : (serviceStatusRead ? QStringLiteral("未运行")
                                                                   : QStringLiteral("无法读取服务状态"));
    QString configuration;
    if (!readSshConfiguration(configPath, configuration, result.error))
    {
        result.error += QStringLiteral("\nsshd 服务状态：%1").arg(result.serviceState);
        return result;
    }
    result.listenWarning = sshListenConfigurationWarning(configuration);
    if (!resolveAuthorizedKeys(configuration, result.administrator, QDir::homePath(), programData,
                               result.authorizedKeysPath, result.port, result.error))
        return result;
    if (!result.sshAvailable)
    {
        result.error = serviceStatusRead
                           ? QStringLiteral("OpenSSH 已安装但服务未运行。可在确认后启动 sshd，再重新检测；"
                                            "启动失败请保留系统错误并核对配置，此工具不自动安装或重启服务。")
                           : QStringLiteral("OpenSSH 已安装，但无法读取服务状态；请检查权限后重新检测，"
                                            "不能据此判断服务未运行。");
        return result;
    }
    QString tailscale = QStandardPaths::findExecutable(QStringLiteral("tailscale.exe"));
    if (tailscale.isEmpty())
        tailscale =
            QDir(qEnvironmentVariable("ProgramFiles")).filePath(QStringLiteral("Tailscale/tailscale.exe"));
    result.host = run(tailscale, {QStringLiteral("ip"), QStringLiteral("-4")})
                      .section(QLatin1Char('\n'), 0, 0)
                      .trimmed();
    const QString keygen =
        QDir(QFileInfo(executable).absolutePath()).filePath(QStringLiteral("ssh-keygen.exe"));
    const QString hostKey = QDir(programData).filePath(QStringLiteral("ssh/ssh_host_ed25519_key.pub"));
    result.hostFingerprint = run(keygen, {QStringLiteral("-l"), QStringLiteral("-E"),
                                          QStringLiteral("sha256"), QStringLiteral("-f"), hostKey});
    result.hostFingerprint = QRegularExpression(QStringLiteral("(?:^|\\s)(SHA256:[A-Za-z0-9+/]+)(?:\\s|$)"))
                                 .match(result.hostFingerprint)
                                 .captured(1);
    if (result.host.isEmpty())
        result.error = QStringLiteral("未取得 Tailscale IPv4 地址，请检查登录和网络连接。");
    else if (result.hostFingerprint.isEmpty())
        result.error = QStringLiteral("无法读取 SSH 主机公钥指纹，暂不能导出可信连接信息。");
    return result;
}

bool matchesAuthorizationPreview(const HomeEnvironment &expected, const HomeEnvironment &actual,
                                 QString &error)
{
    if (!actual.error.isEmpty())
    {
        error = actual.error;
        return false;
    }
    if (!expected.error.isEmpty() || !expected.sshAvailable || !actual.sshAvailable ||
        expected.hostName != actual.hostName || expected.serviceExecutable != actual.serviceExecutable ||
        expected.listenWarning != actual.listenWarning || expected.userName != actual.userName ||
        expected.authorizedKeysPath != actual.authorizedKeysPath ||
        expected.configurationPath != actual.configurationPath ||
        expected.administrator != actual.administrator || expected.port != actual.port ||
        expected.host != actual.host || expected.hostFingerprint != actual.hostFingerprint ||
        expected.authorizedKeysPath.isEmpty() || expected.hostFingerprint.isEmpty())
    {
        error = QStringLiteral("家庭环境或授权位置在预览后发生变化，请重新检测并核对后再应用。");
        return false;
    }
    error.clear();
    return true;
}

bool authorizeCurrentUser(const QString &publicKey, const HomeEnvironment &expected, QString &error)
{
    const HomeEnvironment environment = inspectHomeEnvironment();
    if (!matchesAuthorizationPreview(expected, environment, error))
        return false;
    QString sid;
    bool administrator = false;
    if (!currentIdentity(sid, administrator, error))
        return false;
    const bool sharedAdministratorFile = QFileInfo(environment.authorizedKeysPath).fileName() ==
                                         QStringLiteral("administrators_authorized_keys");
    return appendAuthorizedKey(
        environment.authorizedKeysPath, publicKey,
        [&](QString &permissionError)
        {
            return secureAuthorizationFile(environment.authorizedKeysPath, sid, sharedAdministratorFile,
                                           permissionError);
        },
        error);
}
}
