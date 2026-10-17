#include "WinConPtyBackend.h"
#include "ConPtyProcess.h"
#include <QDir>
#include <QFileInfo>
#include <QThread>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <thread>

namespace rad
{
namespace
{
OperationResult reject(ErrorCode code, const char *message)
{
    return OperationResult::failure({code, QString::fromUtf8(message)});
}
bool lexicalPath(const QString &path)
{
    // 拒绝设备命名空间、相对路径与备用数据流，避免绕过规范路径边界。
    const QString normalized = QDir::fromNativeSeparators(path);
    return normalized.size() > 3 && normalized[0].isLetter() && normalized[1] == QLatin1Char(':') &&
           normalized[2] == QLatin1Char('/') && !normalized.contains(QChar::Null) &&
           !normalized.contains(QLatin1Char('"')) && !normalized.mid(2).contains(QLatin1Char(':'));
}
// 打开后按句柄解析 junction/symlink，并在启动期间禁止删除或替换目标。
class CheckedPath
{
public:
    CheckedPath(const QString &path, bool directory)
    {
        if (!lexicalPath(path))
            return;
        m_handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL, nullptr);
        if (m_handle == INVALID_HANDLE_VALUE)
            return;
        BY_HANDLE_FILE_INFORMATION information{};
        if (!GetFileInformationByHandle(m_handle, &information) ||
            ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory)
            return;
        const DWORD required = GetFinalPathNameByHandleW(m_handle, nullptr, 0, FILE_NAME_NORMALIZED);
        if (!required)
            return;
        std::wstring canonical(required, L'\0');
        const DWORD length =
            GetFinalPathNameByHandleW(m_handle, canonical.data(), required, FILE_NAME_NORMALIZED);
        if (!length || length >= required)
            return;
        canonical.resize(length);
        m_path = QDir::fromNativeSeparators(QString::fromStdWString(canonical));
        if (m_path.startsWith(QStringLiteral("//?/")))
            m_path.remove(0, 4);
        if (!lexicalPath(m_path))
            m_path.clear();
    }
    ~CheckedPath()
    {
        if (m_handle != INVALID_HANDLE_VALUE)
            CloseHandle(m_handle);
    }
    CheckedPath(const CheckedPath &) = delete;
    CheckedPath &operator=(const CheckedPath &) = delete;
    const QString &path() const
    {
        return m_path;
    }

private:
    HANDLE m_handle = INVALID_HANDLE_VALUE;
    QString m_path;
};
bool containsDirectory(const QString &root, const QString &directory)
{
    // 最终路径按大小写精确匹配，避免 NTFS 大小写敏感目录的同名兄弟目录越权。
    return !root.isEmpty() &&
           (root.compare(directory, Qt::CaseSensitive) == 0 ||
            directory.startsWith(root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/'),
                                 Qt::CaseSensitive));
}
}

