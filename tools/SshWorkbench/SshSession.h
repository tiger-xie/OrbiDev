#pragma once

#include "EnvironmentService.h"

#include <QByteArray>
#include <QObject>
#include <QStringList>
#include <memory>

namespace sshworkbench
{
// 仅构造固定的远端启动脚本；目录作为编码数据传入，不作为命令解释。
bool remoteCommandArguments(const QString &directory, QStringList &arguments, QString &error);

// 所有方法与信号属于创建线程。每个实例只启动一次：未启动 → 准备中 → 校验主机 → 本地运行 → 已退出。
// hostVerificationStarted 异步报告主机校验开始，始终先于 started 或 exited，不表示校验已通过。
// started 只表示本地 ssh 已创建，不代表远端认证成功；真实提示由 outputReady 显示。
// stop 异步且幂等；析构等待后台收尾，包括有超时的主机校验，不保证远端任务继续运行。
// 输入上限 256 KiB、输出上限 1 MiB；溢出会报告错误并停止，避免静默破坏终端状态。
class SshSession final : public QObject
{
    Q_OBJECT
public:
    explicit SshSession(QObject *parent = nullptr);
    ~SshSession() override;
    void start(const sshsetup::ConnectionProfile &profile, const QString &remoteDirectory, int columns = 120,
               int rows = 32);
    void writeInput(const QByteArray &bytes);
    void resize(int columns, int rows);
    void stop();
    bool isActive() const;

signals:
    void hostVerificationStarted();
    void started();
    void outputReady(const QByteArray &bytes);
    void errorOccurred(const QString &message);
    void exited(int exitCode);

private:
    friend class SshSessionTests;
    void startProcess(const sshsetup::ConnectionProfile &profile, const QString &directory, int columns,
                      int rows, const QString &testProgram = {}, const QStringList &testArguments = {});
    void poll();
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
}
