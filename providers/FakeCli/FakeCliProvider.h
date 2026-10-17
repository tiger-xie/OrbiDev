#pragma once
#include <provider/IProviderAdapter.h>

namespace rad
{
// 配置层预先校验固定可执行文件；控制线程仅转换参数，不接受远程路径或命令。
class FakeCliProvider final : public IProviderAdapter
{
public:
    explicit FakeCliProvider(QString executablePath);
    QString id() const override;
    ProviderCapabilities capabilities() const override;
    Result<LaunchSpec> makeLaunchSpec(const SessionRequest &request,
                                      const WorkspaceContext &workspace) const override;

private:
    QString m_executablePath;
};
}
