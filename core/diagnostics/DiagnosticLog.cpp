#include "DiagnosticLog.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

namespace rad
{
namespace
{
OperationResult ioFailure()
{
    return OperationResult::failure({ErrorCode::SystemError, QStringLiteral("诊断日志文件操作失败")});
}
QString errorName(ErrorCode code)
{
    switch (code)
    {
    case ErrorCode::InvalidArgument:
        return QStringLiteral("InvalidArgument");
    case ErrorCode::InvalidState:
        return QStringLiteral("InvalidState");
    case ErrorCode::NotFound:
        return QStringLiteral("NotFound");
    case ErrorCode::PermissionDenied:
        return QStringLiteral("PermissionDenied");
    case ErrorCode::ControlUnavailable:
        return QStringLiteral("ControlUnavailable");
    case ErrorCode::ProviderUnavailable:
        return QStringLiteral("ProviderUnavailable");
    case ErrorCode::QueueFull:
        return QStringLiteral("QueueFull");
    case ErrorCode::LaunchFailed:
        return QStringLiteral("LaunchFailed");
    case ErrorCode::InputFailed:
        return QStringLiteral("InputFailed");
    case ErrorCode::ResizeFailed:
        return QStringLiteral("ResizeFailed");
    case ErrorCode::SystemError:
        return QStringLiteral("SystemError");
    }
    return {};
}
}
DiagnosticLog::DiagnosticLog() : DiagnosticLog(Options{})
{
}
DiagnosticLog::~DiagnosticLog()
{
    m_file.close();
}
DiagnosticLog::DiagnosticLog(Options options) : m_options(std::move(options))
{
}
QString DiagnosticLog::archivePath(int index) const
{
    return QDir(m_directory).filePath(QStringLiteral("agent.%1.log").arg(index));
}
OperationResult DiagnosticLog::open(const QString &directory)
{
    if (m_lock || directory.isEmpty() || m_options.maxBytes < 256 || m_options.maxArchives < 1 ||
        m_options.maxArchives > 100 || m_options.retentionDays < 1 || !m_options.clock)
        return OperationResult::failure({ErrorCode::InvalidArgument, QStringLiteral("诊断日志配置无效")});
    if (!QDir().mkpath(directory))
        return ioFailure();
    m_directory = QDir(directory).absolutePath();
    auto lock = std::make_unique<QLockFile>(QDir(m_directory).filePath(QStringLiteral("agent-log.lock")));
    lock->setStaleLockTime(0);
    if (!lock->tryLock(0))
        return ioFailure();
    const auto now = m_options.clock().toUTC();
    if (!now.isValid())
        return ioFailure();
    m_file.setFileName(QDir(m_directory).filePath(QStringLiteral("agent.log")));
    const QFileInfo previous(m_file);
    m_day = previous.exists() ? previous.lastModified().toUTC().date() : now.date();
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Append))
        return ioFailure();
    m_lock = std::move(lock);
    return prune(now);
}
OperationResult DiagnosticLog::prune(const QDateTime &now)
{
    for (int index = 1; index <= 100; ++index)
    {
        const QFileInfo archive(archivePath(index));
        if (!archive.exists())
            continue;
        if (index > m_options.maxArchives ||
            archive.lastModified().toUTC().date().daysTo(now.date()) >= m_options.retentionDays)
        {
            if (!QFile::remove(archive.filePath()))
                return ioFailure();
        }
    }
    return accepted();
}
OperationResult DiagnosticLog::rotate(const QDateTime &now)
{
    m_file.close();
    const auto last = archivePath(m_options.maxArchives);
    if (QFileInfo::exists(last) && !QFile::remove(last))
        return ioFailure();
    for (int index = m_options.maxArchives - 1; index >= 1; --index)
    {
        const auto source = archivePath(index);
        if (QFileInfo::exists(source) && !QFile::rename(source, archivePath(index + 1)))
            return ioFailure();
    }
    if (!QFile::rename(m_file.fileName(), archivePath(1)))
        return ioFailure();
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Append))
        return ioFailure();
    m_day = now.date();
    return prune(now);
}
OperationResult DiagnosticLog::record(const QString &event, SessionId sessionId,
                                      std::optional<ErrorCode> errorCode)
{
    static const QSet<QString> events{QStringLiteral("disconnected"),
                                      QStringLiteral("connection_rejected"),
                                      QStringLiteral("authenticated"),
                                      QStringLiteral("slow_client"),
                                      QStringLiteral("operation_rejected"),
                                      QStringLiteral("session_created"),
                                      QStringLiteral("session_attached"),
                                      QStringLiteral("session_detached"),
                                      QStringLiteral("session_terminate_accepted"),
                                      QStringLiteral("agent_started"),
                                      QStringLiteral("agent_stopped"),
                                      QStringLiteral("session_state_changed"),
                                      QStringLiteral("session_finished"),
                                      QStringLiteral("session_error"),
                                      QStringLiteral("storage_error")};
    const auto category = errorCode ? errorName(*errorCode) : QString{};
    if (!events.contains(event) || (errorCode && category.isEmpty()))
        return OperationResult::failure({ErrorCode::InvalidArgument, QStringLiteral("诊断事件类别无效")});
    if (!m_file.isOpen())
        return ioFailure();
    const auto now = m_options.clock().toUTC();
    if (!now.isValid())
        return ioFailure();
    QJsonObject object{{QStringLiteral("time"), now.toString(Qt::ISODateWithMs)},
                       {QStringLiteral("event"), event}};
    if (sessionId.isValid())
        object.insert(QStringLiteral("session_id"), sessionId.toString());
    if (errorCode)
        object.insert(QStringLiteral("error_code"), category);
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    if (bytes.size() > m_options.maxBytes)
        return ioFailure();
    if (m_file.size() > m_options.maxBytes - bytes.size() || m_day != now.date())
    {
        const auto result = rotate(now);
        if (!result.isSuccess())
            return result;
    }
    if (m_file.write(bytes) != bytes.size() || !m_file.flush())
        return ioFailure();
    return accepted();
}
}
