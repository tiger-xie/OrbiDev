#include <terminal/TerminalView.h>

#include <QApplication>
#include <QClipboard>
#include <QPointer>
#include <QSignalSpy>
#include <QThread>
#include <QWebEnginePage>
#include <QWebEngineView>
#include <QtTest>

#include <memory>

using rad::client::TerminalView;

namespace
{
QVariant evaluate(TerminalView &view, const QString &script)
{
    struct Evaluation
    {
        QVariant result;
        bool completed = false;
    };
    const auto state = std::make_shared<Evaluation>();
    view.webView()->page()->runJavaScript(script,
                                          [state](const QVariant &value)
                                          {
                                              state->result = value;
                                              state->completed = true;
                                          });
    QElapsedTimer deadline;
    deadline.start();
    while (!state->completed && deadline.elapsed() < 5000)
        QTest::qWait(10);
    return state->result;
}
}

class TerminalViewTests final : public QObject
{
    Q_OBJECT
private slots:
    void rendersRawUtf8AndAnsi();
    void gatesInputAndResizes();
    void clipboardAndNavigation();
    void rejectsOutputOverflow();
    void replayDoesNotAnswerHistoricalQueries();
    void destructionCancelsPendingWork_data();
    void destructionCancelsPendingWork();
    void synchronousWindowSetupDoesNotConsumeLoadDeadline();
};

void TerminalViewTests::replayDoesNotAnswerHistoricalQueries()
{
    TerminalView view;
    QSignalSpy ready(&view, &TerminalView::ready);
    QSignalSpy inputs(&view, &TerminalView::inputReady);
    QSignalSpy failed(&view, &TerminalView::failed);
    view.resize(800, 480);
    view.show();
    QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty() || !failed.isEmpty(), 20000);
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
    QCOMPARE(ready.count(), 1);
    view.setInputEnabled(true);
    // 同一输出块跨越历史边界：历史 DA/DSR/颜色查询无回复，实时 DA 仍需正常回答。
    const QByteArray history("\x1b[c\x1b[6n\x1b]10;?\x07history");
    view.appendOutput(history + "\x1b[c live", history.size());
    QTRY_VERIFY_WITH_TIMEOUT(
        evaluate(view, QStringLiteral("clientTerminal.term.buffer.active.getLine(0).translateToString(true)"))
            .toString()
            .contains(QStringLiteral("history live")),
        5000);
    QTRY_COMPARE(inputs.count(), 1);
    QCOMPARE(inputs.front().at(0).toByteArray(), QByteArray("\x1b[?1;2c"));
    QTRY_VERIFY(!evaluate(view, QStringLiteral("clientTerminal.term.options.disableStdin")).toBool());
    evaluate(view, QStringLiteral("clientTerminal.term.paste('after-history')"));
    QTRY_COMPARE(inputs.count(), 2);
    QCOMPARE(inputs.last().at(0).toByteArray(), QByteArray("after-history"));
    QCOMPARE(failed.count(), 0);
}

void TerminalViewTests::rendersRawUtf8AndAnsi()
{
    TerminalView view;
    QSignalSpy ready(&view, &TerminalView::ready);
    QSignalSpy failed(&view, &TerminalView::failed);
    view.resize(800, 480);
    view.show();
    QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty() || !failed.isEmpty(), 20000);
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
    QCOMPARE(ready.count(), 1);
    const QByteArray chinese = QStringLiteral("中文").toUtf8();
    view.appendOutput("\x1b[31mRED\x1b[0m ");
    view.appendOutput(chinese.left(2));
    view.appendOutput(chinese.mid(2));
    QTRY_VERIFY_WITH_TIMEOUT(
        evaluate(view, QStringLiteral("clientTerminal.term.buffer.active.getLine(0).translateToString(true)"))
            .toString()
            .contains(QStringLiteral("RED 中文")),
        5000);
    QCOMPARE(
        evaluate(view, QStringLiteral("clientTerminal.term.buffer.active.getLine(0).getCell(0).getFgColor()"))
            .toInt(),
        1);
    QCOMPARE(failed.count(), 0);
}

