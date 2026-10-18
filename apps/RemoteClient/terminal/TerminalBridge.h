#pragma once

#include <QObject>

namespace rad::client
{
// 页面仅能传递字节、尺寸和确认，不获得网络、文件或进程能力。
class TerminalBridge final : public QObject
{
    Q_OBJECT
public:
    explicit TerminalBridge(QObject *parent) : QObject(parent)
    {
    }
    Q_INVOKABLE void loaded()
    {
        emit pageLoaded();
    }
    Q_INVOKABLE void input(const QString &text)
    {
        emit textInput(text);
    }
    Q_INVOKABLE void binaryInput(const QString &base64)
    {
        emit rawInput(base64);
    }
    Q_INVOKABLE void resized(int columns, int rows)
    {
        emit sizeChanged(columns, rows);
    }
    Q_INVOKABLE void acknowledged()
    {
        emit outputAcknowledged();
    }
    Q_INVOKABLE void copy()
    {
        emit copyRequested();
    }
    Q_INVOKABLE void paste()
    {
        emit pasteRequested();
    }

signals:
    void output(QString base64);
    void inputState(bool enabled);
    void pageLoaded();
    void textInput(QString text);
    void rawInput(QString base64);
    void sizeChanged(int columns, int rows);
    void outputAcknowledged();
    void copyRequested();
    void pasteRequested();
};
}
