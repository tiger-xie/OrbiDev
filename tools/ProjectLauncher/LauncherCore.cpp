#include "LauncherCore.h"
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUuid>

namespace launcher
{
namespace
{
bool safeText(const QString &text, int maximum)
{
    if (text.size() > maximum)
        return false;
    for (int i = 0; i < text.size(); ++i)
    {
        const auto character = text[i];
        if (character.category() == QChar::Other_Control || character == QChar(0x2028) ||
            character == QChar(0x2029))
            return false;
        if (character.isHighSurrogate())
        {
            if (++i >= text.size() || !text[i].isLowSurrogate())
                return false;
        }
        else if (character.isLowSurrogate())
        {
            return false;
        }
    }
    return true;
}

bool absoluteWindowsPath(const QString &path)
{
    static const QRegularExpression expression(QStringLiteral("^[A-Za-z]:[/\\\\]"));
    return expression.match(path).hasMatch() && safeText(path, 1024) && !path.contains(QLatin1Char('"'));
}

bool exactKeys(const QJsonObject &object, const QStringList &keys)
{
    if (object.size() != keys.size())
        return false;
    for (const auto &key : keys)
        if (!object.contains(key))
            return false;
    return true;
}

QString encodeScript(const QString &script)
{
    QByteArray bytes;
    bytes.reserve(script.size() * 2);
    for (const auto character : script)
    {
        bytes.append(char(character.unicode() & 0xff));
        bytes.append(char(character.unicode() >> 8));
    }
    return QString::fromLatin1(bytes.toBase64());
}
}

bool validateConfiguration(const Configuration &configuration, QString &error)
{
    error.clear();
    static const QRegularExpression host(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,252}\\z"));
    static const QRegularExpression user(QStringLiteral("^[A-Za-z0-9_][A-Za-z0-9_.-]{0,63}\\z"));
    if (!host.match(configuration.sshHost).hasMatch())
        error = QStringLiteral("主机只能填写 IPv4、域名或 SSH 别名，不能包含命令参数。");
    else if (!configuration.sshUser.isEmpty() && !user.match(configuration.sshUser).hasMatch())
        error = QStringLiteral("SSH 用户名格式无效；使用 SSH 别名时可以留空。");
    else if (!configuration.identityFile.isEmpty() && (!absoluteWindowsPath(configuration.identityFile) ||
                                                       configuration.identityFile.contains(QLatin1Char(';'))))
        error = QStringLiteral("密钥文件请选择本机绝对路径，且路径不能包含分号。");
    else if (!configuration.terminalPath.isEmpty() && !absoluteWindowsPath(configuration.terminalPath))
        error = QStringLiteral("Windows Terminal 路径必须是本机绝对路径或留空自动查找。");
    else if (configuration.projects.size() > 32)
        error = QStringLiteral("轻量验证版最多保存 32 个项目。");
    if (!error.isEmpty())
        return false;
    for (const auto &project : configuration.projects)
    {
        if (project.name.trimmed().isEmpty() || !safeText(project.name, 80) ||
            project.name.contains(QLatin1Char(';')) || project.name.contains(QLatin1Char('"')))
        {
            error = QStringLiteral("项目名称不能为空，最多 80 个字符，不能包含控制字符、分号或双引号。");
            return false;
        }
        if (!project.directory.isEmpty() && !absoluteWindowsPath(project.directory))
        {
            error = QStringLiteral("远程目录必须是 Windows 盘符绝对路径；留空表示家庭用户目录。");
            return false;
        }
    }
    return true;
}

namespace
{
bool validProjectIds(const Configuration &configuration, QString &error)
{
    QSet<QString> identities;
    for (const auto &project : configuration.projects)
    {
        const QUuid uuid(project.id);
        if (uuid.isNull() || uuid.toString(QUuid::WithoutBraces) != project.id ||
            identities.contains(project.id))
        {
            error = QStringLiteral("项目 ID 必须是非空、唯一的标准 UUID；配置未修改。");
            return false;
        }
        identities.insert(project.id);
    }
    return true;
}

bool readConfiguration(const QString &path, Configuration &configuration, QString &error, bool workbench,
                       QByteArray *originalBytes = nullptr, int *originalVersion = nullptr)
{
    error.clear();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        error = QStringLiteral("无法读取启动器配置：%1").arg(file.errorString());
        return false;
    }
    if (file.size() > 256 * 1024)
    {
        error = QStringLiteral("启动器配置超过 256 KiB 限制。");
        return false;
    }
    const auto bytes = file.readAll();
    if (file.error() != QFileDevice::NoError)
    {
        error = QStringLiteral("读取启动器配置失败：%1").arg(file.errorString());
        return false;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(bytes, &parseError);
    const auto object = document.object();
    if (parseError.error != QJsonParseError::NoError || !document.isObject() ||
        !exactKeys(object, {QStringLiteral("version"), QStringLiteral("ssh_host"), QStringLiteral("ssh_user"),
                            QStringLiteral("identity_file"), QStringLiteral("terminal_path"),
                            QStringLiteral("projects")}) ||
        !object.value(QStringLiteral("version")).isDouble() ||
        (object.value(QStringLiteral("version")).toDouble() != 1 &&
         (!workbench || object.value(QStringLiteral("version")).toDouble() != 2)) ||
        !object.value(QStringLiteral("ssh_host")).isString() ||
        !object.value(QStringLiteral("ssh_user")).isString() ||
        !object.value(QStringLiteral("identity_file")).isString() ||
        !object.value(QStringLiteral("terminal_path")).isString() ||
        !object.value(QStringLiteral("projects")).isArray())
    {
        error = QStringLiteral("启动器配置格式或版本不受支持；原文件未修改。");
        return false;
    }
    Configuration loaded;
    const int version = object.value(QStringLiteral("version")).toInt();
    loaded.sshHost = object.value(QStringLiteral("ssh_host")).toString();
    loaded.sshUser = object.value(QStringLiteral("ssh_user")).toString();
    loaded.identityFile = object.value(QStringLiteral("identity_file")).toString();
    loaded.terminalPath = object.value(QStringLiteral("terminal_path")).toString();
    for (const auto value : object.value(QStringLiteral("projects")).toArray())
    {
        const auto entry = value.toObject();
        QStringList keys{QStringLiteral("name"), QStringLiteral("directory"), QStringLiteral("enabled")};
        if (version == 2)
            keys.append(QStringLiteral("id"));
        if (!value.isObject() || !exactKeys(entry, keys) || !entry.value(QStringLiteral("name")).isString() ||
            !entry.value(QStringLiteral("directory")).isString() ||
            !entry.value(QStringLiteral("enabled")).isBool() ||
            (version == 2 && !entry.value(QStringLiteral("id")).isString()))
        {
            error = QStringLiteral("项目配置字段无效；原文件未修改。");
            return false;
        }
        loaded.projects.append({entry.value(QStringLiteral("name")).toString(),
                                entry.value(QStringLiteral("directory")).toString(),
                                entry.value(QStringLiteral("enabled")).toBool(),
                                version == 2 ? entry.value(QStringLiteral("id")).toString()
                                : workbench  ? QUuid::createUuid().toString(QUuid::WithoutBraces)
                                             : QString()});
    }
    if (!validateConfiguration(loaded, error))
        return false;
    if (workbench && !validProjectIds(loaded, error))
        return false;
    if (originalBytes)
        *originalBytes = bytes;
    if (originalVersion)
        *originalVersion = version;
    configuration = loaded;
    return true;
}

bool writeConfiguration(const QString &path, const Configuration &configuration, QString &error,
                        bool workbench)
{
    if (!validateConfiguration(configuration, error))
        return false;
    QJsonArray projects;
    for (const auto &project : configuration.projects)
    {
        QJsonObject entry{{QStringLiteral("name"), project.name},
                          {QStringLiteral("directory"), project.directory},
                          {QStringLiteral("enabled"), project.enabled}};
        if (workbench)
            entry.insert(QStringLiteral("id"), project.id);
        projects.append(entry);
    }
    const QJsonObject object{{QStringLiteral("version"), workbench ? 2 : 1},
                             {QStringLiteral("ssh_host"), configuration.sshHost},
                             {QStringLiteral("ssh_user"), configuration.sshUser},
                             {QStringLiteral("identity_file"), configuration.identityFile},
                             {QStringLiteral("terminal_path"), configuration.terminalPath},
                             {QStringLiteral("projects"), projects}};
    QSaveFile file(path);
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
    {
        error = QStringLiteral("无法保存启动器配置：%1").arg(file.errorString());
        return false;
    }
    return true;
}
}

bool loadConfiguration(const QString &path, Configuration &configuration, QString &error)
{
    return readConfiguration(path, configuration, error, false);
}

bool saveConfiguration(const QString &path, const Configuration &configuration, QString &error)
{
    return writeConfiguration(path, configuration, error, false);
}

bool loadProjectConfiguration(const QString &path, Configuration &configuration, QString &error)
{
    return readConfiguration(path, configuration, error, true);
}

bool saveProjectConfiguration(const QString &path, const Configuration &configuration, QString &error)
{
    if (!validateConfiguration(configuration, error) || !validProjectIds(configuration, error))
        return false;
    if (QFileInfo::exists(path))
    {
        Configuration previous;
        QByteArray original;
        int version = 0;
        if (!readConfiguration(path, previous, error, true, &original, &version))
            return false;
        if (version == 1)
        {
            // 固定备份名不覆盖已有不同内容，避免把唯一可回退的原配置丢掉。
            const QString backupPath = path + QStringLiteral(".v1.bak");
            QFile backup(backupPath);
            if (QFileInfo::exists(backupPath))
            {
                if (!backup.open(QIODevice::ReadOnly) || backup.size() != original.size() ||
                    backup.readAll() != original || backup.error() != QFileDevice::NoError)
                {
                    error =
                        QStringLiteral("已有迁移备份无法读取或内容不同：%1；原配置未修改。").arg(backupPath);
                    return false;
                }
            }
            else
            {
                if (!backup.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
                    backup.write(original) != original.size() || !backup.flush())
                {
                    error =
                        QStringLiteral("无法备份旧项目配置：%1；原配置未修改。").arg(backup.errorString());
                    return false;
                }
            }
        }
    }
    return writeConfiguration(path, configuration, error, true);
}

bool buildLaunchArguments(const Configuration &configuration, const Project &project, const QString &sshPath,
                          QStringList &arguments, QString &error)
{
    arguments.clear();
    if (!validateConfiguration(configuration, error))
        return false;
    bool present = false;
    for (const auto &candidate : configuration.projects)
        if (candidate.name == project.name && candidate.directory == project.directory && candidate.enabled &&
            project.enabled)
            present = true;
    if (!present)
    {
        error = QStringLiteral("该项目未启用或不属于当前配置。");
        return false;
    }
    if (sshPath.isEmpty() || !safeText(sshPath, 1024) || sshPath.contains(QLatin1Char(';')) ||
        sshPath.contains(QLatin1Char('"')))
    {
        error = QStringLiteral("SSH 程序路径无效。");
        return false;
    }
    QString location = QStringLiteral("$HOME");
    if (!project.directory.isEmpty())
    {
        location = QStringLiteral("([Text.Encoding]::Unicode.GetString([Convert]::FromBase64String('%1')))")
                       .arg(encodeScript(project.directory));
    }
    // 目录独立编码，避免 PowerShell 把弯引号当分隔符；整段脚本再编码，不暴露 Terminal 命令分隔符。
    const QString script = QStringLiteral("try { Set-Location -LiteralPath %1 -ErrorAction Stop; "
                                          "& claude.cmd } catch { Write-Error $_ }")
                               .arg(location);
    arguments = QStringList{QStringLiteral("-w"),
                            QStringLiteral("OrbiDev-SSH"),
                            QStringLiteral("new-tab"),
                            QStringLiteral("--title"),
                            project.name,
                            sshPath,
                            QStringLiteral("-t"),
                            QStringLiteral("-o"),
                            QStringLiteral("StrictHostKeyChecking=ask")};
    if (!configuration.identityFile.isEmpty())
        arguments << QStringLiteral("-i") << configuration.identityFile;
    arguments << (configuration.sshUser.isEmpty()
                      ? configuration.sshHost
                      : configuration.sshUser + QLatin1Char('@') + configuration.sshHost)
              << QStringLiteral("powershell.exe") << QStringLiteral("-NoLogo") << QStringLiteral("-NoProfile")
              << QStringLiteral("-NoExit") << QStringLiteral("-EncodedCommand") << encodeScript(script);
    return true;
}
}
