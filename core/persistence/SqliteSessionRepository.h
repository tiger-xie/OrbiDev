#pragma once
#include "persistence/ISessionRepository.h"
#include <QSqlDatabase>

namespace rad
{
class SqliteSessionRepository final : public ISessionRepository
{
public:
    SqliteSessionRepository();
    ~SqliteSessionRepository() override;
    SqliteSessionRepository(const SqliteSessionRepository &) = delete;
    SqliteSessionRepository &operator=(const SqliteSessionRepository &) = delete;
    OperationResult open(const QString &databasePath) override;
    OperationResult recover(qint64 nowUtcMs) override;
    Result<QVector<SessionSnapshot>> load(int limit = 128) override;
    OperationResult save(const SessionSnapshot &snapshot) override;

private:
    void close();
    OperationResult migrate();
    QString m_connectionName;
    QSqlDatabase m_database;
    bool m_ready = false;
};
}
