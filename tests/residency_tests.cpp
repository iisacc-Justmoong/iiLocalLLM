#include "service/ModelResidencyManager.h"
#include "service/Scheduler.h"
#include "runtimes/MemoryEstimate.h"
#include "QtEventWait.h"
#include <QtCore/QCoreApplication>
#include <atomic>
using namespace iiLocalLLM;
using namespace iiLocalLLM::detail;
using namespace std::chrono_literals;
class ResidencyTests {
public:
    void llamaQuantizedKvReservation()
    {
        // One 128-element K and V head in 32 layers, two contexts of 4096 tokens.
        const auto f16 = llamaKvReservation(32, 1, 128, 128, 4096, 2, LlamaKvCache::F16);
        const auto q8 = llamaKvReservation(32, 1, 128, 128, 4096, 2, LlamaKvCache::Q8_0);
        const auto q4 = llamaKvReservation(32, 1, 128, 128, 4096, 2, LlamaKvCache::Q4_0);
        CTEST_COMPARE(f16, quint64(32) * 256 * 2 * 4096 * 2);
        CTEST_COMPARE(q8, quint64(32) * 8 * 34 * 4096 * 2);
        CTEST_COMPARE(q4, quint64(32) * 8 * 18 * 4096 * 2);
        CTEST_CHECK(q4 < q8 && q8 < f16);
        CTEST_COMPARE(llamaKvReservation(1, 1, 128, 128, 257, 1, LlamaKvCache::Q8_0),
                 llamaKvReservation(1, 1, 128, 128, 512, 1, LlamaKvCache::Q8_0));
        // Unknown dimensions cannot safely justify a reduced reservation.
        CTEST_COMPARE(llamaKvReservation(1, 1, 127, 128, 256, 1, LlamaKvCache::Q8_0),
                 llamaKvReservation(1, 1, 127, 128, 256, 1, LlamaKvCache::F16));
        CTEST_THROWS(Error, llamaKvReservation(std::numeric_limits<quint64>::max(), 32, 128, 128,
                                                           4096, 2, LlamaKvCache::Q4_0));
    }
    void budgetLruAndActiveProtection()
    {
        ServiceOptions o; o.memoryBudgetBytes = 100; o.memoryReserveBytes = 10;
        ModelResidencyManager m(o, HardwareInfo{});
        CTEST_CHECK(m.fits(60, 100)); CTEST_CHECK(!m.fits(91, 100));
        m.loaded("a", {60, 0, 0, "test"}, -1);
        m.loaded("b", {30, 0, 0, "test"}, -1);
        CTEST_COMPARE(m.bytes(), quint64(90)); CTEST_CHECK(!m.fits(20, 100));
        CTEST_COMPARE(m.oldestIdle(), QString("a"));
        m.acquire("a", -1); CTEST_COMPARE(m.oldestIdle(), QString("b"));
        CTEST_THROWS(Error, m.erased("a", true));
        m.erased("b", true); CTEST_CHECK(m.fits(40, 100));
        m.release("a"); CTEST_COMPARE(m.oldestIdle(), QString("a"));
        CTEST_COMPARE(m.loads(), quint64(2)); CTEST_COMPARE(m.evictions(), quint64(1));
        CTEST_CHECK(!m.fits(101, std::nullopt));
    }
    void expiryAndSmallMachinePolicy()
    {
        HardwareInfo h; h.ramBytes = quint64(8) << 30;
        ModelResidencyManager small({}, h); CTEST_COMPARE(small.defaultKeepAlive(), qint64(0));
        h.ramBytes = quint64(32) << 30;
        ModelResidencyManager m({}, h); CTEST_COMPARE(m.defaultKeepAlive(), qint64(300000));
        const auto now = ModelResidencyManager::Clock::now();
        m.loaded("a", {1}, 10, now);
        CTEST_CHECK(m.expired(now + 9ms).isEmpty()); CTEST_COMPARE(m.expired(now + 10ms), QList<QString>{"a"});
        m.acquire("a", 0); CTEST_CHECK(m.expired(now + 1h).isEmpty());
        m.release("a", now); CTEST_COMPARE(m.expired(now), QList<QString>{"a"});
        ServiceOptions o; o.keepAliveMs = 300000; h.ramBytes = quint64(8) << 30;
        CTEST_COMPARE(ModelResidencyManager(o, h).defaultKeepAlive(), qint64(300000));
    }
    void durationAndOverflow()
    {
        CTEST_COMPARE(parseKeepAlive(QJsonValue(QJsonValue::Undefined)), qint64(-1));
        CTEST_COMPARE(parseKeepAlive("5m"), qint64(300000));
        CTEST_COMPARE(parseKeepAlive(0), qint64(0));
        CTEST_COMPARE(parseKeepAlive(0.25), qint64(250));
        for (const auto& value : {QJsonValue(-1), QJsonValue("forever"), QJsonValue(true), QJsonValue(QJsonValue::Null), QJsonValue("999999999999h")})
            CTEST_THROWS(Error, parseKeepAlive(value));
        CTEST_THROWS(Error, (MemoryEstimate{std::numeric_limits<quint64>::max(), 1}.totalBytes()));
        const auto available = availableRamBytes();
#ifdef Q_OS_WIN
        CTEST_CHECK(available.has_value());
#endif
        CTEST_CHECK(!available || *available > 0);
    }
    void pinnedModelsAreNeverIdleEvictionCandidates()
    {
        ServiceOptions options; options.memoryBudgetBytes = 100;
        ModelResidencyManager models(options, HardwareInfo{});
        const auto now = ModelResidencyManager::Clock::now();
        models.loaded("resident", {60}, 0, now);
        models.pin("resident");
        CTEST_CHECK(models.get("resident").pinned);
        CTEST_CHECK(models.expired(now + 24h).isEmpty());
        CTEST_CHECK(models.oldestIdle().isEmpty());
        CTEST_CHECK(!models.nextExpiry());
        CTEST_THROWS(Error, models.erased("resident", false));
        models.loaded("other", {30}, 10, now);
        CTEST_COMPARE(*models.nextExpiry(), now + 10ms);
        CTEST_COMPARE(models.oldestIdle(), QString("other"));
        models.acquire("resident", 0);
        models.release("resident", now);
        CTEST_CHECK(models.get("resident").pinned);
        models.unpin("resident", now);
        CTEST_CHECK(!models.get("resident").pinned);
        CTEST_COMPARE(*models.nextExpiry(), now);
        CTEST_CHECK(models.expired(now).contains("resident"));
        models.erased("resident", false);
    }
    void idleSchedulerDoesNotPollWithoutADeadline()
    {
        std::atomic<int> maintenance{0};
        Scheduler scheduler(4, {}, [&] { ++maintenance; }, [] { return std::optional<Scheduler::Deadline>{}; });
        iillm_ctest::processFor(250);
        CTEST_COMPARE(maintenance.load(), 0);
        scheduler.submit([](const auto &) {}).get();
        CTEST_WAIT_VERIFY(maintenance.load() == 1, 5000);
        iillm_ctest::processFor(250);
        CTEST_COMPARE(maintenance.load(), 1);
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    iillm_ctest::CTestRunner runner;
    ResidencyTests tests;
    runner.run("llamaQuantizedKvReservation", [&] { tests.llamaQuantizedKvReservation(); });
    runner.run("budgetLruAndActiveProtection", [&] { tests.budgetLruAndActiveProtection(); });
    runner.run("expiryAndSmallMachinePolicy", [&] { tests.expiryAndSmallMachinePolicy(); });
    runner.run("durationAndOverflow", [&] { tests.durationAndOverflow(); });
    runner.run("pinnedModelsAreNeverIdleEvictionCandidates", [&] { tests.pinnedModelsAreNeverIdleEvictionCandidates(); });
    runner.run("idleSchedulerDoesNotPollWithoutADeadline", [&] { tests.idleSchedulerDoesNotPollWithoutADeadline(); });
    return runner.result();
}
