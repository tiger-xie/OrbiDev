#pragma once

#include <QString>

namespace sshsetup
{
struct Identity
{
    QString privateKeyPath;
    QString publicKey;
    QString fingerprint;
};

// 同步操作，调用方须放在工作线程；失败不修改输出身份，仅填写错误。
// 专用目录的父目录必须存在；已有目录和密钥只检查，不覆盖或静默修改权限。
bool ensureIdentity(const QString &directory, Identity &identity, QString &error);
// 只读取公钥并调用系统工具验证私钥；不读取、展示或导出私钥内容。
bool inspectIdentity(const QString &privateKeyPath, Identity &identity, QString &error);
}
