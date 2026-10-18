#include "model/BuildBaseline.h"
#include "protocol/ProtocolVersion.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <Windows.h>
#include <cstdio>

#ifdef RAD_AGENT_SERVER
#include "AgentConfiguration.h"
#include <FakeCli/FakeCliProvider.h>
#include <QDateTime>
#include <QDir>
#include <QLockFile>
#include <QTimer>
#include <WinConPtyBackend.h>
#include <atomic>
#include <diagnostics/DiagnosticLog.h>
#include <persistence/SqliteSessionRepository.h>
#endif

namespace
{
#ifdef RAD_AGENT_SERVER
std::atomic_bool stopRequested{false};
static_assert(std::atomic_bool::is_always_lock_free);
BOOL WINAPI handleConsoleControl(DWORD event)
{
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT)
        return FALSE;
    // 系统回调只置位，Qt 主线程负责停止监听并析构全部会话。
    stopRequested.store(true, std::memory_order_relaxed);
    return TRUE;
}
int runServer(QCoreApplication &application, const QString &configPath)
{
    const auto loaded = rad::loadAgentConfiguration(configPath);
    if (!loaded.isSuccess())
    {
        std::fprintf(stderr, "%s\n", loaded.error().message.toUtf8().constData());
        return 1;
    }
    auto configuration = loaded.value();
    if (!QDir().mkpath(configuration.dataDirectory))
    {
        std::fprintf(stderr, "无法创建 Agent 数据目录\n");
        return 1;
    }
    // 先独占数据目录再迁移，避免第二个 Agent 把仍在运行的会话标记为失效。
    QLockFile dataLock(QDir(configuration.dataDirectory).filePath(QStringLiteral("agent.lock")));
    dataLock.setStaleLockTime(0);
    if (!dataLock.tryLock(0))
    {
        std::fprintf(stderr, "Agent 数据目录被占用或无法加锁\n");
        return 1;
    }
    rad::DiagnosticLog log;
    const auto logOpened = log.open(QDir(configuration.dataDirectory).filePath(QStringLiteral("logs")));
    if (!logOpened.isSuccess())
    {
        std::fprintf(stderr, "%s\n", logOpened.error().message.toUtf8().constData());
        return 1;
    }
    const auto reportStorageFailure = [&log](const rad::Error &error)
    {
        std::fprintf(stderr, "%s\n", error.message.toUtf8().constData());
        const auto logged = log.record(QStringLiteral("storage_error"), {}, error.code);
        if (!logged.isSuccess())
            std::fprintf(stderr, "%s\n", logged.error().message.toUtf8().constData());
    };
    const auto repository = std::make_shared<rad::SqliteSessionRepository>();
    const auto opened =
        repository->open(QDir(configuration.dataDirectory).filePath(QStringLiteral("sessions.sqlite")));
    if (!opened.isSuccess())
    {
        reportStorageFailure(opened.error());
        return 1;
    }
    const auto recovered = repository->recover(QDateTime::currentMSecsSinceEpoch());
    if (!recovered.isSuccess())
    {
        reportStorageFailure(recovered.error());
        return 1;
    }
    configuration.sessions.repository = repository;
    configuration.sessions.providers.push_back(
        std::make_shared<rad::FakeCliProvider>(configuration.fakeCliPath));
    rad::WinConPtyBackend::Options backend;
    for (const auto &workspace : configuration.sessions.workspaces)
        backend.allowedWorkspaceRoots.append(workspace.context.workingDirectory);
    bool diagnosticFailed = false;
    rad::SessionManager manager(std::move(configuration.sessions),
                                [backend]() { return std::make_unique<rad::WinConPtyBackend>(backend); });
    const auto restored = manager.restoreSessions();
    if (!restored.isSuccess())
    {
        reportStorageFailure(restored.error());
        return 1;
    }
    rad::AgentWebSocketServer server(manager, configuration.server);
    const auto record = [&log, &application, &diagnosticFailed](const QString &event, rad::SessionId id = {},
                                                                std::optional<rad::ErrorCode> code = {})
    {
        const auto result = log.record(event, id, code);
        if (!result.isSuccess())
        {
            diagnosticFailed = true;
            std::fprintf(stderr, "%s\n", result.error().message.toUtf8().constData());
            application.exit(1);
        }
    };
    QObject::connect(&server, &rad::AgentWebSocketServer::auditEvent, &application,
                     [record](const QString &event, const QString &) { record(event); });
    QObject::connect(&manager, &rad::ISessionManager::stateChanged, &application,
                     [record](const rad::SessionSnapshot &snapshot)
                     { record(QStringLiteral("session_state_changed"), snapshot.id); });
    QObject::connect(&manager, &rad::ISessionManager::finished, &application,
                     [record](rad::SessionId id, rad::ExitStatus)
                     { record(QStringLiteral("session_finished"), id); });
    QObject::connect(&manager, &rad::ISessionManager::failed, &application,
                     [record](rad::SessionId id, const rad::Error &error, bool)
                     { record(QStringLiteral("session_error"), id, error.code); });
    const auto started = server.start();
    if (!started.isSuccess())
    {
        std::fprintf(stderr, "%s\n", started.error().message.toUtf8().constData());
        return 1;
    }
    if (!SetConsoleCtrlHandler(handleConsoleControl, TRUE))
    {
        std::fprintf(stderr, "无法注册控制台退出处理器\n");
        return 1;
    }
    QTimer stopTimer;
    QObject::connect(&stopTimer, &QTimer::timeout, &application,
                     [&application]()
                     {
                         if (stopRequested.load(std::memory_order_relaxed))
                             application.quit();
                     });
    stopTimer.start(100);
    const auto loggedStart = log.record(QStringLiteral("agent_started"));
    if (!loggedStart.isSuccess())
    {
        std::fprintf(stderr, "%s\n", loggedStart.error().message.toUtf8().constData());
        return 1;
    }
    std::fprintf(stdout, "RemoteAgent 已监听 %s:%u；仅支持 FakeCli 验证。Ctrl+C 停止 Agent 及其会话。\n",
                 configuration.server.bindAddress.toString().toUtf8().constData(),
                 static_cast<unsigned>(server.serverPort()));
    std::fflush(stdout);
    const int result = application.exec();
    server.stop();
    const auto loggedStop = log.record(QStringLiteral("agent_stopped"));
    if (!loggedStop.isSuccess())
    {
        std::fprintf(stderr, "%s\n", loggedStop.error().message.toUtf8().constData());
        return 1;
    }
    if (!SetConsoleCtrlHandler(handleConsoleControl, FALSE))
    {
        std::fprintf(stderr, "无法移除控制台退出处理器\n");
        return 1;
    }
    return diagnosticFailed ? 1 : result;
}
#endif
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
    // 日志按 UTF-8 输出；仅在附有控制台时设置显示编码，重定向仍保留 UTF-8 字节。
    if (GetConsoleOutputCP() != 0 && !SetConsoleOutputCP(CP_UTF8))
    {
        std::fprintf(stderr, "Unable to configure UTF-8 console output: %lu\n", GetLastError());
        return 1;
    }
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("RemoteAgent"));
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringLiteral("smoke-test"), QStringLiteral("验证 SQLite 插件及内存库读写")});
    parser.addOption(
        {QStringLiteral("config"), QStringLiteral("加载本地配置并启动 Agent 服务"), QStringLiteral("json")});
    parser.process(application);
    if (!parser.positionalArguments().isEmpty() ||
        (parser.isSet(QStringLiteral("config")) && parser.isSet(QStringLiteral("smoke-test"))))
    {
        std::fprintf(stderr, "参数无效：--config 与 --smoke-test 不可同时使用\n");
        return 1;
    }
    if (parser.isSet(QStringLiteral("config")))
    {
#ifdef RAD_AGENT_SERVER
        return runServer(application, parser.value(QStringLiteral("config")));
#else
        std::fprintf(stderr, "当前构建未启用 Windows 终端后端，无法启动服务\n");
        return 1;
#endif
    }
    std::fprintf(stdout, "RemoteAgent 工程骨架，未监听网络。%s 协议版本：%d\n",
                 rad::buildBaseline().toUtf8().constData(), rad::protocolVersion());
    if (!parser.isSet(QStringLiteral("smoke-test")))
    {
        std::fprintf(stdout, "使用 --config <json> 启动服务，使用 --help 查看参数。\n");
        return 0;
    }
    const bool passed = verifySqlite();
    QSqlDatabase::removeDatabase(QStringLiteral("smoke"));
    std::fprintf(passed ? stdout : stderr, "%s\n", passed ? "PASS: agent.smoke" : "FAIL: agent.smoke");
    return passed ? 0 : 1;
}