void TerminalViewTests::gatesInputAndResizes()
{
    TerminalView view;
    QSignalSpy ready(&view, &TerminalView::ready);
    QSignalSpy failed(&view, &TerminalView::failed);
    QSignalSpy inputs(&view, &TerminalView::inputReady);
    QSignalSpy sizes(&view, &TerminalView::terminalResized);
    view.resize(800, 480);
    view.show();
    QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty() || !failed.isEmpty(), 20000);
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
    QCOMPARE(ready.count(), 1);
    evaluate(view, QStringLiteral("clientTerminal.term.paste('blocked')"));
    QTest::qWait(50);
    QCOMPARE(inputs.count(), 0);
    view.setInputEnabled(true);
    QTRY_VERIFY(!evaluate(view, QStringLiteral("clientTerminal.term.options.disableStdin")).toBool());
    evaluate(view, QStringLiteral("clientTerminal.term.paste('中文/')"));
    QTRY_COMPARE(inputs.count(), 1);
    QCOMPARE(inputs.takeFirst().at(0).toByteArray(), QStringLiteral("中文/").toUtf8());
    // 使用真实浏览器焦点接收按键，验证 Qt 不把 Tab 当控件导航。
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    view.raise();
    view.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&view));
    view.focusTerminal();
    QTRY_VERIFY(QApplication::focusWidget() && (QApplication::focusWidget() == view.webView() ||
                                                view.webView()->isAncestorOf(QApplication::focusWidget())));
    // 浏览器可重建内部焦点控件；窗口按每个事件的实际焦点分发组合键。
    auto *window = view.windowHandle();
    QVERIFY(window);
    QTest::keyClick(window, Qt::Key_Tab);
    QTest::keyClick(window, Qt::Key_Escape);
    QTest::keyClick(window, Qt::Key_Up);
    QTest::keyClick(window, Qt::Key_C, Qt::ControlModifier);
    QTRY_COMPARE(inputs.count(), 4);
    QCOMPARE(inputs.at(0).at(0).toByteArray(), QByteArray("\t"));
    QCOMPARE(inputs.at(1).at(0).toByteArray(), QByteArray("\x1b"));
    QCOMPARE(inputs.at(2).at(0).toByteArray(), QByteArray("\x1b[A"));
    QCOMPARE(inputs.at(3).at(0).toByteArray(), QByteArray("\x03"));
    sizes.clear();
    view.resize(1000, 600);
    QTRY_VERIFY(!sizes.isEmpty());
    const int columns = sizes.last().at(0).toInt();
    const int sizeCount = sizes.count();
    view.setInputEnabled(true);
    QCOMPARE(sizes.count(), sizeCount);
    view.zoomBy(3);
    QTRY_VERIFY(sizes.last().at(0).toInt() < columns);
    view.setInputEnabled(false);
    inputs.clear();
    evaluate(view, QStringLiteral("clientTerminal.term.paste('blocked')"));
    QTest::qWait(50);
    QCOMPARE(inputs.count(), 0);
}

