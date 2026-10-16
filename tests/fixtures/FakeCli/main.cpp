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

void configureConsole()
{
    checkWin32(SetConsoleCP(CP_UTF8), "SetConsoleCP");
    checkWin32(SetConsoleOutputCP(CP_UTF8), "SetConsoleOutputCP");
    // 使用原始 VT 输入，Ctrl+C 作为字节验证，不触发系统默认退出处理器。
    checkWin32(SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), ENABLE_VIRTUAL_TERMINAL_INPUT),
               "SetConsoleMode(input)");
    checkWin32(SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), ENABLE_PROCESSED_OUTPUT |
                                                                   ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                                                                   ENABLE_WRAP_AT_EOL_OUTPUT),
               "SetConsoleMode(output)");
}

std::optional<int> execute(std::string_view command)
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
        writeOutput("[UNKNOWN]\r\n");
    }
    return std::nullopt;
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

int main()
{
    try
    {
        return run();
    }
    catch (const std::exception &error)
    {
        std::cerr << "FakeCli error: " << error.what() << '\n';
        return 1;
    }
}
