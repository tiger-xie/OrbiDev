#include <Windows.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{

void checkWin32(BOOL success, const char *operation)
{
    if (!success)
    {
        throw std::runtime_error(std::string(operation) + ": " + std::to_string(GetLastError()));
    }
}

void writeOutput(std::string_view text)
{
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    while (!text.empty())
    {
        DWORD written = 0;
        checkWin32(WriteFile(output, text.data(), static_cast<DWORD>(text.size()), &written, nullptr),
                   "WriteFile");
        if (written == 0)
        {
            throw std::runtime_error("WriteFile made no progress");
        }
        text.remove_prefix(written);
    }
}

void configureConsole(bool interactive = false)
{
    checkWin32(SetConsoleCP(CP_UTF8), "SetConsoleCP");
    checkWin32(SetConsoleOutputCP(CP_UTF8), "SetConsoleOutputCP");
    // 交互模式由远端控制台完成回显与行编辑，避免客户端重复显示或自行计算中文宽度。
    const DWORD inputMode =
        interactive ? ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_INSERT_MODE | ENABLE_PROCESSED_INPUT
                    : ENABLE_VIRTUAL_TERMINAL_INPUT;
    checkWin32(SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), inputMode), "SetConsoleMode(input)");
    checkWin32(SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), ENABLE_PROCESSED_OUTPUT |
                                                                   ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                                                                   ENABLE_WRAP_AT_EOL_OUTPUT),
               "SetConsoleMode(output)");
}

void spawnTestChild()
{
    std::wstring executable(32768, L'\0');
    const DWORD length =
        GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    checkWin32(length != 0 && length < executable.size(), "GetModuleFileNameW");
    executable.resize(length);
    std::wstring command = L"\"" + executable + L"\" --sleep-child";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    checkWin32(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                              nullptr, nullptr, &startup, &child),
               "CreateProcessW(child)");
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    writeOutput("[CHILD]" + std::to_string(child.dwProcessId) + "\r\n");
}

std::optional<int> execute(std::string_view command, bool interactive = false)
{
    if (command.starts_with("echo "))
    {
        writeOutput("[ECHO]" + std::string(command.substr(5)) + "\r\n");
    }
    else if (command == "ansi")
    {
        writeOutput("\x1b[32m[COLOR]绿色输出\x1b[0m\r\n");
    }
    else if (command == "size")
    {
        CONSOLE_SCREEN_BUFFER_INFO screen{};
        checkWin32(GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &screen),
                   "GetConsoleScreenBufferInfo");
        const int columns = screen.srWindow.Right - screen.srWindow.Left + 1;
        const int rows = screen.srWindow.Bottom - screen.srWindow.Top + 1;
        writeOutput("[SIZE]" + std::to_string(columns) + "x" + std::to_string(rows) + "\r\n");
    }
    else if (command == "flood" || command == "stream")
    {
        for (std::uint64_t index = 0; command == "stream" || index < 24000; ++index)
        {
            writeOutput("[ROW]" + std::to_string(index) + ":" + std::string(80, 'x') + "\r\n");
        }
        writeOutput("[FLOOD_DONE]\r\n");
    }
    else if (command == "pause")
    {
        writeOutput("[PAUSED]\r\n");
        Sleep(30000);
        writeOutput("[RESUMED]\r\n");
    }
    else if (command == "tree")
    {
        spawnTestChild();
    }
    else if (command.starts_with("flood "))
    {
        const std::string marker(command.substr(6));
        for (int index = 0; index < 24000; ++index)
        {
            writeOutput("[" + marker + "]" + std::to_string(index) + ":" + std::string(80, 'x') + "\r\n");
        }
        writeOutput("[DONE:" + marker + "]\r\n");
    }
    else if (command == "fail-tail")
    {
        writeOutput("[FAIL_TAIL]中文尾部\r\n");
        return 23;
    }
    else if (command == "exit")
    {
        writeOutput("[BYE]\r\n");
        return 0;
    }
    else if (command == "fail")
    {
        // 用确定的非零退出模拟异常结束，避免触发系统错误对话框干扰自动测试。
        ExitProcess(23);
    }
    else if (!command.empty())
    {
        writeOutput(interactive ? "[TEXT]" + std::string(command) + "\r\n" : "[UNKNOWN]\r\n");
    }
    return std::nullopt;
}

BOOL WINAPI handleInteractiveControl(DWORD event)
{
    // Ctrl+C 中止当前控制台行读取，但不退出测试进程。
    return event == CTRL_C_EVENT;
}

DWORD readInteractiveInput(std::wstring &input, bool &cancelled)
{
    DWORD received = 0;
    SetLastError(ERROR_SUCCESS);
    const BOOL read = ReadConsoleW(GetStdHandle(STD_INPUT_HANDLE), input.data(),
                                   static_cast<DWORD>(input.size()), &received, nullptr);
    cancelled = GetLastError() == ERROR_OPERATION_ABORTED;
    if (!cancelled)
        checkWin32(read, "ReadConsoleW(interactive)");
    return received;
}

