#pragma once

#include <QByteArray>
#include <QString>
#include <functional>

namespace sshsetup
{

struct HomeEnvironment
{
    QString hostName;
    QString userName;
    QString host;
    int port = 22;
    QString hostFingerprint;
    QString authorizedKeysPath;
    QString configurationPath;
    QString serviceState;
    QString serviceExecutable;
    QString listenWarning;
    bool serviceInstalled = false;
    bool administrator = false;
    bool sshAvailable = false;
    QString error;
};

// 以下操作是同步的，应从工作线程调用；错误信息不包含密钥正文。
bool validatePublicKey(const QString &input, QString &normalized, QString &fingerprint, QString &error);
// 只读检查配置，保留系统错误类别与路径；失败时不改动输出内容或创建文件。
bool readSshConfiguration(const QString &path, QString &configuration, QString &error);
HomeEnvironment inspectHomeEnvironment();
// 仅分析静态配置并返回固定警告；不能据此确认服务的实际监听地址。
QString sshListenConfigurationWarning(const QString &configuration);
// 仅输出固定状态类别，不包含账户、地址、路径、密钥或原始错误正文。
QString homeEnvironmentReport(const HomeEnvironment &environment);
// 应用前重新检查，并要求结果与界面展示且用户确认的快照一致。
bool authorizeCurrentUser(const QString &publicKey, const HomeEnvironment &expected, QString &error);
bool matchesAuthorizationPreview(const HomeEnvironment &expected, const HomeEnvironment &actual,
                                 QString &error);

// 纯解析入口供配置检查与临时目录测试复用，未知授权规则一律拒绝。
bool resolveAuthorizedKeys(const QString &configuration, bool administrator, const QString &home,
                           const QString &programData, QString &path, int &port, QString &error);
// 仅追加新公钥；权限处理失败时恢复原内容。回调负责权限设置及其回滚。
bool appendAuthorizedKey(const QString &path, const QString &publicKey,
                         const std::function<bool(QString &)> &secureFile, QString &error);

}
