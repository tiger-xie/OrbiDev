#include <terminal/TerminalView.h>

#include <QApplication>
#include <QClipboard>
#include <QSignalSpy>
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
};

void TerminalViewTests::rendersRawUtf8AndAnsi()
{
    TerminalView view;
    QSignalSpy ready(&view, &TerminalView::ready);
    QSignalSpy failed(&view, &TerminalView::failed);
    view.resize(800, 480);
    view.show();
    QVERIFY(ready.wait(20000));
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
    QSignalSpy inputs(&view, &TerminalView::inputReady);
    QSignalSpy sizes(&view, &TerminalView::terminalResized);
    view.resize(800, 480);
    view.show();
    QVERIFY(ready.wait(20000));
    evaluate(view, QStringLiteral("clientTerminal.term.paste('blocked')"));
    QTest::qWait(50);
    QCOMPARE(inputs.count(), 0);
    view.setInputEnabled(true);
    QTRY_VERIFY(!evaluate(view, QStringLiteral("clientTerminal.term.options.disableStdin")).toBool());
    evaluate(view, QStringLiteral("clientTerminal.term.paste('中文/')"));
    QTRY_COMPARE(inputs.count(), 1);
    QCOMPARE(inputs.takeFirst().at(0).toByteArray(), QStringLiteral("中文/").toUtf8());
    view.focusTerminal();
    // 使用真实浏览器焦点接收按键，验证 Qt 不把 Tab 当控件导航。
    QTRY_VERIFY(QApplication::focusWidget() != nullptr);
    QWidget *focus = QApplication::focusWidget();
    QTest::keyClick(focus, Qt::Key_Tab);
    QTest::keyClick(focus, Qt::Key_Escape);
    QTest::keyClick(focus, Qt::Key_Up);
    QTest::keyClick(focus, Qt::Key_C, Qt::ControlModifier);
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
    QSignalSpy inputs(&view, &TerminalView::inputReady);
    view.resize(800, 480);
    view.show();
    QVERIFY(ready.wait(20000));
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
    view.focusTerminal();
    QTest::keyClick(QApplication::focusWidget(), Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_COMPARE(QApplication::clipboard()->text(), QStringLiteral("safe"));
    QTest::keyClick(QApplication::focusWidget(), Qt::Key_V, Qt::ControlModifier | Qt::ShiftModifier);
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
