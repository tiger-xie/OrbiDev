#include "EnvironmentService.h"
#include "AuthorizationService.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QTemporaryDir>

namespace sshsetup
{
namespace
{
QString openSshTool(const QString &name)
{
    const QString path = qEnvironmentVariable("SystemRoot") + QStringLiteral("/System32/OpenSSH/") + name;
    return QFileInfo::exists(path) ? path : QString();
}
bool run(const QString &program, const QStringList &arguments, QByteArray &output, int timeout = 12000,
         bool *timedOut = nullptr)
{
    if (timedOut)
        *timedOut = false;
    if (program.isEmpty())
        return false;
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(program, arguments);
    if (!process.waitForStarted(3000))
        return false;
    process.closeWriteChannel();
    if (!process.waitForFinished(timeout))
    {
        if (timedOut)
            *timedOut = true;
        process.kill();
        process.waitForFinished(3000);
        output = process.readAll().left(65536);
        return false;
    }
    output = process.readAll().left(65536);
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}
bool readObject(const QString &path, QJsonObject &object, QString &error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024)
    {
        error = QStringLiteral("无法读取连接配置，或文件超过 1 MB。");
        return false;
    }
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject())
    {
        error = QStringLiteral("连接信息不是有效 JSON 对象。");
        return false;
    }
    object = document.object();
    return true;
}
bool decode(const QJsonObject &object, ConnectionProfile &profile, QString &error)
{
    for (const auto &field :
         {QStringLiteral("host"), QStringLiteral("user"), QStringLiteral("hostFingerprint")})
    {
        if (object.contains(field) && !object.value(field).isString())
        {
            error = QStringLiteral("连接字段 %1 必须是文本。");
            return false;
        }
    }
    ConnectionProfile candidate = profile;
    candidate.host = object.value(QStringLiteral("host")).toString();
    candidate.user = object.value(QStringLiteral("user")).toString();
    candidate.port = object.value(QStringLiteral("port")).toInt(22);
    candidate.hostFingerprint = object.value(QStringLiteral("hostFingerprint")).toString();
    if (object.contains(QStringLiteral("port")) &&
        (!object.value(QStringLiteral("port")).isDouble() ||
         object.value(QStringLiteral("port")).toDouble() != candidate.port))
    {
        error = QStringLiteral("端口必须为整数。");
        return false;
    }
    if (!validateProfile(candidate, error))
        return false;
    profile = candidate;
    return true;
}
}

bool validateProfile(const ConnectionProfile &profile, QString &error)
{
    error.clear();
    static const QRegularExpression host(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,252}\\z"));
    static const QRegularExpression user(QStringLiteral("^[A-Za-z0-9_][A-Za-z0-9_.-]{0,63}\\z"));
    static const QRegularExpression fingerprint(QStringLiteral("^SHA256:[A-Za-z0-9+/]{43}\\z"));
    if (!host.match(profile.host).hasMatch())
        error = QStringLiteral("请输入 IPv4、主机名或 SSH 别名，不能包含命令参数。");
    else if (!profile.user.isEmpty() && !user.match(profile.user).hasMatch())
        error = QStringLiteral("登录用户名格式不正确。");
    else if (profile.port < 1 || profile.port > 65535)
        error = QStringLiteral("端口范围为 1 至 65535。");
    else if (!profile.hostFingerprint.isEmpty() && !fingerprint.match(profile.hostFingerprint).hasMatch())
        error = QStringLiteral("主机指纹应是完整的 SHA256 指纹。");
    else if (profile.identityFile.contains(QChar('\n')) || profile.identityFile.contains(QChar('\0')))
        error = QStringLiteral("密钥路径包含无效字符。");
    return error.isEmpty();
}

EnvironmentStatus inspectClientEnvironment()
{
    EnvironmentStatus result;
    result.sshAvailable = !openSshTool(QStringLiteral("ssh.exe")).isEmpty() &&
                          !openSshTool(QStringLiteral("ssh-keygen.exe")).isEmpty();
    QString tailscale = qEnvironmentVariable("ProgramFiles") + QStringLiteral("/Tailscale/tailscale.exe");
    if (!QFileInfo::exists(tailscale))
        tailscale = QStandardPaths::findExecutable(QStringLiteral("tailscale.exe"));
    QByteArray output;
    if (run(tailscale, {QStringLiteral("status"), QStringLiteral("--json")}, output, 5000))
    {
        const auto status = QJsonDocument::fromJson(output).object();
        result.tailscaleOnline =
            status.value(QStringLiteral("BackendState")).toString() == QStringLiteral("Running") &&
            status.value(QStringLiteral("Self")).toObject().value(QStringLiteral("Online")).toBool();
    }
    result.details =
        QStringLiteral("OpenSSH 客户端：%1\nTailscale：%2\n网络在线不代表 SSH 已授权。")
            .arg(result.sshAvailable ? QStringLiteral("已安装") : QStringLiteral("未找到系统组件"),
                 result.tailscaleOnline ? QStringLiteral("在线") : QStringLiteral("未连接或无法检测"));
    return result;
}

