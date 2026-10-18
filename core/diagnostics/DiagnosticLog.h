#pragma once
#include <QDateTime>
#include <QFile>
#include <QLockFile>
#include <functional>
#include <memory>
#include <model/SessionTypes.h>
#include <optional>

namespace rad
{
// 仅供 Agent 控制线程调用；每条记录同步写入并 flush。仅保存固定类别与随机会话 ID。
class DiagnosticLog final
{
public:
    struct Options
    {
        qint64 maxBytes = 1024 * 1024;
        int maxArchives = 5;
        int retentionDays = 7;
        std::function<QDateTime()> clock = [] { return QDateTime::currentDateTimeUtc(); };
    };
    DiagnosticLog();
    ~DiagnosticLog();
    explicit DiagnosticLog(Options options);
    OperationResult open(const QString &directory);
    // 未知事件/错误枚举被拒绝；不接受任意消息、路径、身份、命令或终端字节。
    OperationResult record(const QString &event, SessionId sessionId = {},
                           std::optional<ErrorCode> errorCode = {});

private:
    OperationResult rotate(const QDateTime &now);
    OperationResult prune(const QDateTime &now);
    QString archivePath(int index) const;
    Options m_options;
    QString m_directory;
    QFile m_file;
    std::unique_ptr<QLockFile> m_lock;
    QDate m_day;
};
}
