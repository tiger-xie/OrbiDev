#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace rad::poc
{

// 单次启动的技术验证会话，不承担产品级工作区授权或客户端附加管理。
// 控制方法及析构仅由构造线程调用；三个输出查询可跨线程调用。
// 同步错误抛标准异常，异步读写错误由 ioError() 返回首个错误。
class ConPtySession
{
public:
    explicit ConPtySession();
    ~ConPtySession();
    ConPtySession(const ConPtySession &) = delete;
    ConPtySession &operator=(const ConPtySession &) = delete;

    // 只启动规范化后位于工作目录内的显式 EXE，不接收 shell 或参数。
    // 启动尝试后不允许重用实例；目录限制仅用于本 POC，不等于产品白名单。
    void start(const std::filesystem::path &executable, const std::filesystem::path &workingDirectory,
               short columns = 120, short rows = 32);
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

} // 命名空间 rad::poc
