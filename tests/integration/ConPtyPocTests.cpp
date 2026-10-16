#include "ConPtySession.h"

#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using rad::poc::ConPtySession;

namespace
{

void require(bool condition, const std::string &message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

std::filesystem::path executableDirectory()
{
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    require(length > 0 && length < path.size(), "GetModuleFileNameW failed");
    path.resize(length);
    return std::filesystem::path(path).parent_path();
}

void waitForText(const ConPtySession &session, const std::string &expected,
                 std::chrono::milliseconds timeout = 10s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        require(session.ioError().empty(), "I/O error: " + session.ioError());
        if (session.outputSnapshot().find(expected) != std::string::npos)
        {
            return;
        }
        std::this_thread::sleep_for(10ms);
    }
    throw std::runtime_error("Missing marker: " + expected +
                             "\nTail: " + session.outputSnapshot().substr(0, 2000));
}

void start(ConPtySession &session)
{
    const auto directory = executableDirectory();
    session.start(directory / L"FakeCli.exe", directory);
    waitForText(session, "[READY]");
}

void finish(ConPtySession &session)
{
    session.writeInput("exit\r");
    require(session.waitForExit(10s), "Normal exit timed out");
    require(session.exitCode() == 0, "Unexpected normal exit code");
    require(session.outputSnapshot().find("[BYE]") != std::string::npos,
            "Final output was lost during close");
    require(session.ioError().empty(), "Unexpected I/O error: " + session.ioError());
}

void expectFailure(const std::function<void()> &operation)
{
    bool rejected = false;
    try
    {
        operation();
    }
    catch (const std::exception &)
    {
        rejected = true;
    }
    require(rejected, "Invalid operation unexpectedly succeeded");
}

void interaction()
{
    ConPtySession session;
    start(session);
    require(!session.waitForExit(1ms), "Live child unexpectedly exited");
    session.writeInput("echo 中文往返验证\r");
    waitForText(session, "[ECHO]中文往返验证");
    session.writeInput("ansi\r");
    waitForText(session, "[COLOR]绿色输出");
    require(session.outputSnapshot().find("\x1b[32m[COLOR]绿色输出") != std::string::npos,
            "Green SGR sequence is not associated with the color fixture");
    session.writeInput("\x1b[A");
    waitForText(session, "[UP]");
    session.writeInput(std::string(1, '\x03'));
    waitForText(session, "[CTRL_C]");
    session.resize(90, 24);
    // 尺寸变更可能先于控制台内部事件完成，轮询子进程查询而不假定固定延迟。
    for (int attempt = 0; attempt < 50; ++attempt)
    {
        session.writeInput("size\r");
        std::this_thread::sleep_for(40ms);
        if (session.outputSnapshot().find("[SIZE]90x24") != std::string::npos)
        {
            break;
        }
    }
    waitForText(session, "[SIZE]90x24");
    finish(session);
}

void throughput()
{
    ConPtySession session;
    start(session);
    session.writeInput("flood\r");
    waitForText(session, "[FLOOD_DONE]", 45s);
    require(session.outputBytes() > 1024 * 1024, "Stress stream did not exceed 1 MiB");
    require(session.outputSnapshot().size() <= 1024 * 1024, "Output retention exceeded its bound");
    session.writeInput("echo after-flood\r");
    waitForText(session, "[ECHO]after-flood");
    finish(session);
}

void shutdown()
{
    {
        ConPtySession session;
        start(session);
        session.writeInput("fail\r");
        require(session.waitForExit(10s), "Abnormal exit timed out");
        require(session.exitCode() == 23, "Abnormal exit code not preserved");
    }
    {
        ConPtySession session;
        start(session);
        session.writeInput("pause\r");
        waitForText(session, "[PAUSED]");
        // 子进程不读输入时灌满管道，验证终止能取消阻塞写入。
        for (int index = 0; index < 32; ++index)
        {
            try
            {
                session.writeInput(std::string(8192, 'x'));
            }
            catch (const std::length_error &)
            {
                break;
            }
        }
        const auto began = std::chrono::steady_clock::now();
        session.terminate();
        require(session.waitForExit(5s), "Terminate did not stop child");
        require(std::chrono::steady_clock::now() - began < 8s, "Blocked writer prevented prompt shutdown");
    }
    {
        ConPtySession session;
        start(session);
        session.writeInput("stream\r");
        waitForText(session, "[ROW]");
        // 正在输出时销毁对象，覆盖排空输出与关闭伪控制台的顺序。
    }
}

void validation()
{
    const auto directory = executableDirectory();
    ConPtySession notStarted;
    expectFailure([&] { notStarted.writeInput("echo invalid\r"); });
    expectFailure([&] { notStarted.resize(90, 24); });
    {
        ConPtySession session;
        expectFailure([&] { session.start(directory / L"absent.exe", directory); });
    }
    {
        ConPtySession session;
        expectFailure([&] { session.start(directory / L"InvalidProgram.exe", directory); });
    }
    {
        ConPtySession session;
        expectFailure([&] { session.start(directory / L"FakeCli.exe", directory, 0, 24); });
    }
    {
        ConPtySession session;
        expectFailure([&] { session.start(directory / L"FakeCli.exe", directory.parent_path()); });
    }
    ConPtySession session;
    start(session);
    expectFailure([&] { session.start(directory / L"FakeCli.exe", directory); });
    expectFailure([&] { session.resize(-1, 24); });
    expectFailure([&] { session.writeInput(std::string(2 * 1024 * 1024, 'x')); });
    finish(session);
}

DWORD handleCount()
{
    DWORD count = 0;
    require(GetProcessHandleCount(GetCurrentProcess(), &count) != FALSE, "Handle counting failed");
    return count;
}

void lifecycle()
{
    {
        ConPtySession warmup;
        start(warmup);
        finish(warmup);
    }
    const DWORD before = handleCount();
    for (int index = 0; index < 12; ++index)
    {
        ConPtySession session;
        start(session);
        if (index % 3 == 0)
        {
            finish(session);
        }
        else if (index % 3 == 1)
        {
            session.terminate();
        }
    }
    const DWORD after = handleCount();
    std::cout << "handles before=" << before << " after=" << after << '\n';
    require(after <= before, "Handle count increased after lifecycle loop");
}

} // 匿名命名空间

int main(int argc, char *argv[])
{
    try
    {
        require(argc == 2, "Usage: ConPtyPoc interaction|throughput|shutdown|validation|lifecycle");
        const std::string scenario = argv[1];
        if (scenario == "interaction")
            interaction();
        else if (scenario == "throughput")
            throughput();
        else if (scenario == "shutdown")
            shutdown();
        else if (scenario == "validation")
            validation();
        else if (scenario == "lifecycle")
            lifecycle();
        else
            throw std::invalid_argument("Unknown test scenario");
        std::cout << "PASS " << scenario << '\n';
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
