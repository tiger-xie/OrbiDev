#pragma once
#include <QWebSocket>
#include <protocol/ProtocolCodec.h>

namespace rad
{
// Qt 5.15.2 满缓存会暂停 TCP 读取，而 WebSocket 必须等完整帧才消费负载。
// 每帧消费后通过公开 API 重新启用读取，避免剩余半帧使缓存永远无法排空。
// 留足最大帧及 WebSocket 头空间；不取消帧/消息限制，也不使用无限缓存。
inline void configureWebSocketReadBuffer(QWebSocket &socket)
{
    constexpr qint64 capacity = protocol::kMaxFrameBytes + 16;
    socket.setReadBufferSize(capacity);
    const auto resume = [&socket]
    {
        socket.setReadBufferSize(capacity + 1);
        socket.setReadBufferSize(capacity);
    };
    QObject::connect(&socket, &QWebSocket::binaryFrameReceived, &socket, resume);
    QObject::connect(&socket, &QWebSocket::textFrameReceived, &socket, resume);
    // 控制帧也会消费缓存：Pong 直接通知，Ping 的自动回复经 bytesWritten 通知。
    QObject::connect(&socket, &QWebSocket::pong, &socket, resume);
    QObject::connect(&socket, &QWebSocket::bytesWritten, &socket, resume);
}
}