int runInteractive()
{
    checkWin32(SetConsoleCtrlHandler(handleInteractiveControl, TRUE), "SetConsoleCtrlHandler");
    configureConsole(true);
    writeOutput("FakeCli 中文终端验证\r\n[READY]\r\n> ");
    std::wstring input(8194, L'\0');
    while (true)
    {
        // 行编辑、中文退格与跨行重绘由控制台实现。Ctrl+C 提前唤醒并取消整行。
        bool cancelled = false;
        DWORD received = readInteractiveInput(input, cancelled);
        if (cancelled)
        {
            writeOutput("\r\n[CTRL_C]\r\n> ");
            continue;
        }
        if (received == 0)
            return 0;
        std::wstring_view line(input.data(), received);
        const bool complete = line.back() == L'\r' || line.back() == L'\n';
        while (!line.empty() && (line.back() == L'\r' || line.back() == L'\n'))
            line.remove_suffix(1);
        if (!complete)
        {
            // 超出单次行缓冲时拒绝整行，不能把余下字符当成新命令执行。
            do
            {
                received = readInteractiveInput(input, cancelled);
                if (cancelled)
                    break;
                if (received == 0)
                    return 0;
                line = std::wstring_view(input.data(), received);
            } while (line.find_first_of(L"\r\n") == std::wstring_view::npos);
            writeOutput(cancelled ? "\r\n[CTRL_C]\r\n> " : "\r\n[INPUT_LIMIT]\r\n> ");
            continue;
        }
        std::string utf8(line.size() * 4, '\0');
        if (!line.empty())
        {
            const int encoded =
                WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, line.data(), static_cast<int>(line.size()),
                                    utf8.data(), static_cast<int>(utf8.size()), nullptr, nullptr);
            checkWin32(encoded != 0, "WideCharToMultiByte(interactive)");
            utf8.resize(static_cast<std::size_t>(encoded));
        }
        if (utf8.size() > 8192)
            writeOutput("[INPUT_LIMIT]\r\n");
        else if (const auto exitCode = execute(utf8, true))
            return *exitCode;
        writeOutput("> ");
    }
}

int run()
{
    configureConsole();
    writeOutput("FakeCli 中文终端验证\r\n[READY]\r\n");
    std::string line;
    std::string escape;
    while (true)
    {
        // 控制台输入使用 Unicode API，避开旧版控制台 ReadFile 的 UTF-8 代码页兼容问题。
        // ConPTY 管道仍传输 UTF-8；这里仅转换 FakeCli 自己读到的 UTF-16 字符。
        std::array<wchar_t, 2048> input{};
        DWORD received = 0;
        checkWin32(ReadConsoleW(GetStdHandle(STD_INPUT_HANDLE), input.data(),
                                static_cast<DWORD>(input.size()), &received, nullptr),
                   "ReadConsoleW");
        if (received == 0)
        {
            return 0;
        }
        std::string utf8(received * 4, '\0');
        const int encoded =
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), static_cast<int>(received),
                                utf8.data(), static_cast<int>(utf8.size()), nullptr, nullptr);
        checkWin32(encoded != 0, "WideCharToMultiByte");
        utf8.resize(static_cast<std::size_t>(encoded));
        for (const char byte : utf8)
        {
            if (byte == '\x03')
            {
                line.clear();
                escape.clear();
                writeOutput("[CTRL_C]\r\n");
                continue;
            }
            if (byte == '\b' || byte == '\x7f')
            {
                // 原始模式需要自行处理退格；按完整 UTF-8 字符删除，不能留下中文续字节。
                if (!line.empty())
                {
                    auto begin = line.size() - 1;
                    while (begin > 0 && (static_cast<unsigned char>(line[begin]) & 0xc0) == 0x80)
                        --begin;
                    line.resize(begin);
                }
                continue;
            }
            if (byte == '\x1b' || !escape.empty())
            {
                escape += byte;
                if (escape.size() == 3)
                {
                    if (escape == "\x1b[A")
                    {
                        writeOutput("[UP]\r\n");
                    }
                    escape.clear();
                }
                continue;
            }
            if (byte == '\r' || byte == '\n')
            {
                if (const auto exitCode = execute(line))
                {
                    return *exitCode;
                }
                line.clear();
                continue;
            }
            if (line.size() >= 8192)
            {
                writeOutput("[INPUT_LIMIT]\r\n");
                line.clear();
            }
            line += byte;
        }
    }
}

} // 匿名命名空间

int wmain(int argc, wchar_t **argv)
{
    try
    {
        if (argc > 1 && std::wstring_view(argv[1]) == L"--sleep-child")
        {
            Sleep(120000);
            return 0;
        }
        if (argc > 1 && std::wstring_view(argv[1]) == L"--report-args")
        {
            configureConsole();
            constexpr char hex[] = "0123456789abcdef";
            for (int index = 2; index < argc; ++index)
            {
                const int length = static_cast<int>(std::wstring_view(argv[index]).size());
                std::string bytes(static_cast<std::size_t>(length) * 4, '\0');
                const int size =
                    length == 0
                        ? 0
                        : WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[index], length,
                                              bytes.data(), static_cast<int>(bytes.size()), nullptr, nullptr);
                checkWin32(length == 0 || size > 0, "WideCharToMultiByte(arguments)");
                bytes.resize(static_cast<std::size_t>(size));
                std::string encoded;
                for (const unsigned char byte : bytes)
                {
                    encoded += hex[byte >> 4];
                    encoded += hex[byte & 15];
                }
                writeOutput("[ARG" + std::to_string(index - 2) + "]" + encoded + "[END]\r\n");
            }
            return 0;
        }
        if (argc > 1 && std::wstring_view(argv[1]) == L"--interactive")
            return runInteractive();
        return run();
    }
    catch (const std::exception &error)
    {
        std::cerr << "FakeCli error: " << error.what() << '\n';
        return 1;
    }
}
