#include "WorkbenchStore.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUuid>

namespace sshworkbench
{
namespace
{
bool keys(const QJsonObject &object, const QStringList &expected)
{
    if (object.size() != expected.size())
        return false;
    for (const auto &key : expected)
        if (!object.contains(key))
            return false;
    return true;
}

bool validId(const QString &id)
{
    const QUuid uuid(id);
    return !uuid.isNull() && uuid.toString(QUuid::WithoutBraces) == id;
}

bool validate(const WorkbenchState &state, QString &error)
{
    if (!launcher::validateConfiguration(state.projects, error))
        return false;
    if (state.hosts.size() > 32)
    {
        error = QStringLiteral("最多保存 32 台主机。");
        return false;
    }
    QSet<QString> hosts;
    for (const auto &host : state.hosts)
    {
        bool nameValid = !host.name.trimmed().isEmpty() && host.name.size() <= 80;
        for (const auto character : host.name)
            nameValid = nameValid && character.category() != QChar::Other_Control;
        if (!validId(host.id) || hosts.contains(host.id) || !nameValid)
        {
            error = QStringLiteral("主机名称或唯一 ID 无效。");
            return false;
        }
        if (!sshsetup::validateProfile(host.connection, error))
            return false;
        launcher::Configuration pathCheck;
        pathCheck.identityFile = host.connection.identityFile;
        if (!launcher::validateConfiguration(pathCheck, error))
            return false;
        hosts.insert(host.id);
    }
    if ((hosts.isEmpty() && !state.currentHostId.isEmpty()) ||
        (!hosts.isEmpty() && !hosts.contains(state.currentHostId)))
    {
        error = QStringLiteral("当前主机引用无效。");
        return false;
    }
    QSet<QString> projects;
    for (const auto &project : state.projects.projects)
    {
        if (!validId(project.id) || projects.contains(project.id) ||
            !hosts.contains(state.projectHosts.value(project.id)))
        {
            error = QStringLiteral("项目 ID 重复、无效或所属主机不存在。");
            return false;
        }
        projects.insert(project.id);
    }
    if (state.projectHosts.size() != projects.size())
    {
        error = QStringLiteral("项目主机引用包含不存在的项目。");
        return false;
    }
    return true;
}

bool readBytes(const QString &path, QByteArray &bytes, QString &error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024)
    {
        error = QStringLiteral("无法读取配置或超过 1 MiB 限制：%1").arg(path);
        return false;
    }
    bytes = file.readAll();
    if (file.error() != QFileDevice::NoError)
    {
        error = QStringLiteral("读取配置失败：%1").arg(path);
        return false;
    }
    return true;
}

bool backup(const QString &path, QString &error)
{
    QByteArray original;
    if (!readBytes(path, original, error))
        return false;
    const QString backupPath = path + QStringLiteral(".ssh02.bak");
    if (QFileInfo::exists(backupPath))
    {
        QByteArray saved;
        if (!readBytes(backupPath, saved, error) || saved != original)
        {
            error = QStringLiteral("已有 SSH-02 备份内容不同或无法读取：%1；停止迁移。").arg(backupPath);
            return false;
        }
        return true;
    }
    QFile file(backupPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) || file.write(original) != original.size() ||
        !file.flush())
    {
        error = QStringLiteral("无法备份原配置：%1；停止迁移。").arg(backupPath);
        return false;
    }
    return true;
}
}

const HostProfile *findHost(const WorkbenchState &state, const QString &id)
{
    for (const auto &host : state.hosts)
        if (host.id == id)
            return &host;
    return nullptr;
}

bool canRemoveHost(const WorkbenchState &state, const QString &id, const QSet<QString> &referencedHostIds,
                   QString &error)
{
    error.clear();
    if (!findHost(state, id))
        error = QStringLiteral("该主机不存在。");
    else if (state.projectHosts.values().contains(id))
        error = QStringLiteral("主机仍有关联项目，请先移动或移除项目。");
    else if (referencedHostIds.contains(id))
        error = QStringLiteral("主机仍有终端标签，请先关闭关联标签。");
    return error.isEmpty();
}

