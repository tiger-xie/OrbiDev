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
bool run(const QString &program, const QStringList &arguments, QByteArray &output, int timeout = 12000)
{
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
        process.kill();
        process.waitForFinished(3000);
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

ConnectionResult testConnection(const ConnectionProfile &profile)
{
    ConnectionResult result;
    if (!validateProfile(profile, result.details))
        return result;
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
            result.details = QStringLiteral("所选本机密钥文件不存在。");
            return result;
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
        result.details = QStringLiteral("无法解析 SSH 配置，请检查 OpenSSH 客户端。");
        return result;
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
    QTcpSocket socket;
    socket.connectToHost(resolvedHost, quint16(profile.port));
    if (!socket.waitForConnected(5000))
    {
        result.details = QStringLiteral("网络或 SSH 端口不可达。请检查家庭电脑、Tailscale 与服务端口。");
        return result;
    }
    result.networkReachable = true;
    QElapsedTimer bannerTimer;
    bannerTimer.start();
    QByteArray banner;
    while (bannerTimer.elapsed() < 3000 && banner.size() < 8192 && !result.sshReachable)
    {
        if (socket.bytesAvailable() == 0 && !socket.waitForReadyRead(300))
            continue;
        banner += socket.read(8192 - banner.size());
        for (const auto &line : banner.split('\n'))
            if (line.startsWith("SSH-2.0-") || line.startsWith("SSH-1.99-"))
                result.sshReachable = true;
    }
    socket.abort();
    if (!result.sshReachable)
    {
        result.details = QStringLiteral("端口可达，但未收到 SSH 服务标识。");
        return result;
    }
    QTemporaryDir trustedDirectory;
    if (!profile.hostFingerprint.isEmpty())
    {
        QByteArray scanned;
        if (!trustedDirectory.isValid() ||
            !run(openSshTool(QStringLiteral("ssh-keyscan.exe")),
                 {QStringLiteral("-T"), QStringLiteral("5"), QStringLiteral("-t"), QStringLiteral("ed25519"),
                  QStringLiteral("-p"), QString::number(profile.port), resolvedHost},
                 scanned))
        {
            result.details = QStringLiteral("无法读取家庭主机公钥；未建立信任。");
            return result;
        }
        QString trustedKey;
        for (const auto &line : scanned.split('\n'))
        {
            if (line.startsWith('#'))
                continue;
            const int offset = line.indexOf("ssh-ed25519 ");
            QString normalized, fingerprint, error;
            if (offset >= 0 &&
                validatePublicKey(QString::fromUtf8(line.mid(offset)).trimmed(), normalized, fingerprint,
                                  error) &&
                fingerprint == profile.hostFingerprint)
                trustedKey = normalized;
        }
        if (trustedKey.isEmpty())
        {
            result.details =
                QStringLiteral("家庭主机指纹与已核对的指纹不一致。已停止连接，请在家庭电脑重新核对。");
            return result;
        }
        QFile knownHosts(trustedDirectory.filePath(QStringLiteral("known_hosts")));
        const QString record = QStringLiteral("orbidev-verified-host ") + trustedKey + QLatin1Char('\n');
        if (!knownHosts.open(QIODevice::WriteOnly) ||
            knownHosts.write(record.toUtf8()) != record.toUtf8().size())
        {
            result.details = QStringLiteral("无法建立本次连接的主机信任文件。");
            return result;
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
    arguments << QStringLiteral("powershell.exe") << QStringLiteral("-NoLogo") << QStringLiteral("-NoProfile")
              << QStringLiteral("-NonInteractive") << QStringLiteral("-EncodedCommand")
              << QString::fromLatin1(encoded.toBase64());
    QByteArray output;
    const bool success = run(openSshTool(QStringLiteral("ssh.exe")), arguments, output, 15000);
    result.authenticated = success && output.contains("ORBIDEV_AUTH_OK");
    result.claudeAvailable = result.authenticated && output.contains("ORBIDEV_CLAUDE_OK");
    if (result.claudeAvailable)
        result.details =
            QStringLiteral("网络、SSH 服务、密钥认证均通过；远端可找到 Claude。尚未启动 Claude 会话。");
    else if (result.authenticated)
        result.details =
            QStringLiteral("SSH 认证通过，但远端 PowerShell 找不到 Claude，请检查家庭电脑安装与 PATH。");
    else
        result.details =
            QStringLiteral("SSH 验证未通过：可能是密钥未授权、主机指纹未知或变化、远端 PowerShell 不可用。"
                           "请核对家庭端导出的指纹和授权账户；未自动接受主机密钥。");
    return result;
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
