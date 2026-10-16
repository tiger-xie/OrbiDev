#include "ConPtySession.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <conpty.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace rad::poc
{
namespace
{

constexpr std::size_t kOutputCapacity = 1024 * 1024;
constexpr std::size_t kInputCapacity = 256 * 1024;

[[noreturn]] void throwWindowsError(const char *operation, DWORD error = GetLastError())
{
    throw std::system_error(static_cast<int>(error), std::system_category(), operation);
}

class Handle
{
public:
    explicit Handle(HANDLE handle = nullptr) : m_handle(handle)
    {
    }
    ~Handle()
    {
        reset();
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    HANDLE get() const
    {
        return m_handle;
    }
    void reset(HANDLE handle = nullptr) noexcept
    {
        if (m_handle && m_handle != INVALID_HANDLE_VALUE && !CloseHandle(m_handle))
            std::fprintf(stderr, "CloseHandle failed: %lu\n", GetLastError());
        m_handle = handle;
    }

private:
    HANDLE m_handle = nullptr;
};

class PseudoConsole
{
public:
    ~PseudoConsole()
    {
        reset();
    }
    PseudoConsole() = default;
    PseudoConsole(const PseudoConsole &) = delete;
    PseudoConsole &operator=(const PseudoConsole &) = delete;
    HPCON get() const
    {
        return m_console;
    }
    void set(HPCON console)
    {
        m_console = console;
    }
    void reset() noexcept
    {
        if (m_console)
            ConptyClosePseudoConsole(std::exchange(m_console, nullptr));
    }

private:
    HPCON m_console = nullptr;
};

class ProcessAttributes
{
public:
    ProcessAttributes()
    {
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0)
            throwWindowsError("InitializeProcThreadAttributeList(size)");
        m_storage.resize(bytes);
        auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(m_storage.data());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes))
            throwWindowsError("InitializeProcThreadAttributeList");
        m_attributes = attributes;
    }
    ~ProcessAttributes()
    {
        if (m_attributes)
            DeleteProcThreadAttributeList(m_attributes);
    }
    ProcessAttributes(const ProcessAttributes &) = delete;
    ProcessAttributes &operator=(const ProcessAttributes &) = delete;
    LPPROC_THREAD_ATTRIBUTE_LIST get() const
    {
        return m_attributes;
    }

private:
    std::vector<unsigned char> m_storage;
    LPPROC_THREAD_ATTRIBUTE_LIST m_attributes = nullptr;
};

void createPipe(Handle &readEnd, Handle &writeEnd)
{
    HANDLE readHandle = nullptr;
    HANDLE writeHandle = nullptr;
    if (!CreatePipe(&readHandle, &writeHandle, nullptr, 0))
        throwWindowsError("CreatePipe");
    readEnd.reset(readHandle);
    writeEnd.reset(writeHandle);
}

void validateSize(short columns, short rows)
{
    if (columns <= 0 || rows <= 0)
        throw std::invalid_argument("Terminal dimensions must be positive");
}

} // 匿名命名空间

class ConPtySession::Impl
{
public:
    ~Impl()
    {
        cleanup();
    }

    void requireOwner() const
    {
        if (std::this_thread::get_id() != m_owner)
            throw std::logic_error("ConPTY control requires the constructing thread");
    }

    void requireRunning() const
    {
        requireOwner();
        if (!m_process.get() || m_finished)
            throw std::logic_error("ConPTY session is not running");
        const DWORD state = WaitForSingleObject(m_process.get(), 0);
        if (state == WAIT_FAILED)
            throwWindowsError("WaitForSingleObject");
        if (state != WAIT_TIMEOUT)
            throw std::logic_error("ConPTY process has exited");
    }

    void recordError(const char *operation, DWORD error) noexcept
    {
        std::fprintf(stderr, "%s failed: %lu\n", operation, error);
        try
        {
            std::lock_guard lock(m_outputMutex);
            if (m_ioError.empty())
                m_ioError = std::string(operation) + ": " + std::to_string(error);
        }
        catch (...)
        {
            std::fputs("Unable to store ConPTY error diagnostic\n", stderr);
        }
    }

    void readOutput() noexcept
    {
        std::array<char, 8192> buffer{};
        for (;;)
        {
            DWORD count = 0;
            if (!ReadFile(m_outputRead.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &count,
                          nullptr))
            {
                const DWORD error = GetLastError();
                if (error != ERROR_BROKEN_PIPE)
                    recordError("ReadFile", error);
                break;
            }
            if (count == 0)
                break;
            std::lock_guard lock(m_outputMutex);
            // 固定环形存储保证高吞吐读取不因输出无限增长而耗尽内存。
            const std::size_t first = std::min<std::size_t>(count, kOutputCapacity - m_outputPosition);
            std::copy_n(buffer.data(), first, m_output.data() + m_outputPosition);
            std::copy_n(buffer.data() + first, count - first, m_output.data());
            m_outputPosition = (m_outputPosition + count) % kOutputCapacity;
            m_outputSize = std::min(kOutputCapacity, m_outputSize + count);
            m_outputBytes += count;
        }
    }

