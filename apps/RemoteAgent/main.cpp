#include "model/BuildBaseline.h"
#include "protocol/ProtocolVersion.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <cstdio>

namespace
{
bool verifySqlite()
{
    // 连接与查询在本函数返回时释放；调用方随后移除连接注册。
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("smoke"));
    database.setDatabaseName(QStringLiteral(":memory:"));
    if (!database.open())
    {
        std::fprintf(stderr, "QSQLITE 打开失败：%s\n", database.lastError().text().toUtf8().constData());
        return false;
    }
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("CREATE TABLE smoke (id INTEGER PRIMARY KEY, value TEXT NOT NULL)")))
    {
        std::fprintf(stderr, "SQLite 建表失败：%s\n", query.lastError().text().toUtf8().constData());
        return false;
    }
    if (!query.prepare(QStringLiteral("INSERT INTO smoke (id, value) VALUES (:id, :value)")))
    {
        std::fprintf(stderr, "SQLite 预编译失败：%s\n", query.lastError().text().toUtf8().constData());
        return false;
    }
    const QString expected = QStringLiteral("工程骨架 · SQLite 中文读写");
    query.bindValue(QStringLiteral(":id"), 1);
    query.bindValue(QStringLiteral(":value"), expected);
    if (!query.exec())
    {
        std::fprintf(stderr, "SQLite 写入失败：%s\n", query.lastError().text().toUtf8().constData());
        return false;
    }
    if (!query.exec(QStringLiteral("SELECT value FROM smoke WHERE id = 1")))
    {
        std::fprintf(stderr, "SQLite 查询失败：%s\n", query.lastError().text().toUtf8().constData());
        return false;
    }
    if (!query.next() || query.value(0).toString() != expected || query.next() || query.lastError().isValid())
    {
        std::fprintf(stderr, "SQLite 结果校验失败：%s\n", query.lastError().text().toUtf8().constData());
        return false;
    }
    return true;
}
}

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("RemoteAgent"));
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringLiteral("smoke-test"), QStringLiteral("验证 SQLite 插件及内存库读写")});
    parser.process(application);
    std::fprintf(stdout, "RemoteAgent 工程骨架，未监听网络。%s 协议版本：%d\n",
                 rad::buildBaseline().toUtf8().constData(), rad::protocolVersion());
    if (!parser.isSet(QStringLiteral("smoke-test")))
        return 0;
    const bool passed = verifySqlite();
    QSqlDatabase::removeDatabase(QStringLiteral("smoke"));
    std::fprintf(passed ? stdout : stderr, "%s\n", passed ? "PASS: agent.smoke" : "FAIL: agent.smoke");
    return passed ? 0 : 1;
}
