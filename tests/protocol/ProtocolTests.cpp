#include "protocol/ProtocolCodec.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QtTest>
#include <limits>
using namespace rad::protocol;
namespace
{
const QString id = QStringLiteral("12345678-1234-4321-9876-123456789abc");
Envelope resizeMessage()
{
    return {1, QStringLiteral("session.resize"), id, id, {{"columns", 120}, {"rows", 32}}};
}
}
class ProtocolTests : public QObject
{
    Q_OBJECT
private slots:
    void replayBoundary()
    {
        Error error;
        QByteArray wire;
        Envelope decoded;
        Envelope message{1, "session.attached", id, id, {{"mode", "read_only"}}};
        QVERIFY(encodeJson(message, wire, error));
        for (const auto &value : {QStringLiteral("0"), QStringLiteral("9007199254740993"),
                                  QStringLiteral("18446744073709551615")})
        {
            message.payload["replay_end_sequence"] = value;
            QVERIFY(encodeJson(message, wire, error));
            QVERIFY(decodeJson(wire, decoded, error));
            QCOMPARE(decoded.payload["replay_end_sequence"].toString(), value);
        }
        const QList<QJsonValue> invalid = {0,
                                           true,
                                           QJsonValue(QJsonValue::Null),
                                           QJsonArray{},
                                           QStringLiteral(""),
                                           QStringLiteral("01"),
                                           QStringLiteral("-1"),
                                           QStringLiteral("+1"),
                                           QStringLiteral("1.0"),
                                           QStringLiteral(" 1"),
                                           QStringLiteral("18446744073709551616")};
        for (const auto &value : invalid)
        {
            message.payload["replay_end_sequence"] = value;
            QVERIFY(!encodeJson(message, wire, error));
            const QJsonObject raw{{"version", 1},
                                  {"type", "session.attached"},
                                  {"request_id", id},
                                  {"session_id", id},
                                  {"payload", message.payload}};
            QVERIFY(!decodeJson(QJsonDocument(raw).toJson(QJsonDocument::Compact), decoded, error));
        }
    }
    void jsonRoundTrip()
    {
        Error error;
        QByteArray wire;
        Envelope decoded;
        const auto message = resizeMessage();
        QVERIFY(encodeJson(message, wire, error));
        QVERIFY(decodeJson(wire, decoded, error));
        QCOMPARE(decoded.type, message.type);
        QCOMPARE(decoded.payload, message.payload);
    }
    void jsonInvalid()
    {
        Error error;
        Envelope decoded;
        QByteArray wire;
        auto message = resizeMessage();
        message.version = 2;
        QVERIFY(!encodeJson(message, wire, error));
        message = resizeMessage();
        message.sessionId = id.toUpper();
        QVERIFY(!encodeJson(message, wire, error));
        message = resizeMessage();
        message.payload["rows"] = 0;
        QVERIFY(!encodeJson(message, wire, error));
        QVERIFY(!decodeJson("[]", decoded, error));
        QVERIFY(!decodeJson(QByteArray(kMaxFrameBytes + 1, ' '), decoded, error));
        QVERIFY(!decodeJson("{\"version\":1}", decoded, error));
    }
    void binaryRoundTrip()
    {
        Error error;
        TerminalFrame frame{1, TerminalKind::Output, id, std::numeric_limits<quint64>::max(),
                            QByteArray("\0\x1b[31m", 6)};
        QByteArray wire;
        TerminalFrame decoded;
        QVERIFY(encodeBinary(frame, wire, error));
        QCOMPARE(wire.size(), kBinaryHeaderBytes + frame.bytes.size());
        QVERIFY(decodeBinary(wire, decoded, error));
        QCOMPARE(decoded.sequence, frame.sequence);
        QCOMPARE(decoded.bytes, frame.bytes);
        QCOMPARE(decoded.sessionId, id);
        frame.bytes = QByteArray(kMaxFrameBytes - kBinaryHeaderBytes, 'x');
        QVERIFY(encodeBinary(frame, wire, error));
        QVERIFY(decodeBinary(wire, decoded, error));
        frame.bytes.append('x');
        QVERIFY(!encodeBinary(frame, wire, error));
    }
    void binaryInvalid()
    {
        Error error;
        QByteArray wire;
        TerminalFrame decoded;
        TerminalFrame frame{1, TerminalKind::Input, id, 0, "x"};
        QVERIFY(encodeBinary(frame, wire, error));
        for (int size = 0; size < kBinaryHeaderBytes; ++size)
            QVERIFY(!decodeBinary(wire.left(size), decoded, error));
        auto broken = wire;
        broken[0] = 'X';
        QVERIFY(!decodeBinary(broken, decoded, error));
        broken = wire;
        broken[4] = 2;
        QVERIFY(!decodeBinary(broken, decoded, error));
        broken = wire;
        broken[5] = 3;
        QVERIFY(!decodeBinary(broken, decoded, error));
        broken = wire;
        broken[33] = 2;
        QVERIFY(!decodeBinary(broken, decoded, error));
        broken = wire;
        broken.replace(6, 16, QByteArray(16, 0));
        QVERIFY(!decodeBinary(broken, decoded, error));
        QVERIFY(!decodeBinary(wire + 'x', decoded, error));
    }
    void versionsAndSessions()
    {
        Error error;
        int version = 0;
        QVERIFY(negotiateVersion({2, 1}, version, error));
        QCOMPARE(version, 1);
        QVERIFY(!negotiateVersion({2}, version, error));
        QVERIFY(validateSession(id, QSet<QString>{id}, error));
        QVERIFY(!validateSession(id, {}, error));
        QCOMPARE(error.code, ErrorCode::SessionNotFound);
        QVERIFY(!validateSession("bad", {}, error));
        QCOMPARE(error.code, ErrorCode::InvalidIdentifier);
    }
    void safeError()
    {
        Error error;
        QByteArray wire;
        Envelope decoded;
        const auto message = makeErrorEnvelope(ErrorCode::Unauthorized, id);
        QVERIFY(encodeJson(message, wire, error));
        QVERIFY(decodeJson(wire, decoded, error));
        QCOMPARE(decoded.payload, message.payload);
        auto unsafe = message;
        auto details = unsafe.payload["error"].toObject();
        details["message"] = "secret";
        unsafe.payload["error"] = details;
        QVERIFY(!encodeJson(unsafe, wire, error));
    }