    void writeQueuedInput() noexcept
    {
        for (;;)
        {
            std::string bytes;
            {
                std::unique_lock lock(m_inputMutex);
                m_inputReady.wait(lock, [this] { return m_stopping || !m_input.empty(); });
                if (m_stopping)
                    break;
                bytes = std::move(m_input.front());
                m_input.pop_front();
            }
            std::size_t offset = 0;
            while (offset < bytes.size() && !m_stopping)
            {
                DWORD written = 0;
                if (!WriteFile(m_inputWrite.get(), bytes.data() + offset,
                               static_cast<DWORD>(bytes.size() - offset), &written, nullptr))
                {
                    const DWORD error = GetLastError();
                    if (!m_stopping)
                        recordError("WriteFile", error);
                    m_stopping = true;
                    break;
                }
                if (written == 0)
                {
                    recordError("WriteFile(zero bytes)", ERROR_WRITE_FAULT);
                    m_stopping = true;
                    break;
                }
                offset += written;
            }
            std::lock_guard lock(m_inputMutex);
            m_pendingBytes -= bytes.size();
        }
    }

    void stopWriter() noexcept
    {
        {
            std::lock_guard lock(m_inputMutex);
            m_stopping = true;
        }
        m_inputReady.notify_all();
        if (m_writer.joinable())
        {
            // 写线程可能在停止标记检查后才进入 WriteFile，因此反复取消到线程退出。
            const HANDLE thread = m_writer.native_handle();
            while (WaitForSingleObject(thread, 10) == WAIT_TIMEOUT)
            {
                if (!CancelSynchronousIo(thread))
                {
                    const DWORD error = GetLastError();
                    if (error != ERROR_NOT_FOUND)
                        recordError("CancelSynchronousIo", error);
                }
            }
            m_writer.join();
        }
        m_inputWrite.reset();
        std::lock_guard lock(m_inputMutex);
        m_input.clear();
        m_pendingBytes = 0;
    }

    void cleanup() noexcept
    {
        if (m_process.get() && !m_finished)
        {
            // 启动失败时进程可能尚未加入 Job；显式兜底终止悬挂进程。
            const DWORD state = WaitForSingleObject(m_process.get(), 0);
            if (state == WAIT_TIMEOUT && !TerminateProcess(m_process.get(), 1))
                recordError("TerminateProcess(cleanup)", GetLastError());
        }
        m_job.reset();
        stopWriter();
        // ConptyClosePseudoConsole 会同步写出最后一帧，必须让读线程继续排空。
        m_console.reset();
        if (m_reader.joinable())
            m_reader.join();
        m_outputRead.reset();
        m_process.reset();
    }

    void finish()
    {
        if (!GetExitCodeProcess(m_process.get(), &m_exitCode))
            throwWindowsError("GetExitCodeProcess");
        m_finished = true;
        cleanup();
    }

    const std::thread::id m_owner = std::this_thread::get_id();
    Handle m_inputWrite;
    Handle m_outputRead;
    Handle m_process;
    Handle m_job;
    PseudoConsole m_console;
    std::thread m_reader;
    std::thread m_writer;
    bool m_attempted = false;
    bool m_finished = false;
    DWORD m_exitCode = STILL_ACTIVE;
    std::atomic<bool> m_stopping{false};
    std::mutex m_inputMutex;
    std::condition_variable m_inputReady;
    std::deque<std::string> m_input;
    std::size_t m_pendingBytes = 0;
    mutable std::mutex m_outputMutex;
    std::array<char, kOutputCapacity> m_output{};
    std::size_t m_outputPosition = 0;
    std::size_t m_outputSize = 0;
    std::uint64_t m_outputBytes = 0;
    std::string m_ioError;
};

ConPtySession::ConPtySession() : m_impl(std::make_unique<Impl>())
{
}
ConPtySession::~ConPtySession() = default;

