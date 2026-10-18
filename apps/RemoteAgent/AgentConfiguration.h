#pragma once
#include <session/SessionManager.h>
#include <transport/AgentWebSocketServer.h>

namespace rad
{
struct AgentConfiguration
{
    AgentWebSocketServer::Options server;
    SessionManager::Options sessions;
    QString fakeCliPath;
    QString dataDirectory;
};
// 启动阶段同步读取最多 64 KiB 的本地配置；失败返回固定安全错误，不回显内容。
Result<AgentConfiguration> loadAgentConfiguration(const QString &path);
}
