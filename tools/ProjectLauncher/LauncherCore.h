#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace launcher
{
struct Project
{
    QString name;
    QString directory;
    bool enabled = true;
    QString id;
};

struct Configuration
{
    QString sshHost = QStringLiteral("home");
    QString sshUser;
    QString identityFile;
    QString terminalPath;
    QVector<Project> projects;
};

// 同步操作，仅处理调用方提供的数据；失败返回中文原因，不覆盖已加载的配置。
bool validateConfiguration(const Configuration &configuration, QString &error);
bool loadConfiguration(const QString &path, Configuration &configuration, QString &error);
bool saveConfiguration(const QString &path, const Configuration &configuration, QString &error);

// 工作台同步读写：加载 v1 时仅在内存补齐稳定 ID，保存为 v2；读取失败不修改输出。
bool loadProjectConfiguration(const QString &path, Configuration &configuration, QString &error);
// 覆盖 v1 前保留原字节到 .v1.bak；不同备份或读取失败时拒绝覆盖。调用方串行调用。
bool saveProjectConfiguration(const QString &path, const Configuration &configuration, QString &error);

// 只生成 Windows Terminal 参数，不启动进程；失败时清空参数。项目必须启用且包含于配置。
bool buildLaunchArguments(const Configuration &configuration, const Project &project, const QString &sshPath,
                          QStringList &arguments, QString &error);
}
