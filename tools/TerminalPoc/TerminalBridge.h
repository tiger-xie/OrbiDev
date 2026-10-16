#pragma once

#include <QObject>
#include <QString>

// 仅供 M0 本地验证；全部调用位于 GUI 线程，不启动进程或执行 shell 命令。
class TerminalBridge final : public QObject
{
    Q_OBJECT
public:
    explicit TerminalBridge(QObject *parent = nullptr);

public slots:
    void ready();
    void input(const QString &text);
    void resized(int columns, int rows);
    void report(bool passed, const QString &detail);
    void ackOutput();

signals:
    void output(const QString &text);
    void status(const QString &text);
    void terminalReady();
    void testReported(bool passed, const QString &detail);

private:
    void send(const QString &text);
    void executeLine();
    void sendFloodBatch();

    QString m_line;
    int m_pendingOutput = 0;
    int m_floodRow = -1;
    bool m_ready = false;
};
