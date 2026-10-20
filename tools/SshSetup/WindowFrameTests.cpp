#include "SetupTheme.h"
#include "WindowFrame.h"

#include <QApplication>
#include <QCloseEvent>
#include <QLabel>
#include <QScreen>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtTest>
#include <atomic>
#include <thread>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

class CloseGuardWindow final : public QWidget
{
public:
    bool allowClose = false;
    int closeRequests = 0;

protected:
    void closeEvent(QCloseEvent *event) override
    {
        ++closeRequests;
        event->setAccepted(allowClose);
    }
};

class WindowFrameTests final : public QObject
{
    Q_OBJECT
private slots:
    void dialogFrameOnlyClosesAndDoesNotMaximize()
    {
        CloseGuardWindow window;
        auto *layout = new QVBoxLayout(&window);
        auto *frame = new sshsetup::WindowFrame(&window, true);
        layout->addWidget(frame);
        window.resize(600, 400);
        window.show();
        QVERIFY(!frame->findChild<QToolButton *>(QStringLiteral("frameMinimize")));
        QVERIFY(!frame->findChild<QToolButton *>(QStringLiteral("frameMaximize")));
        QTest::mouseDClick(frame, Qt::LeftButton, Qt::NoModifier, QPoint(100, 20));
        QVERIFY(!window.isMaximized());
        auto *close = frame->findChild<QToolButton *>(QStringLiteral("frameClose"));
        QVERIFY(close);
        close->click();
        QCOMPARE(window.closeRequests, 1);
        QVERIFY(window.isVisible());
        window.allowClose = true;
        close->click();
        QVERIFY(!window.isVisible());
    }
    void nativeDragAndResize()
    {
        if (!qEnvironmentVariableIsSet("ORBIDEV_NATIVE_FRAME_TESTS"))
            QSKIP("原生鼠标检查需显式开启并使用独立测试窗口。");
        QWidget window;
        auto *layout = new QVBoxLayout(&window);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        layout->addWidget(new sshsetup::WindowFrame(&window));
        layout->addStretch();
        window.setGeometry(150, 150, 700, 400);
        window.show();
        const HWND handle = reinterpret_cast<HWND>(window.winId());
        SetForegroundWindow(handle);
        QTest::qWait(200);
        if (GetForegroundWindow() != handle)
            QSKIP("测试窗口未获得前台，不发送系统鼠标输入。");
        POINT originalCursor{};
        QVERIFY(GetCursorPos(&originalCursor));
        const auto drag = [&](POINT from, POINT to)
        {
            std::atomic_bool done{false};
            bool success = true;
            std::thread input(
                [&]
                {
                    if (GetForegroundWindow() != handle)
                    {
                        success = false;
                        done = true;
                        return;
                    }
                    success = SetCursorPos(from.x, from.y) != FALSE;
                    Sleep(80);
                    INPUT mouse{};
                    mouse.type = INPUT_MOUSE;
                    mouse.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
                    success = (SendInput(1, &mouse, sizeof(mouse)) == 1) && success;
                    Sleep(120);
                    success = (SetCursorPos(to.x, to.y) != FALSE) && success;
                    Sleep(150);
                    mouse.mi.dwFlags = MOUSEEVENTF_LEFTUP;
                    success = (SendInput(1, &mouse, sizeof(mouse)) == 1) && success;
                    done = true;
                });
            while (!done)
                QTest::qWait(20);
            input.join();
            QTest::qWait(100);
            SetCursorPos(originalCursor.x, originalCursor.y);
            return success;
        };
        QRect before = window.geometry();
        POINT caption{200, 20};
        QVERIFY(ClientToScreen(handle, &caption));
        QVERIFY(drag(caption, {caption.x + 30, caption.y + 25}));
        QVERIFY(window.pos() != before.topLeft());
        // 逐一走系统八方向缩放，而不是直接调用 resize 模拟。
        const QList<Qt::Edges> cases = {Qt::LeftEdge,
                                        Qt::RightEdge,
                                        Qt::TopEdge,
                                        Qt::BottomEdge,
                                        Qt::LeftEdge | Qt::TopEdge,
                                        Qt::RightEdge | Qt::TopEdge,
                                        Qt::LeftEdge | Qt::BottomEdge,
                                        Qt::RightEdge | Qt::BottomEdge};
        for (const auto edges : cases)
        {
            window.setGeometry(150, 150, 700, 400);
            QTest::qWait(80);
            RECT bounds{};
            QVERIFY(GetWindowRect(handle, &bounds));
            POINT from{(bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2};
            POINT delta{0, 0};
            if (edges.testFlag(Qt::LeftEdge))
            {
                from.x = bounds.left + 2;
                delta.x = -24;
            }
            if (edges.testFlag(Qt::RightEdge))
            {
                from.x = bounds.right - 2;
                delta.x = 24;
            }
            if (edges.testFlag(Qt::TopEdge))
            {
                from.y = bounds.top + 2;
                delta.y = -24;
            }
            if (edges.testFlag(Qt::BottomEdge))
            {
                from.y = bounds.bottom - 2;
                delta.y = 24;
            }
            before = window.geometry();
            QVERIFY(drag(from, {from.x + delta.x, from.y + delta.y}));
            if (delta.x)
                QVERIFY(window.width() > before.width());
            if (delta.y)
                QVERIFY(window.height() > before.height());
        }
        window.close();
    }
    void edgeHitTesting()
    {
        using sshsetup::WindowFrame;
        const QSize size(800, 600);
        QCOMPARE(WindowFrame::resizeEdges({0, 0}, size), Qt::Edges(Qt::TopEdge | Qt::LeftEdge));
        QCOMPARE(WindowFrame::resizeEdges({799, 0}, size), Qt::Edges(Qt::TopEdge | Qt::RightEdge));
        QCOMPARE(WindowFrame::resizeEdges({0, 599}, size), Qt::Edges(Qt::BottomEdge | Qt::LeftEdge));
        QCOMPARE(WindowFrame::resizeEdges({799, 599}, size), Qt::Edges(Qt::BottomEdge | Qt::RightEdge));
        QCOMPARE(WindowFrame::resizeEdges({400, 0}, size), Qt::Edges(Qt::TopEdge));
        QCOMPARE(WindowFrame::resizeEdges({400, 599}, size), Qt::Edges(Qt::BottomEdge));
        QCOMPARE(WindowFrame::resizeEdges({0, 300}, size), Qt::Edges(Qt::LeftEdge));
        QCOMPARE(WindowFrame::resizeEdges({799, 300}, size), Qt::Edges(Qt::RightEdge));
        QCOMPARE(WindowFrame::resizeEdges({400, 300}, size), Qt::Edges());
        QCOMPARE(WindowFrame::resizeEdges({-1, 0}, size), Qt::Edges());
        QCOMPARE(WindowFrame::resizeEdges({800, 0}, size), Qt::Edges());
        QCOMPARE(WindowFrame::resizeEdges({0, 0}, size, 6, true), Qt::Edges());
        QCOMPARE(WindowFrame::resizeEdges({0, 0}, {}), Qt::Edges());
    }

    void titleAndCloseFollowWindow()
    {
        CloseGuardWindow window;
        auto *layout = new QVBoxLayout(&window);
        auto *frame = new sshsetup::WindowFrame(&window);
        layout->addWidget(frame);
        window.resize(800, 400);
        window.setWindowTitle(QStringLiteral("标题同步检查"));
        window.show();
        QVERIFY(window.windowFlags().testFlag(Qt::FramelessWindowHint));
        auto *title = frame->findChild<QLabel *>(QStringLiteral("frameTitle"));
        QVERIFY(title);
        QTRY_VERIFY(title->text().contains(QStringLiteral("标题同步检查")));
        auto *close = frame->findChild<QToolButton *>(QStringLiteral("frameClose"));
        QVERIFY(close);
        close->click();
        QCOMPARE(window.closeRequests, 1);
        QVERIFY(window.isVisible());
        window.allowClose = true;
        close->click();
        QCOMPARE(window.closeRequests, 2);
        QVERIFY(!window.isVisible());
    }

    void maximizeMinimizeAndRestore()
    {
        QWidget window;
        auto *layout = new QVBoxLayout(&window);
        auto *frame = new sshsetup::WindowFrame(&window);
        layout->addWidget(frame);
        const QRect available = QApplication::primaryScreen()->availableGeometry();
        window.setGeometry(available.x() + 30, available.y() + 30, 700, 400);
        window.show();
        QTest::qWait(100);
        const QRect normal = window.geometry();
        auto *maximize = frame->findChild<QToolButton *>(QStringLiteral("frameMaximize"));
        auto *minimize = frame->findChild<QToolButton *>(QStringLiteral("frameMinimize"));
        QVERIFY(maximize && minimize);
        maximize->click();
        QTRY_VERIFY(window.isMaximized());
        QVERIFY(available.adjusted(-2, -2, 2, 2).contains(window.geometry()));
        maximize->click();
        QTRY_VERIFY(!window.isMaximized());
        QTRY_COMPARE(window.geometry(), normal);
        minimize->click();
        QTRY_VERIFY(window.isMinimized());
        window.showNormal();
        QTRY_COMPARE(window.geometry(), normal);
        QTest::mouseDClick(frame, Qt::LeftButton, Qt::NoModifier, QPoint(80, 20));
        QTRY_VERIFY(window.isMaximized());
        QTest::mouseDClick(frame, Qt::LeftButton, Qt::NoModifier, QPoint(80, 20));
        QTRY_COMPARE(window.geometry(), normal);
        window.close();
    }
};

int main(int argc, char **argv)
{
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    sshsetup::applySetupTheme(application);
    WindowFrameTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "WindowFrameTests.moc"