    void goldenAndFailureAtomicity()
    {
        Error error;
        QByteArray wire;
        TerminalFrame frame{1, TerminalKind::Input, id, 0x0102030405060708ULL, "x"};
        QVERIFY(encodeBinary(frame, wire, error));
        QCOMPARE(wire.toHex(),
                 QByteArray("52414450010112345678123443219876123456789abc01020304050607080000000178"));
        auto broken = wire;
        broken.replace(30, 4, QByteArray::fromHex("ffffffff"));
        TerminalFrame decoded = frame;
        QVERIFY(!decodeBinary(broken, decoded, error));
        QCOMPARE(decoded.bytes, frame.bytes);
        QCOMPARE(decoded.sequence, frame.sequence);
        QByteArray unchanged = "sentinel";
        frame.bytes.clear();
        QVERIFY(!encodeBinary(frame, unchanged, error));
        QCOMPARE(unchanged, QByteArray("sentinel"));
        Envelope envelope = resizeMessage();
        QVERIFY(!decodeJson("{}", envelope, error));
        QCOMPARE(envelope.type, QString("session.resize"));
        auto invalid = resizeMessage();
        invalid.type = "unknown";
        QVERIFY(!encodeJson(invalid, unchanged, error));
        QCOMPARE(unchanged, QByteArray("sentinel"));
        int version = 17;
        QVERIFY(!negotiateVersion({}, version, error));
        QCOMPARE(version, 17);
        QVERIFY(!negotiateVersion({1, 256}, version, error));
        QCOMPARE(version, 17);
    }
    void jsonBoundaryValidation()
    {
        Error error;
        QByteArray wire;
        QVERIFY(encodeJson(resizeMessage(), wire, error));
        const auto valid = QJsonDocument::fromJson(wire).object();
        Envelope envelope;
        for (const auto &key : {"version", "type", "request_id", "session_id", "payload"})
        {
            auto object = valid;
            object.remove(key);
            QVERIFY(!decodeJson(QJsonDocument(object).toJson(), envelope, error));
        }
        auto object = valid;
        object["unknown"] = true;
        QVERIFY(!decodeJson(QJsonDocument(object).toJson(), envelope, error));
        object = valid;
        object["request_id"] = QJsonValue::Null;
        QVERIFY(!decodeJson(QJsonDocument(object).toJson(), envelope, error));
        object = valid;
        object["session_id"] = "00000000-0000-0000-0000-000000000000";
        QVERIFY(!decodeJson(QJsonDocument(object).toJson(), envelope, error));
        auto invalid = resizeMessage();
        invalid.payload["unknown"] = true;
        QVERIFY(!encodeJson(invalid, wire, error));
        invalid = resizeMessage();
        invalid.payload["rows"] = 1.5;
        QVERIFY(!encodeJson(invalid, wire, error));
        QByteArray badUtf8 = "{\"x\":\"";
        badUtf8.append(static_cast<char>(-1));
        badUtf8.append("\"}");
        QVERIFY(!decodeJson(badUtf8, envelope, error));
        QVERIFY(!decodeJson(QByteArray::fromHex("e282"), envelope, error));
        for (const auto &digits : {"9007199254740993", "18446744073709551615"})
        {
            Envelope message{1, "session.heartbeat", id, id, {{"last_ack_sequence", digits}}};
            QVERIFY(encodeJson(message, wire, error));
            QVERIFY(decodeJson(wire, envelope, error));
            QCOMPARE(envelope.payload, message.payload);
        }
        for (const auto &digits : {"18446744073709551616", "01", "-1", "1.5", ""})
        {
            Envelope message{1, "session.heartbeat", id, id, {{"last_ack_sequence", digits}}};
            QVERIFY(!encodeJson(message, wire, error));
        }
    }
    void allMessageKinds()
    {
        QVector<Envelope> messages = {
            {1, "auth.hello", id, {}, {{"versions", QJsonArray{1, 2}}}},
            {1, "auth.welcome", id, {}, {{"selected_version", 1}}},
            {1,
             "session.create",
             id,
             {},
             {{"name", "中文"},
              {"provider_id", "fake"},
              {"workspace_id", "test"},
              {"columns", 80},
              {"rows", 24}}},
            {1, "session.list", id, {}, {}},
            {1, "session.listed", id, {}, {{"sessions", QJsonArray{}}}},
            {1, "session.created", id, id, {{"process_state", "creating"}, {"attachable", false}}},
            {1, "session.attach", id, id, {{"mode", "read_write"}}},
            {1, "session.attached", id, id, {{"mode", "read_only"}}},
            {1, "session.status", {}, id, {{"process_state", "running"}, {"attachable", true}}},
            {1,
             "terminal.overrun",
             {},
             id,
             {{"next_sequence", "18446744073709551615"}, {"dropped_bytes", "1"}}},
            makeErrorEnvelope(ErrorCode::InternalError),
            resizeMessage()};
        for (const auto &type :
             {"session.detach", "session.detached", "session.terminate", "session.terminated",
              "session.resized", "session.heartbeat", "session.heartbeat_ack"})
            messages.append({1, type, id, id, {}});
        for (const auto &message : messages)
        {
            Error error;
            QByteArray wire;
            Envelope decoded;
            QVERIFY2(encodeJson(message, wire, error), qPrintable(message.type));
            QVERIFY(decodeJson(wire, decoded, error));
            QCOMPARE(decoded.payload, message.payload);
            QCOMPARE(decoded.requestId, message.requestId);
            QCOMPARE(decoded.sessionId, message.sessionId);
        }
    }
    void unicodeAndListValidation()
    {
        Error error;
        QByteArray wire;
        Envelope decoded;
        Envelope create{1,
                        "session.create",
                        id,
                        {},
                        {{"name", QString(QChar(0xd800))},
                         {"provider_id", "fake"},
                         {"workspace_id", "test"},
                         {"columns", 80},
                         {"rows", 24}}};
        QVERIFY(!encodeJson(create, wire, error));
        create.payload["name"] = QString::fromUtf8("中文😀");
        QVERIFY(encodeJson(create, wire, error));
        QVERIFY(decodeJson(wire, decoded, error));
        QCOMPARE(decoded.payload, create.payload);
        wire.replace(QString::fromUtf8("中文😀").toUtf8(), "\\ud800");
        QVERIFY(!decodeJson(wire, decoded, error));
        QJsonObject summary{{"session_id", id},           {"name", "test"},
                            {"provider_id", "fake"},      {"workspace_id", "test"},
                            {"process_state", "running"}, {"attachable", true}};
        Envelope listed{1, "session.listed", id, {}, {{"sessions", QJsonArray{summary}}}};
        QVERIFY(encodeJson(listed, wire, error));
        QVERIFY(decodeJson(wire, decoded, error));
        QCOMPARE(decoded.payload, listed.payload);
        listed.payload["sessions"] = QJsonArray{summary, summary};
        QVERIFY(!encodeJson(listed, wire, error));
        summary["attachable"] = "yes";
        listed.payload["sessions"] = QJsonArray{summary};
        QVERIFY(!encodeJson(listed, wire, error));
    }
    void mutationInputs()
    {
        QRandomGenerator random(12345);
        Error error;
        QByteArray binary, json;
        QVERIFY(encodeBinary({1, TerminalKind::Output, id, 42, "hello"}, binary, error));
        QVERIFY(encodeJson(resizeMessage(), json, error));
        for (int i = 0; i < 3000; ++i)
        {
            auto wire = (i % 2) ? binary : json;
            for (int j = 0; j < 1 + i % 8; ++j)
                wire[static_cast<int>(random.bounded(static_cast<quint32>(wire.size())))] =
                    static_cast<char>(random.bounded(256u));
            if (i % 3 == 0)
                wire.truncate(static_cast<int>(random.bounded(static_cast<quint32>(wire.size() + 1))));
            TerminalFrame frame;
            Envelope envelope;
            decodeBinary(wire, frame, error);
            decodeJson(wire, envelope, error);
        }
    }
};
QTEST_GUILESS_MAIN(ProtocolTests)
#include "ProtocolTests.moc"