bool loadWorkbenchState(const QString &path, WorkbenchState &state, QString &error)
{
    error.clear();
    QByteArray bytes;
    if (!readBytes(path, bytes, error))
        return false;
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(bytes, &parseError);
    const auto root = document.object();
    const QStringList projectSettings{QStringLiteral("ssh_host"), QStringLiteral("ssh_user"),
                                      QStringLiteral("identity_file"), QStringLiteral("terminal_path")};
    const auto settings = root.value(QStringLiteral("legacy_settings")).toObject();
    bool schema =
        parseError.error == QJsonParseError::NoError && document.isObject() &&
        keys(root, {QStringLiteral("version"), QStringLiteral("hosts"), QStringLiteral("current_host_id"),
                    QStringLiteral("legacy_settings"), QStringLiteral("projects")}) &&
        root.value(QStringLiteral("version")).isDouble() &&
        root.value(QStringLiteral("version")).toDouble() == 1 &&
        root.value(QStringLiteral("hosts")).isArray() && root.value(QStringLiteral("projects")).isArray() &&
        root.value(QStringLiteral("current_host_id")).isString() && keys(settings, projectSettings);
    for (const auto &key : projectSettings)
        schema = schema && settings.value(key).isString();
    WorkbenchState candidate;
    candidate.currentHostId = root.value(QStringLiteral("current_host_id")).toString();
    candidate.projects.sshHost = settings.value(QStringLiteral("ssh_host")).toString();
    candidate.projects.sshUser = settings.value(QStringLiteral("ssh_user")).toString();
    candidate.projects.identityFile = settings.value(QStringLiteral("identity_file")).toString();
    candidate.projects.terminalPath = settings.value(QStringLiteral("terminal_path")).toString();
    for (const auto value : root.value(QStringLiteral("hosts")).toArray())
    {
        const auto entry = value.toObject();
        const QStringList strings{
            QStringLiteral("id"),   QStringLiteral("name"),          QStringLiteral("host"),
            QStringLiteral("user"), QStringLiteral("identity_file"), QStringLiteral("host_fingerprint")};
        auto expected = strings;
        expected.append(QStringLiteral("port"));
        schema =
            schema && value.isObject() && keys(entry, expected) &&
            entry.value(QStringLiteral("port")).isDouble() &&
            entry.value(QStringLiteral("port")).toDouble() == entry.value(QStringLiteral("port")).toInt();
        for (const auto &key : strings)
            schema = schema && entry.value(key).isString();
        candidate.hosts.append(
            {entry.value(QStringLiteral("id")).toString(),
             entry.value(QStringLiteral("name")).toString(),
             {entry.value(QStringLiteral("host")).toString(), entry.value(QStringLiteral("user")).toString(),
              entry.value(QStringLiteral("identity_file")).toString(),
              entry.value(QStringLiteral("port")).toInt(),
              entry.value(QStringLiteral("host_fingerprint")).toString()}});
    }
    for (const auto value : root.value(QStringLiteral("projects")).toArray())
    {
        const auto entry = value.toObject();
        const QStringList strings{QStringLiteral("id"), QStringLiteral("name"), QStringLiteral("directory"),
                                  QStringLiteral("host_id")};
        auto expected = strings;
        expected.append(QStringLiteral("enabled"));
        schema = schema && value.isObject() && keys(entry, expected) &&
                 entry.value(QStringLiteral("enabled")).isBool();
        for (const auto &key : strings)
            schema = schema && entry.value(key).isString();
        const QString id = entry.value(QStringLiteral("id")).toString();
        candidate.projects.projects.append({entry.value(QStringLiteral("name")).toString(),
                                            entry.value(QStringLiteral("directory")).toString(),
                                            entry.value(QStringLiteral("enabled")).toBool(), id});
        candidate.projectHosts.insert(id, entry.value(QStringLiteral("host_id")).toString());
    }
    if (!schema)
    {
        error = QStringLiteral("工作台配置格式或版本不受支持；原文件未修改。");
        return false;
    }
    if (!validate(candidate, error))
        return false;
    state = candidate;
    return true;
}