void TerminalViewTests::clipboardAndNavigation()
{
    TerminalView view;
    QSignalSpy ready(&view, &TerminalView::ready);
    QSignalSpy failed(&view, &TerminalView::failed);
    QSignalSpy inputs(&view, &TerminalView::inputReady);
    view.resize(800, 480);
    view.show();
    QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty() || !failed.isEmpty(), 20000);
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
    QCOMPARE(ready.count(), 1);
    view.setInputEnabled(true);
    QTRY_VERIFY(!evaluate(view, QStringLiteral("clientTerminal.term.options.disableStdin")).toBool());
    QApplication::clipboard()->setText(QStringLiteral("clipboard"));
    view.appendOutput("\x1b]52;c;ZXZpbA==\x07safe");
    QTRY_VERIFY(
        evaluate(view, QStringLiteral("clientTerminal.term.buffer.active.getLine(0).translateToString(true)"))
            .toString()
            .contains(QStringLiteral("safe")));
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("clipboard"));
    view.focusTerminal();
    QTRY_VERIFY(
        evaluate(view, QStringLiteral("document.activeElement === clientTerminal.term.textarea")).toBool());
    evaluate(view, QStringLiteral("clientTerminal.term.select(0,0,4)"));
    QCOMPARE(evaluate(view, QStringLiteral("clientTerminal.term.getSelection()")).toString(),
             QStringLiteral("safe"));
    // 前一个测试窗口销毁后，Windows 可能尚未将焦点移交给新窗口。
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    view.raise();
    view.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&view));
    view.focusTerminal();
    QTRY_VERIFY(QApplication::focusWidget() && (QApplication::focusWidget() == view.webView() ||
                                                view.webView()->isAncestorOf(QApplication::focusWidget())));
    auto *window = view.windowHandle();
    QVERIFY(window);
    QTest::keyClick(window, Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_COMPARE(QApplication::clipboard()->text(), QStringLiteral("safe"));
    QTest::keyClick(window, Qt::Key_V, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_COMPARE(inputs.count(), 1);
    QCOMPARE(inputs.first().at(0).toByteArray(), QByteArray("safe"));
    evaluate(view, QStringLiteral("window.location='https://example.com/'"));
    QTest::qWait(100);
    QCOMPARE(view.webView()->url(), QUrl(QStringLiteral("qrc:/client-terminal/client-index.html")));
    view.appendOutput(" after blocked navigation");
    QTRY_VERIFY(
        evaluate(view, QStringLiteral("clientTerminal.term.buffer.active.getLine(0).translateToString(true)"))
            .toString()
            .contains(QStringLiteral("safe after blocked navigation")));
}

void TerminalViewTests::rejectsOutputOverflow()
{
    TerminalView view;
    QSignalSpy failed(&view, &TerminalView::failed);
    view.appendOutput(QByteArray(1024 * 1024 + 1, 'x'));
    QCOMPARE(failed.count(), 1);
    view.appendOutput("ignored after fatal");
    QCOMPARE(failed.count(), 1);
}

void TerminalViewTests::destructionCancelsPendingWork_data()
{
    QTest::addColumn<int>("stage");
    QTest::newRow("before-event-delivery") << 0;
    QTest::newRow("during-page-load") << 1;
    QTest::newRow("ready-with-pending-script") << 2;
}

void TerminalViewTests::destructionCancelsPendingWork()
{
    QFETCH(int, stage);
    for (int iteration = 0; iteration < 2; ++iteration)
    {
        // 父窗口兜住断言失败路径，避免测试提前返回时泄漏终端。
        QWidget owner;
        owner.resize(800, 480);
        auto *terminal = new TerminalView(&owner);
        terminal->resize(owner.size());
        const QPointer<TerminalView> guard(terminal);
        QSignalSpy ready(terminal, &TerminalView::ready);
        QSignalSpy failed(terminal, &TerminalView::failed);
        QSignalSpy loading(terminal->webView()->page(), &QWebEnginePage::loadStarted);
        auto callbacks = std::make_shared<int>(0);
        auto callbackResult = std::make_shared<QVariant>();
        if (stage == 0)
            delete terminal;
        else if (stage == 1)
            connect(terminal->webView()->page(), &QWebEnginePage::loadStarted, terminal,
                    &QObject::deleteLater);
        if (stage != 0)
        {
            terminal->show();
            owner.show();
        }
        if (stage == 2)
        {
            QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty() || !failed.isEmpty(), 20000);
            QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
            QCOMPARE(ready.count(), 1);
            // 离开就绪信号栈后发起异步请求，不分派事件就立即销毁，强制覆盖取消回调。
            terminal->webView()->page()->runJavaScript(QStringLiteral("clientTerminal.term.cols"),
                                                       [callbacks, callbackResult](const QVariant &result)
                                                       {
                                                           ++*callbacks;
                                                           *callbackResult = result;
                                                       });
            QCOMPARE(*callbacks, 0);
            delete terminal;
        }
        QTRY_VERIFY_WITH_TIMEOUT(guard.isNull(), 5000);
        QCOMPARE(failed.count(), 0);
        if (stage == 0)
        {
            // 冲洗已排队的启动事件，确认对象销毁后不会再尝试加载页面。
            QTest::qWait(50);
            QCOMPARE(loading.count(), 0);
            QCOMPARE(ready.count(), 0);
        }
        else if (stage == 1)
        {
            QVERIFY(!loading.isEmpty());
            QCOMPARE(ready.count(), 0);
        }
        else
        {
            QTRY_COMPARE_WITH_TIMEOUT(*callbacks, 1, 5000);
            QVERIFY(!callbackResult->isValid());
        }
    }
}

void TerminalViewTests::synchronousWindowSetupDoesNotConsumeLoadDeadline()
{
    TerminalView view;
    QSignalSpy ready(&view, &TerminalView::ready);
    QSignalSpy failed(&view, &TerminalView::failed);
    // 模拟首次窗口显示或批量构造终端阻塞事件循环；握手在此期间没有处理机会。
    QThread::msleep(21000);
    view.resize(800, 480);
    view.show();
    QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty() || !failed.isEmpty(), 20000);
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
    QCOMPARE(ready.count(), 1);
    view.appendOutput("after-synchronous-setup");
    QTRY_VERIFY_WITH_TIMEOUT(
        evaluate(view, QStringLiteral("clientTerminal.term.buffer.active.getLine(0).translateToString(true)"))
            .toString()
            .contains(QStringLiteral("after-synchronous-setup")),
        5000);
}

int main(int argc, char **argv)
{
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS",
            qgetenv("QTWEBENGINE_CHROMIUM_FLAGS") + " --disable-background-timer-throttling");
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication app(argc, argv);
    TerminalViewTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "TerminalViewTests.moc"
