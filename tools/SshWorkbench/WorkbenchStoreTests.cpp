#include "WorkbenchStore.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

using namespace sshworkbench;
namespace
{
QString id()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}
WorkbenchState sample()
{
    WorkbenchState state;
    HostProfile first{id(), QStringLiteral("家庭电脑"), {QStringLiteral("home-a"), QStringLiteral("user")}};
    HostProfile second{id(), QStringLiteral("测试电脑"), {QStringLiteral("home-b"), QStringLiteral("user")}};
    state.hosts = {first, second};
    state.currentHostId = second.id;
    state.projects.terminalPath = QStringLiteral("C:/Terminal/wt.exe");
    state.projects.projects = {{QStringLiteral("同名项目"), QStringLiteral("D:/Project A"), true, id()},
                               {QStringLiteral("同名项目"), QStringLiteral("D:/Project B"), false, id()}};
    state.projectHosts.insert(state.projects.projects[0].id, first.id);
    state.projectHosts.insert(state.projects.projects[1].id, second.id);
    return state;
}
QByteArray bytes(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}
bool write(const QString &path, const QByteArray &content)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(content) == content.size();
}
}

class WorkbenchStoreTests : public QObject
{
    Q_OBJECT
private slots:
    void twoHostsRoundTrip()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("state.json"));
        const auto expected = sample();
        QString error;
        QVERIFY2(saveWorkbenchState(path, expected, error), qPrintable(error));
        WorkbenchState actual;
        QVERIFY2(loadWorkbenchState(path, actual, error), qPrintable(error));
        QCOMPARE(actual.currentHostId, expected.currentHostId);
        QCOMPARE(actual.projectHosts, expected.projectHosts);
        QCOMPARE(actual.hosts.size(), 2);
        QCOMPARE(actual.hosts[0].connection.host, QStringLiteral("home-a"));
        QCOMPARE(actual.projects.terminalPath, expected.projects.terminalPath);
        QCOMPARE(actual.projects.projects[1].enabled, false);
    }

    void migration_data()
    {
        QTest::addColumn<bool>("versionTwo");
        QTest::newRow("v1") << false;
        QTest::newRow("v2") << true;
    }
    void migration()
    {
        QFETCH(bool, versionTwo);
        QTemporaryDir directory;
        const auto statePath = directory.filePath(QStringLiteral("state.json"));
        const auto projectPath = directory.filePath(QStringLiteral("projects.json"));
        const auto profilePath = directory.filePath(QStringLiteral("connection.json"));
        const auto source = sample();
        QString error;
        QVERIFY(versionTwo ? launcher::saveProjectConfiguration(projectPath, source.projects, error)
                           : launcher::saveConfiguration(projectPath, source.projects, error));
        QVERIFY(sshsetup::saveProfile(profilePath, source.hosts[0].connection, error));
        const auto projectBytes = bytes(projectPath);
        const auto profileBytes = bytes(profilePath);
        WorkbenchState migrated;
        QVERIFY2(loadOrMigrateWorkbenchState(statePath, profilePath, projectPath, {}, migrated, error),
                 qPrintable(error));
        QCOMPARE(migrated.hosts.size(), 1);
        QCOMPARE(migrated.projects.projects.size(), 2);
        QCOMPARE(migrated.projectHosts.value(migrated.projects.projects[0].id), migrated.currentHostId);
        QCOMPARE(migrated.projects.terminalPath, source.projects.terminalPath);
        QCOMPARE(bytes(projectPath), projectBytes);
        QCOMPARE(bytes(profilePath), profileBytes);
        QCOMPARE(bytes(projectPath + QStringLiteral(".ssh02.bak")), projectBytes);
        QCOMPARE(bytes(profilePath + QStringLiteral(".ssh02.bak")), profileBytes);
        if (versionTwo)
            QCOMPARE(migrated.projects.projects[0].id, source.projects.projects[0].id);
        // 新配置一旦建立，就不再读取旧文件或重复导入。
        QVERIFY(write(profilePath, "broken"));
        WorkbenchState again;
        QVERIFY(loadOrMigrateWorkbenchState(statePath, profilePath, projectPath, {}, again, error));
        QCOMPARE(again.currentHostId, migrated.currentHostId);
        QCOMPARE(again.projects.projects[0].id, migrated.projects.projects[0].id);
    }

    void invalidConfiguration_data()
    {
        QTest::addColumn<int>("kind");
        for (int kind = 0; kind < 7; ++kind)
            QTest::newRow(qPrintable(QString::number(kind))) << kind;
    }
    void invalidConfiguration()
    {
        QFETCH(int, kind);
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("state.json"));
        const auto original = sample();
        QString error;
        QVERIFY(saveWorkbenchState(path, original, error));
        auto root = QJsonDocument::fromJson(bytes(path)).object();
        if (kind == 0)
            root.insert(QStringLiteral("version"), 99);
        else if (kind == 1)
            root.insert(QStringLiteral("current_host_id"), id());
        else if (kind == 2)
        {
            auto projects = root.value(QStringLiteral("projects")).toArray();
            auto project = projects[0].toObject();
            project.insert(QStringLiteral("host_id"), id());
            projects[0] = project;
            root.insert(QStringLiteral("projects"), projects);
        }
        else if (kind == 3 || kind == 4)
        {
            const auto key = kind == 3 ? QStringLiteral("hosts") : QStringLiteral("projects");
            auto entries = root.value(key).toArray();
            entries[1] = entries[0];
            root.insert(key, entries);
        }
        else if (kind == 5)
        {
            auto hosts = root.value(QStringLiteral("hosts")).toArray();
            auto host = hosts[0].toObject();
            host.insert(QStringLiteral("identity_file"), QStringLiteral("relative/key"));
            hosts[0] = host;
            root.insert(QStringLiteral("hosts"), hosts);
        }
        const auto malformed = kind == 6 ? QByteArray("{invalid") : QJsonDocument(root).toJson();
        QVERIFY(write(path, malformed));
        WorkbenchState preserved = original;
        QVERIFY(!loadWorkbenchState(path, preserved, error));
        QCOMPARE(preserved.currentHostId, original.currentHostId);
        QVERIFY(!saveWorkbenchState(path, original, error));
        QVERIFY(!loadOrMigrateWorkbenchState(path, {}, {}, {}, preserved, error));
        QCOMPARE(bytes(path), malformed);
    }

    void noSourceDoesNotCreateFile()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("state.json"));
        WorkbenchState state;
        QString error;
        QVERIFY(loadOrMigrateWorkbenchState(path, {}, {}, {}, state, error));
        QVERIFY(state.hosts.isEmpty());
        QVERIFY(!QFile::exists(path));
    }

    void legacyFallbackAndInvalidPreferredSource()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("state.json"));
        const auto projects = directory.filePath(QStringLiteral("projects.json"));
        const auto legacy = directory.filePath(QStringLiteral("legacy.json"));
        const auto profile = directory.filePath(QStringLiteral("connection.json"));
        const auto original = sample();
        QString error;
        QVERIFY(launcher::saveConfiguration(legacy, original.projects, error));
        QVERIFY(sshsetup::saveProfile(profile, original.hosts[0].connection, error));
        QVERIFY(write(projects, "broken"));
        WorkbenchState output;
        QVERIFY(!loadOrMigrateWorkbenchState(path, profile, projects, legacy, output, error));
        QVERIFY(!QFile::exists(path));
        QVERIFY(QFile::remove(projects));
        QVERIFY2(loadOrMigrateWorkbenchState(path, profile, projects, legacy, output, error),
                 qPrintable(error));
        QCOMPARE(output.projects.projects.size(), 2);
        QCOMPARE(bytes(legacy + QStringLiteral(".ssh02.bak")), bytes(legacy));
    }

    void profileOnlyMigration()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("state.json"));
        const auto profile = directory.filePath(QStringLiteral("connection.json"));
        QString error;
        QVERIFY(sshsetup::saveProfile(profile, sample().hosts[0].connection, error));
        WorkbenchState output;
        QVERIFY2(loadOrMigrateWorkbenchState(path, profile, {}, {}, output, error), qPrintable(error));
        QCOMPARE(output.hosts.size(), 1);
        QVERIFY(output.projects.projects.isEmpty());
        QCOMPARE(output.currentHostId, output.hosts[0].id);
        QVERIFY(QFile::exists(path));
    }

    void orphanProjectsAndBackupConflictStopMigration()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("state.json"));
        const auto projects = directory.filePath(QStringLiteral("projects.json"));
        const auto profile = directory.filePath(QStringLiteral("connection.json"));
        const auto original = sample();
        QString error;
        QVERIFY(launcher::saveProjectConfiguration(projects, original.projects, error));
        WorkbenchState output = original;
        QVERIFY(!loadOrMigrateWorkbenchState(path, profile, projects, {}, output, error));
        QCOMPARE(output.currentHostId, original.currentHostId);
        QVERIFY(!QFile::exists(path));
        QVERIFY(sshsetup::saveProfile(profile, original.hosts[0].connection, error));
        QVERIFY(write(projects + QStringLiteral(".ssh02.bak"), "different backup"));
        const auto originalBytes = bytes(projects);
        QVERIFY(!loadOrMigrateWorkbenchState(path, profile, projects, {}, output, error));
        QVERIFY(!QFile::exists(path));
        QCOMPARE(bytes(projects), originalBytes);
        QCOMPARE(bytes(projects + QStringLiteral(".ssh02.bak")), QByteArray("different backup"));
    }

    void removalChecksBothProjectsAndTabs()
    {
        auto state = sample();
        QString error;
        const auto hostId = state.hosts[0].id;
        QVERIFY(!canRemoveHost(state, hostId, {}, error));
        state.projectHosts.remove(state.projects.projects[0].id);
        state.projects.projects.removeFirst();
        QVERIFY(!canRemoveHost(state, hostId, {hostId}, error));
        QVERIFY(canRemoveHost(state, hostId, {}, error));
        QVERIFY(!canRemoveHost(state, id(), {}, error));
    }
};
QTEST_GUILESS_MAIN(WorkbenchStoreTests)
#include "WorkbenchStoreTests.moc"
