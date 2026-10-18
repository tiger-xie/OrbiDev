#pragma once

#include <QByteArray>
#include <QMetaType>
#include <QStringList>
#include <QUuid>
#include <QVector>
#include <utility>
#include <variant>

namespace rad
{

enum class ErrorCode
{
    InvalidArgument,
    InvalidState,
    NotFound,
    PermissionDenied,
    ControlUnavailable,
    ProviderUnavailable,
    QueueFull,
    LaunchFailed,
    InputFailed,
    ResizeFailed,
    SystemError
};

struct Error
{
    ErrorCode code = ErrorCode::SystemError;
    QString message; // 必须脱敏，不携带凭据或完整启动环境。
};

// 结果始终且仅包含成功值或错误；读取前必须检查 isSuccess()。
template <typename T> class Result
{
public:
    static Result success(T value)
    {
        return Result(std::move(value));
    }
    static Result failure(Error error)
    {
        return Result(std::move(error));
    }
    bool isSuccess() const
    {
        return std::holds_alternative<T>(m_result);
    }
    const T &value() const
    {
        return std::get<T>(m_result);
    }
    const Error &error() const
    {
        return std::get<Error>(m_result);
    }

private:
    explicit Result(T value) : m_result(std::move(value))
    {
    }
    explicit Result(Error error) : m_result(std::move(error))
    {
    }
    std::variant<T, Error> m_result;
};
using OperationResult = Result<std::monostate>;
inline OperationResult accepted()
{
    return OperationResult::success({});
}

// 默认值无效，仅用于尚未赋值的快照与 Qt 元类型；边界必须拒绝无效 ID。
class SessionId
{
public:
    SessionId() = default;
    static SessionId create();
    static Result<SessionId> parse(const QString &text);
    bool isValid() const
    {
        return !m_uuid.isNull();
    }
    QString toString() const
    {
        return m_uuid.toString(QUuid::WithoutBraces);
    }
    bool operator==(const SessionId &other) const
    {
        return m_uuid == other.m_uuid;
    }
    bool operator!=(const SessionId &other) const
    {
        return !(*this == other);
    }

private:
    explicit SessionId(QUuid uuid) : m_uuid(std::move(uuid))
    {
    }
    QUuid m_uuid;
};

struct TerminalSize
{
    int columns = 120;
    int rows = 32;
    OperationResult validate() const;
};

struct SessionRequest
{
    QString name;
    QString providerId;
    QString workspaceId;
    QString worktreeName;
    TerminalSize size;
};

// 由授权/工作区层解析后提供；普通结构体不是鉴权凭据，Provider 不得自行接受客户端路径。
struct WorkspaceContext
{
    QString workspaceId;
    QString workingDirectory; // 已规范化且在白名单内的绝对路径，包含已解析的 worktree。
};

struct LaunchSpec
{
    QString executablePath; // Provider 探测并确认的受信任绝对路径。
    QStringList arguments;  // 原始未转义逻辑参数；仅由平台启动边界转义一次，不拼接任意 shell 命令。
    QString workingDirectory;
    TerminalSize size;
};

struct ProviderCapabilities
{
    bool interactivePty = true;
    bool nativeBackgroundSession = false;
    bool structuredOutput = false;
    bool canResumeConversation = false;
};

enum class SessionStatus
{
    Creating,
    Running,
    Exiting,
    Exited,
    Failed
};
enum class AttachMode
{
    ReadOnly,
    ReadWrite
};

struct SessionSnapshot
{
    SessionId id;
    QString name;
    QString providerId;
    QString workspaceId;
    SessionStatus status = SessionStatus::Creating;
    int subscriberCount = 0;
    QString creatorClientId;    // 用于启动完成前取消，仍须通过授权校验。
    QString writerClientId;     // 空表示无写者；进程状态不随附加/分离改变。
    quint64 outputSequence = 0; // 累计原始输出末尾偏移，包含已丢弃字节，用于附加时确定回放边界。
};

struct ExitStatus
{
    int exitCode = 0;
    bool exitCodeValid = false; // 启动失败或强制终止时不能伪造正常退出码。
};

struct OutputChunk
{
    QByteArray bytes;         // 保留原始 VT 字节，允许在 UTF-8 字符中间分块。
    quint64 droppedBytes = 0; // 自上次拉取后丢弃量；即使 bytes 为空也须交付。
    bool hasMore = false;
};

// 抽象 QObject 的构造函数自动注册；也可在建立队列连接前显式调用，重复调用安全。
void registerSessionMetaTypes();
}

Q_DECLARE_METATYPE(rad::SessionId)
Q_DECLARE_METATYPE(rad::SessionStatus)
Q_DECLARE_METATYPE(rad::SessionSnapshot)
Q_DECLARE_METATYPE(rad::Error)
Q_DECLARE_METATYPE(rad::ExitStatus)
