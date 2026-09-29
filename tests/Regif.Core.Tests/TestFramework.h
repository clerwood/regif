#pragma once

// A deliberately tiny test framework so the test project has no dependencies.
// TEST_CASE registers a function; CHECK* record failures and keep going.

#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace regif::test {

struct TestCase {
    const char* name;
    std::function<void()> body;
};

inline std::vector<TestCase>& registry()
{
    static std::vector<TestCase> tests;
    return tests;
}

inline int& failureCount()
{
    static int failures = 0;
    return failures;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> body) { registry().push_back({ name, std::move(body) }); }
};

inline void reportFailure(const char* file, int line, const std::string& message)
{
    ++failureCount();
    std::cerr << "  " << file << "(" << line << "): " << message << "\n";
}

template <class A, class B>
void checkEqual(const A& actual, const B& expected, const char* actualText, const char* expectedText,
                const char* file, int line)
{
    if (actual == expected) return;
    std::ostringstream text;
    text << "CHECK_EQ(" << actualText << ", " << expectedText << ")\n      actual:   " << actual
         << "\n      expected: " << expected;
    reportFailure(file, line, text.str());
}

inline int runAll()
{
    int failedTests = 0;
    for (const TestCase& test : registry()) {
        const int before = failureCount();
        try {
            test.body();
        } catch (const std::exception& e) {
            reportFailure(test.name, 0, std::string("unexpected exception: ") + e.what());
        } catch (...) {
            reportFailure(test.name, 0, "unexpected non-standard exception");
        }
        const bool passed = failureCount() == before;
        if (!passed) ++failedTests;
        std::cout << (passed ? "[pass] " : "[FAIL] ") << test.name << "\n";
    }
    std::cout << "\n" << static_cast<int>(registry().size()) - failedTests << " passed, " << failedTests << " failed\n";
    return failedTests == 0 ? 0 : 1;
}

} // namespace regif::test

#define REGIF_CONCAT_INNER(a, b) a##b
#define REGIF_CONCAT(a, b) REGIF_CONCAT_INNER(a, b)

#define TEST_CASE(name)                                                                          \
    static void REGIF_CONCAT(regifTest_, __LINE__)();                                           \
    static ::regif::test::Registrar REGIF_CONCAT(regifRegistrar_, __LINE__)(name, &REGIF_CONCAT(regifTest_, __LINE__)); \
    static void REGIF_CONCAT(regifTest_, __LINE__)()

#define CHECK(expr)                                                                              \
    do {                                                                                         \
        if (!(expr)) ::regif::test::reportFailure(__FILE__, __LINE__, "CHECK(" #expr ")");       \
    } while (false)

#define CHECK_EQ(actual, expected) \
    ::regif::test::checkEqual((actual), (expected), #actual, #expected, __FILE__, __LINE__)

#define CHECK_THROWS_AS(expr, type)                                                              \
    do {                                                                                         \
        bool regifThrew = false;                                                                 \
        try { (void)(expr); } catch (const type&) { regifThrew = true; } catch (...) {}          \
        if (!regifThrew) ::regif::test::reportFailure(__FILE__, __LINE__, "CHECK_THROWS_AS(" #expr ", " #type ")"); \
    } while (false)

#define CHECK_CONTAINS(haystack, needle)                                                         \
    do {                                                                                         \
        const std::string regifHay(haystack);                                                    \
        if (regifHay.find(needle) == std::string::npos)                                          \
            ::regif::test::reportFailure(__FILE__, __LINE__,                                     \
                std::string("CHECK_CONTAINS: \"") + regifHay + "\" does not contain \"" + (needle) + "\""); \
    } while (false)
