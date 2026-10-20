#pragma once

// A deliberately tiny test framework: register named test functions, run them
// all, count failed checks. Enough for this project without a dependency.

#include <exception>
#include <functional>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace testing {

struct TestCase {
    std::string name;
    std::function<void()> body;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

inline int& failed_checks() {
    static int count = 0;
    return count;
}

inline bool add_test(std::string name, std::function<void()> body) {
    registry().push_back({std::move(name), std::move(body)});
    return true;
}

// Thrown by REQUIRE to abort the current test.
struct RequireFailed {};

inline int run_all() {
    int failed_tests = 0;
    for (const TestCase& test : registry()) {
        const int before = failed_checks();
        try {
            test.body();
        } catch (const RequireFailed&) {
            // already reported
        } catch (const std::exception& e) {
            ++failed_checks();
            std::cerr << "  unexpected exception: " << e.what() << '\n';
        }
        const bool ok = failed_checks() == before;
        failed_tests += ok ? 0 : 1;
        std::cout << (ok ? "[ PASS ] " : "[ FAIL ] ") << test.name << '\n';
    }
    std::cout << '\n'
              << registry().size() - static_cast<std::size_t>(failed_tests) << " passed, "
              << failed_tests << " failed\n";
    return failed_tests == 0 ? 0 : 1;
}

}  // namespace testing

#define TEST_CONCAT_INNER(a, b) a##b
#define TEST_CONCAT(a, b) TEST_CONCAT_INNER(a, b)

#define TEST_CASE(name)                                                              \
    static void TEST_CONCAT(test_fn_, __LINE__)();                                   \
    static const bool TEST_CONCAT(test_reg_, __LINE__) =                             \
        testing::add_test(name, TEST_CONCAT(test_fn_, __LINE__));                    \
    static void TEST_CONCAT(test_fn_, __LINE__)()

#define CHECK(expr)                                                                  \
    do {                                                                             \
        if (!(expr)) {                                                               \
            ++testing::failed_checks();                                              \
            std::cerr << "  " << __FILE__ << ':' << __LINE__ << ": CHECK(" #expr    \
                      << ") failed\n";                                               \
        }                                                                            \
    } while (false)

#define CHECK_EQ(actual, expected)                                                   \
    do {                                                                             \
        const auto& check_actual_ = (actual);                                        \
        const auto& check_expected_ = (expected);                                    \
        if (!(check_actual_ == check_expected_)) {                                   \
            ++testing::failed_checks();                                              \
            std::cerr << "  " << __FILE__ << ':' << __LINE__ << ": CHECK_EQ(" #actual \
                      << ", " #expected ") failed\n";                                \
        }                                                                            \
    } while (false)

#define REQUIRE(expr)                                                                \
    do {                                                                             \
        if (!(expr)) {                                                               \
            ++testing::failed_checks();                                              \
            std::cerr << "  " << __FILE__ << ':' << __LINE__ << ": REQUIRE(" #expr  \
                      << ") failed\n";                                               \
            throw testing::RequireFailed{};                                          \
        }                                                                            \
    } while (false)
