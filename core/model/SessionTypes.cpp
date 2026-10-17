#include "SessionTypes.h"

namespace rad
{
SessionId SessionId::create()
{
    return SessionId(QUuid::createUuid());
}

Result<SessionId> SessionId::parse(const QString &text)
{
    const QUuid uuid(text);
    // 仅接受无大括号的完整 UUID；避免宽松解析导致多个协议表示。
    if (uuid.isNull() || text.size() != 36 || uuid.toString(QUuid::WithoutBraces) != text.toLower())
    {
        return Result<SessionId>::failure(
            {ErrorCode::InvalidArgument, QStringLiteral("会话 ID 必须是非空 UUID")});
    }
    return Result<SessionId>::success(SessionId(uuid));
}

OperationResult TerminalSize::validate() const
{
    constexpr int maximumDimension = 32767;
    if (columns < 1 || rows < 1 || columns > maximumDimension || rows > maximumDimension)
    {
        return OperationResult::failure(
            {ErrorCode::InvalidArgument, QStringLiteral("终端列数和行数必须在 1 至 32767 之间")});
    }
    return accepted();
}

void registerSessionMetaTypes()
{
    qRegisterMetaType<SessionId>("rad::SessionId");
    qRegisterMetaType<SessionStatus>("rad::SessionStatus");
    qRegisterMetaType<SessionSnapshot>("rad::SessionSnapshot");
    qRegisterMetaType<Error>("rad::Error");
    qRegisterMetaType<ExitStatus>("rad::ExitStatus");
}
}
