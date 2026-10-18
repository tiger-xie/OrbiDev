#include "persistence/SqliteSessionRepository.h"
#include <QFile>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>

namespace
{
rad::SessionSnapshot snapshot(rad::SessionStatus status = rad::SessionStatus::Running)
{
    rad::SessionSnapshot value;
    value.id = rad::SessionId::create();
    value.name = QStringLiteral("中文测试");
    value.providerId = QStringLiteral("fake");
    value.workspaceId = QStringLiteral("demo");
    value.status = status;
    value.createdAtUtcMs = 100;
    value.updatedAtUtcMs = 200;
    return value;
}
bool execute(const QString &path, const QStringList &statements)
{
    const QString connection = QUuid::createUuid().toString();
    bool success = true;
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(path);
        success = database.open();
        QSqlQuery query(database);
        for (const auto &statement : statements)
            success = success && query.exec(statement);
    }
    QSqlDatabase::removeDatabase(connection);
    return success;
}
}
class SessionRepositoryTests : public QObject
{
    Q_OBJECT
private slots:
    void restartMarksOnlyActiveSessions();
    void migrationPreservesRows();
    void migrationFailureIsAtomic();
    void recoveryFailureIsAtomic();
    void rejectsFutureAndCorruptDatabase();
    void boundedLoadAndSensitiveFieldsExcluded();
    void rejectsInvalidRowsAndPrunesHistory();
};
void SessionRepositoryTests::restartMarksOnlyActiveSessions()
{
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("sessions.sqlite"));
    QVector<rad::SessionSnapshot> saved;
    {
        rad::SqliteSessionRepository repository;
        QVERIFY(repository.open(path).isSuccess());
        for (const auto state :
             {rad::SessionStatus::Creating, rad::SessionStatus::Running, rad::SessionStatus::Exiting,
              rad::SessionStatus::Exited, rad::SessionStatus::Failed})
        {
            auto value = snapshot(state);
            value.exitCode = 42;
            value.exitCodeValid = state == rad::SessionStatus::Exited;
            if (state == rad::SessionStatus::Failed)
                value.failureCode = QStringLiteral("launch_failed");
            QVERIFY(repository.save(value).isSuccess());
            saved.append(value);
        }
    }
    rad::SqliteSessionRepository repository;
    QVERIFY(repository.open(path).isSuccess());
    QVERIFY(repository.recover(500).isSuccess());
    const auto loaded = repository.load();
    QVERIFY(loaded.isSuccess());
    QCOMPARE(loaded.value().size(), 5);
    for (const auto &value : loaded.value())
    {
        const auto original =
            std::find_if(saved.cbegin(), saved.cend(), [&](const auto &item) { return item.id == value.id; });
        QVERIFY(original != saved.cend());
        if (original->status == rad::SessionStatus::Exited)
        {
            QCOMPARE(value.status, rad::SessionStatus::Exited);
            QVERIFY(value.exitCodeValid);
            QCOMPARE(value.exitCode, 42);
        }
        else if (original->status == rad::SessionStatus::Failed)
            QCOMPARE(value.failureCode, QStringLiteral("launch_failed"));
        else
        {
            QCOMPARE(value.status, rad::SessionStatus::Failed);
            QCOMPARE(value.failureCode, QStringLiteral("agent_restarted"));
            QVERIFY(!value.exitCodeValid);
            QCOMPARE(value.updatedAtUtcMs, qint64(500));
        }
        QVERIFY(value.writerClientId.isEmpty());
        QVERIFY(value.creatorClientId.isEmpty());
        QCOMPARE(value.subscriberCount, 0);
        QCOMPARE(value.outputSequence, quint64(0));
    }
}
void SessionRepositoryTests::migrationPreservesRows()
{
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("migration.sqlite"));
    const auto id = rad::SessionId::create();
    QVERIFY(execute(
        path,
        {QStringLiteral("CREATE TABLE sessions (id TEXT PRIMARY KEY NOT NULL,name TEXT NOT NULL,provider_id "
                        "TEXT NOT NULL,workspace_id TEXT NOT NULL,status INTEGER NOT NULL)"),
         QStringLiteral("INSERT INTO sessions VALUES ('%1','legacy','fake','demo',1)").arg(id.toString()),
         QStringLiteral("PRAGMA user_version=1")}));
    rad::SqliteSessionRepository repository;
    QVERIFY(repository.open(path).isSuccess());
    QVERIFY(repository.recover(700).isSuccess());
    auto loaded = repository.load();
    QVERIFY(loaded.isSuccess());
    QCOMPARE(loaded.value().size(), 1);
    QCOMPARE(loaded.value().first().id, id);
    QCOMPARE(loaded.value().first().name, QStringLiteral("legacy"));
    QCOMPARE(loaded.value().first().failureCode, QStringLiteral("agent_restarted"));
    QCOMPARE(loaded.value().first().createdAtUtcMs, qint64(0));
}
void SessionRepositoryTests::migrationFailureIsAtomic()
{
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("bad-schema.sqlite"));
    QVERIFY(execute(path, {QStringLiteral("CREATE TABLE sessions (id TEXT, updated_at_utc_ms INTEGER)"),
                           QStringLiteral("PRAGMA user_version=1")}));
    rad::SqliteSessionRepository repository;
    QVERIFY(!repository.open(path).isSuccess());
    // 第一个 ALTER 已执行，第二个因同名列失败；回滚后仍可添加第一个列。
    QVERIFY(execute(path, {QStringLiteral("ALTER TABLE sessions ADD COLUMN created_at_utc_ms INTEGER")}));
}
void SessionRepositoryTests::recoveryFailureIsAtomic()
{
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("recovery.sqlite"));
    rad::SqliteSessionRepository repository;
    QVERIFY(repository.open(path).isSuccess());
    QVERIFY(repository.save(snapshot()).isSuccess());
    QVERIFY(repository.save(snapshot()).isSuccess());
    // 故障注入模拟更新中止，整批恢复不能出现半成功。
    QVERIFY(execute(path, {QStringLiteral("CREATE TRIGGER reject_recovery BEFORE UPDATE ON sessions BEGIN "
                                          "SELECT RAISE(ABORT,'private-secret'); END")}));
    const auto recovered = repository.recover(800);
    QVERIFY(!recovered.isSuccess());
    QVERIFY(!recovered.error().message.contains(QStringLiteral("private-secret")));
    auto loaded = repository.load();
    QVERIFY(loaded.isSuccess());
    for (const auto &value : loaded.value())
        QCOMPARE(value.status, rad::SessionStatus::Running);
    auto changed = loaded.value().first();
    changed.name = QStringLiteral("updated");
    // REPLACE 是单条原子写入，另用 INSERT 故障验证旧记录不会丢失。
    QVERIFY(execute(path, {QStringLiteral("CREATE TRIGGER reject_save BEFORE INSERT ON sessions BEGIN SELECT "
                                          "RAISE(ABORT,'private-secret'); END")}));
    QVERIFY(!repository.save(changed).isSuccess());
    loaded = repository.load();
    QVERIFY(loaded.isSuccess());
    for (const auto &value : loaded.value())
        QCOMPARE(value.name, QStringLiteral("中文测试"));
}
void SessionRepositoryTests::rejectsFutureAndCorruptDatabase()
{
    QTemporaryDir directory;
    const auto future = directory.filePath(QStringLiteral("future.sqlite"));
    QVERIFY(execute(future, {QStringLiteral("PRAGMA user_version=99")}));
    rad::SqliteSessionRepository repository;
    QVERIFY(!repository.open(future).isSuccess());
    const auto corrupt = directory.filePath(QStringLiteral("corrupt.sqlite"));
    QFile file(corrupt);
    QVERIFY(file.open(QIODevice::WriteOnly));
    const QByteArray original("not sqlite private-secret");
    QCOMPARE(file.write(original), qint64(original.size()));
    file.close();
    const auto result = repository.open(corrupt);
    QVERIFY(!result.isSuccess());
    QVERIFY(!result.error().message.contains(QStringLiteral("private-secret")));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), original);
}
void SessionRepositoryTests::boundedLoadAndSensitiveFieldsExcluded()
{
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("privacy.sqlite"));
    {
        rad::SqliteSessionRepository repository;
        QVERIFY(repository.open(path).isSuccess());
        for (int index = 0; index < 130; ++index)
        {
            auto value = snapshot();
            value.writerClientId = QStringLiteral("SENSITIVE_WRITER_MARKER");
            value.creatorClientId = QStringLiteral("SENSITIVE_CREATOR_MARKER");
            value.outputSequence = 123456;
            value.subscriberCount = 4;
            value.updatedAtUtcMs = 100 + index;
            QVERIFY(repository.save(value).isSuccess());
        }
        auto value = snapshot();
        value.failureCode = QStringLiteral("secret-token-in-error");
        QVERIFY(!repository.save(value).isSuccess());
        auto loaded = repository.load();
        QVERIFY(loaded.isSuccess());
        QCOMPARE(loaded.value().size(), 128);
        QCOMPARE(loaded.value().first().updatedAtUtcMs, qint64(229));
        QVERIFY(!repository.load(0).isSuccess());
    }
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto bytes = file.readAll();
    QVERIFY(!bytes.contains("SENSITIVE_WRITER_MARKER"));
    QVERIFY(!bytes.contains("SENSITIVE_CREATOR_MARKER"));
    QVERIFY(!bytes.contains("secret-token-in-error"));
    QVERIFY(!bytes.contains("pid"));
}
void SessionRepositoryTests::rejectsInvalidRowsAndPrunesHistory()
{
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("invalid.sqlite"));
    rad::SqliteSessionRepository repository;
    QVERIFY(repository.open(path).isSuccess());
    auto active = snapshot();
    QVERIFY(repository.save(active).isSuccess());
    for (int index = 0; index < 135; ++index)
    {
        auto value = snapshot(rad::SessionStatus::Exited);
        value.updatedAtUtcMs = 300 + index;
        QVERIFY(repository.save(value).isSuccess());
    }
    auto loaded = repository.load(4096);
    QVERIFY(loaded.isSuccess());
    QCOMPARE(loaded.value().size(), 129);
    QVERIFY(std::any_of(loaded.value().cbegin(), loaded.value().cend(),
                        [&](const auto &value) { return value.id == active.id; }));
    auto bad = snapshot();
    bad.name = QString(257, QLatin1Char('x'));
    QVERIFY(!repository.save(bad).isSuccess());
    QVERIFY(execute(path, {QStringLiteral("UPDATE sessions SET created_at_utc_ms='broken' WHERE id='%1'")
                               .arg(active.id.toString())}));
    QVERIFY(!repository.load(4096).isSuccess());
    QVERIFY(execute(path, {QStringLiteral("UPDATE sessions SET created_at_utc_ms=100,name='' WHERE id='%1'")
                               .arg(active.id.toString())}));
    QVERIFY(!repository.load(4096).isSuccess());
    QVERIFY(execute(
        path, {QStringLiteral("UPDATE sessions SET name='active' WHERE id='%1'").arg(active.id.toString())}));
    QVERIFY(repository.recover(10).isSuccess());
    loaded = repository.load(4096);
    QVERIFY(loaded.isSuccess());
    QCOMPARE(loaded.value().size(), 128);
}
QTEST_GUILESS_MAIN(SessionRepositoryTests)
#include "SessionRepositoryTests.moc"