bool prepareSshArguments(const ConnectionProfile &profile, const QString &trustedDirectoryPath,
                         QStringList &preparedArguments, QString &error, bool interactive)
{
    if (!validateProfile(profile, error))
        return false;
    QStringList arguments{QStringLiteral("-F"), QStringLiteral("NUL"),
                          QStringLiteral("-o"), QStringLiteral("IdentityAgent=none"),
                          QStringLiteral("-o"), QStringLiteral("BatchMode=yes"),
                          QStringLiteral("-o"), QStringLiteral("StrictHostKeyChecking=yes"),
                          QStringLiteral("-o"), QStringLiteral("UpdateHostKeys=no"),
                          QStringLiteral("-o"), QStringLiteral("ConnectTimeout=8"),
                          QStringLiteral("-o"), QStringLiteral("ConnectionAttempts=1"),
                          QStringLiteral("-o"), QStringLiteral("ClearAllForwardings=yes"),
                          QStringLiteral("-p"), QString::number(profile.port)};
    if (!profile.identityFile.isEmpty())
    {
        if (!QFileInfo(profile.identityFile).isFile())
        {
            error = QStringLiteral("所选本机密钥文件不存在。");
            return false;
        }
        arguments << QStringLiteral("-o") << QStringLiteral("IdentitiesOnly=yes") << QStringLiteral("-i")
                  << profile.identityFile;
    }
    // SSH 别名先由 OpenSSH 展开，仅用于诊断目标；不执行用户提供的远端命令。
    QByteArray effective;
    if (!run(openSshTool(QStringLiteral("ssh.exe")),
             {QStringLiteral("-G"), QStringLiteral("-p"), QString::number(profile.port), profile.host},
             effective))
    {
        error = QStringLiteral("无法解析 SSH 配置，请检查 OpenSSH 客户端。");
        return false;
    }
    QString resolvedHost = profile.host;
    QString resolvedUser = profile.user;
    for (const auto &line : effective.split('\n'))
    {
        if (line.startsWith("hostname "))
            resolvedHost = QString::fromUtf8(line.mid(9)).trimmed();
        else if (profile.user.isEmpty() && line.startsWith("user "))
            resolvedUser = QString::fromUtf8(line.mid(5)).trimmed();
    }
    if (!profile.hostFingerprint.isEmpty())
    {
        QByteArray scanned;
        if (trustedDirectoryPath.isEmpty() || !QDir::isAbsolutePath(trustedDirectoryPath) ||
            !QDir(trustedDirectoryPath).exists() ||
            !run(openSshTool(QStringLiteral("ssh-keyscan.exe")),
                 {QStringLiteral("-T"), QStringLiteral("5"), QStringLiteral("-t"), QStringLiteral("ed25519"),
                  QStringLiteral("-p"), QString::number(profile.port), resolvedHost},
                 scanned))
        {
            error = QStringLiteral("无法读取家庭主机公钥；未建立信任。");
            return false;
        }
        QString trustedKey;
        for (const auto &line : scanned.split('\n'))
        {
            if (line.startsWith('#'))
                continue;
            const int offset = line.indexOf("ssh-ed25519 ");
            QString normalized, fingerprint, keyError;
            if (offset >= 0 &&
                validatePublicKey(QString::fromUtf8(line.mid(offset)).trimmed(), normalized, fingerprint,
                                  keyError) &&
                fingerprint == profile.hostFingerprint)
                trustedKey = normalized;
        }
        if (trustedKey.isEmpty())
        {
            error = QStringLiteral("家庭主机指纹与已核对的指纹不一致。已停止连接，请在家庭电脑重新核对。");
            return false;
        }
        QFile knownHosts(QDir(trustedDirectoryPath).filePath(QStringLiteral("known_hosts")));
        const QString record = QStringLiteral("orbidev-verified-host ") + trustedKey + QLatin1Char('\n');
        if (!knownHosts.open(QIODevice::WriteOnly) ||
            knownHosts.write(record.toUtf8()) != record.toUtf8().size())
        {
            error = QStringLiteral("无法建立本次连接的主机信任文件。");
            return false;
        }
        knownHosts.close();
        arguments << QStringLiteral("-o") << QStringLiteral("HostKeyAlias=orbidev-verified-host")
                  << QStringLiteral("-o")
                  << QStringLiteral("UserKnownHostsFile=\"") + knownHosts.fileName() + QLatin1Char('"')
                  << QStringLiteral("-o") << QStringLiteral("GlobalKnownHostsFile=NUL")
                  << QStringLiteral("-o") << QStringLiteral("HostKeyAlgorithms=ssh-ed25519");
    }
    arguments << QStringLiteral("--")
              << (resolvedUser.isEmpty() ? resolvedHost : resolvedUser + QLatin1Char('@') + resolvedHost);
    if (interactive)
    {
        arguments.replace(arguments.indexOf(QStringLiteral("BatchMode=yes")), QStringLiteral("BatchMode=no"));
        const int position = arguments.indexOf(QStringLiteral("--"));
        arguments.insert(position, QStringLiteral("-tt"));
    }
    preparedArguments = arguments;
    return true;
}

