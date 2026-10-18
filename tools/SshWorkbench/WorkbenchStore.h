#pragma once

#include "EnvironmentService.h"
#include "LauncherCore.h"
#include <QMap>
#include <QSet>

namespace sshworkbench
{
struct HostProfile
{
    QString id;
    QString name;
    sshsetup::ConnectionProfile connection;
};

struct WorkbenchState
{
    launcher::Configuration projects;
    QVector<HostProfile> hosts;
    QString currentHostId;
    QMap<QString, QString> projectHosts;
};

// 同步配置操作；读取失败保持输出，保存失败保留原文件。调用方串行调用。
bool loadWorkbenchState(const QString &path, WorkbenchState &state, QString &error);
bool saveWorkbenchState(const QString &path, const WorkbenchState &state, QString &error);
// 仅新配置不存在时迁移，旧文件保持原字节并备份；未知格式不自动覆盖。
bool loadOrMigrateWorkbenchState(const QString &statePath, const QString &profilePath,
                                 const QString &projectsPath, const QString &legacyPath,
                                 WorkbenchState &state, QString &error);
const HostProfile *findHost(const WorkbenchState &state, const QString &id);
bool canRemoveHost(const WorkbenchState &state, const QString &id, const QSet<QString> &referencedHostIds,
                   QString &error);
}
