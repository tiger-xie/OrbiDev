#include "WindowFrame.h"

#include <QApplication>
#include <QDebug>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QShortcut>
#include <QToolButton>
#include <QWindow>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace sshsetup
{

WindowFrame::WindowFrame(QWidget *target, bool dialog) : QWidget(target), m_target(target), m_dialog(dialog)
{
    Q_ASSERT(target && target->isWindow());
    setObjectName(QStringLiteral("windowFrame"));
    setAttribute(Qt::WA_StyledBackground);
    setFixedHeight(40);
    setMouseTracking(true);
    target->setWindowFlag(Qt::FramelessWindowHint, true);
    if (m_dialog)
    {
        target->setWindowFlag(Qt::WindowMinimizeButtonHint, false);
        target->setWindowFlag(Qt::WindowMaximizeButtonHint, false);
    }
    target->setMouseTracking(true);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(16, 0, 0, 0);
    layout->setSpacing(10);
    m_icon = new QLabel(this);
    m_icon->setFixedSize(20, 20);
    m_icon->setAttribute(Qt::WA_TransparentForMouseEvents);
    layout->addWidget(m_icon);
    m_title = new QLabel(target->windowTitle(), this);
    m_title->setObjectName(QStringLiteral("frameTitle"));
    m_title->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(m_title, 1);

    const auto addButton = [this, layout](const QString &name, const QString &text, const QString &icon)
    {
        auto *button = new QToolButton(this);
        button->setObjectName(name);
        button->setAccessibleName(text);
        button->setToolTip(text);
        button->setIcon(QIcon(icon));
        button->setIconSize(QSize(16, 16));
        button->setFixedSize(46, 40);
        button->setFocusPolicy(Qt::StrongFocus);
        layout->addWidget(button);
        return button;
    };
    if (!m_dialog)
    {
        auto *minimize = addButton(QStringLiteral("frameMinimize"), QStringLiteral("最小化"),
                                   QStringLiteral(":/resources/icon/window/minimize.svg"));
        m_maximize = addButton(QStringLiteral("frameMaximize"), QStringLiteral("最大化"),
                               QStringLiteral(":/resources/icon/window/maximize.svg"));
        connect(minimize, &QToolButton::clicked, target, &QWidget::showMinimized);
        connect(m_maximize, &QToolButton::clicked, this, &WindowFrame::toggleMaximized);
    }
    auto *close = addButton(QStringLiteral("frameClose"), QStringLiteral("关闭"),
                            QStringLiteral(":/resources/icon/window/close.svg"));
    connect(close, &QToolButton::clicked, target, &QWidget::close);
    connect(target, &QWidget::windowTitleChanged, m_title, &QLabel::setText);
    connect(target, &QWidget::windowIconChanged, this, &WindowFrame::updateIcon);
    auto *systemMenu = new QShortcut(QKeySequence(QStringLiteral("Alt+Space")), target);
    connect(systemMenu, &QShortcut::activated, this, [this]() { showSystemMenu(false); });
    updateIcon();
    updateWindowState();
    qApp->installEventFilter(this);
}

WindowFrame::~WindowFrame()
{
    qApp->removeEventFilter(this);
}

Qt::Edges WindowFrame::resizeEdges(const QPoint &position, const QSize &size, int borderWidth, bool maximized)
{
    if (maximized || borderWidth <= 0 || size.isEmpty() || position.x() < 0 || position.y() < 0 ||
        position.x() >= size.width() || position.y() >= size.height())
        return {};
    Qt::Edges edges;
    if (position.x() < borderWidth)
        edges |= Qt::LeftEdge;
    else if (position.x() >= size.width() - borderWidth)
        edges |= Qt::RightEdge;
    if (position.y() < borderWidth)
        edges |= Qt::TopEdge;
    else if (position.y() >= size.height() - borderWidth)
        edges |= Qt::BottomEdge;
    return edges;
}

void WindowFrame::updateIcon()
{
    const QIcon icon = m_target->windowIcon().isNull() ? QApplication::windowIcon() : m_target->windowIcon();
    m_icon->setPixmap(icon.pixmap(QSize(20, 20)));
}

void WindowFrame::updateWindowState()
{
    const bool maximized = m_target->isMaximized();
    if (m_maximize)
    {
        m_maximize->setIcon(QIcon(maximized ? QStringLiteral(":/resources/icon/window/restore.svg")
                                            : QStringLiteral(":/resources/icon/window/maximize.svg")));
        const QString text = maximized ? QStringLiteral("还原") : QStringLiteral("最大化");
        m_maximize->setAccessibleName(text);
        m_maximize->setToolTip(text);
    }
    if (m_resizeCursor)
    {
        m_target->unsetCursor();
        m_resizeCursor = false;
    }
}

void WindowFrame::toggleMaximized()
{
    if (m_dialog)
        return;
    if (m_target->isMaximized())
        m_target->showNormal();
    else
        m_target->showMaximized();
}

bool WindowFrame::isTitleArea(QWidget *widget) const
{
    return widget == this || widget == m_icon || widget == m_title;
}

bool WindowFrame::eventFilter(QObject *watched, QEvent *event)
{
    if (!m_target)
        return false;
    if (watched == m_target && event->type() == QEvent::WindowStateChange)
        updateWindowState();
    auto *widget = qobject_cast<QWidget *>(watched);
    if (!widget || widget->window() != m_target || !m_target->isEnabled())
        return false;
    if (event->type() != QEvent::MouseButtonPress && event->type() != QEvent::MouseButtonDblClick &&
        event->type() != QEvent::MouseMove)
        return false;
    auto *mouse = static_cast<QMouseEvent *>(event);
    const QPoint position = m_target->mapFromGlobal(mouse->globalPos());
    Qt::Edges edges =
        resizeEdges(position, m_target->size(), 6, m_target->isMaximized() || m_target->isFullScreen());
    if (m_target->minimumWidth() == m_target->maximumWidth())
        edges &= ~(Qt::LeftEdge | Qt::RightEdge);
    if (m_target->minimumHeight() == m_target->maximumHeight())
        edges &= ~(Qt::TopEdge | Qt::BottomEdge);
    if (event->type() == QEvent::MouseMove && mouse->buttons() == Qt::NoButton)
    {
        if (edges)
        {
            Qt::CursorShape cursor = Qt::SizeVerCursor;
            if (edges == (Qt::TopEdge | Qt::LeftEdge) || edges == (Qt::BottomEdge | Qt::RightEdge))
                cursor = Qt::SizeFDiagCursor;
            else if (edges == (Qt::TopEdge | Qt::RightEdge) || edges == (Qt::BottomEdge | Qt::LeftEdge))
                cursor = Qt::SizeBDiagCursor;
            else if (edges == Qt::LeftEdge || edges == Qt::RightEdge)
                cursor = Qt::SizeHorCursor;
            m_target->setCursor(cursor);
            m_resizeCursor = true;
        }
        else if (m_resizeCursor)
        {
            m_target->unsetCursor();
            m_resizeCursor = false;
        }
        return false;
    }
    if (event->type() == QEvent::MouseButtonPress && mouse->button() == Qt::LeftButton && edges)
    {
        if (m_target->windowHandle() && m_target->windowHandle()->startSystemResize(edges))
            return true;
        qWarning("WindowFrame: system resize could not be started.");
        return false;
    }
    if (!isTitleArea(widget))
        return false;
    if (event->type() == QEvent::MouseButtonDblClick && mouse->button() == Qt::LeftButton)
    {
        toggleMaximized();
        return true;
    }
    if (event->type() == QEvent::MouseButtonPress && mouse->button() == Qt::RightButton)
    {
        showSystemMenu(true);
        return true;
    }
    if (event->type() == QEvent::MouseButtonPress && mouse->button() == Qt::LeftButton)
    {
        if (m_target->windowHandle() && m_target->windowHandle()->startSystemMove())
            return true;
        qWarning("WindowFrame: system move could not be started.");
    }
    return false;
}

void WindowFrame::showSystemMenu(bool atCursor)
{
#ifdef Q_OS_WIN
    const HWND handle = reinterpret_cast<HWND>(m_target->winId());
    // 菜单属于系统窗口，不由本组件释放；系统 API 使用物理坐标。
    const HMENU menu = GetSystemMenu(handle, FALSE);
    if (!menu)
    {
        qWarning("WindowFrame: system menu is unavailable.");
        return;
    }
    const bool maximized = m_target->isMaximized();
    EnableMenuItem(menu, SC_RESTORE, MF_BYCOMMAND | (!m_dialog && maximized ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(menu, SC_MINIMIZE, MF_BYCOMMAND | (m_dialog ? MF_GRAYED : MF_ENABLED));
    EnableMenuItem(menu, SC_MOVE, MF_BYCOMMAND | (maximized ? MF_GRAYED : MF_ENABLED));
    EnableMenuItem(menu, SC_SIZE, MF_BYCOMMAND | (maximized ? MF_GRAYED : MF_ENABLED));
    EnableMenuItem(menu, SC_MAXIMIZE, MF_BYCOMMAND | (m_dialog || maximized ? MF_GRAYED : MF_ENABLED));
    POINT position{0, static_cast<LONG>(height() * m_target->devicePixelRatioF())};
    const BOOL positioned = atCursor ? GetCursorPos(&position) : ClientToScreen(handle, &position);
    if (!positioned)
    {
        qWarning("WindowFrame: system menu position is unavailable.");
        return;
    }
    const UINT command =
        TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, position.x, position.y, handle, nullptr);
    if (!command)
        return;
    if (m_dialog && (command == SC_MINIMIZE || command == SC_MAXIMIZE || command == SC_RESTORE))
        return;
    if (command == SC_CLOSE)
        m_target->close();
    else if (command == SC_MINIMIZE)
        m_target->showMinimized();
    else if (command == SC_MAXIMIZE)
        m_target->showMaximized();
    else if (command == SC_RESTORE)
        m_target->showNormal();
    else if (!PostMessage(handle, WM_SYSCOMMAND, command, 0))
        qWarning("WindowFrame: system menu command could not be dispatched.");
#else
    Q_UNUSED(atCursor);
    qWarning("WindowFrame: native system menu is only implemented on Windows.");
#endif
}

}
