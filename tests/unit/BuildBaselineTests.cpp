#include <model/BuildBaseline.h>
#include <protocol/ProtocolVersion.h>

#include <QtTest>

class BuildBaselineTests final : public QObject
{
    Q_OBJECT
private slots:
    void compiledAndLoadedQtMatch()
    {
        // 防止 PATH 中其他 Qt 版本被误当成本机技术基线测试成功。
        QCOMPARE(QString::fromLatin1(qVersion()), QStringLiteral("5.15.2"));
        QCOMPARE(QString::fromLatin1(QT_VERSION_STR), QString::fromLatin1(qVersion()));
        QCOMPARE(sizeof(void *), std::size_t(8));
        QVERIFY(_MSVC_LANG >= 202002L);
        QVERIFY(rad::buildBaseline().contains(QString::fromLatin1(qVersion())));
    }

    void protocolTargetLinks()
    {
        QCOMPARE(rad::protocolVersion(), 1);
    }
};

QTEST_GUILESS_MAIN(BuildBaselineTests)
#include "BuildBaselineTests.moc"
