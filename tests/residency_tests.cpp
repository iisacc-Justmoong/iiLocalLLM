#include "service/ModelResidencyManager.h"
#include <QtTest/QtTest>
using namespace iiLocalLLM;
using namespace iiLocalLLM::detail;
using namespace std::chrono_literals;
class ResidencyTests : public QObject {
    Q_OBJECT
private slots:
    void budgetLruAndActiveProtection()
    {
        ServiceOptions o; o.memoryBudgetBytes = 100; o.memoryReserveBytes = 10;
        ModelResidencyManager m(o, HardwareInfo{});
        QVERIFY(m.fits(60, 100)); QVERIFY(!m.fits(91, 100));
        m.loaded("a", {60, 0, 0, "test"}, -1);
        m.loaded("b", {30, 0, 0, "test"}, -1);
        QCOMPARE(m.bytes(), quint64(90)); QVERIFY(!m.fits(20, 100));
        QCOMPARE(m.oldestIdle(), QString("a"));
        m.acquire("a", -1); QCOMPARE(m.oldestIdle(), QString("b"));
        QVERIFY_THROWS_EXCEPTION(Error, m.erased("a", true));
        m.erased("b", true); QVERIFY(m.fits(40, 100));
        m.release("a"); QCOMPARE(m.oldestIdle(), QString("a"));
        QCOMPARE(m.loads(), quint64(2)); QCOMPARE(m.evictions(), quint64(1));
        QVERIFY(!m.fits(101, std::nullopt));
    }
    void expiryAndSmallMachinePolicy()
    {
        HardwareInfo h; h.ramBytes = quint64(8) << 30;
        ModelResidencyManager small({}, h); QCOMPARE(small.defaultKeepAlive(), qint64(0));
        h.ramBytes = quint64(32) << 30;
        ModelResidencyManager m({}, h); QCOMPARE(m.defaultKeepAlive(), qint64(300000));
        const auto now = ModelResidencyManager::Clock::now();
        m.loaded("a", {1}, 10, now);
        QVERIFY(m.expired(now + 9ms).isEmpty()); QCOMPARE(m.expired(now + 10ms), QList<QString>{"a"});
        m.acquire("a", 0); QVERIFY(m.expired(now + 1h).isEmpty());
        m.release("a", now); QCOMPARE(m.expired(now), QList<QString>{"a"});
        ServiceOptions o; o.keepAliveMs = 300000; h.ramBytes = quint64(8) << 30;
        QCOMPARE(ModelResidencyManager(o, h).defaultKeepAlive(), qint64(300000));
    }
    void durationAndOverflow()
    {
        QCOMPARE(parseKeepAlive(QJsonValue(QJsonValue::Undefined)), qint64(-1));
        QCOMPARE(parseKeepAlive("5m"), qint64(300000));
        QCOMPARE(parseKeepAlive(0), qint64(0));
        QCOMPARE(parseKeepAlive(0.25), qint64(250));
        for (const auto& value : {QJsonValue(-1), QJsonValue("forever"), QJsonValue(true), QJsonValue(QJsonValue::Null), QJsonValue("999999999999h")})
            QVERIFY_THROWS_EXCEPTION(Error, parseKeepAlive(value));
        QVERIFY_THROWS_EXCEPTION(Error, (MemoryEstimate{std::numeric_limits<quint64>::max(), 1}.totalBytes()));
        const auto available = availableRamBytes();
        QVERIFY(!available || *available > 0);
    }
};
QTEST_GUILESS_MAIN(ResidencyTests)
#include "residency_tests.moc"
