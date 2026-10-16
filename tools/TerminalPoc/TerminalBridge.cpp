#include "TerminalBridge.h"

#include <QTimer>

TerminalBridge::TerminalBridge(QObject *parent) : QObject(parent)
{
}

void TerminalBridge::send(const QString &text)
{
    ++m_pendingOutput;
    emit output(text);
}

void TerminalBridge::ready()
{
    if (m_ready)
        return;
    m_ready = true;
    send(QStringLiteral("\x1b[36mRADManager · 本地终端验证\x1b[0m\r\n"
                        "输入 help 查看模拟命令；此窗口不执行系统命令。\r\n> "));
    emit terminalReady();
}

void TerminalBridge::input(const QString &text)
{
    if (!m_ready)
        return;
    if (text.size() > 65536)
    {
        emit status(QStringLiteral("输入超过 65536 字符，已拒绝。"));
        return;
    }
    if (text.contains(QChar(3)))
    {
        m_line.clear();
        m_floodRow = -1;
        send(QStringLiteral("^C\r\n[CTRL_C]\r\n> "));
        return;
    }
    if (m_floodRow >= 0)
    {
        emit status(QStringLiteral("高吞吐输出进行中，请按 Ctrl+C 中断后输入。"));
        return;
    }
    if (text == QStringLiteral("\x1b[A") || text == QStringLiteral("\x1bOA"))
    {
        send(QStringLiteral("\r\n[UP]\r\n> ") + m_line);
        return;
    }
    if (text.startsWith(QChar(27)))
    {
        emit status(QStringLiteral("收到控制序列：%1").arg(QString::fromLatin1(text.toUtf8().toHex(' '))));
        return;
    }
    QString echo;
    const auto flushEcho = [this, &echo]
    {
        if (!echo.isEmpty())
        {
            send(echo);
            echo.clear();
        }
    };
    for (const QChar character : text)
    {
        if (character == QChar('\r') || character == QChar('\n'))
        {
            flushEcho();
            send(QStringLiteral("\r\n"));
            executeLine();
            if (m_floodRow >= 0)
            {
                emit status(QStringLiteral("高吞吐输出进行中；后续粘贴命令请在完成后重试，Ctrl+C 可中断。"));
                return;
            }
        }
        else if (character == QChar(127) || character == QChar(8))
        {
            if (!m_line.isEmpty())
            {
                flushEcho();
                const bool surrogatePair = m_line.size() >= 2 &&
                                           m_line.at(m_line.size() - 1).isLowSurrogate() &&
                                           m_line.at(m_line.size() - 2).isHighSurrogate();
                m_line.chop(1);
                if (surrogatePair)
                    m_line.chop(1);
                // 重绘整行，避免宽字符只擦除半格。
                send(QStringLiteral("\r\x1b[2K> ") + m_line);
            }
        }
        else if (character.unicode() >= 32)
        {
            if (m_line.size() >= 8192)
            {
                flushEcho();
                emit status(QStringLiteral("命令行超过 8192 字符，剩余输入已拒绝；Ctrl+C 可清空。"));
                return;
            }
            m_line += character;
            echo += character;
        }
    }
    flushEcho();
}

void TerminalBridge::executeLine()
{
    const QString command = m_line;
    m_line.clear();
    if (command.startsWith(QStringLiteral("echo ")))
        send(QStringLiteral("[ECHO]") + command.mid(5) + QStringLiteral("\r\n"));
    else if (command == QStringLiteral("ansi"))
        send(QStringLiteral("\x1b[31m红色\x1b[0m \x1b[32m绿色\x1b[0m "
                            "\x1b[1;36m加粗青色\x1b[0m\r\n"));
    else if (command == QStringLiteral("clear"))
        send(QStringLiteral("\x1b[2J\x1b[H"));
    else if (command == QStringLiteral("flood"))
    {
        m_floodRow = 0;
        if (m_pendingOutput == 0)
            sendFloodBatch();
        return;
    }
    else if (command == QStringLiteral("help"))
        send(QStringLiteral("echo 文本 | ansi | clear | flood | help\r\n"
                            "Ctrl+C 中断；Ctrl+Shift+C/V 复制粘贴；工具栏调整字号。\r\n"));
    else if (!command.isEmpty())
        send(QStringLiteral("未知模拟命令，请输入 help。\r\n"));
    send(QStringLiteral("> "));
}

void TerminalBridge::ackOutput()
{
    if (m_pendingOutput > 0)
        --m_pendingOutput;
    if (m_pendingOutput == 0 && m_floodRow >= 0)
        QTimer::singleShot(0, this, &TerminalBridge::sendFloodBatch);
}

void TerminalBridge::sendFloodBatch()
{
    if (m_floodRow < 0 || m_pendingOutput != 0)
        return;
    if (m_floodRow >= 24000)
    {
        m_floodRow = -1;
        send(QStringLiteral("[FLOOD_DONE]\r\n> "));
        emit status(QStringLiteral("24,000 行输出已发送，等待终端排空；完成标记后可继续输入。"));
        return;
    }
    QString batch;
    // 约 200 KiB 单批且只允许一批在途，减少跨进程确认开销而不积压整段输出。
    batch.reserve(200 * 1024);
    for (int count = 0; count < 2048 && m_floodRow < 24000; ++count, ++m_floodRow)
        batch += QStringLiteral("[ROW %1] 中文高吞吐输出 0123456789 abcdefghijklmnopqrstuvwxyz "
                                "ABCDEFGHIJKLMNOPQRSTUVWXYZ\r\n")
                     .arg(m_floodRow);
    send(batch);
}

void TerminalBridge::resized(int columns, int rows)
{
    if (columns > 0 && rows > 0 && columns <= 1000 && rows <= 1000)
        emit status(
            QStringLiteral("本地模拟 · %1 列 × %2 行 · Qt %3").arg(columns).arg(rows).arg(QT_VERSION_STR));
}

void TerminalBridge::report(bool passed, const QString &detail)
{
    emit testReported(passed, detail);
}
