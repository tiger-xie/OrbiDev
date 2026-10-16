#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#ifdef RAD_CONPTY_RUNTIME
#include <conpty.h>
#endif

#include <cstdio>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace
{

// 同一组严格断言分别构建为系统诊断与运行库验收，绝不混用同一个 HPCON 的 API。
#ifdef RAD_CONPTY_RUNTIME
constexpr auto createConsole = ConptyCreatePseudoConsole;
constexpr auto closeConsole = ConptyClosePseudoConsole;
#else
constexpr auto createConsole = CreatePseudoConsole;
constexpr auto closeConsole = ClosePseudoConsole;
#endif

[[noreturn]] void fail(const char *operation, DWORD error = GetLastError())
{
    throw std::system_error(static_cast<int>(error), std::system_category(), operation);
}

class Handle
{
public:
    Handle() = default;
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
    HANDLE *address()
    {
        return &m_handle;
    }
    void reset() noexcept
    {
        if (m_handle && !CloseHandle(std::exchange(m_handle, nullptr)))
            std::fprintf(stderr, "CloseHandle failed: %lu\n", GetLastError());
    }

private:
    HANDLE m_handle = nullptr;
};

class PseudoConsole
{
public:
    explicit PseudoConsole(Handle &outputRead) : m_outputRead(outputRead)
    {
    }
    ~PseudoConsole()
    {
        // 官方允许先关闭输出管道再关闭终端，避免旧 Windows 同步刷新最后一帧时阻塞。
        // 此探针不启动子进程、不消费输出，因此无需引入读取线程和额外线程句柄。
        m_outputRead.reset();
        if (m_console)
            closeConsole(m_console);
    }
    PseudoConsole(const PseudoConsole &) = delete;
    PseudoConsole &operator=(const PseudoConsole &) = delete;
    HPCON *address()
    {
        return &m_console;
    }

private:
    Handle &m_outputRead;
    HPCON m_console = nullptr;
};

void createAndCloseConsole()
{
    Handle inputRead;
    Handle inputWrite;
    Handle outputRead;
    Handle outputWrite;
    PseudoConsole console(outputRead);
    if (!CreatePipe(inputRead.address(), inputWrite.address(), nullptr, 0))
        fail("CreatePipe(input)");
    if (!CreatePipe(outputRead.address(), outputWrite.address(), nullptr, 0))
        fail("CreatePipe(output)");
    const HRESULT result = createConsole({80, 25}, inputRead.get(), outputWrite.get(), 0, console.address());
    if (FAILED(result))
        fail("CreatePseudoConsole", static_cast<DWORD>(result));
    // 不建立进程或属性列表；关闭本地副本后，析构只通过公共 API 释放 HPCON。
    inputRead.reset();
    outputWrite.reset();
    inputWrite.reset();
}

DWORD handleCount()
{
    DWORD count = 0;
    if (!GetProcessHandleCount(GetCurrentProcess(), &count))
        fail("GetProcessHandleCount");
    return count;
}

} // 匿名命名空间

int main()
{
    try
    {
        // 提前完成标准输出及系统组件的首次初始化，避免把固定初始化开销当成逐次泄漏。
#ifdef RAD_CONPTY_RUNTIME
        std::puts("ConPTY official runtime probe; no child process, no session backend");
#else
        std::puts("ConPTY native OS probe; no child process, no session backend");
#endif
        createAndCloseConsole();
        const DWORD baseline = handleCount();
        bool grew = false;
        for (int index = 0; index < 12; ++index)
        {
            const DWORD before = handleCount();
            createAndCloseConsole();
            const DWORD after = handleCount();
            std::printf("cycle=%d before=%lu after=%lu\n", index + 1, before, after);
            grew = grew || after > before;
        }
        const DWORD finalCount = handleCount();
        std::printf("baseline=%lu final=%lu\n", baseline, finalCount);
        if (grew || finalCount > baseline)
        {
            std::puts("ConPTY_HANDLE_GROWTH");
            return 1;
        }
        std::puts("ConPTY_HANDLE_STABLE");
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "ConPTY_PROBE_ERROR: %s\n", error.what());
        return 2;
    }
}
