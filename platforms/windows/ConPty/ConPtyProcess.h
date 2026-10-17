#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace rad::windows
{

// 单次启动的 Win32 引擎；工作区授权由产品适配层承担。
// 控制方法及析构仅由构造线程调用；三个输出查询可跨线程调用。
// 同步错误抛标准异常，异步读写错误由 ioError() 返回首个错误。
class ConPtyProcess
{
public:
    explicit ConPtyProcess();
    ~ConPtyProcess();
    ConPtyProcess(const ConPtyProcess &) = delete;
    ConPtyProcess &operator=(const ConPtyProcess &) = delete;

    // 只启动规范化后位于工作目录内的显式 EXE，不接收 shell 或参数。
    // 启动尝试后不允许重用实例；目录限制仅用于本 POC，不等于产品白名单。
    void start(const std::filesystem::path &executable, const std::filesystem::path &workingDirectory,
               short columns = 120, short rows = 32);
    void startValidated(const std::filesystem::path &executable,
                        const std::filesystem::path &workingDirectory,
                        const std::vector<std::wstring> &arguments, short columns, short rows);
    // 可跨线程进行有界入队；与收尾共用输入锁，收尾开始后拒绝输入。
    void enqueueInput(std::string bytes, std::size_t capacity);
    struct OutputDelta
    {
        std::string bytes;
        std::uint64_t droppedBytes = 0;
    };
    // 原子读取增量并更新游标，最多保留内部 1 MiB 环形缓冲。
    OutputDelta outputSince(std::uint64_t &cursor) const;
    // 异常路径收尾并等待管道排空，调用后仍可读取输出。
    void close() noexcept;
    // 入队成功不等于 CLI 已消费；待发送字节含正在写入部分最多 256 KiB。
    void writeInput(std::string bytes);
    void resize(short columns, short rows);
    // 超时返回 false；退出后关闭终端并排空输出，最终收尾可能超过 timeout。
    bool waitForExit(std::chrono::milliseconds timeout);
    unsigned long exitCode() const;
    // 显式终止本会话进程树并收尾；重复调用无副作用。
    void terminate();
    // 保留最近 1 MiB 原始字节；截断点可能在 UTF-8 或 VT 序列中间。
    std::string outputSnapshot() const;
    std::uint64_t outputBytes() const;
    std::string ioError() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // 命名空间 rad::windows