bool saveWorkbenchState(const QString &path, const WorkbenchState &state, QString &error)
{
    error.clear();
    if (!validate(state, error))
        return false;
    if (QFileInfo::exists(path))
    {
        WorkbenchState original;
        if (!loadWorkbenchState(path, original, error))
            return false;
    }
    QJsonArray hosts;
    for (const auto &host : state.hosts)
        hosts.append(QJsonObject{{QStringLiteral("id"), host.id},
                                 {QStringLiteral("name"), host.name},
                                 {QStringLiteral("host"), host.connection.host},
                                 {QStringLiteral("user"), host.connection.user},
                                 {QStringLiteral("identity_file"), host.connection.identityFile},
                                 {QStringLiteral("port"), host.connection.port},
                                 {QStringLiteral("host_fingerprint"), host.connection.hostFingerprint}});
    QJsonArray projects;
    for (const auto &project : state.projects.projects)
        projects.append(QJsonObject{{QStringLiteral("id"), project.id},
                                    {QStringLiteral("name"), project.name},
                                    {QStringLiteral("directory"), project.directory},
                                    {QStringLiteral("enabled"), project.enabled},
                                    {QStringLiteral("host_id"), state.projectHosts.value(project.id)}});
    const QJsonObject settings{{QStringLiteral("ssh_host"), state.projects.sshHost},
                               {QStringLiteral("ssh_user"), state.projects.sshUser},
                               {QStringLiteral("identity_file"), state.projects.identityFile},
                               {QStringLiteral("terminal_path"), state.projects.terminalPath}};
    const QJsonObject root{{QStringLiteral("version"), 1},
                           {QStringLiteral("hosts"), hosts},
                           {QStringLiteral("current_host_id"), state.currentHostId},
                           {QStringLiteral("legacy_settings"), settings},
                           {QStringLiteral("projects"), projects}};
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
    {
        error = QStringLiteral("无法创建工作台配置目录。");
        return false;
    }
    const auto bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
    {
        error = QStringLiteral("无法保存工作台配置：%1").arg(file.errorString());
        return false;
    }
    return true;
}

bool loadOrMigrateWorkbenchState(const QString &statePath, const QString &profilePath,
                                 const QString &projectsPath, const QString &legacyPath,
                                 WorkbenchState &state, QString &error)
{
    error.clear();
    if (QFileInfo::exists(statePath))
        return loadWorkbenchState(statePath, state, error);
    WorkbenchState candidate;
    QStringList sources;
    const QString projectSource = QFileInfo::exists(projectsPath) ? projectsPath : legacyPath;
    if (!projectSource.isEmpty() && QFileInfo::exists(projectSource))
    {
        if (!launcher::loadProjectConfiguration(projectSource, candidate.projects, error))
            return false;
        sources.append(projectSource);
    }
    if (QFileInfo::exists(profilePath))
    {
        HostProfile host;
        host.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (!sshsetup::loadProfile(profilePath, host.connection, error))
            return false;
        host.name = host.connection.host;
        candidate.hosts.append(host);
        candidate.currentHostId = host.id;
        for (const auto &project : candidate.projects.projects)
            candidate.projectHosts.insert(project.id, host.id);
        sources.append(profilePath);
    }
    else if (!candidate.projects.projects.isEmpty())
    {
        error = QStringLiteral("旧项目存在但未找到连接配置，请先完成首次设置；不会猜测项目所属主机。");
        return false;
    }
    if (!validate(candidate, error))
        return false;
    for (const auto &source : sources)
        if (!backup(source, error))
            return false;
    if (!sources.isEmpty() && !saveWorkbenchState(statePath, candidate, error))
        return false;
    state = candidate;
    return true;
}
}
