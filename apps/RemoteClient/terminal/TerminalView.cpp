#include "TerminalView.h"
#include "TerminalBridge.h"

#include <QApplication>
#include <QClipboard>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QTimer>
#include <QVBoxLayout>
#include <QWebChannel>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineView>

// 静态库需要显式引用资源，防止链接器移除资源对象。
static void initializeTerminalResources()
{
    Q_INIT_RESOURCE(client_terminal);
}

namespace rad::client
{
namespace
{
constexpr int maxPendingBytes = 1024 * 1024;
constexpr int maxBatchBytes = 64 * 1024;
const QUrl terminalUrl(QStringLiteral("qrc:/client-terminal/client-index.html"));

class OfflineRequests final : public QWebEngineUrlRequestInterceptor
{
public:
    explicit OfflineRequests(QObject *parent) : QWebEngineUrlRequestInterceptor(parent)
    {
    }
    void interceptRequest(QWebEngineUrlRequestInfo &request) override
    {
        const QUrl url = request.requestUrl();
        request.block(url.scheme() != QStringLiteral("qrc") ||
                      (!url.path().startsWith(QStringLiteral("/client-terminal/")) &&
                       url.path() != QStringLiteral("/qtwebchannel/qwebchannel.js")));
    }
};

class OfflinePage final : public QWebEnginePage
{
public:
    OfflinePage(QWebEngineProfile *profile, QObject *parent) : QWebEnginePage(profile, parent)
    {
    }

protected:
    bool acceptNavigationRequest(const QUrl &url, NavigationType, bool isMainFrame) override
    {
        return isMainFrame && url == terminalUrl;
    }
    QWebEnginePage *createWindow(WebWindowType) override
    {
        return nullptr;
    }
    // 不输出页面内容，避免错误消息包含终端数据。
    void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel, const QString &, int,
                                  const QString &) override
    {
    }
};
}

TerminalView::TerminalView(QWidget *parent) : QWidget(parent)
{
    initializeTerminalResources();
    m_profile = new QWebEngineProfile(this);
    m_profile->setUrlRequestInterceptor(new OfflineRequests(m_profile));
    m_view = new QWebEngineView(this);
    auto *page = new OfflinePage(m_profile, m_view);
    m_view->setPage(page);
    m_view->setContextMenuPolicy(Qt::NoContextMenu);
    setFocusProxy(m_view);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_view);
    auto *settings = page->settings();
    settings->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
    settings->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, false);
    settings->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, false);
    settings->setAttribute(QWebEngineSettings::JavascriptCanAccessClipboard, false);
    settings->setAttribute(QWebEngineSettings::FullScreenSupportEnabled, false);
    m_bridge = new TerminalBridge(page);
    auto *channel = new QWebChannel(page);
    channel->registerObject(QStringLiteral("bridge"), m_bridge);
    page->setWebChannel(channel);
    m_timeout = new QTimer(this);
    m_timeout->setSingleShot(true);
    connect(m_timeout, &QTimer::timeout, this,
            [this] { fail(QStringLiteral("终端加载或输出确认超时，请重新附加会话。")); });
    connect(page, &QWebEnginePage::loadFinished, this,
            [this](bool success)
            {
                // 被拒绝的导航不会替换当前可信文档。
                if (!success && !m_ready)
                    fail(QStringLiteral("离线终端资源加载失败。"));
            });
    connect(page, &QWebEnginePage::renderProcessTerminated, this,
            [this](QWebEnginePage::RenderProcessTerminationStatus, int)
            { fail(QStringLiteral("终端渲染进程已退出，请重新附加会话。")); });
    connect(m_bridge, &TerminalBridge::pageLoaded, this,
            [this]
            {
                if (m_failed || m_ready)
                    return;
                m_ready = true;
                m_timeout->stop();
                emit m_bridge->inputState(m_inputEnabled);
                emit ready();
                if (m_inputEnabled)
                    emit terminalResized(m_columns, m_rows);
                flushOutput();
            });
    connect(m_bridge, &TerminalBridge::textInput, this,
            [this](const QString &text)
            {
                if (!m_ready || m_failed || !m_inputEnabled)
                    return;
                const QByteArray bytes = text.toUtf8();
                if (bytes.size() > maxBatchBytes)
                    fail(QStringLiteral("终端输入超过 64 KiB，已停止输入。"));
                else if (!bytes.isEmpty())
                    emit inputReady(bytes);
            });
    connect(m_bridge, &TerminalBridge::rawInput, this,
            [this](const QString &base64)
            {
                if (!m_ready || m_failed || !m_inputEnabled)
                    return;
                const QByteArray bytes = QByteArray::fromBase64(base64.toLatin1());
                if (bytes.size() > maxBatchBytes)
                    fail(QStringLiteral("终端输入超过 64 KiB，已停止输入。"));
                else if (!bytes.isEmpty())
                    emit inputReady(bytes);
            });
    connect(m_bridge, &TerminalBridge::sizeChanged, this,
            [this](int columns, int rows)
            {
                if (columns < 2 || columns > 1000 || rows < 1 || rows > 1000)
                    return;
                m_columns = columns;
                m_rows = rows;
                if (m_ready && !m_failed && m_inputEnabled)
                    emit terminalResized(columns, rows);
            });
    connect(m_bridge, &TerminalBridge::outputAcknowledged, this,
            [this]
            {
                if (!m_ready || m_failed || m_inFlight == 0)
                    return;
                m_inFlight = 0;
                m_timeout->stop();
                flushOutput();
            });
    connect(m_bridge, &TerminalBridge::copyRequested, this, &TerminalView::copySelection);
    connect(m_bridge, &TerminalBridge::pasteRequested, this, &TerminalView::pasteClipboard);
    m_timeout->start(20000);
    m_view->load(terminalUrl);
}

