#pragma once

#include <QByteArray>
#include <QWidget>

class QWebEngineView;
class QWebEngineProfile;
class QTimer;

namespace rad::client
{
class TerminalBridge;

// 仅在 GUI 线程使用；初始化后发 ready，错误发 failed 并永久停止输入输出。
// 输出保留原始字节，最多积压 1 MiB；调用方在失败后分离会话并显示原因。
// 初始禁止输入，调用方取得写权限后显式启用；析构只释放视图，不控制远程进程。
class TerminalView final : public QWidget
{
    Q_OBJECT
public:
    explicit TerminalView(QWidget *parent = nullptr);
    ~TerminalView() override;
    // historyBytes 表示此块的历史前缀，历史写入期间禁止回传查询回复或用户输入。
    void appendOutput(const QByteArray &bytes, int historyBytes = 0);
    void setInputEnabled(bool enabled);
    void focusTerminal();
    void zoomBy(int delta);
    QWebEngineView *webView() const;

signals:
    void inputReady(QByteArray bytes);
    void terminalResized(int columns, int rows);
    void ready();
    void failed(QString reason);

private:
    void flushOutput();
    void fail(const QString &reason);
    void copySelection();
    void pasteClipboard();
    QWebEngineProfile *m_profile = nullptr;
    QWebEngineView *m_view = nullptr;
    TerminalBridge *m_bridge = nullptr;
    QTimer *m_timeout = nullptr;
    QByteArray m_pending;
    int m_inFlight = 0;
    int m_pendingHistoryBytes = 0;
    bool m_replayingBatch = false;
    int m_columns = 80;
    int m_rows = 24;
    bool m_ready = false;
    bool m_failed = false;
    bool m_inputEnabled = false;
};
}