void ConPtySession::start(const std::filesystem::path &executable,
                          const std::filesystem::path &workingDirectory, short columns, short rows)
{
    m_impl->requireOwner();
    if (m_impl->m_attempted)
        throw std::logic_error("A ConPTY session can only be started once");
    m_impl->m_attempted = true;
    validateSize(columns, rows);
    const auto program = std::filesystem::canonical(executable);
    const auto directory = std::filesystem::canonical(workingDirectory);
    if (!std::filesystem::is_regular_file(program) || !std::filesystem::is_directory(directory) ||
        !std::filesystem::equivalent(program.parent_path(), directory) ||
        _wcsicmp(program.extension().c_str(), L".exe") != 0)
        throw std::invalid_argument("POC requires an EXE located directly in its working directory");
    const std::wstring application = program.wstring();
    if (application.find(L'\"') != std::wstring::npos || application.find(L'\0') != std::wstring::npos)
        throw std::invalid_argument("Invalid executable path");
    std::wstring commandLine = L"\"" + application + L"\"";

    try
    {
        Handle inputRead;
        Handle outputWrite;
        createPipe(inputRead, m_impl->m_inputWrite);
        createPipe(m_impl->m_outputRead, outputWrite);
        HPCON console = nullptr;
        const HRESULT result =
            ConptyCreatePseudoConsole({columns, rows}, inputRead.get(), outputWrite.get(), 0, &console);
        if (FAILED(result))
            throwWindowsError("ConptyCreatePseudoConsole", static_cast<DWORD>(result));
        m_impl->m_console.set(console);
        m_impl->m_reader = std::thread([this] { m_impl->readOutput(); });
        ProcessAttributes attributes;
        if (!UpdateProcThreadAttribute(attributes.get(), 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, console,
                                       sizeof(console), nullptr, nullptr))
            throwWindowsError("UpdateProcThreadAttribute");
        m_impl->m_job.reset(CreateJobObjectW(nullptr, nullptr));
        if (!m_impl->m_job.get())
            throwWindowsError("CreateJobObjectW");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(m_impl->m_job.get(), JobObjectExtendedLimitInformation, &limits,
                                     sizeof(limits)))
            throwWindowsError("SetInformationJobObject");
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        // 阻止 Windows 复制父进程重定向的标准句柄；空句柄由子进程连接 ConPTY 时填充。
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.lpAttributeList = attributes.get();
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(application.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                            EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED, nullptr, directory.c_str(),
                            &startup.StartupInfo, &process))
            throwWindowsError("CreateProcessW");
        m_impl->m_process.reset(process.hProcess);
        Handle primaryThread(process.hThread);
        inputRead.reset();
        outputWrite.reset();
        if (!AssignProcessToJobObject(m_impl->m_job.get(), m_impl->m_process.get()))
            throwWindowsError("AssignProcessToJobObject");
        m_impl->m_writer = std::thread([this] { m_impl->writeQueuedInput(); });
        if (ResumeThread(primaryThread.get()) == static_cast<DWORD>(-1))
            throwWindowsError("ResumeThread");
    }
    catch (...)
    {
        m_impl->cleanup();
        throw;
    }
}

void ConPtySession::writeInput(std::string bytes)
{
    m_impl->requireRunning();
    std::lock_guard lock(m_impl->m_inputMutex);
    if (m_impl->m_stopping)
        throw std::runtime_error("ConPTY input channel has stopped");
    if (bytes.size() > kInputCapacity - m_impl->m_pendingBytes)
        throw std::length_error("ConPTY input queue exceeds 256 KiB");
    if (bytes.empty())
        return;
    const auto count = bytes.size();
    m_impl->m_input.push_back(std::move(bytes));
    m_impl->m_pendingBytes += count;
    m_impl->m_inputReady.notify_one();
}

void ConPtySession::resize(short columns, short rows)
{
    m_impl->requireRunning();
    validateSize(columns, rows);
    const HRESULT result = ConptyResizePseudoConsole(m_impl->m_console.get(), {columns, rows});
    if (FAILED(result))
        throwWindowsError("ConptyResizePseudoConsole", static_cast<DWORD>(result));
}

bool ConPtySession::waitForExit(std::chrono::milliseconds timeout)
{
    m_impl->requireOwner();
    if (timeout.count() < 0 || timeout.count() >= INFINITE)
        throw std::invalid_argument("Wait timeout must fit a finite Windows wait interval");
    if (m_impl->m_finished)
        return true;
    if (!m_impl->m_process.get())
        throw std::logic_error("ConPTY session has not started successfully");
    const DWORD state = WaitForSingleObject(m_impl->m_process.get(), static_cast<DWORD>(timeout.count()));
    if (state == WAIT_TIMEOUT)
        return false;
    if (state != WAIT_OBJECT_0)
        throwWindowsError("WaitForSingleObject");
    m_impl->finish();
    return true;
}

unsigned long ConPtySession::exitCode() const
{
    m_impl->requireOwner();
    if (!m_impl->m_finished)
        throw std::logic_error("Exit code requires completed waitForExit or terminate");
    return m_impl->m_exitCode;
}

void ConPtySession::terminate()
{
    m_impl->requireOwner();
    if (m_impl->m_finished || !m_impl->m_process.get())
        return;
    if (!TerminateJobObject(m_impl->m_job.get(), 1))
        throwWindowsError("TerminateJobObject");
    if (WaitForSingleObject(m_impl->m_process.get(), 5000) != WAIT_OBJECT_0)
        throw std::runtime_error("ConPTY process did not terminate within five seconds");
    m_impl->finish();
}

std::string ConPtySession::outputSnapshot() const
{
    std::lock_guard lock(m_impl->m_outputMutex);
    const auto start = (m_impl->m_outputPosition + kOutputCapacity - m_impl->m_outputSize) % kOutputCapacity;
    const auto first = std::min(m_impl->m_outputSize, kOutputCapacity - start);
    std::string output(m_impl->m_output.data() + start, first);
    output.append(m_impl->m_output.data(), m_impl->m_outputSize - first);
    return output;
}

std::uint64_t ConPtySession::outputBytes() const
{
    std::lock_guard lock(m_impl->m_outputMutex);
    return m_impl->m_outputBytes;
}

std::string ConPtySession::ioError() const
{
    std::lock_guard lock(m_impl->m_outputMutex);
    return m_impl->m_ioError;
}

} // 命名空间 rad::poc
