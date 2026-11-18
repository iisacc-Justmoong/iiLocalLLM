#pragma once

#include "CTestSupport.h"
#include <QDeadlineTimer>
#include <QEventLoop>
#include <QTimer>

namespace iillm_ctest {
inline void processFor(int milliseconds)
{
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    timer.setTimerType(Qt::PreciseTimer);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(milliseconds);
    loop.exec();
}

template <typename Predicate>
bool waitFor(Predicate&& predicate, int timeoutMs)
{
    QDeadlineTimer deadline(timeoutMs);
    while (!predicate()) {
        if (deadline.hasExpired()) return false;
        QEventLoop loop;
        QTimer::singleShot(10, &loop, &QEventLoop::quit);
        loop.exec();
    }
    return true;
}
} // namespace iillm_ctest

#define CTEST_WAIT_VERIFY(expression, timeout_ms) \
    do { \
        if (!::iillm_ctest::waitFor([&] { return bool(expression); }, (timeout_ms))) \
            ::iillm_ctest::fail(__FILE__, __LINE__, "timed out waiting for " #expression); \
    } while (false)

#define CTEST_WAIT_DEFAULT(expression) CTEST_WAIT_VERIFY(expression, 5000)

#define CTEST_WAIT_COMPARE_TIMEOUT(actual, expected, timeout_ms) \
    CTEST_WAIT_VERIFY(((actual) == (expected)), (timeout_ms))
#define CTEST_WAIT_COMPARE(actual, expected) CTEST_WAIT_COMPARE_TIMEOUT(actual, expected, 5000)
