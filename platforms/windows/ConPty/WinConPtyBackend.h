#pragma once
#include <memory>
#include <terminal/ITerminalBackend.h>

namespace rad
{
// 控制方法和信号仅在对象所属线程执行，阻塞启动/收尾放到独立工作线程。
class WinConPtyBackend final : public ITerminalBackend
{
    Q_OBJECT
public:
    struct Options
    {
        QStringList allowedWorkspaceRoots;
        int inputCapacityBytes = 256 * 1024;
        int outputCapacityBytes = 1024 * 1024;
    };
    explicit WinConPtyBackend(QObject *parent = nullptr);
    explicit WinConPtyBackend(Options options, QObject *parent = nullptr);
    ~WinConPtyBackend() override;
    OperationResult start(const LaunchSpec &spec) override;
    OperationResult writeInput(const QByteArray &bytes) override;
    OperationResult resize(TerminalSize size) override;
    OperationResult terminate() override;
    Result<OutputChunk> takeOutput(int maxBytes) override;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
}
