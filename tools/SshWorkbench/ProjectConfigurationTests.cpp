#include "LauncherCore.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

namespace
{
QByteArray readBytes(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

bool writeBytes(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

launcher::Configuration legacyConfiguration()
{
    launcher::Configuration configuration;
    configuration.sshHost = QStringLiteral("home");
    configuration.sshUser = QStringLiteral("tester");
    configuration.identityFile = QStringLiteral("C:/identity/id_ed25519");
    configuration.terminalPath = QStringLiteral("C:/terminal/wt.exe");
    configuration.projects = {{QStringLiteral("同名项目"), QStringLiteral("D:/中文目录"), true},
                              {QStringLiteral("同名项目"), QStringLiteral("D:/中文目录"), false}};
    return configuration;
}
}

class ProjectConfigurationTests : public QObject
{
    Q_OBJECT
private slots:
    void migrationPreservesFieldsAndOriginalBytes()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath(QStringLiteral("projects.json"));
        QString error;
        QVERIFY(launcher::saveConfiguration(path, legacyConfiguration(), error));
        const auto original = readBytes(path);
        launcher::Configuration loaded;
        QVERIFY2(launcher::loadProjectConfiguration(path, loaded, error), qPrintable(error));
        QCOMPARE(readBytes(path), original);
        QVERIFY(!loaded.projects[0].id.isEmpty());
        QVERIFY(loaded.projects[0].id != loaded.projects[1].id);
        QVERIFY(launcher::saveProjectConfiguration(path, loaded, error));
        QCOMPARE(readBytes(path + QStringLiteral(".v1.bak")), original);
        launcher::Configuration reloaded;
        QVERIFY(launcher::loadProjectConfiguration(path, reloaded, error));
        QCOMPARE(reloaded.sshHost, loaded.sshHost);
        QCOMPARE(reloaded.sshUser, loaded.sshUser);
        QCOMPARE(reloaded.identityFile, loaded.identityFile);
        QCOMPARE(reloaded.terminalPath, loaded.terminalPath);
        for (int i = 0; i < loaded.projects.size(); ++i)
        {
            QCOMPARE(reloaded.projects[i].name, loaded.projects[i].name);
            QCOMPARE(reloaded.projects[i].directory, loaded.projects[i].directory);
            QCOMPARE(reloaded.projects[i].enabled, loaded.projects[i].enabled);
            QCOMPARE(reloaded.projects[i].id, loaded.projects[i].id);
        }
        QVERIFY(launcher::saveProjectConfiguration(path, reloaded, error));
        QCOMPARE(readBytes(path + QStringLiteral(".v1.bak")), original);
        QVERIFY(!launcher::loadConfiguration(path, reloaded, error));
    }

    void savingSeparatelyDoesNotChangeLegacy()
    {
        QTemporaryDir directory;
        const auto legacy = directory.filePath(QStringLiteral("legacy.json"));
        const auto current = directory.filePath(QStringLiteral("projects.json"));
        QString error;
        QVERIFY(launcher::saveConfiguration(legacy, legacyConfiguration(), error));
        const auto original = readBytes(legacy);
        launcher::Configuration loaded;
        QVERIFY(launcher::loadProjectConfiguration(legacy, loaded, error));
        QVERIFY(launcher::saveProjectConfiguration(current, loaded, error));
        QCOMPARE(readBytes(legacy), original);
        QVERIFY(!QFile::exists(legacy + QStringLiteral(".v1.bak")));
    }

    void backupConflictDoesNotOverwriteEitherFile()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("projects.json"));
        const auto backup = path + QStringLiteral(".v1.bak");
        QString error;
        QVERIFY(launcher::saveConfiguration(path, legacyConfiguration(), error));
        const auto original = readBytes(path);
        launcher::Configuration loaded;
        QVERIFY(launcher::loadProjectConfiguration(path, loaded, error));
        QVERIFY(writeBytes(backup, "previous backup"));
        QVERIFY(!launcher::saveProjectConfiguration(path, loaded, error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(readBytes(path), original);
        QCOMPARE(readBytes(backup), QByteArray("previous backup"));
        QVERIFY(writeBytes(backup, original));
        QVERIFY(launcher::saveProjectConfiguration(path, loaded, error));
        QCOMPARE(readBytes(backup), original);
    }

    void invalidIdentityAndVersionAreRejected_data()
    {
        QTest::addColumn<QString>("identity");
        QTest::addColumn<int>("version");
        QTest::addColumn<bool>("duplicate");
        QTest::newRow("empty") << QString() << 2 << false;
        QTest::newRow("invalid") << QStringLiteral("not-an-id") << 2 << false;
        QTest::newRow("null-uuid") << QStringLiteral("00000000-0000-0000-0000-000000000000") << 2 << false;
        QTest::newRow("duplicate") << QUuid::createUuid().toString(QUuid::WithoutBraces) << 2 << true;
        QTest::newRow("unsupported-version")
            << QUuid::createUuid().toString(QUuid::WithoutBraces) << 3 << false;
    }

    void invalidIdentityAndVersionAreRejected()
    {
        QFETCH(QString, identity);
        QFETCH(int, version);
        QFETCH(bool, duplicate);
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("projects.json"));
        QString error;
        QVERIFY(launcher::saveConfiguration(path, legacyConfiguration(), error));
        auto object = QJsonDocument::fromJson(readBytes(path)).object();
        object[QStringLiteral("version")] = version;
        auto projects = object.value(QStringLiteral("projects")).toArray();
        for (int i = 0; i < projects.size(); ++i)
        {
            auto project = projects[i].toObject();
            project[QStringLiteral("id")] =
                i == 0 || duplicate ? identity : QUuid::createUuid().toString(QUuid::WithoutBraces);
            projects[i] = project;
        }
        object[QStringLiteral("projects")] = projects;
        const auto invalidBytes = QJsonDocument(object).toJson();
        QVERIFY(writeBytes(path, invalidBytes));
        auto output = legacyConfiguration();
        output.sshHost = QStringLiteral("untouched");
        QVERIFY(!launcher::loadProjectConfiguration(path, output, error));
        QCOMPARE(output.sshHost, QStringLiteral("untouched"));
        QCOMPARE(readBytes(path), invalidBytes);
        auto valid = legacyConfiguration();
        for (auto &project : valid.projects)
            project.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QVERIFY(!launcher::saveProjectConfiguration(path, valid, error));
        QCOMPARE(readBytes(path), invalidBytes);
        QVERIFY(!QFile::exists(path + QStringLiteral(".v1.bak")));
    }

