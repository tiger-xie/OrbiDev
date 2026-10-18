#include "persistence/SqliteSessionRepository.h"
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace rad
{
namespace
{
Error storageError()
{
    return {ErrorCode::SystemError, QStringLiteral("会话数据库操作失败，请检查运行诊断与数据库可用性")};
}
bool validFailure(const QString &value)
{
    static const QStringList allowed{QString(), QStringLiteral("agent_restarted"),
                                     QStringLiteral("launch_failed"), QStringLiteral("backend_failed")};
    return allowed.contains(value);
}
bool validText(const QString &value, int maximum)
{
    if (value.trimmed().isEmpty() || value.size() > maximum || value.contains(QChar(0)))
        return false;
    for (int index = 0; index < value.size(); ++index)
    {
        if (value[index].isHighSurrogate())
        {
            if (++index >= value.size() || !value[index].isLowSurrogate())
                return false;
        }
        else if (value[index].isLowSurrogate())
            return false;
    }
    return true;
}
bool validSnapshot(const SessionSnapshot &snapshot)
{
    const int state = static_cast<int>(snapshot.status);
    return snapshot.id.isValid() && state >= 0 && state <= 4 && validText(snapshot.name, 256) &&
           validText(snapshot.providerId, 128) && validText(snapshot.workspaceId, 128) &&
           validFailure(snapshot.failureCode) && snapshot.createdAtUtcMs >= 0 &&
           snapshot.updatedAtUtcMs >= snapshot.createdAtUtcMs;
}
bool pruneHistory(QSqlDatabase &database)
{
    QSqlQuery query(database);
    // 历史终态最多保留 128 条；活跃会话不得因容量裁剪而失去崩溃恢复记录。
    return query.exec(
        QStringLiteral("DELETE FROM sessions WHERE status IN (3,4) AND id NOT IN (SELECT id FROM sessions "
                       "WHERE status IN (3,4) ORDER BY updated_at_utc_ms DESC,id LIMIT 128)"));
}

}
SqliteSessionRepository::SqliteSessionRepository()
    : m_connectionName(QStringLiteral("rad-session-") + QUuid::createUuid().toString(QUuid::WithoutBraces))
{
}
SqliteSessionRepository::~SqliteSessionRepository()
{
    close();
}
void SqliteSessionRepository::close()
{
    m_ready = false;
    if (m_database.isValid())
        m_database.close();
    m_database = QSqlDatabase();
    QSqlDatabase::removeDatabase(m_connectionName);
}
OperationResult SqliteSessionRepository::open(const QString &databasePath)
{
    if (m_database.isValid() || databasePath.isEmpty())
        return OperationResult::failure(storageError());
    m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_database.setDatabaseName(databasePath);
    m_database.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=1000"));
    if (!m_database.open())
    {
        close();
        return OperationResult::failure(storageError());
    }
    const auto result = migrate();
    if (!result.isSuccess())
        close();
    else
        m_ready = true;
    return result;
}
OperationResult SqliteSessionRepository::migrate()
{
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("PRAGMA quick_check")) || !query.next() ||
        query.value(0).toString() != QStringLiteral("ok"))
        return OperationResult::failure(storageError());
    query.finish();
    if (!query.exec(QStringLiteral("PRAGMA user_version")) || !query.next())
        return OperationResult::failure(storageError());
    const int version = query.value(0).toInt();
    query.finish();
    if (version < 0 || version > 2 || !m_database.transaction())
        return OperationResult::failure(storageError());
    const auto fail = [this]()
    {
        m_database.rollback();
        return OperationResult::failure(storageError());
    };
    if (version == 0)
    {
        // 未标版本的已有表不能当作空数据库覆盖。
        if (!query.exec(QStringLiteral(
                "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'")) ||
            query.next())
            return fail();
        query.finish();
        if (!query.exec(QStringLiteral("CREATE TABLE sessions (id TEXT PRIMARY KEY NOT NULL, name TEXT NOT "
                                       "NULL, provider_id TEXT NOT NULL, workspace_id TEXT NOT NULL, status "
                                       "INTEGER NOT NULL CHECK(status BETWEEN 0 AND 4))")))
            return fail();
    }
    if (version < 2)
    {
        const QStringList migrations{
            QStringLiteral("ALTER TABLE sessions ADD COLUMN created_at_utc_ms INTEGER NOT NULL DEFAULT 0"),
            QStringLiteral("ALTER TABLE sessions ADD COLUMN updated_at_utc_ms INTEGER NOT NULL DEFAULT 0"),
            QStringLiteral("ALTER TABLE sessions ADD COLUMN exit_code INTEGER NOT NULL DEFAULT 0"),
            QStringLiteral("ALTER TABLE sessions ADD COLUMN exit_code_valid INTEGER NOT NULL DEFAULT 0"),
            QStringLiteral("ALTER TABLE sessions ADD COLUMN failure_code TEXT NOT NULL DEFAULT ''")};
        for (const auto &statement : migrations)
            if (!query.exec(statement))
                return fail();
    }
    if (!query.exec(QStringLiteral("SELECT "
                                   "id,name,provider_id,workspace_id,status,created_at_utc_ms,updated_at_utc_"
                                   "ms,exit_code,exit_code_valid,failure_code FROM sessions LIMIT 0")))
        return fail();
    query.finish();
    if (!query.exec(QStringLiteral("PRAGMA user_version=2")) || !m_database.commit())
        return fail();
    return accepted();
}
OperationResult SqliteSessionRepository::recover(qint64 nowUtcMs)
{
    if (!m_ready || nowUtcMs < 0 || !m_database.transaction())
        return OperationResult::failure(storageError());
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("UPDATE sessions SET "
                                 "status=4,failure_code='agent_restarted',exit_code=0,exit_code_valid=0,"
                                 "updated_at_utc_ms=MAX(updated_at_utc_ms,?) WHERE status IN (0,1,2)"));
    query.addBindValue(nowUtcMs);
    if (!query.exec() || !pruneHistory(m_database) || !m_database.commit())
    {
        m_database.rollback();
        return OperationResult::failure(storageError());
    }
    return accepted();
}
Result<QVector<SessionSnapshot>> SqliteSessionRepository::load(int limit)
{
    if (!m_ready || limit < 1 || limit > 4096)
        return Result<QVector<SessionSnapshot>>::failure(storageError());
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT "
        "id,name,provider_id,workspace_id,status,created_at_utc_ms,updated_at_utc_ms,exit_code,exit_code_"
        "valid,failure_code FROM sessions ORDER BY updated_at_utc_ms DESC,id LIMIT ?"));
    query.addBindValue(limit);
    if (!query.exec())
        return Result<QVector<SessionSnapshot>>::failure(storageError());
    QVector<SessionSnapshot> sessions;
    while (query.next())
    {
        for (const int column : {4, 5, 6, 7, 8})
        {
            const auto type = query.value(column).type();
            if (type != QVariant::Int && type != QVariant::LongLong)
                return Result<QVector<SessionSnapshot>>::failure(storageError());
        }
        const auto id = SessionId::parse(query.value(0).toString());
        bool stateValid = false;
        bool createdValid = false;
        bool updatedValid = false;
        bool exitValid = false;
        bool exitFlagValid = false;
        const int state = query.value(4).toInt(&stateValid);
        const qint64 createdAt = query.value(5).toLongLong(&createdValid);
        const qint64 updatedAt = query.value(6).toLongLong(&updatedValid);
        const int exitCode = query.value(7).toInt(&exitValid);
        const int exitFlag = query.value(8).toInt(&exitFlagValid);
        if (!id.isSuccess() || !stateValid || state < 0 || state > 4 || !createdValid || createdAt < 0 ||
            !updatedValid || updatedAt < 0 || !exitValid || !exitFlagValid ||
            (exitFlag != 0 && exitFlag != 1) || !validFailure(query.value(9).toString()))
            return Result<QVector<SessionSnapshot>>::failure(storageError());
        SessionSnapshot snapshot;
        snapshot.id = id.value();
        snapshot.name = query.value(1).toString();
        snapshot.providerId = query.value(2).toString();
        snapshot.workspaceId = query.value(3).toString();
        snapshot.status = static_cast<SessionStatus>(state);
        snapshot.createdAtUtcMs = createdAt;
        snapshot.updatedAtUtcMs = updatedAt;
        snapshot.exitCode = exitCode;
        snapshot.exitCodeValid = exitFlag != 0;
        snapshot.failureCode = query.value(9).toString();
        if (!validSnapshot(snapshot))
            return Result<QVector<SessionSnapshot>>::failure(storageError());
        sessions.append(snapshot);
    }
    if (query.lastError().isValid())
        return Result<QVector<SessionSnapshot>>::failure(storageError());
    return Result<QVector<SessionSnapshot>>::success(std::move(sessions));
}
OperationResult SqliteSessionRepository::save(const SessionSnapshot &snapshot)
{
    const int state = static_cast<int>(snapshot.status);
    if (!m_ready || !validSnapshot(snapshot) || !m_database.transaction())
        return OperationResult::failure(storageError());
    QSqlQuery query(m_database);
    // 只绑定明确允许的元数据，绝不序列化完整快照或启动参数。
    query.prepare(QStringLiteral("INSERT OR REPLACE INTO sessions "
                                 "(id,name,provider_id,workspace_id,status,created_at_utc_ms,updated_at_utc_"
                                 "ms,exit_code,exit_code_valid,failure_code) VALUES (?,?,?,?,?,?,?,?,?,?)"));
    query.addBindValue(snapshot.id.toString());
    query.addBindValue(snapshot.name.isNull() ? QStringLiteral("") : snapshot.name);
    query.addBindValue(snapshot.providerId.isNull() ? QStringLiteral("") : snapshot.providerId);
    query.addBindValue(snapshot.workspaceId.isNull() ? QStringLiteral("") : snapshot.workspaceId);
    query.addBindValue(state);
    query.addBindValue(snapshot.createdAtUtcMs);
    query.addBindValue(snapshot.updatedAtUtcMs);
    query.addBindValue(snapshot.exitCodeValid ? snapshot.exitCode : 0);
    query.addBindValue(snapshot.exitCodeValid ? 1 : 0);
    query.addBindValue(snapshot.failureCode.isNull() ? QStringLiteral("") : snapshot.failureCode);
    if (!query.exec() || !pruneHistory(m_database) || !m_database.commit())
    {
        m_database.rollback();
        return OperationResult::failure(storageError());
    }
    return accepted();
}
}
