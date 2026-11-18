#include "QtEventWait.h"
#include <QCoreApplication>
#include <QElapsedTimer>

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    iillm_ctest::CTestRunner runner;
    runner.run("waitsForTimer", [] {
        bool ready = false;
        QTimer::singleShot(0, [&] { ready = true; });
        CTEST_WAIT_VERIFY(ready, 1000);
    });
    runner.run("waitsForComparisonAndReportsTimeout", [] {
        int value = 0;
        QTimer::singleShot(0, [&] { value = 2; });
        CTEST_WAIT_COMPARE(value, 2);
        CTEST_THROWS(std::runtime_error, ([&] { CTEST_WAIT_COMPARE_TIMEOUT(value, 3, 0); }()));
    });
    runner.run("reportsTimeout", [] {
        CTEST_CHECK(!iillm_ctest::waitFor([] { return false; }, 0));
    });
    runner.run("processesEventsForTheRequestedDuration", [] {
        bool delivered = false;
        QTimer::singleShot(0, [&] { delivered = true; });
        QElapsedTimer elapsed;
        elapsed.start();
        iillm_ctest::processFor(15);
        CTEST_CHECK(delivered);
        CTEST_CHECK(elapsed.elapsed() >= 15);
    });
    return runner.result();
}
