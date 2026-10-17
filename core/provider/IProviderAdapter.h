#pragma once
#include <model/SessionTypes.h>

namespace rad
{
// 调用方独占或共享拥有，不接管任何参数；仅在 Agent 控制线程调用。
// 路径探测须在实例构造前由工作线程完成；本接口只做非阻塞参数转换，不启动进程。
class IProviderAdapter
{
public:
    virtual ~IProviderAdapter() = default;
    virtual QString id() const = 0;
    virtual ProviderCapabilities capabilities() const = 0;
    // request.providerId/workspaceId 必须与本 Provider/可信上下文匹配，否则明确拒绝。
    // workspace 由授权层解析，方法不自行解释客户端路径，也不执行 shell 命令。
    virtual Result<LaunchSpec> makeLaunchSpec(const SessionRequest &request,
                                              const WorkspaceContext &workspace) const = 0;
};
}