    void invalidIdentityCannotBeSaved()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("projects.json"));
        auto configuration = legacyConfiguration();
        QString error;
        QVERIFY(!launcher::saveProjectConfiguration(path, configuration, error));
        QVERIFY(!QFile::exists(path));
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        for (auto &project : configuration.projects)
            project.id = id;
        QVERIFY(!launcher::saveProjectConfiguration(path, configuration, error));
        QVERIFY(!QFile::exists(path));
    }

    void backupFailureLeavesOriginalUntouched()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("projects.json"));
        QString error;
        QVERIFY(launcher::saveConfiguration(path, legacyConfiguration(), error));
        const auto original = readBytes(path);
        launcher::Configuration configuration;
        QVERIFY(launcher::loadProjectConfiguration(path, configuration, error));
        QVERIFY(QDir().mkdir(path + QStringLiteral(".v1.bak")));
        QVERIFY(!launcher::saveProjectConfiguration(path, configuration, error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(readBytes(path), original);
        QVERIFY(!launcher::saveProjectConfiguration(
            directory.filePath(QStringLiteral("missing/projects.json")), configuration, error));
        QVERIFY(!error.isEmpty());
    }
};

QTEST_GUILESS_MAIN(ProjectConfigurationTests)
#include "ProjectConfigurationTests.moc"
