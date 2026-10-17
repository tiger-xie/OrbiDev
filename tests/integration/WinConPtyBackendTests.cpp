#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QMutex>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <WinConPtyBackend.h>
#include <Windows.h>
#include <array>
#include <cstring>
#include <memory>
#include <thread>
#include <winioctl.h>

namespace
{
rad::LaunchSpec launchSpec(const QString &directory)
{
    rad::LaunchSpec spec;
    spec.executablePath = qEnvironmentVariable("RAD_FAKECLI_PATH");
    spec.workingDirectory = directory;
    return spec;
}

// 测试收集器与生产接口一样按通知拉取，记录线程及完成后的违规回调。
class Capture final : public QObject
{
public:
    explicit Capture(rad::WinConPtyBackend &backend) : m_backend(backend)
    {
        connect(&backend, &rad::ITerminalBackend::outputAvailable, this,
                [this]
                {
                    m_wrongThread |= QThread::currentThread() != thread();
                    m_outputAfterFinished |= m_finished;
                    ++m_notifications;
                    if (m_autoDrain)
                        drain();
                });
        connect(&backend, &rad::ITerminalBackend::finished, this,
                [this](rad::ExitStatus) { m_finished = true; });
    }
    void drain()
    {
        for (;;)
        {
            const auto result = m_backend.takeOutput(4096);
            if (!result.isSuccess())
            {
                m_readFailed = true;
                return;
            }
            m_bytes += result.value().bytes;
            m_dropped += result.value().droppedBytes;
            if (!result.value().hasMore)
                return;
        }
    }
    rad::WinConPtyBackend &m_backend;
    QByteArray m_bytes;
    quint64 m_dropped = 0;
    int m_notifications = 0;
    bool m_autoDrain = true;
    bool m_finished = false;
    bool m_wrongThread = false;
    bool m_outputAfterFinished = false;
    bool m_readFailed = false;
};

struct ProcessHandle
{
    HANDLE value = nullptr;
    ~ProcessHandle()
    {
        if (value)
            CloseHandle(value);
    }
};

// 仅移除本测试创建的重解析点本身，绝不递归遍历其目标。
struct Junction
{
    QString path;
    ~Junction()
    {
        if (!path.isEmpty())
            RemoveDirectoryW(reinterpret_cast<LPCWSTR>(path.utf16()));
    }
    bool create(const QString &link, const QString &target)
    {
        path = QDir::toNativeSeparators(link);
        if (!CreateDirectoryW(reinterpret_cast<LPCWSTR>(path.utf16()), nullptr))
            return false;
        ProcessHandle handle{CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_WRITE, 0, nullptr,
                                         OPEN_EXISTING,
                                         FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
        if (handle.value == INVALID_HANDLE_VALUE)
        {
            handle.value = nullptr;
            return false;
        }
        const QString print = QDir::toNativeSeparators(target);
        const QString substitute = QStringLiteral("\\??\\") + print;
        if (substitute.size() + print.size() + 2 > 4096)
            return false;
        struct MountPoint
        {
            DWORD tag;
            WORD dataLength;
            WORD reserved;
            WORD substituteOffset;
            WORD substituteLength;
            WORD printOffset;
            WORD printLength;
            wchar_t names[4096];
        } buffer{};
        buffer.tag = IO_REPARSE_TAG_MOUNT_POINT;
        buffer.substituteLength = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
        buffer.printOffset = static_cast<WORD>(buffer.substituteLength + sizeof(wchar_t));
        buffer.printLength = static_cast<WORD>(print.size() * sizeof(wchar_t));
        buffer.dataLength = static_cast<WORD>(8 + buffer.printOffset + buffer.printLength + sizeof(wchar_t));
        memcpy(buffer.names, substitute.utf16(), buffer.substituteLength);
        memcpy(reinterpret_cast<char *>(buffer.names) + buffer.printOffset, print.utf16(),
               buffer.printLength);
        DWORD returned = 0;
        return DeviceIoControl(handle.value, FSCTL_SET_REPARSE_POINT, &buffer,
                               static_cast<DWORD>(buffer.dataLength) + 8, nullptr, 0, &returned,
                               nullptr) != FALSE;
    }
    bool remove()
    {
        if (!RemoveDirectoryW(reinterpret_cast<LPCWSTR>(path.utf16())))
            return false;
        path.clear();
        return true;
    }
};
}

class WinConPtyBackendTests final : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QVERIFY(QFile::exists(qEnvironmentVariable("RAD_FAKECLI_PATH")));
    }

    void integration()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        rad::WinConPtyBackend backend({{workspace.path()}});
        Capture capture(backend);
        QSignalSpy started(&backend, &rad::ITerminalBackend::started);
        QSignalSpy finished(&backend, &rad::ITerminalBackend::finished);
        QSignalSpy failed(&backend, &rad::ITerminalBackend::failed);
        QVERIFY(backend.start(launchSpec(workspace.path())).isSuccess());
        QCOMPARE(started.count(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(capture.m_bytes.contains("[READY]"), 15000);
        QCOMPARE(started.count(), 1);
        QVERIFY(!backend.resize({0, 24}).isSuccess());
        QVERIFY(!backend.start(launchSpec(workspace.path())).isSuccess());
        QVERIFY(backend.writeInput(QStringLiteral("echo 中文往返验证\ransi\r").toUtf8()).isSuccess());
        QTRY_VERIFY_WITH_TIMEOUT(capture.m_bytes.contains(QStringLiteral("[ECHO]中文往返验证").toUtf8()),
                                 10000);
        QTRY_VERIFY_WITH_TIMEOUT(capture.m_bytes.contains("\x1b[32m[COLOR]"), 10000);
        QVERIFY(backend.writeInput(QByteArray("\x03\x1b[A", 4)).isSuccess());
        QTRY_VERIFY_WITH_TIMEOUT(capture.m_bytes.contains("[CTRL_C]"), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(capture.m_bytes.contains("[UP]"), 10000);
        QVERIFY(backend.resize({91, 27}).isSuccess());
        // Resize 与输入分属执行上下文，先给异步尺寸请求一次执行机会。
        QTest::qWait(100);
        QVERIFY(backend.writeInput("size\r").isSuccess());
        QTRY_VERIFY_WITH_TIMEOUT(capture.m_bytes.contains("[SIZE]91x27"), 10000);
        QVERIFY(backend.writeInput("exit\r").isSuccess());
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 15000);
        capture.drain();
        QVERIFY(capture.m_bytes.contains("[BYE]"));
        const auto exit = qvariant_cast<rad::ExitStatus>(finished.at(0).at(0));
        QVERIFY(exit.exitCodeValid);
        QCOMPARE(exit.exitCode, 0);
        QVERIFY(backend.terminate().isSuccess());
        QTest::qWait(30);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(failed.count(), 0);
        QVERIFY(!capture.m_readFailed && !capture.m_wrongThread && !capture.m_outputAfterFinished);
    }

    void argumentsAndTail()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        rad::WinConPtyBackend backend({{workspace.path()}});
        Capture capture(backend);
        QSignalSpy finished(&backend, &rad::ITerminalBackend::finished);
        auto spec = launchSpec(workspace.path());
        const QStringList arguments{QString(),
                                    QStringLiteral("with space"),
                                    QStringLiteral("a\"b"),
                                    QStringLiteral("a\\\\\"b"),
                                    QStringLiteral("trailing space\\"),
                                    QStringLiteral("中文参数"),
                                    QStringLiteral("& | > %PATH%")};
        spec.arguments = QStringList{QStringLiteral("--report-args")} + arguments;
        QVERIFY(backend.start(spec).isSuccess());
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 15000);
        capture.drain();
        for (int index = 0; index < arguments.size(); ++index)
        {
            const QByteArray expected =
                "[ARG" + QByteArray::number(index) + "]" + arguments.at(index).toUtf8().toHex() + "[END]";
            QVERIFY2(capture.m_bytes.contains(expected), expected.constData());
        }
        rad::WinConPtyBackend abnormal({{workspace.path()}});
        Capture tail(abnormal);
        QSignalSpy abnormalFinished(&abnormal, &rad::ITerminalBackend::finished);
        QVERIFY(abnormal.start(launchSpec(workspace.path())).isSuccess());
        QTRY_VERIFY_WITH_TIMEOUT(tail.m_bytes.contains("[READY]"), 15000);
        QVERIFY(abnormal.writeInput("fail-tail\r").isSuccess());
        QTRY_COMPARE_WITH_TIMEOUT(abnormalFinished.count(), 1, 15000);
        tail.drain();
        QVERIFY(tail.m_bytes.contains(QStringLiteral("[FAIL_TAIL]中文尾部").toUtf8()));
        const auto exit = qvariant_cast<rad::ExitStatus>(abnormalFinished.at(0).at(0));
        QVERIFY(exit.exitCodeValid);
        QCOMPARE(exit.exitCode, 23);
    }

    void validation()
    {
        QTemporaryDir workspace;
        QTemporaryDir outside;
        QVERIFY(workspace.isValid() && outside.isValid());
        rad::WinConPtyBackend backend({{workspace.path()}});
        QSignalSpy started(&backend, &rad::ITerminalBackend::started);
        QSignalSpy finished(&backend, &rad::ITerminalBackend::finished);
        QSignalSpy failed(&backend, &rad::ITerminalBackend::failed);
        auto spec = launchSpec(workspace.path());
        spec.size.columns = 0;
        QVERIFY(!backend.start(spec).isSuccess());
        QCOMPARE(finished.count(), 0);
        QCOMPARE(failed.count(), 0);
        spec = launchSpec(workspace.path());
        spec.arguments.append(QString(QChar::Null));
        QVERIFY(!backend.start(spec).isSuccess());
        QCOMPARE(finished.count(), 0);
        QVERIFY(!backend.takeOutput(0).isSuccess());
        QVERIFY(!backend.writeInput("x").isSuccess());
        QVERIFY(!backend.resize({80, 24}).isSuccess());
        spec = launchSpec(outside.path());
        QVERIFY(backend.start(spec).isSuccess());
        QCOMPARE(failed.count(), 0);
        QVERIFY(!backend.start(spec).isSuccess());
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 15000);
        QCOMPARE(started.count(), 0);
        QCOMPARE(failed.count(), 1);
        QVERIFY(failed.at(0).at(1).toBool());
        QVERIFY(!qvariant_cast<rad::ExitStatus>(finished.at(0).at(0)).exitCodeValid);
        rad::WinConPtyBackend missing({{workspace.path()}});
        QSignalSpy missingFinished(&missing, &rad::ITerminalBackend::finished);
        QSignalSpy missingFailed(&missing, &rad::ITerminalBackend::failed);
        spec = launchSpec(workspace.path());
        spec.executablePath = workspace.filePath(QStringLiteral("missing.exe"));
        QVERIFY(missing.start(spec).isSuccess());
        QTRY_COMPARE_WITH_TIMEOUT(missingFinished.count(), 1, 15000);
        QCOMPARE(missingFailed.count(), 1);

        const auto checkDenied = [&](rad::WinConPtyBackend &candidate, const QString &directory)
        {
            QSignalSpy denied(&candidate, &rad::ITerminalBackend::failed);
            QSignalSpy ended(&candidate, &rad::ITerminalBackend::finished);
            QVERIFY(candidate.start(launchSpec(directory)).isSuccess());
            QCOMPARE(denied.count(), 0);
            QTRY_COMPARE_WITH_TIMEOUT(ended.count(), 1, 15000);
            QCOMPARE(denied.count(), 1);
            QCOMPARE(qvariant_cast<rad::Error>(denied.at(0).at(0)).code, rad::ErrorCode::PermissionDenied);
        };
        rad::WinConPtyBackend emptyWhitelist;
        checkDenied(emptyWhitelist, workspace.path());
        const QString allowed = workspace.filePath(QStringLiteral("allowed"));
        const QString sibling = workspace.filePath(QStringLiteral("allowed-other"));
        QVERIFY(QDir().mkdir(allowed));
        QVERIFY(QDir().mkdir(sibling));
        rad::WinConPtyBackend prefix({{allowed}});
        checkDenied(prefix, sibling);
        Junction junction;
        const QString linked = QDir(allowed).filePath(QStringLiteral("outside-link"));
        QVERIFY(junction.create(linked, outside.path()));
        rad::WinConPtyBackend redirected({{allowed}});
        checkDenied(redirected, linked);
        QVERIFY(junction.remove());
    }

    void lifecycleStress()
    {
        // Qt 5 Windows 首次 QMutex 争用会缓存 16 个 Event；确定性初始化后再测会话资源。
        // 不增加容差：下面的 100 次会话仍要求结束后句柄数严格等于基线。
        {
            QMutex mutex;
            mutex.lock();
            bool acquired = false;
            std::thread contender(
                [&]
                {
                    acquired = mutex.tryLock(1);
                    if (acquired)
                        mutex.unlock();
                });
            contender.join();
            mutex.unlock();
            QVERIFY(!acquired);
        }
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        DWORD baseline = 0;
        for (int index = 0; index < 103; ++index)
        {
            {
                rad::WinConPtyBackend backend({{workspace.path()}});
                Capture capture(backend);
                QSignalSpy finished(&backend, &rad::ITerminalBackend::finished);
                QVERIFY(backend.start(launchSpec(workspace.path())).isSuccess());
                QTRY_VERIFY_WITH_TIMEOUT(capture.m_bytes.contains("[READY]"), 15000);
                QVERIFY(backend.writeInput("exit\r").isSuccess());
                QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 15000);
                capture.drain();
                QVERIFY(capture.m_bytes.contains("[BYE]"));
            }
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            QCoreApplication::processEvents();
            if (index == 1)
                QVERIFY(GetProcessHandleCount(GetCurrentProcess(), &baseline));
        }
        DWORD finalCount = 0;
        QVERIFY(GetProcessHandleCount(GetCurrentProcess(), &finalCount));
        QCOMPARE(finalCount, baseline);
    }

    void concurrency()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        std::array<std::unique_ptr<rad::WinConPtyBackend>, 3> backends;
        std::array<std::unique_ptr<Capture>, 3> captures;
        std::array<std::unique_ptr<QSignalSpy>, 3> finished;
        for (std::size_t index = 0; index < backends.size(); ++index)
        {
            backends[index] = std::make_unique<rad::WinConPtyBackend>(
                rad::WinConPtyBackend::Options{{workspace.path()}, 256 * 1024, 64 * 1024});
            captures[index] = std::make_unique<Capture>(*backends[index]);
            finished[index] =
                std::make_unique<QSignalSpy>(backends[index].get(), &rad::ITerminalBackend::finished);
            QVERIFY(backends[index]->start(launchSpec(workspace.path())).isSuccess());
        }
        for (const auto &capture : captures)
            QTRY_VERIFY_WITH_TIMEOUT(capture->m_bytes.contains("[READY]"), 15000);
        // 第一个消费者完全停读，另外两个仍须完成各自的高输出任务。
        captures[0]->m_autoDrain = false;
        for (std::size_t index = 0; index < backends.size(); ++index)
            QVERIFY(backends[index]
                        ->writeInput("flood S" + QByteArray::number(static_cast<int>(index)) + "\r")
                        .isSuccess());
        for (std::size_t index = 1; index < backends.size(); ++index)
        {
            const QByteArray marker = "[DONE:S" + QByteArray::number(static_cast<int>(index)) + "]";
            QTRY_VERIFY_WITH_TIMEOUT(captures[index]->m_bytes.contains(marker), 30000);
        }
        QVERIFY(backends[0]->writeInput("echo slow-finished\r").isSuccess());
        QElapsedTimer deadline;
        deadline.start();
        while (!captures[0]->m_bytes.contains("[ECHO]slow-finished") && deadline.elapsed() < 30000)
        {
            captures[0]->drain();
            QTest::qWait(20);
        }
        QVERIFY(captures[0]->m_bytes.contains("[ECHO]slow-finished"));
        QVERIFY(captures[0]->m_dropped > 0);
        for (std::size_t index = 0; index < backends.size(); ++index)
        {
            for (std::size_t other = 0; other < backends.size(); ++other)
            {
                if (other != index)
                    QVERIFY(!captures[index]->m_bytes.contains(
                        "[S" + QByteArray::number(static_cast<int>(other)) + "]"));
            }
            QVERIFY(backends[index]->writeInput("exit\r").isSuccess());
        }
        for (const auto &spy : finished)
            QTRY_COMPARE_WITH_TIMEOUT(spy->count(), 1, 15000);
    }

    void backpressure()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        rad::WinConPtyBackend backend({{workspace.path()}, 64, 8192});
        Capture capture(backend);
        QSignalSpy finished(&backend, &rad::ITerminalBackend::finished);
        QVERIFY(backend.start(launchSpec(workspace.path())).isSuccess());
        QTRY_VERIFY_WITH_TIMEOUT(capture.m_bytes.contains("[READY]"), 15000);
        const auto rejected = backend.writeInput(QByteArray(65, 'x'));
        QVERIFY(!rejected.isSuccess());
        QCOMPARE(rejected.error().code, rad::ErrorCode::QueueFull);
        QVERIFY(backend.writeInput("echo intact\r").isSuccess());
        QTRY_VERIFY_WITH_TIMEOUT(capture.m_bytes.contains("[ECHO]intact"), 10000);
        capture.m_autoDrain = false;
        const int before = capture.m_notifications;
        QVERIFY(backend.writeInput("flood\rexit\r").isSuccess());
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 30000);
        QVERIFY(capture.m_notifications - before <= 2);
        capture.drain();
        QVERIFY(capture.m_dropped > 0);
        QVERIFY(capture.m_bytes.contains("[BYE]"));
        const auto empty = backend.takeOutput(8192);
        QVERIFY(empty.isSuccess());
        QVERIFY(empty.value().bytes.isEmpty());
        QCOMPARE(empty.value().droppedBytes, quint64(0));
        QVERIFY(!empty.value().hasMore);

        rad::WinConPtyBackend polled({{workspace.path()}, 64, 8192});
        Capture polling(polled);
        QVERIFY(polled.start(launchSpec(workspace.path())).isSuccess());
        QTRY_VERIFY_WITH_TIMEOUT(polling.m_bytes.contains("[READY]"), 15000);
        polling.m_autoDrain = false;
        const int notifications = polling.m_notifications;
        QVERIFY(polled.writeInput("stream\r").isSuccess());
        // 主动拉空缓存但暂停 Qt 事件处理，工作线程不能累积逐块通知。
        QElapsedTimer interval;
        interval.start();
        int nonemptyReads = 0;
        while (interval.elapsed() < 500)
        {
            const auto chunk = polled.takeOutput(8192);
            QVERIFY(chunk.isSuccess());
            if (!chunk.value().bytes.isEmpty())
                ++nonemptyReads;
            QThread::msleep(2);
        }
        QVERIFY(nonemptyReads > 2);
        QCOMPARE(polling.m_notifications, notifications);
        QCoreApplication::sendPostedEvents(&polled, QEvent::MetaCall);
        QVERIFY(polling.m_notifications - notifications <= 1);
        QSignalSpy polledFinished(&polled, &rad::ITerminalBackend::finished);
        QVERIFY(polled.terminate().isSuccess());
        QTRY_COMPARE_WITH_TIMEOUT(polledFinished.count(), 1, 15000);
    }

    void processCleanup()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        {
            rad::WinConPtyBackend cancelled({{workspace.path()}});
            QSignalSpy finished(&cancelled, &rad::ITerminalBackend::finished);
            QVERIFY(cancelled.start(launchSpec(workspace.path())).isSuccess());
            QVERIFY(cancelled.terminate().isSuccess());
            QVERIFY(cancelled.terminate().isSuccess());
            QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 15000);
            QVERIFY(!qvariant_cast<rad::ExitStatus>(finished.at(0).at(0)).exitCodeValid);
        }
        for (const bool useDestructor : {false, true})
        {
            auto backend =
                std::make_unique<rad::WinConPtyBackend>(rad::WinConPtyBackend::Options{{workspace.path()}});
            Capture capture(*backend);
            QSignalSpy finished(backend.get(), &rad::ITerminalBackend::finished);
            QVERIFY(backend->start(launchSpec(workspace.path())).isSuccess());
            QTRY_VERIFY_WITH_TIMEOUT(capture.m_bytes.contains("[READY]"), 15000);
            QVERIFY(backend->writeInput("tree\r").isSuccess());
            const QRegularExpression pattern(QStringLiteral("\\[CHILD\\](\\d+)"));
            QRegularExpressionMatch match;
            QTRY_VERIFY_WITH_TIMEOUT((match = pattern.match(QString::fromUtf8(capture.m_bytes))).hasMatch(),
                                     10000);
            ProcessHandle child{OpenProcess(SYNCHRONIZE, FALSE, match.captured(1).toULong())};
            QVERIFY(child.value != nullptr);
            QCOMPARE(WaitForSingleObject(child.value, 0), DWORD(WAIT_TIMEOUT));
            if (useDestructor)
                backend.reset();
            else
            {
                QVERIFY(backend->terminate().isSuccess());
                QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 15000);
            }
            QTRY_COMPARE_WITH_TIMEOUT(WaitForSingleObject(child.value, 0), DWORD(WAIT_OBJECT_0), 15000);
        }
    }
};

QTEST_GUILESS_MAIN(WinConPtyBackendTests)
#include "WinConPtyBackendTests.moc"