QString formatConnectionReport(const ConnectionResult &result)
{
    const auto status = [](CheckStatus value)
    {
        switch (value)
        {
        case CheckStatus::Passed:
            return QStringLiteral("通过");
        case CheckStatus::Failed:
            return QStringLiteral("失败");
        case CheckStatus::Inconclusive:
            return QStringLiteral("未能确认");
        default:
            return QStringLiteral("未执行");
        }
    };
    QString reason;
    switch (result.issue)
    {
    case ConnectionIssue::None:
        reason = QStringLiteral("检测完成；未启动 Claude，也未确认工作区信任。");
        break;
    case ConnectionIssue::InvalidProfile:
        reason = QStringLiteral("连接字段无效，请检查地址、用户名、端口及指纹格式。");
        break;
    case ConnectionIssue::ClientUnavailable:
        reason = QStringLiteral("本机 OpenSSH 不可用或无法解析连接配置，请安装或检查客户端。");
        break;
    case ConnectionIssue::NetworkUnavailable:
        reason = QStringLiteral("网络或 SSH 端口不可达，请检查 Tailscale、家庭电脑及监听地址。");
        break;
    case ConnectionIssue::BannerTimeout:
        reason = QStringLiteral("端口可达，但等待 SSH 服务标识超时；请检查该端口是否由 sshd 监听。");
        break;
    case ConnectionIssue::BannerClosed:
        reason = QStringLiteral("端口可达，但对端在发送 SSH 服务标识前关闭连接。");
        break;
    case ConnectionIssue::UnknownHost:
        reason = QStringLiteral("主机身份尚未信任，请在家庭端核对完整指纹后配置；未自动接受主机密钥。");
        break;
    case ConnectionIssue::HostKeyChanged:
        reason =
            QStringLiteral("主机指纹不匹配或已变化，已停止连接；请在家庭端重新核对，不要直接删除信任记录。");
        break;
    case ConnectionIssue::TrustPreparationFailed:
        reason = QStringLiteral("未能准备严格主机校验，请检查本机身份文件与家庭主机公钥；未建立新信任。");
        break;
    case ConnectionIssue::AuthenticationFailed:
        reason = QStringLiteral(
            "SSH 认证失败，请核对登录账户、公钥授权及本机身份；加密私钥可在交互终端输入口令。");
        break;
    case ConnectionIssue::ProbeTimeout:
        reason = QStringLiteral("SSH 检测超时；未完成的阶段不能视为通过，请检查网络及家庭端服务。");
        break;
    case ConnectionIssue::PowerShellUnavailable:
        reason = QStringLiteral(
            "SSH 已认证，但远端 PowerShell 检查未完成，请检查 PowerShell 可执行文件和远端 Shell 配置。");
        break;
    case ConnectionIssue::ClaudeUnavailable:
        reason = QStringLiteral("远端 PowerShell 可用，但找不到 Claude，请检查家庭端安装及该账户的 PATH。");
        break;
    case ConnectionIssue::ProbeFailed:
        reason = QStringLiteral("SSH 检测未完成且无法确定原因，请检查家庭端服务日志；未放宽主机校验。");
        break;
    }
    return QStringLiteral("SSH 连接诊断（脱敏）\n网络端口：%1\nSSH 服务：%2\n主机信任：%3\n认证：%4\n远端 "
                          "PowerShell：%5\nClaude 命令：%6\n%7")
        .arg(status(result.networkStatus), status(result.sshStatus), status(result.trustStatus),
             status(result.authenticationStatus), status(result.powershellStatus),
             status(result.claudeStatus), reason);
}

