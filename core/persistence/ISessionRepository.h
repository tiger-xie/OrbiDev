#pragma once
#include "model/SessionTypes.h"

namespace rad
{
// 所有方法在创建线程同步调用；调用方必须检查错误，数据库失败不得降级为内存成功。
class ISessionRepository
{
public:
    virtual ~ISessionRepository() = default;
    virtual OperationResult open(const QString &databasePath) = 0;
    // 原子地将上次 Agent 遗留的活跃会话标记失败；不恢复进程或输出。
    virtual OperationResult recover(qint64 nowUtcMs) = 0;
    // 只加载最近的有界元数据；控制权、订阅与输出偏移始终使用空默认值。
    virtual Result<QVector<SessionSnapshot>> load(int limit = 128) = 0;
    virtual OperationResult save(const SessionSnapshot &snapshot) = 0;
};
}
