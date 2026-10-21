#pragma once

#include "EnvironmentService.h"
#include <QObject>
#include <QVector>
#include <memory>

namespace sshworkbench
{
struct RemoteDirectoryEntry
{
    QString name;
    QString path;
};
struct RemoteDirectoryResult
{
    bool success = false;
    // 本机单调时钟计时；查询阶段包含 SSH 连接及远端 PowerShell，不代表纯网络延迟。
    qint64 preparationMilliseconds = -1;
    qint64 queryMilliseconds = -1;
    QString currentPath;
    QString parentPath;
    QString homePath;
    QString error;
    QVector<RemoteDirectoryEntry> drives;
    QVector<RemoteDirectoryEntry> directories;
};

// 纯函数：路径仅作为编码数据传入；解析器拒绝超限和结构不完整的响应。
bool remoteDirectoryCommandArguments(const QString &path, QStringList &arguments, QString &error);
RemoteDirectoryResult parseRemoteDirectoryResult(const QByteArray &output);

// 方法和信号均属于创建线程；每次只处理一项请求，完成后可再次 start。
// 取消不发送 finished；超时发送失败，后台信任检查收尾期间仍 busy，禁止堆积线程。
// 析构不等待同步信任检查，后台仅持有自己的配置快照和临时信任文件。
class RemoteDirectoryService final : public QObject
{
    Q_OBJECT
public:
    explicit RemoteDirectoryService(QObject *parent = nullptr);
    ~RemoteDirectoryService() override;
    void start(const sshsetup::ConnectionProfile &profile, const QString &path);
    void cancel();
    bool isBusy() const;

signals:
    void finished(const sshworkbench::RemoteDirectoryResult &result);
    void busyChanged(bool busy);

private:
    friend class RemoteDirectoryServiceTests;
    void beginPreparation(const sshsetup::ConnectionProfile &profile, const QStringList &command,
                          int preparationDelay = 0, int timeout = 15000);
    void startLocalProcessForTest(const QString &program, const QStringList &arguments, int timeout);
    static QStringList directoryTransportArguments(const QStringList &command, QByteArray &input);
    void launchProcess(const QString &program, const QStringList &arguments, const QByteArray &input = {});
    void complete(const RemoteDirectoryResult &result);
    void updateBusy();
    void readOutput();
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
}
Q_DECLARE_METATYPE(sshworkbench::RemoteDirectoryResult)