ConnectionResult classifySshProbeResult(const QByteArray &output, bool success, bool timedOut)
{
    ConnectionResult result;
    result.networkReachable = result.sshReachable = true;
    result.networkStatus = result.sshStatus = CheckStatus::Passed;
    const QByteArray lower = output.toLower();
    // SSH 的详细输出只在内存中用于阶段判断，绝不拼入用户报告。
    const bool authenticated = output.contains("ORBIDEV_AUTH_OK") || lower.contains("authenticated to ") ||
                               lower.contains("authentication succeeded (");
    if (lower.contains("remote host identification has changed") ||
        (lower.contains("host key verification failed") && lower.contains("offending ")))
    {
        result.trustStatus = CheckStatus::Failed;
        result.issue = ConnectionIssue::HostKeyChanged;
    }
    else if (lower.contains("host key verification failed") ||
             lower.contains("no ed25519 host key is known") || lower.contains("no rsa host key is known") ||
             lower.contains("no ecdsa host key is known"))
    {
        result.trustStatus = CheckStatus::Failed;
        result.issue = ConnectionIssue::UnknownHost;
    }
    else if (authenticated)
    {
        result.authenticated = true;
        result.trustStatus = result.authenticationStatus = CheckStatus::Passed;
        if (success && output.contains("ORBIDEV_AUTH_OK"))
        {
            result.powershellStatus = CheckStatus::Passed;
            result.claudeAvailable = output.contains("ORBIDEV_CLAUDE_OK");
            result.claudeStatus = result.claudeAvailable ? CheckStatus::Passed : CheckStatus::Failed;
            result.issue =
                result.claudeAvailable ? ConnectionIssue::None : ConnectionIssue::ClaudeUnavailable;
        }
        else
        {
            result.powershellStatus = timedOut ? CheckStatus::Inconclusive : CheckStatus::Failed;
            result.issue = timedOut ? ConnectionIssue::ProbeTimeout : ConnectionIssue::PowerShellUnavailable;
        }
    }
    else if (lower.contains("permission denied (") || lower.contains("no more authentication methods to try"))
    {
        result.trustStatus = CheckStatus::Passed;
        result.authenticationStatus = CheckStatus::Failed;
        result.issue = ConnectionIssue::AuthenticationFailed;
    }
    else
    {
        result.trustStatus = CheckStatus::Inconclusive;
        result.issue = timedOut ? ConnectionIssue::ProbeTimeout : ConnectionIssue::ProbeFailed;
    }
    result.details = formatConnectionReport(result);
    return result;
}

