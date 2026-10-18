#include "FakeCliProvider.h"

#include <QDir>

namespace rad
{
FakeCliProvider::FakeCliProvider(QString executablePath) : m_executablePath(std::move(executablePath))
{
}
QString FakeCliProvider::id() const
{
    return QStringLiteral("fake");
}
ProviderCapabilities FakeCliProvider::capabilities() const
{
    return {};
}
Result<LaunchSpec> FakeCliProvider::makeLaunchSpec(const SessionRequest &request,
                                                   const WorkspaceContext &workspace) const
{
    if (request.providerId != id() || request.workspaceId.isEmpty() ||
        request.workspaceId != workspace.workspaceId || !request.worktreeName.isEmpty() ||
        !request.size.validate().isSuccess() || !QDir::isAbsolutePath(workspace.workingDirectory) ||
        !QDir::isAbsolutePath(m_executablePath))
        return Result<LaunchSpec>::failure({ErrorCode::InvalidArgument, QStringLiteral("FakeCli 请求无效")});
    return Result<LaunchSpec>::success(
        {m_executablePath, {QStringLiteral("--interactive")}, workspace.workingDirectory, request.size});
}
}
