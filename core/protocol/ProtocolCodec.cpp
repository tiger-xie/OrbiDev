#include "ProtocolCodec.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QTextCodec>
#include <QUuid>
#include <QtEndian>
#include <cmath>
namespace rad::protocol
{
namespace
{
struct ErrorSpec
{
    const char *code;
    const char *message;
    bool retryable;
};
const ErrorSpec specs[] = {{"invalid_message", "Invalid protocol message", false},
                           {"frame_too_large", "Frame exceeds limit", false},
                           {"unsupported_version", "Unsupported protocol version", false},
                           {"invalid_identifier", "Invalid identifier", false},
                           {"invalid_payload", "Invalid message payload", false},
                           {"session_not_found", "Session not found", false},
                           {"unauthorized", "Authentication required", false},
                           {"forbidden", "Operation not permitted", false},
                           {"busy", "Resource temporarily unavailable", true},
                           {"internal_error", "Operation failed", false}};
const ErrorSpec &spec(ErrorCode code)
{
    const int index = static_cast<int>(code);
    return specs[index >= 0 && index < 10 ? index : 9];
}
bool fail(ErrorCode code, Error &error)
{
    const auto &entry = spec(code);
    error = {code, QString::fromLatin1(entry.message), entry.retryable};
    return false;
}
bool fields(const QJsonObject &object, const QStringList &required, const QStringList &optional = {})
{
    for (const auto &key : required)
        if (!object.contains(key))
            return false;
    for (auto it = object.begin(); it != object.end(); ++it)
        if (!required.contains(it.key()) && !optional.contains(it.key()))
            return false;
    return true;
}
bool integer(const QJsonValue &value, int minimum, int maximum)
{
    if (!value.isDouble())
        return false;
    const double number = value.toDouble();
    return std::isfinite(number) && std::floor(number) == number && number >= minimum && number <= maximum;
}
bool text(const QJsonValue &value, int maximum)
{
    if (!value.isString())
        return false;
    const QString string = value.toString();
    if (string.trimmed().isEmpty() || string.size() > maximum || string.contains(QChar(0)))
        return false;
    for (int index = 0; index < string.size(); ++index)
    {
        if (string[index].isHighSurrogate())
        {
            if (++index >= string.size() || !string[index].isLowSurrogate())
                return false;
        }
        else if (string[index].isLowSurrogate())
            return false;
    }
    return true;
}
bool counter(const QJsonValue &value)
{
    if (!value.isString())
        return false;
    const QString digits = value.toString();
    if (digits.isEmpty() || digits.size() > 20 || (digits.size() > 1 && digits[0] == '0'))
        return false;
    for (const QChar digit : digits)
        if (digit < '0' || digit > '9')
            return false;
    bool ok = false;
    digits.toULongLong(&ok);
    return ok;
}
bool size(const QJsonObject &payload)
{
    return integer(payload["columns"], 1, 32767) && integer(payload["rows"], 1, 32767);
}
bool state(const QJsonValue &value)
{
    return value.isString() &&
           QStringList{"creating", "running", "exiting", "exited", "failed"}.contains(value.toString());
}
bool summary(const QJsonObject &payload)
{
    return fields(payload,
                  {"session_id", "name", "provider_id", "workspace_id", "process_state", "attachable"}) &&
           isCanonicalUuid(payload["session_id"].toString()) && text(payload["name"], 256) &&
           text(payload["provider_id"], 128) && text(payload["workspace_id"], 128) &&
           state(payload["process_state"]) && payload["attachable"].isBool();
}
bool payloadValid(const Envelope &message)
{
    const auto &p = message.payload;
    const auto &type = message.type;
    if (type == "auth.hello")
    {
        if (!fields(p, {"versions"}) || !p["versions"].isArray())
            return false;
        const auto versions = p["versions"].toArray();
        if (versions.isEmpty() || versions.size() > 16)
            return false;
        for (const auto &version : versions)
            if (!integer(version, 1, 255))
                return false;
        return true;
    }
    if (type == "auth.welcome")
        return fields(p, {"selected_version"}) &&
               integer(p["selected_version"], kProtocolVersion, kProtocolVersion);
    if (type == "session.create")
        return fields(p, {"name", "provider_id", "workspace_id", "columns", "rows"}) &&
               text(p["name"], 256) && text(p["provider_id"], 128) && text(p["workspace_id"], 128) && size(p);
    if (type == "session.resize")
        return fields(p, {"columns", "rows"}) && size(p);
    if (type == "session.attach")
        return fields(p, {"mode"}, {"last_ack_sequence"}) &&
               (p["mode"] == QJsonValue("read_only") || p["mode"] == QJsonValue("read_write")) &&
               (!p.contains("last_ack_sequence") || counter(p["last_ack_sequence"]));
    if (type == "session.heartbeat")
        return fields(p, {}, {"last_ack_sequence"}) &&
               (!p.contains("last_ack_sequence") || counter(p["last_ack_sequence"]));
    if (type == "session.created" || type == "session.status")
        return fields(p, {"process_state", "attachable"}) && state(p["process_state"]) &&
               p["attachable"].isBool();
    if (type == "session.attached")
        return fields(p, {"mode"}, {"replay_end_sequence"}) &&
               (p["mode"] == QJsonValue("read_only") || p["mode"] == QJsonValue("read_write")) &&
               (!p.contains("replay_end_sequence") || counter(p["replay_end_sequence"]));
    if (type == "session.listed")
    {
        if (!fields(p, {"sessions"}) || !p["sessions"].isArray())
            return false;
        const auto sessions = p["sessions"].toArray();
        if (sessions.size() > 128)
            return false;
        QSet<QString> ids;
        for (const auto &session : sessions)
        {
            if (!session.isObject() || !summary(session.toObject()))
                return false;
            const auto sessionId = session.toObject()["session_id"].toString();
            if (ids.contains(sessionId))
                return false;
            ids.insert(sessionId);
        }
        return true;
    }
    if (type == "terminal.overrun")
        return fields(p, {"next_sequence", "dropped_bytes"}) && counter(p["next_sequence"]) &&
               counter(p["dropped_bytes"]) && p["dropped_bytes"].toString() != "0";
    if (type == "error")
    {
        if (!fields(p, {"error"}) || !p["error"].isObject())
            return false;
        const auto details = p["error"].toObject();
        if (!fields(details, {"code", "message", "retryable"}))
            return false;
        for (const auto &entry : specs)
            if (details["code"] == QJsonValue(entry.code) &&
                details["message"] == QJsonValue(entry.message) &&
                details["retryable"] == QJsonValue(entry.retryable))
                return true;
        return false;
    }
    return p.isEmpty();
}
bool validate(const Envelope &message, Error &error)
{
    if (message.version != kProtocolVersion)
        return fail(ErrorCode::UnsupportedVersion, error);
    const QStringList global = {"auth.hello", "auth.welcome", "session.create", "session.list",
                                "session.listed"};
    const QStringList scoped = {"session.created",    "session.attach",        "session.attached",
                                "session.detach",     "session.detached",      "session.terminate",
                                "session.terminated", "session.resize",        "session.resized",
                                "session.heartbeat",  "session.heartbeat_ack", "session.status",
                                "terminal.overrun"};
    const bool isError = message.type == "error";
    const bool event = message.type == "session.status" || message.type == "terminal.overrun";
    if (!global.contains(message.type) && !scoped.contains(message.type) && !isError)
        return fail(ErrorCode::InvalidMessage, error);
    if ((event && !message.requestId.isEmpty()) ||
        (!event && !isError && !isCanonicalUuid(message.requestId)) ||
        (!message.requestId.isEmpty() && !isCanonicalUuid(message.requestId)))
        return fail(ErrorCode::InvalidIdentifier, error);
    if ((global.contains(message.type) && !message.sessionId.isEmpty()) ||
        (scoped.contains(message.type) && !isCanonicalUuid(message.sessionId)) ||
        (!message.sessionId.isEmpty() && !isCanonicalUuid(message.sessionId)))
        return fail(ErrorCode::InvalidIdentifier, error);
    if (!payloadValid(message))
        return fail(ErrorCode::InvalidPayload, error);
    return true;
}
QJsonValue identifier(const QString &id)
{
    return id.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(id);
}
bool validFrame(const TerminalFrame &frame, Error &error)
{
    if (frame.version != kProtocolVersion)
        return fail(ErrorCode::UnsupportedVersion, error);
    if (frame.kind != TerminalKind::Input && frame.kind != TerminalKind::Output)
        return fail(ErrorCode::InvalidMessage, error);
    if (!isCanonicalUuid(frame.sessionId))
        return fail(ErrorCode::InvalidIdentifier, error);
    if (frame.bytes.isEmpty())
        return fail(ErrorCode::InvalidPayload, error);
    if (frame.bytes.size() > kMaxFrameBytes - kBinaryHeaderBytes)
        return fail(ErrorCode::FrameTooLarge, error);
    return true;
}
}
bool isCanonicalUuid(const QString &text)
{
    const QUuid uuid(text);
    return text.size() == 36 && !uuid.isNull() && uuid.toString(QUuid::WithoutBraces) == text;
}
bool encodeJson(const Envelope &message, QByteArray &wire, Error &error)
{
    if (!validate(message, error))
        return false;
    const QJsonObject object{{"version", message.version},
                             {"type", message.type},
                             {"request_id", identifier(message.requestId)},
                             {"session_id", identifier(message.sessionId)},
                             {"payload", message.payload}};
    const auto encoded = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (encoded.size() > kMaxFrameBytes)
        return fail(ErrorCode::FrameTooLarge, error);
    wire = encoded;
    return true;
}
bool decodeJson(const QByteArray &wire, Envelope &message, Error &error)
{
    if (wire.size() > kMaxFrameBytes)
        return fail(ErrorCode::FrameTooLarge, error);
    QTextCodec::ConverterState utf8;
    QTextCodec::codecForName("UTF-8")->toUnicode(wire.constData(), wire.size(), &utf8);
    if (utf8.invalidChars || utf8.remainingChars)
        return fail(ErrorCode::InvalidMessage, error);
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(wire, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return fail(ErrorCode::InvalidMessage, error);
    const auto object = document.object();
    if (!fields(object, {"version", "type", "request_id", "session_id", "payload"}) ||
        !integer(object["version"], 0, 255) || !object["type"].isString() || !object["payload"].isObject())
        return fail(ErrorCode::InvalidMessage, error);
    for (const auto &key : {"request_id", "session_id"})
        if (!object[key].isNull() && (!object[key].isString() || object[key].toString().isEmpty()))
            return fail(ErrorCode::InvalidIdentifier, error);
    Envelope decoded{object["version"].toInt(), object["type"].toString(), object["request_id"].toString(),
                     object["session_id"].toString(), object["payload"].toObject()};
    if (!validate(decoded, error))
        return false;
    message = decoded;
    return true;
}
bool encodeBinary(const TerminalFrame &frame, QByteArray &wire, Error &error)
{
    if (!validFrame(frame, error))
        return false;
    QByteArray encoded(kBinaryHeaderBytes, 0);
    encoded.replace(0, 4, "RADP", 4);
    encoded[4] = static_cast<char>(kProtocolVersion);
    encoded[5] = static_cast<char>(frame.kind);
    encoded.replace(6, 16, QUuid(frame.sessionId).toRfc4122());
    qToBigEndian<quint64>(frame.sequence, encoded.data() + 22);
    qToBigEndian<quint32>(static_cast<quint32>(frame.bytes.size()), encoded.data() + 30);
    encoded.append(frame.bytes);
    wire = encoded;
    return true;
}
bool decodeBinary(const QByteArray &wire, TerminalFrame &frame, Error &error)
{
    if (wire.size() > kMaxFrameBytes)
        return fail(ErrorCode::FrameTooLarge, error);
    if (wire.size() < kBinaryHeaderBytes || wire.left(4) != "RADP")
        return fail(ErrorCode::InvalidMessage, error);
    const auto length = qFromBigEndian<quint32>(wire.constData() + 30);
    if (length != static_cast<quint32>(wire.size() - kBinaryHeaderBytes))
        return fail(ErrorCode::InvalidMessage, error);
    TerminalFrame decoded{static_cast<quint8>(wire[4]),
                          static_cast<TerminalKind>(static_cast<quint8>(wire[5])),
                          QUuid::fromRfc4122(wire.mid(6, 16)).toString(QUuid::WithoutBraces),
                          qFromBigEndian<quint64>(wire.constData() + 22), wire.mid(kBinaryHeaderBytes)};
    if (!validFrame(decoded, error))
        return false;
    frame = decoded;
    return true;
}
bool negotiateVersion(const QVector<int> &offered, int &selected, Error &error)
{
    if (offered.isEmpty() || offered.size() > 16)
        return fail(ErrorCode::InvalidPayload, error);
    for (int version : offered)
        if (version < 1 || version > 255)
            return fail(ErrorCode::InvalidPayload, error);
    if (!offered.contains(kProtocolVersion))
        return fail(ErrorCode::UnsupportedVersion, error);
    selected = kProtocolVersion;
    return true;
}
bool validateSession(const QString &sessionId, const QSet<QString> &knownSessions, Error &error)
{
    if (!isCanonicalUuid(sessionId))
        return fail(ErrorCode::InvalidIdentifier, error);
    if (!knownSessions.contains(sessionId))
        return fail(ErrorCode::SessionNotFound, error);
    return true;
}
Envelope makeErrorEnvelope(ErrorCode code, const QString &requestId, const QString &sessionId)
{
    const auto &entry = spec(code);
    return {
        kProtocolVersion,
        QStringLiteral("error"),
        requestId,
        sessionId,
        {{"error",
          QJsonObject{{"code", entry.code}, {"message", entry.message}, {"retryable", entry.retryable}}}}};
}
}
