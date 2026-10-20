#pragma once

#include <QPointer>
#include <QWidget>

class QLabel;
class QToolButton;

namespace sshsetup
{

// 仅用于顶层窗口；窗口移动与缩放交给操作系统，关闭仍由原窗口处理。
class WindowFrame final : public QWidget
{
    Q_OBJECT
public:
    explicit WindowFrame(QWidget *target);
    ~WindowFrame() override;

    // 输入为窗口逻辑坐标；最大化、窗口外部和无效尺寸不允许调整大小。
    static Qt::Edges resizeEdges(const QPoint &position, const QSize &size, int borderWidth = 6,
                                 bool maximized = false);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void updateWindowState();
    void updateIcon();
    void toggleMaximized();
    void showSystemMenu(bool atCursor);
    bool isTitleArea(QWidget *widget) const;

    QPointer<QWidget> m_target;
    QLabel *m_icon = nullptr;
    QLabel *m_title = nullptr;
    QToolButton *m_maximize = nullptr;
    bool m_resizeCursor = false;
};

}
