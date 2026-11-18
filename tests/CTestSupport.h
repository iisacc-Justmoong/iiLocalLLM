#pragma once

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace iillm_ctest {

class CTestRunner final {
public:
    explicit CTestRunner(std::string_view selection = {}) : selection_(selection) {}

    template <typename Function>
    void run(std::string_view name, Function&& function)
    {
        if (!selection_.empty() && selection_ != name) return;
        ++cases_;
        try {
            std::forward<Function>(function)();
        } catch (const std::exception& error) {
            ++failures_;
            std::cerr << name << ": " << error.what() << '\n';
        } catch (...) {
            ++failures_;
            std::cerr << name << ": unexpected non-standard exception\n";
        }
    }

    [[nodiscard]] int result() const
    {
        if (cases_ == 0) {
            std::cerr << "No test cases were executed\n";
            return 1;
        }
        std::cout << cases_ << " test cases, " << failures_ << " failures\n";
        return failures_ == 0 ? 0 : 1;
    }

private:
    std::string selection_;
    unsigned cases_ = 0;
    unsigned failures_ = 0;
};

[[noreturn]] inline void fail(const char* file, int line, const char* message)
{
    throw std::runtime_error(std::string(file) + ":" + std::to_string(line) + ": " + message);
}

template <typename Actual, typename Expected>
inline void compare(Actual&& actual, Expected&& expected,
                    const char* file, int line, const char* message)
{
    if (!(actual == expected))
        fail(file, line, message);
}

} // namespace iillm_ctest

#define CTEST_CHECK(expression) \
    do { if (!(expression)) ::iillm_ctest::fail(__FILE__, __LINE__, #expression); } while (false)

#define CTEST_CHECK_MESSAGE(expression, message) \
    do { \
        if (!(expression)) \
            ::iillm_ctest::fail(__FILE__, __LINE__, (message)); \
    } while (false)

#define CTEST_COMPARE(actual, expected) \
    ::iillm_ctest::compare((actual), (expected), __FILE__, __LINE__, #actual " == " #expected)

#define CTEST_THROWS(exception_type, expression) \
    do { \
        bool ctest_thrown = false; \
        try { (void)(expression); } \
        catch (const exception_type&) { ctest_thrown = true; } \
        if (!ctest_thrown) \
            ::iillm_ctest::fail(__FILE__, __LINE__, "expected " #exception_type " from " #expression); \
    } while (false)
