#pragma once

// Minimal dependency-free test harness for MiniSearch.
//
// Rationale (spec section 12 says "use a testing framework such as
// GoogleTest if appropriate"): phase 1 keeps the build free of external
// dependencies, so the suite runs on this ~100-line harness. Its
// MINI_TEST / MINI_EXPECT_* interface deliberately mirrors GoogleTest,
// so test bodies survive a mechanical migration if we vendor GTest later.

#include <cmath>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace minitest {

struct TestCase {
    std::string name;
    std::function<void()> body;
};

inline std::vector<TestCase>& tests() {
    static std::vector<TestCase> registry;
    return registry;
}

inline int& failureCount() {
    static int count = 0;
    return count;
}

inline std::string& currentTest() {
    static std::string name;
    return name;
}

struct Registrar {
    Registrar(std::string name, std::function<void()> body) {
        tests().push_back(TestCase{std::move(name), std::move(body)});
    }
};

inline void reportFailure(const char* file, int line, const std::string& expression,
                          const std::string& actual, const std::string& expected) {
    ++failureCount();
    std::cerr << "  FAIL " << file << ":" << line << " in \"" << currentTest() << "\"\n"
              << "       expected: " << expected << "\n"
              << "       actual:   " << actual << "\n"
              << "       expr:     " << expression << "\n";
}

template <typename T>
std::string stringify(const T& value) {
    std::ostringstream oss;
    oss << value;
    return oss.str();
}

template <typename T>
std::string stringify(const std::vector<T>& values) {
    std::ostringstream oss;
    oss << '[';
    bool first = true;
    for (const T& value : values) {
        if (!first) oss << ", ";
        oss << stringify(value);
        first = false;
    }
    oss << ']';
    return oss.str();
}

inline int runAll(const char* suiteName) {
    std::cout << "[========] Running " << tests().size() << " tests from " << suiteName << "\n";
    int passed = 0;
    for (const auto& test : tests()) {
        currentTest() = test.name;
        const int failuresBefore = failureCount();
        std::cout << "[ RUN    ] " << test.name << "\n";
        test.body();
        if (failureCount() == failuresBefore) {
            ++passed;
            std::cout << "[     OK ] " << test.name << "\n";
        } else {
            std::cout << "[  FAILED] " << test.name << "\n";
        }
    }
    const int failed = failureCount();
    std::cout << "[========] " << passed << " passed, " << failed << " failed, "
              << tests().size() << " total\n";
    return failed == 0 ? 0 : 1;
}

} // namespace minitest

#define MINI_TEST(name)                                                     \
    static void minisearch_test_##name();                                   \
    [[maybe_unused]] static const ::minitest::Registrar                     \
        minisearch_test_registrar_##name(#name, minisearch_test_##name);    \
    static void minisearch_test_##name()

#define MINI_EXPECT_TRUE(expression)                                        \
    do {                                                                    \
        if (!(expression)) {                                                \
            ::minitest::reportFailure(__FILE__, __LINE__, #expression,      \
                                      "false", "true");                     \
        }                                                                   \
    } while (false)

#define MINI_EXPECT_FALSE(expression)                                       \
    do {                                                                    \
        if ((expression)) {                                                 \
            ::minitest::reportFailure(__FILE__, __LINE__, #expression,      \
                                      "true", "false");                     \
        }                                                                   \
    } while (false)

#define MINI_EXPECT_EQ(actual, expected)                                    \
    do {                                                                    \
        const auto actualValue = (actual);                                 \
        const auto expectedValue = (expected);                             \
        if (!(actualValue == expectedValue)) {                              \
            ::minitest::reportFailure(__FILE__, __LINE__,                   \
                                      #actual " == " #expected,             \
                                      ::minitest::stringify(actualValue),   \
                                      ::minitest::stringify(expectedValue));\
        }                                                                   \
    } while (false)

#define MINI_EXPECT_NE(actual, expected)                                    \
    do {                                                                    \
        const auto actualValue = (actual);                                 \
        const auto expectedValue = (expected);                             \
        if (actualValue == expectedValue) {                                 \
            ::minitest::reportFailure(__FILE__, __LINE__,                   \
                                      #actual " != " #expected,             \
                                      ::minitest::stringify(actualValue),   \
                                      ::minitest::stringify(expectedValue));\
        }                                                                   \
    } while (false)

#define MINI_EXPECT_NEAR(actual, expected, epsilon)                         \
    do {                                                                    \
        const double actualValue = static_cast<double>(actual);             \
        const double expectedValue = static_cast<double>(expected);         \
        if (std::fabs(actualValue - expectedValue) > (epsilon)) {           \
            ::minitest::reportFailure(__FILE__, __LINE__,                   \
                                      #actual " ~= " #expected,             \
                                      ::minitest::stringify(actualValue),   \
                                      ::minitest::stringify(expectedValue));\
        }                                                                   \
    } while (false)

#define MINI_CMP(op, actual, expected)                                      \
    do {                                                                    \
        const auto actualValue = (actual);                                 \
        const auto expectedValue = (expected);                             \
        if (!(actualValue op expectedValue)) {                              \
            ::minitest::reportFailure(__FILE__, __LINE__,                   \
                                      #actual " " #op " " #expected,        \
                                      ::minitest::stringify(actualValue),   \
                                      ::minitest::stringify(expectedValue));\
        }                                                                   \
    } while (false)

#define MINI_EXPECT_GT(actual, expected) MINI_CMP(>, actual, expected)
#define MINI_EXPECT_GE(actual, expected) MINI_CMP(>=, actual, expected)
#define MINI_EXPECT_LT(actual, expected) MINI_CMP(<, actual, expected)
#define MINI_EXPECT_LE(actual, expected) MINI_CMP(<=, actual, expected)