class WinConPtyBackend::Impl
{
public:
    Impl(WinConPtyBackend *owner, Options options) : m_owner(owner), m_options(std::move(options))
    {
    }
    void assertOwner() const
    {
        Q_ASSERT(QThread::currentThread() == m_owner->thread());
    }
    void postError(ErrorCode code, const char *message, bool fatal, const std::exception *exception = nullptr)
    {
        Error error{code, QString::fromUtf8(message)};
        // 仅传递数值错误码，绝不把可能含参数或凭据的 what() 带到上层。
        if (const auto *systemError = dynamic_cast<const std::system_error *>(exception))
            error.message += QStringLiteral(" (Windows: %1)").arg(systemError->code().value());
        QMetaObject::invokeMethod(
            m_owner,
            [this, error, fatal]
            {
                if (fatal)
                    m_state = SessionStatus::Failed;
                emit m_owner->failed(error, fatal);
            },
            Qt::QueuedConnection);
    }
    void collect(windows::ConPtyProcess &process)
    {
        auto delta = process.outputSince(m_cursor);
        if (delta.bytes.empty() && delta.droppedBytes == 0)
            return;
        bool notify = false;
        {
            std::lock_guard lock(m_mutex);
            m_output.append(delta.bytes.data(), static_cast<int>(delta.bytes.size()));
            m_dropped += delta.droppedBytes;
            const int excess = m_output.size() - m_options.outputCapacityBytes;
            if (excess > 0)
            {
                m_output.remove(0, excess);
                m_dropped += static_cast<quint64>(excess);
            }
            if (!m_notified)
            {
                m_notified = true;
                if (!m_notificationPending)
                {
                    m_notificationPending = true;
                    notify = true;
                }
            }
        }
        if (notify)
            QMetaObject::invokeMethod(
                m_owner,
                [this]
                {
                    bool available = false;
                    {
                        std::lock_guard lock(m_mutex);
                        m_notificationPending = false;
                        available = m_notified;
                    }
                    if (available)
                        emit m_owner->outputAvailable();
                },
                Qt::QueuedConnection);
    }
    void run(LaunchSpec spec)
    {
        windows::ConPtyProcess process;
        bool launched = false;
        bool fatal = false;
        ExitStatus status;
        try
        {
            CheckedPath workingDirectory(spec.workingDirectory, true);
            const QString directory = workingDirectory.path();
            bool allowed = false;
            std::vector<std::unique_ptr<CheckedPath>> roots;
            for (const QString &root : m_options.allowedWorkspaceRoots)
            {
                roots.push_back(std::make_unique<CheckedPath>(root, true));
                allowed = allowed || containsDirectory(roots.back()->path(), directory);
            }
            if (!allowed || directory.isEmpty())
            {
                postError(ErrorCode::PermissionDenied, "工作目录不在授权白名单内或不可访问", true);
                fatal = true;
            }
            else
            {
                CheckedPath executable(spec.executablePath, false);
                const QString program = executable.path();
                if (program.isEmpty() ||
                    QFileInfo(program).suffix().compare(QStringLiteral("exe"), Qt::CaseInsensitive) != 0)
                {
                    postError(ErrorCode::LaunchFailed, "受信任 EXE 不存在或不可执行", true);
                    fatal = true;
                }
                else if (!m_cancel)
                {
                    std::vector<std::wstring> arguments;
                    for (const auto &argument : spec.arguments)
                        arguments.push_back(argument.toStdWString());
                    process.startValidated(std::filesystem::path(program.toStdWString()),
                                           std::filesystem::path(directory.toStdWString()), arguments,
                                           static_cast<short>(spec.size.columns),
                                           static_cast<short>(spec.size.rows));
                    launched = true;
                    {
                        std::lock_guard lock(m_mutex);
                        m_process = &process;
                    }
                    QMetaObject::invokeMethod(
                        m_owner,
                        [this]
                        {
                            if (m_state == SessionStatus::Creating)
                                m_state = SessionStatus::Running;
                            emit m_owner->started();
                        },
                        Qt::QueuedConnection);
                }
            }
            while (launched)
            {
                if (m_cancel)
                {
                    disconnectInput();
                    process.terminate();
                    break;
                }
                std::optional<TerminalSize> size;
                {
                    std::lock_guard lock(m_mutex);
                    size = std::exchange(m_resize, std::nullopt);
                }
                if (size)
                {
                    try
                    {
                        process.resize(static_cast<short>(size->columns), static_cast<short>(size->rows));
                    }
                    catch (const std::exception &exception)
                    {
                        postError(ErrorCode::ResizeFailed, "终端尺寸更新失败", false, &exception);
                    }
                }
                collect(process);
                if (!process.ioError().empty())
                {
                    postError(ErrorCode::InputFailed, "终端输入输出通道失败", true);
                    fatal = true;
                    disconnectInput();
                    process.terminate();
                    break;
                }
                // 等待由本会话独立执行，不占用 Agent 控制线程或其他会话。
                if (process.waitForExit(std::chrono::milliseconds(0)))
                {
                    disconnectInput();
                    status.exitCode = static_cast<int>(process.exitCode());
                    status.exitCodeValid = true;
                    break;
                }
                std::unique_lock lock(m_mutex);
                m_ready.wait_for(lock, std::chrono::milliseconds(20),
                                 [this] { return m_cancel || m_resize.has_value(); });
            }
        }
        catch (const std::exception &exception)
        {
            postError(launched ? ErrorCode::SystemError : ErrorCode::LaunchFailed,
                      "Windows 终端操作失败，正在回收会话资源", true, &exception);
            fatal = true;
            disconnectInput();
            try
            {
                process.terminate();
            }
            catch (const std::exception &)
            {
                postError(ErrorCode::SystemError, "显式收尾失败，资源所有者执行兜底回收", false);
            }
        }
        disconnectInput();
        // 即使显式 terminate 抛错，也先兜底关闭并排空读线程，再发布尾部和终态。
        process.close();
        if (launched)
        {
            collect(process);
            if (!fatal && !process.ioError().empty())
            {
                fatal = true;
                postError(ErrorCode::SystemError, "终端收尾发生输入输出错误", true);
            }
        }
        QMetaObject::invokeMethod(
            m_owner,
            [this, status, fatal]
            {
                m_state = fatal ? SessionStatus::Failed : SessionStatus::Exited;
                emit m_owner->finished(status);
            },
            Qt::QueuedConnection);
    }
    void disconnectInput()
    {
        std::lock_guard lock(m_mutex);
        m_process = nullptr;
    }
    WinConPtyBackend *m_owner;
    Options m_options;
    SessionStatus m_state = SessionStatus::Creating;
    bool m_attempted = false;
    std::atomic<bool> m_cancel{false};
    std::thread m_worker;
    std::mutex m_mutex;
    std::condition_variable m_ready;
    windows::ConPtyProcess *m_process = nullptr;
    std::optional<TerminalSize> m_resize;
    QByteArray m_output;
    quint64 m_dropped = 0;
    std::uint64_t m_cursor = 0;
    bool m_notified = false;
    bool m_notificationPending = false;
};
WinConPtyBackend::WinConPtyBackend(QObject *parent) : WinConPtyBackend(Options{}, parent)
{
}
WinConPtyBackend::WinConPtyBackend(Options options, QObject *parent)
    : ITerminalBackend(parent), m_impl(std::make_unique<Impl>(this, std::move(options)))
{
}
WinConPtyBackend::~WinConPtyBackend()
{
    m_impl->assertOwner();
    m_impl->m_cancel = true;
    m_impl->m_ready.notify_all();
    // 正常调用方先等 finished；异常析构仍等待自己的工作线程，以免回调悬空。
    if (m_impl->m_worker.joinable())
        m_impl->m_worker.join();
}
OperationResult WinConPtyBackend::start(const LaunchSpec &spec)
{
    m_impl->assertOwner();
    if (m_impl->m_attempted)
        return reject(ErrorCode::InvalidState, "终端实例不能重复启动");
    const auto dimensions = spec.size.validate();
    if (!dimensions.isSuccess())
        return dimensions;
    if (!lexicalPath(spec.executablePath) || !lexicalPath(spec.workingDirectory))
        return reject(ErrorCode::InvalidArgument, "启动路径必须为有效绝对路径");
    for (const auto &argument : spec.arguments)
        if (argument.contains(QChar::Null))
            return reject(ErrorCode::InvalidArgument, "参数不能包含空字符");
    if (m_impl->m_options.inputCapacityBytes <= 0 || m_impl->m_options.outputCapacityBytes <= 0 ||
        m_impl->m_options.outputCapacityBytes > 1024 * 1024)
        return reject(ErrorCode::InvalidArgument, "终端队列容量无效");
    try
    {
        m_impl->m_worker = std::thread([this, spec] { m_impl->run(spec); });
    }
    catch (const std::exception &)
    {
        return reject(ErrorCode::SystemError, "无法创建终端工作线程");
    }
    m_impl->m_attempted = true;
    return accepted();
}
OperationResult WinConPtyBackend::writeInput(const QByteArray &bytes)
{
    m_impl->assertOwner();
    if (!m_impl->m_attempted || m_impl->m_state != SessionStatus::Running)
        return reject(ErrorCode::InvalidState, "终端尚未运行或已结束");
    std::lock_guard lock(m_impl->m_mutex);
    if (!m_impl->m_process)
        return reject(ErrorCode::InvalidState, "终端进程已结束");
    try
    {
        m_impl->m_process->enqueueInput(
            std::string(bytes.constData(), static_cast<std::size_t>(bytes.size())),
            static_cast<std::size_t>(m_impl->m_options.inputCapacityBytes));
    }
    catch (const std::length_error &)
    {
        return reject(ErrorCode::QueueFull, "终端输入队列已满");
    }
    catch (const std::exception &)
    {
        return reject(ErrorCode::InputFailed, "终端输入通道不可用");
    }
    return accepted();
}
OperationResult WinConPtyBackend::resize(TerminalSize size)
{
    m_impl->assertOwner();
    if (!m_impl->m_attempted || m_impl->m_state != SessionStatus::Running)
        return reject(ErrorCode::InvalidState, "终端尚未运行或已结束");
    const auto result = size.validate();
    if (!result.isSuccess())
        return result;
    {
        std::lock_guard lock(m_impl->m_mutex);
        m_impl->m_resize = size;
    }
    m_impl->m_ready.notify_one();
    return accepted();
}
OperationResult WinConPtyBackend::terminate()
{
    m_impl->assertOwner();
    if (!m_impl->m_attempted || m_impl->m_state == SessionStatus::Exited ||
        m_impl->m_state == SessionStatus::Failed)
        return accepted();
    m_impl->m_state = SessionStatus::Exiting;
    m_impl->m_cancel = true;
    m_impl->m_ready.notify_one();
    return accepted();
}
Result<OutputChunk> WinConPtyBackend::takeOutput(int maxBytes)
{
    m_impl->assertOwner();
    if (maxBytes <= 0)
        return Result<OutputChunk>::failure(
            {ErrorCode::InvalidArgument, QStringLiteral("输出读取长度必须为正")});
    std::lock_guard lock(m_impl->m_mutex);
    OutputChunk chunk;
    chunk.bytes = m_impl->m_output.left(maxBytes);
    m_impl->m_output.remove(0, chunk.bytes.size());
    chunk.droppedBytes = std::exchange(m_impl->m_dropped, 0);
    chunk.hasMore = !m_impl->m_output.isEmpty();
    if (!chunk.hasMore)
        m_impl->m_notified = false;
    return Result<OutputChunk>::success(std::move(chunk));
}
}
