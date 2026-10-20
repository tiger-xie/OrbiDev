#pragma once
#include <QByteArray>
#include <QString>
#include <QStringList>

namespace sshsetup
{
struct ConnectionProfile
{
    QString host;
    QString user;
    QString identityFile;
    int port = 22;
    QString hostFingerprint;
};
struct EnvironmentStatus
{
    bool sshAvailable = false;
    bool tailscaleOnline = false;
    QString details;
};
enum class CheckStatus
{
    NotRun,
    Passed,
    Failed,
    Inconclusive
};
enum class ConnectionIssue
{
    None,
    InvalidProfile,
    ClientUnavailable,
    NetworkUnavailable,
    BannerTimeout,
    BannerClosed,
    TrustPreparationFailed,
    UnknownHost,
    HostKeyChanged,
    AuthenticationFailed,
    ProbeTimeout,
    PowerShellUnavailable,
    ClaudeUnavailable,
    ProbeFailed
};
struct ConnectionResult
{
    bool networkReachable = false;
    bool sshReachable = false;
    bool authenticated = false;
    bool claudeAvailable = false;
    QString details;
    CheckStatus networkStatus = CheckStatus::NotRun;
    CheckStatus sshStatus = CheckStatus::NotRun;
    CheckStatus trustStatus = CheckStatus::NotRun;
    CheckStatus authenticationStatus = CheckStatus::NotRun;
    CheckStatus powershellStatus = CheckStatus::NotRun;
    CheckStatus claudeStatus = CheckStatus::NotRun;
    ConnectionIssue issue = ConnectionIssue::None;
};
// 纯函数，仅根据固定标记分类，不在结果中保留原始输出；调用方提供进程结束与超时状态。
ConnectionResult classifySshProbeResult(const QByteArray &output, bool success, bool timedOut);
// 统一显示与复制的脱敏报告，仅包含枚举对应的固定文案，忽略自由文本 details。
QString formatConnectionReport(const ConnectionResult &result);
// 以下操作同步且有超时，界面须在工作线程调用；不会接受未知主机或修改全局 SSH 配置。
EnvironmentStatus inspectClientEnvironment();
// 同步准备严格校验主机的参数；信任目录由调用方创建并保持到 SSH 进程退出。
// interactive 只允许终端中的认证交互，仍拒绝未知或已变化的主机密钥。
bool prepareSshArguments(const ConnectionProfile &profile, const QString &trustedDirectoryPath,
                         QStringList &arguments, QString &error, bool interactive = false);
ConnectionResult testConnection(const ConnectionProfile &profile);
bool validateProfile(const ConnectionProfile &profile, QString &error);
bool loadProfile(const QString &path, ConnectionProfile &profile, QString &error);
bool saveProfile(const QString &path, const ConnectionProfile &profile, QString &error);
// 只导入公开连接字段，保留本机身份；也可只读导入旧启动器的连接设置。
bool importConnectionInfo(const QString &path, ConnectionProfile &profile, QString &error);
}
