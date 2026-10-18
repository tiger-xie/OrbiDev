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

// 只生成 Windows Terminal 参数，不启动进程；失败时清空参数。项目必须启用且包含于配置。
bool buildLaunchArguments(const Configuration &configuration, const Project &project, const QString &sshPath,
                          QStringList &arguments, QString &error);
}