TerminalView::~TerminalView()
{
    // 页面和通道必须先于 profile 销毁；页面回调只捕获受保护指针。
    delete m_view;
    delete m_profile;
}

void TerminalView::appendOutput(const QByteArray &bytes)
{
    if (m_failed || bytes.isEmpty())
        return;
    if (bytes.size() > maxPendingBytes - m_pending.size() - m_inFlight)
    {
        fail(QStringLiteral("终端显示积压超过 1 MiB，输出已停止，请重新附加会话。"));
        return;
    }
    m_pending.append(bytes);
    flushOutput();
}

void TerminalView::flushOutput()
{
    if (!m_ready || m_failed || m_inFlight != 0 || m_pending.isEmpty())
        return;
    const QByteArray batch = m_pending.left(maxBatchBytes);
    m_pending.remove(0, batch.size());
    m_inFlight = batch.size();
    m_timeout->start(20000);
    emit m_bridge->output(QString::fromLatin1(batch.toBase64()));
}

void TerminalView::setInputEnabled(bool enabled)
{
    const bool wasEnabled = m_inputEnabled;
    m_inputEnabled = enabled && !m_failed;
    if (!m_ready || m_failed)
        return;
    emit m_bridge->inputState(m_inputEnabled);
    if (m_inputEnabled && !wasEnabled)
        emit terminalResized(m_columns, m_rows);
}

void TerminalView::focusTerminal()
{
    m_view->setFocus(Qt::OtherFocusReason);
    if (m_ready && !m_failed)
        m_view->page()->runJavaScript(QStringLiteral("clientTerminal.term.focus()"));
}

void TerminalView::zoomBy(int delta)
{
    if (m_ready && !m_failed)
        m_view->page()->runJavaScript(QStringLiteral("clientTerminal.zoom(%1)").arg(qBound(-10, delta, 10)));
}

QWebEngineView *TerminalView::webView() const
{
    return m_view;
}

void TerminalView::copySelection()
{
    if (!m_ready || m_failed)
        return;
    const QPointer<TerminalView> guard(this);
    m_view->page()->runJavaScript(QStringLiteral("clientTerminal.term.getSelection()"),
                                  [guard](const QVariant &selection)
                                  {
                                      if (guard && selection.isValid() && !selection.toString().isEmpty())
                                          QApplication::clipboard()->setText(selection.toString());
                                  });
}

void TerminalView::pasteClipboard()
{
    if (!m_ready || m_failed || !m_inputEnabled)
        return;
    const QString text = QApplication::clipboard()->text();
    if (text.toUtf8().size() > maxBatchBytes)
    {
        fail(QStringLiteral("粘贴内容超过 64 KiB，已停止输入。"));
        return;
    }
    const QByteArray arguments = QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact);
    m_view->page()->runJavaScript(QStringLiteral("clientTerminal.term.paste.apply(clientTerminal.term,%1)")
                                      .arg(QString::fromUtf8(arguments)));
}

void TerminalView::fail(const QString &reason)
{
    if (m_failed)
        return;
    m_failed = true;
    m_inputEnabled = false;
    m_pending.clear();
    m_inFlight = 0;
    m_timeout->stop();
    emit m_bridge->inputState(false);
    emit failed(reason);
}
}
