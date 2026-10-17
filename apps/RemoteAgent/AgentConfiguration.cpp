#include "AgentConfiguration.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

namespace rad
{
namespace
{
bool keysAllowed(const QJsonObject &object, const QStringList &allowed)
{
    for (auto it = object.begin(); it != object.end(); ++it)
        if (!allowed.contains(it.key()))
            return false;
    return true;
}
bool validId(const QString &id)
{
    static const QRegularExpression pattern(QStringLiteral("\\A[A-Za-z0-9][A-Za-z0-9_.-]{0,63}\\z"));
    return pattern.match(id).hasMatch();
}
// QJsonDocument 接受重复键，配置边界额外拒绝，避免被覆盖的授权项产生歧义。
bool uniqueJsonKeys(const QByteArray &bytes)
{
    QVector<QSet<QString>> objects;
    for (int i = 0; i < bytes.size(); ++i)
    {
        if (bytes[i] == '{')
            objects.append(QSet<QString>{});
        else if (bytes[i] == '}')
            objects.removeLast();
        else if (bytes[i] == '"')
        {
            const int begin = i++;
            while (i < bytes.size() && bytes[i] != '"')
            {
                if (bytes[i] == '\\')
                    ++i;
                ++i;
            }
            int next = i + 1;
            while (next < bytes.size() && QByteArray(" \r\n\t").contains(bytes[next]))
                ++next;
            if (next < bytes.size() && bytes[next] == ':')
            {
                const QString key = QJsonDocument::fromJson("[" + bytes.mid(begin, i - begin + 1) + "]")
                                        .array()[0]
                                        .toString();
                if (objects.isEmpty() || objects.last().contains(key))
                    return false;
                objects.last().insert(key);
            }
        }
    }
    return true;
}
}
Result<AgentConfiguration> loadAgentConfiguration(const QString &path)
{
    const auto reject = [](const char *message)
    { return Result<AgentConfiguration>::failure({ErrorCode::InvalidArgument, QString::fromUtf8(message)}); };
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 65536)
        return reject("无法读取配置或配置超过 64 KiB");
    const QByteArray bytes = file.read(65537);
    if (file.error() != QFileDevice::NoError || bytes.size() > 65536)
        return reject("配置读取失败");
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject() || !uniqueJsonKeys(bytes))
        return reject("配置 JSON 无效或包含重复键");
    const auto root = document.object();
    if (!keysAllowed(root, {"bind", "port", "clients", "workspaces", "fake_cli_path"}))
        return reject("配置包含未知字段");
    AgentConfiguration configuration;
    // 列举当前使用单帧；限制保留数量，确保最长转义名称也不会超过协议的 64 KiB 上限。
    configuration.sessions.maxRetainedSessions = 32;
    configuration.server.bindAddress = QHostAddress::LocalHost;
    configuration.server.port = 17890;
    if (root.contains("bind") &&
        (!root["bind"].isString() || !configuration.server.bindAddress.setAddress(root["bind"].toString())))
        return reject("bind 必须是有效 IP 地址");
    if (root.contains("port"))
    {
        const int port = root["port"].toInt(-1);
        if (!root["port"].isDouble() || port < 1 || port > 65535 || root["port"].toDouble() != port)
            return reject("port 必须是 1 到 65535 的整数");
        configuration.server.port = static_cast<quint16>(port);
    }
    QSet<QString> clientIds;
    QSet<QString> tokenHashes;
    const auto clients = root["clients"].toArray();
    if (!root["clients"].isArray() || clients.isEmpty() || clients.size() > 32)
        return reject("clients 必须包含 1 到 32 个身份");
    static const QRegularExpression hashPattern(QStringLiteral("\\A[0-9a-f]{64}\\z"));
    for (const auto &value : clients)
    {
        const auto client = value.toObject();
        const QString id = client["id"].toString();
        const QString hash = client["token_sha256"].toString();
        if (!value.isObject() || !keysAllowed(client, {"id", "token_sha256"}) || !validId(id) ||
            clientIds.contains(id) || tokenHashes.contains(hash) || !hashPattern.match(hash).hasMatch())
            return reject("客户端身份或令牌摘要配置无效");
        clientIds.insert(id);
        tokenHashes.insert(hash);
        configuration.server.credentials.append({id, QByteArray::fromHex(hash.toLatin1())});
    }
    const auto workspaces = root["workspaces"].toArray();
    if (!root["workspaces"].isArray() || workspaces.isEmpty() || workspaces.size() > 64)
        return reject("workspaces 必须包含 1 到 64 个工作区");
    QSet<QString> workspaceIds;
    for (const auto &value : workspaces)
    {
        const auto object = value.toObject();
        const QString id = object["id"].toString();
        const QFileInfo directory(object["path"].toString());
        const auto acl = object["allowed_clients"].toArray();
        if (!value.isObject() || !keysAllowed(object, {"id", "path", "allowed_clients"}) || !validId(id) ||
            workspaceIds.contains(id) || !object["path"].isString() ||
            object["path"].toString().contains(QChar::Null) || !directory.isAbsolute() ||
            !directory.isDir() || directory.canonicalFilePath().isEmpty() ||
            !object["allowed_clients"].isArray() || acl.isEmpty() || acl.size() > 32)
            return reject("工作区或授权列表配置无效");
        SessionManager::Workspace workspace;
        workspace.context = {id, directory.canonicalFilePath()};
        for (const auto &allowed : acl)
        {
            const QString clientId = allowed.toString();
            if (!allowed.isString() || !clientIds.contains(clientId) ||
                workspace.allowedClientIds.contains(clientId))
                return reject("工作区引用未知或重复身份");
            workspace.allowedClientIds.append(clientId);
        }
        workspaceIds.insert(id);
        configuration.sessions.workspaces.append(workspace);
    }
    const QFileInfo executable(root["fake_cli_path"].toString());
    if (!root["fake_cli_path"].isString() || root["fake_cli_path"].toString().contains(QChar::Null) ||
        !executable.isAbsolute() || !executable.isFile() ||
        executable.suffix().compare(QStringLiteral("exe"), Qt::CaseInsensitive) != 0 ||
        executable.canonicalFilePath().isEmpty())
        return reject("fake_cli_path 必须是存在的绝对 EXE 路径");
    configuration.fakeCliPath = executable.canonicalFilePath();
    return Result<AgentConfiguration>::success(std::move(configuration));
}
}
