#pragma once
#include "ProtocolVersion.h"

#include <QByteArray>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QVector>
namespace rad::protocol
{
constexpr int kMaxFrameBytes = 64 * 1024;
constexpr int kBinaryHeaderBytes = 34;
enum class ErrorCode
{
    InvalidMessage,
    FrameTooLarge,
    UnsupportedVersion,
    InvalidIdentifier,
    InvalidPayload,
    SessionNotFound,
    Unauthorized,
    Forbidden,
    Busy,
    InternalError
};
struct Error
{
    ErrorCode code = ErrorCode::InvalidMessage;
    QString message;
    bool retryable = false;
};
struct Envelope
{
    int version = kProtocolVersion;
    QString type;
    QString requestId;
    QString sessionId;
    QJsonObject payload;
};
enum class TerminalKind : quint8
{
    Input = 1,
    Output = 2
};
struct TerminalFrame
{
    int version = kProtocolVersion;
    TerminalKind kind = TerminalKind::Input;
    QString sessionId;
    quint64 sequence = 0;
    QByteArray bytes;
};
// 同步无状态编解码，可在不同线程用独立值调用；失败不修改结果，仅填写固定安全错误。
// 空 ID 在 JSON 中编码为 null；只有各消息允许的空 ID 才能通过校验。
bool encodeJson(const Envelope &message, QByteArray &wire, Error &error);
bool decodeJson(const QByteArray &wire, Envelope &message, Error &error);
bool encodeBinary(const TerminalFrame &frame, QByteArray &wire, Error &error);
bool decodeBinary(const QByteArray &wire, TerminalFrame &frame, Error &error);
bool isCanonicalUuid(const QString &text);
// 只协商协议，不代表认证成功；版本列表最多 16 项，每项 1..255。
bool negotiateVersion(const QVector<int> &offered, int &selected, Error &error);
// 存在性检查与结构解析分开，调用方仍必须独立完成身份和会话权限校验。
bool validateSession(const QString &sessionId, const QSet<QString> &knownSessions, Error &error);
// 仅从枚举生成固定公开错误，不接受系统错误或凭据字符串。
Envelope makeErrorEnvelope(ErrorCode code, const QString &requestId = {}, const QString &sessionId = {});
}
