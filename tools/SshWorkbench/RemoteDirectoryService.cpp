#include "RemoteDirectoryService.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

namespace sshworkbench
{
namespace
{
constexpr int kOutputLimit = 2 * 1024 * 1024;
constexpr int kEntryLimit = 5000;
QString encodeUnicode(const QString &text)
{
    QByteArray bytes;
    for (const auto character : text)
    {
        bytes.append(char(character.unicode() & 255));
        bytes.append(char(character.unicode() >> 8));
    }
    return QString::fromLatin1(bytes.toBase64());
}
RemoteDirectoryResult failure(const QString &message)
{
    RemoteDirectoryResult result;
    result.error = message;
    return result;
}
bool validPath(const QString &path, bool allowEmpty = false)
{
    return (allowEmpty || !path.isEmpty()) && path.size() <= 4096 && !path.contains(QChar::Null) &&
           !path.contains('\r') && !path.contains('\n');
}
bool readEntries(const QJsonValue &value, QVector<RemoteDirectoryEntry> &entries)
{
    if (!value.isArray() || value.toArray().size() > kEntryLimit)
        return false;
    for (const auto entry : value.toArray())
    {
        const auto object = entry.toObject();
        const auto name = object.value(QStringLiteral("name"));
        const auto path = object.value(QStringLiteral("path"));
        if (!entry.isObject() || !name.isString() || name.toString().isEmpty() || !path.isString() ||
            !validPath(path.toString()))
            return false;
        entries.append({name.toString(), path.toString()});
    }
    return true;
}
struct Preparation
{
    QTemporaryDir trustedDirectory;
    QStringList arguments;
    QString error;
    bool success = false;
};
}

bool remoteDirectoryCommandArguments(const QString &path, QStringList &arguments, QString &error)
{
    arguments.clear();
    error.clear();
    if (!validPath(path, true))
    {
        error = QStringLiteral("远程目录过长或包含无效字符。");
        return false;
    }
    const QString script =
        QStringLiteral(
            "$ErrorActionPreference='Stop'; $ProgressPreference='SilentlyContinue'; "
            "[Console]::OutputEncoding=New-Object Text.UTF8Encoding($false); "
            "try { $p=[Text.Encoding]::Unicode.GetString([Convert]::FromBase64String('%1')); "
            "if ([string]::IsNullOrEmpty($p)) { $p=$HOME }; "
            "$item=Get-Item -LiteralPath $p -Force -ErrorAction Stop; "
            "if ($item.PSProvider.Name -ne 'FileSystem' -or !$item.PSIsContainer) { throw 'NOT_DIRECTORY' }; "
            "$dirs=@(Get-ChildItem -LiteralPath $item.FullName -Directory -Force -ErrorAction Stop | "
            "Select-Object -First 5001); if ($dirs.Count -gt 5000) { throw 'ENTRY_LIMIT' }; "
            "$children=@($dirs | Sort-Object Name | ForEach-Object { @{name=$_.Name;path=$_.FullName} }); "
            "$drives=@(Get-PSDrive -PSProvider FileSystem | Where-Object { "
            "Test-Path -LiteralPath $_.Root -PathType Container -ErrorAction SilentlyContinue } | "
            "ForEach-Object { @{name=$_.Name+':';path=$_.Root} }); "
            "$parent=''; if ($null -ne $item.Parent) { $parent=$item.Parent.FullName }; "
            "$result=@{success=$true;currentPath=$item.FullName;parentPath=$parent;homePath=$HOME;"
            "drives=$drives;directories=$children}; "
            "$json=ConvertTo-Json -InputObject $result -Depth 5 -Compress; "
            "if ([Text.Encoding]::UTF8.GetByteCount($json) -gt 2097152) { throw 'OUTPUT_LIMIT' }; "
            "[Console]::Out.Write($json) } catch { "
            "$code='QUERY_FAILED'; if ($_.Exception.Message -eq 'ENTRY_LIMIT') {$code='ENTRY_LIMIT'} "
            "elseif ($_.Exception.Message -eq 'OUTPUT_LIMIT') {$code='OUTPUT_LIMIT'} "
            "elseif ($_.Exception.Message -eq 'NOT_DIRECTORY') {$code='NOT_DIRECTORY'} "
            "elseif ($_.CategoryInfo.Category -eq 'PermissionDenied') {$code='ACCESS_DENIED'} "
            "elseif ($_.CategoryInfo.Category -eq 'ObjectNotFound') {$code='NOT_FOUND'}; "
            "[Console]::Out.Write((ConvertTo-Json -Compress -InputObject @{success=$false;error=$code})); "
            "exit 1 }")
            .arg(encodeUnicode(path));
    arguments = QStringList{QStringLiteral("powershell.exe"),  QStringLiteral("-NoLogo"),
                            QStringLiteral("-NoProfile"),      QStringLiteral("-NonInteractive"),
                            QStringLiteral("-EncodedCommand"), encodeUnicode(script)};
    return true;
}

RemoteDirectoryResult parseRemoteDirectoryResult(const QByteArray &output)
{
    if (output.size() > kOutputLimit)
        return failure(QStringLiteral("目录响应超过 2 MiB，请缩小浏览范围。"));
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(output, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return failure(QStringLiteral("远程目录响应格式无效，请检查家庭端 PowerShell。"));
    const auto object = document.object();
    if (!object.value(QStringLiteral("success")).isBool())
        return failure(QStringLiteral("远程目录响应缺少结果状态。"));
    if (!object.value(QStringLiteral("success")).toBool())
    {
        const auto code = object.value(QStringLiteral("error")).toString();
        if (code == QStringLiteral("ENTRY_LIMIT"))
            return failure(QStringLiteral("此目录包含超过 5000 个文件夹，请手动输入更具体的目录。"));
        if (code == QStringLiteral("OUTPUT_LIMIT"))
            return failure(QStringLiteral("目录响应超过 2 MiB，请缩小浏览范围。"));
        if (code == QStringLiteral("ACCESS_DENIED"))
            return failure(QStringLiteral("无权读取此目录，请选择其他目录。"));
        if (code == QStringLiteral("NOT_FOUND"))
            return failure(QStringLiteral("远程目录不存在，请检查路径或返回用户主目录。"));
        if (code == QStringLiteral("NOT_DIRECTORY"))
            return failure(QStringLiteral("所选路径不是文件系统文件夹。"));
        return failure(QStringLiteral("无法读取远程目录，请检查路径、权限及家庭端 PowerShell。"));
    }
    RemoteDirectoryResult result;
    const auto current = object.value(QStringLiteral("currentPath"));
    const auto parent = object.value(QStringLiteral("parentPath"));
    const auto home = object.value(QStringLiteral("homePath"));
    if (!current.isString() || !validPath(current.toString()) || !parent.isString() ||
        !validPath(parent.toString(), true) || !home.isString() || !validPath(home.toString()) ||
        !readEntries(object.value(QStringLiteral("drives")), result.drives) ||
        !readEntries(object.value(QStringLiteral("directories")), result.directories))
        return failure(QStringLiteral("远程目录响应字段无效或条目超过 5000 项。"));
    result.currentPath = current.toString();
    result.parentPath = parent.toString();
    result.homePath = home.toString();
    result.success = true;
    return result;
}

class RemoteDirectoryService::Impl
{
public:
    QElapsedTimer elapsed;
    qint64 preparationMilliseconds = -1;
    QTimer *deadline = nullptr;
    QPointer<QThread> preparationThread;
    std::shared_ptr<Preparation> preparation;
    QProcess *process = nullptr;
    QByteArray output;
    QByteArray diagnostics;
    bool active = false;
    bool busy = false;
    quint64 generation = 0;
};

RemoteDirectoryService::RemoteDirectoryService(QObject *parent) : QObject(parent), m_impl(new Impl)
{
    qRegisterMetaType<RemoteDirectoryResult>();
    m_impl->deadline = new QTimer(this);
    m_impl->deadline->setSingleShot(true);
    m_impl->deadline->setInterval(15000);
    connect(m_impl->deadline, &QTimer::timeout, this,
            [this] { complete(failure(QStringLiteral("目录查询超时，请检查连接后重试。"))); });
}
RemoteDirectoryService::~RemoteDirectoryService()
{
    cancel();
    if (m_impl->process)
    {
        m_impl->process->disconnect(this);
        m_impl->process->kill();
        m_impl->process->waitForFinished(1000);
    }
}
bool RemoteDirectoryService::isBusy() const
{
    return m_impl->busy;
}
void RemoteDirectoryService::updateBusy()
{
    const bool busy = m_impl->active || !m_impl->preparationThread.isNull() || m_impl->process;
    if (busy == m_impl->busy)
        return;
    m_impl->busy = busy;
    emit busyChanged(busy);
}
void RemoteDirectoryService::start(const sshsetup::ConnectionProfile &profile, const QString &path)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (isBusy())
    {
        emit finished(failure(QStringLiteral("上一次目录请求仍在收尾，请稍后重试。")));
        return;
    }
    QStringList command;
    QString error;
    if (!remoteDirectoryCommandArguments(path, command, error))
    {
        emit finished(failure(error));
        return;
    }
    beginPreparation(profile, command);
}
void RemoteDirectoryService::beginPreparation(const sshsetup::ConnectionProfile &profile,
                                              const QStringList &command, int preparationDelay, int timeout)
{
    m_impl->elapsed.start();
    m_impl->preparationMilliseconds = -1;
    m_impl->active = true;
    const auto generation = ++m_impl->generation;
    m_impl->deadline->start(timeout);
    auto preparation = std::make_shared<Preparation>();
    m_impl->preparation = preparation;
    auto *worker = QThread::create(
        [profile, preparation, preparationDelay]
        {
            // 测试延迟仅用于验证取消与超时后的迟到结果；生产调用保持零延迟。
            if (preparationDelay > 0)
                QThread::msleep(static_cast<unsigned long>(preparationDelay));
            if (!preparation->trustedDirectory.isValid())
            {
                preparation->error = QStringLiteral("无法创建临时主机信任目录。");
                return;
            }
            preparation->success = sshsetup::prepareSshArguments(
                profile, preparation->trustedDirectory.path(), preparation->arguments, preparation->error);
        });
    m_impl->preparationThread = worker;
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &QThread::finished, this,
            [this, preparation, command, generation]
            {
                m_impl->preparationThread = nullptr;
                if (!m_impl->active || m_impl->generation != generation)
                {
                    m_impl->preparation.reset();
                    updateBusy();
                    return;
                }
                if (!preparation->success)
                {
                    complete(failure(preparation->error +
                                     QStringLiteral(" 请检查连接设置；不会自动接受主机指纹。")));
                    return;
                }
                const auto program =
                    qEnvironmentVariable("SystemRoot") + QStringLiteral("/System32/OpenSSH/ssh.exe");
                if (!QFileInfo::exists(program))
                {
                    complete(failure(QStringLiteral("未找到 Windows OpenSSH 客户端。")));
                    return;
                }
                QStringList arguments = preparation->arguments;
                arguments.insert(arguments.indexOf(QStringLiteral("--")), QStringLiteral("-T"));
                QByteArray input;
                arguments.append(directoryTransportArguments(command, input));
                launchProcess(program, arguments, input);
            });
    worker->start();
    updateBusy();
}
void RemoteDirectoryService::startLocalProcessForTest(const QString &program, const QStringList &arguments,
                                                      int timeout)
{
    m_impl->elapsed.start();
    Q_ASSERT(!isBusy());
    m_impl->active = true;
    ++m_impl->generation;
    m_impl->deadline->start(timeout);
    launchProcess(program, arguments);
}
QStringList RemoteDirectoryService::directoryTransportArguments(const QStringList &command, QByteArray &input)
{
    const auto encoded = QByteArray::fromBase64(command.last().toLatin1());
    QString script;
    script.reserve(encoded.size() / 2);
    for (int index = 0; index + 1 < encoded.size(); index += 2)
        script.append(QChar(uchar(encoded[index]) | (ushort(uchar(encoded[index + 1])) << 8)));
    input = script.toUtf8();
    // 固定短命令避开家庭端默认 cmd 的 8191 字符上限；路径仍只以编码数据出现在脚本中。
    const QString bootstrap = QStringLiteral("& ([ScriptBlock]::Create([Console]::In.ReadToEnd()))");
    QStringList transport = command;
    transport.last() = encodeUnicode(bootstrap);
    return transport;
}
void RemoteDirectoryService::launchProcess(const QString &program, const QStringList &arguments,
                                           const QByteArray &input)
{
    m_impl->preparationMilliseconds = m_impl->elapsed.elapsed();
    auto *process = new QProcess(this);
    m_impl->process = process;
    m_impl->output.clear();
    m_impl->diagnostics.clear();
    process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(process, &QProcess::readyReadStandardOutput, this, &RemoteDirectoryService::readOutput);
    connect(process, &QProcess::readyReadStandardError, this, &RemoteDirectoryService::readOutput);
    connect(process, &QProcess::started, this,
            [this, process, input]
            {
                if (!m_impl->active)
                {
                    process->kill();
                    return;
                }
                if (!input.isEmpty() && process->write(input) != input.size())
                {
                    complete(failure(QStringLiteral("无法发送远程目录查询脚本。")));
                    return;
                }
                process->closeWriteChannel();
            });
    connect(process, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error)
            {
                if (error == QProcess::FailedToStart)
                {
                    auto *process = m_impl->process;
                    m_impl->process = nullptr;
                    process->deleteLater();
                    if (m_impl->active)
                        complete(failure(QStringLiteral("无法启动 SSH 目录查询，请检查 OpenSSH 安装。")));
                    else
                    {
                        m_impl->preparation.reset();
                        updateBusy();
                    }
                }
            });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, process](int exitCode, QProcess::ExitStatus exitStatus)
            {
                readOutput();
                m_impl->process = nullptr;
                process->deleteLater();
                if (m_impl->active)
                {
                    auto result = parseRemoteDirectoryResult(m_impl->output);
                    if (exitStatus != QProcess::NormalExit || (exitCode != 0 && result.success) ||
                        (exitCode != 0 && m_impl->output.isEmpty()))
                        result = failure(
                            QStringLiteral("SSH 目录查询未成功，请检查连接设置、主机信任和密钥授权。"));
                    complete(result);
                }
                else
                {
                    m_impl->preparation.reset();
                    updateBusy();
                }
            });
    process->start(program, arguments);
    updateBusy();
}
void RemoteDirectoryService::readOutput()
{
    if (!m_impl->process)
        return;
    const auto output = m_impl->process->readAllStandardOutput();
    const auto diagnostics = m_impl->process->readAllStandardError();
    if (!m_impl->active)
        return;
    if (qint64(m_impl->output.size()) + m_impl->diagnostics.size() + output.size() + diagnostics.size() >
        kOutputLimit)
    {
        complete(failure(QStringLiteral("目录查询输出超过 2 MiB，请缩小浏览范围。")));
        return;
    }
    m_impl->output.append(output);
    m_impl->diagnostics.append(diagnostics);
}
void RemoteDirectoryService::complete(const RemoteDirectoryResult &result)
{
    if (!m_impl->active)
        return;
    m_impl->active = false;
    m_impl->deadline->stop();
    if (m_impl->process && m_impl->process->state() != QProcess::NotRunning)
        m_impl->process->kill();
    if (!m_impl->process && m_impl->preparationThread.isNull())
        m_impl->preparation.reset();
    updateBusy();
    auto measured = result;
    const qint64 total = m_impl->elapsed.elapsed();
    measured.preparationMilliseconds =
        m_impl->preparationMilliseconds < 0 ? total : m_impl->preparationMilliseconds;
    measured.queryMilliseconds =
        m_impl->preparationMilliseconds < 0 ? -1 : total - m_impl->preparationMilliseconds;
    emit finished(measured);
}
void RemoteDirectoryService::cancel()
{
    Q_ASSERT(QThread::currentThread() == thread());
    ++m_impl->generation;
    m_impl->active = false;
    m_impl->deadline->stop();
    if (m_impl->process)
        m_impl->process->kill();
    if (!m_impl->process && m_impl->preparationThread.isNull())
        m_impl->preparation.reset();
    updateBusy();
}
}