ConnectionResult testConnection(const ConnectionProfile &profile)
{
    ConnectionResult result;
    const auto finish = [&result](ConnectionIssue issue)
    {
        result.issue = issue;
        result.details = formatConnectionReport(result);
        return result;
    };
    QString error;
    if (!validateProfile(profile, error))
        return finish(ConnectionIssue::InvalidProfile);
    QByteArray effective;
    if (!run(openSshTool(QStringLiteral("ssh.exe")),
             {QStringLiteral("-G"), QStringLiteral("-p"), QString::number(profile.port), profile.host},
             effective))
        return finish(ConnectionIssue::ClientUnavailable);
    QString resolvedHost = profile.host;
    for (const auto &line : effective.split('\n'))
        if (line.startsWith("hostname "))
            resolvedHost = QString::fromUtf8(line.mid(9)).trimmed();
    QTcpSocket socket;
    socket.connectToHost(resolvedHost, quint16(profile.port));
    if (!socket.waitForConnected(5000))
    {
        result.networkStatus = CheckStatus::Failed;
        return finish(ConnectionIssue::NetworkUnavailable);
    }
    result.networkReachable = true;
    result.networkStatus = CheckStatus::Passed;
    QElapsedTimer bannerTimer;
    bannerTimer.start();
    QByteArray banner;
    while (bannerTimer.elapsed() < 3000 && banner.size() < 8192 && !result.sshReachable)
    {
        if (socket.bytesAvailable() == 0 && !socket.waitForReadyRead(300))
        {
            if (socket.state() == QAbstractSocket::UnconnectedState)
                break;
            continue;
        }
        banner += socket.read(8192 - banner.size());
        for (const auto &line : banner.split('\n'))
            if (line.startsWith("SSH-2.0-") || line.startsWith("SSH-1.99-"))
                result.sshReachable = true;
    }
    const bool closed = socket.state() == QAbstractSocket::UnconnectedState;
    socket.abort();
    if (!result.sshReachable)
    {
        result.sshStatus = CheckStatus::Failed;
        return finish(closed ? ConnectionIssue::BannerClosed : ConnectionIssue::BannerTimeout);
    }
    result.sshStatus = CheckStatus::Passed;
    QTemporaryDir trustedDirectory;
    QStringList arguments;
    if (!prepareSshArguments(profile, trustedDirectory.path(), arguments, error))
    {
        result.trustStatus = CheckStatus::Inconclusive;
        if (error.contains(QStringLiteral("指纹不一致")))
        {
            result.trustStatus = CheckStatus::Failed;
            return finish(ConnectionIssue::HostKeyChanged);
        }
        return finish(ConnectionIssue::TrustPreparationFailed);
    }
    // 固定脚本仅检查命令位置，不启动 Claude，也不会确认工作区信任提示。
    const QString script = QStringLiteral("[Console]::WriteLine('ORBIDEV_AUTH_OK'); "
                                          "if (Get-Command claude -ErrorAction SilentlyContinue) { "
                                          "[Console]::WriteLine('ORBIDEV_CLAUDE_OK') }");
    QByteArray encoded;
    for (const QChar character : script)
    {
        encoded.append(char(character.unicode() & 0xff));
        encoded.append(char(character.unicode() >> 8));
    }
    arguments.prepend(QStringLiteral("-v"));
    arguments << QStringLiteral("powershell.exe") << QStringLiteral("-NoLogo") << QStringLiteral("-NoProfile")
              << QStringLiteral("-NonInteractive") << QStringLiteral("-EncodedCommand")
              << QString::fromLatin1(encoded.toBase64());
    QByteArray output;
    bool timedOut = false;
    const bool success = run(openSshTool(QStringLiteral("ssh.exe")), arguments, output, 15000, &timedOut);
    return classifySshProbeResult(output, success, timedOut);
}

bool saveProfile(const QString &path, const ConnectionProfile &profile, QString &error)
{
    if (!validateProfile(profile, error))
        return false;
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
    {
        error = QStringLiteral("无法创建配置目录。");
        return false;
    }
    const QJsonObject object{{QStringLiteral("version"), 1},
                             {QStringLiteral("host"), profile.host},
                             {QStringLiteral("user"), profile.user},
                             {QStringLiteral("port"), profile.port},
                             {QStringLiteral("identityFile"), profile.identityFile},
                             {QStringLiteral("hostFingerprint"), profile.hostFingerprint}};
    QSaveFile file(path);
    const QByteArray bytes = QJsonDocument(object).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
    {
        error = QStringLiteral("连接配置保存失败，旧配置已保留。");
        return false;
    }
    return true;
}
bool loadProfile(const QString &path, ConnectionProfile &profile, QString &error)
{
    QJsonObject object;
    if (!readObject(path, object, error))
        return false;
    if (object.value(QStringLiteral("version")).toInt() != 1)
    {
        error = QStringLiteral("连接配置版本不支持。");
        return false;
    }
    ConnectionProfile candidate = profile;
    if (object.contains(QStringLiteral("identityFile")) &&
        !object.value(QStringLiteral("identityFile")).isString())
    {
        error = QStringLiteral("本机身份路径必须是文本。");
        return false;
    }
    candidate.identityFile = object.value(QStringLiteral("identityFile")).toString();
    if (!decode(object, candidate, error))
        return false;
    profile = candidate;
    return true;
}
bool importConnectionInfo(const QString &path, ConnectionProfile &profile, QString &error)
{
    QJsonObject object;
    if (!readObject(path, object, error))
        return false;
    if (object.contains(QStringLiteral("ssh_host")))
    {
        object.insert(QStringLiteral("host"), object.value(QStringLiteral("ssh_host")));
        object.insert(QStringLiteral("user"), object.value(QStringLiteral("ssh_user")));
    }
    return decode(object, profile, error);
}
}
