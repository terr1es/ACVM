//===--- TestHarness.hpp - Minimal self contained test framework --------===//
#pragma once

#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace test {

struct Case {
    std::string name;
    std::function<void()> function;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& failureCount() {
    static int count = 0;
    return count;
}

inline std::string& currentCase() {
    static std::string name;
    return name;
}

struct Registrar {
    Registrar(std::string name, std::function<void()> function) {
        registry().push_back(Case{std::move(name), std::move(function)});
    }
};

inline void fail(const char* file, int line, const std::string& message) {
    ++failureCount();
    std::cerr << "  FAILED " << currentCase() << '\n'
              << "    " << file << ':' << line << ": " << message << '\n';
}

inline int runAll() {
    for (const Case& testCase : registry()) {
        currentCase() = testCase.name;
        const int before = failureCount();
        try {
            testCase.function();
        } catch (const std::exception& error) {
            fail("<exception>", 0, error.what());
        } catch (...) {
            fail("<exception>", 0, "unknown exception");
        }
        std::cout << (failureCount() == before ? "  ok    " : "  FAIL  ") << testCase.name << '\n';
    }
    std::cout << '\n' << registry().size() << " test(s), " << failureCount() << " failure(s)\n";
    return failureCount() == 0 ? 0 : 1;
}

} // namespace test

#define TEST(name)                                                                                 \
    static void test_##name();                                                                     \
    static ::test::Registrar registrar_##name(#name, test_##name);                                 \
    static void test_##name()

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            ::test::fail(__FILE__, __LINE__, "CHECK failed: " #condition);                         \
        }                                                                                          \
    } while (false)

#define CHECK_EQ(actual, expected)                                                                 \
    do {                                                                                           \
        const auto& checkEqActual = (actual);                                                      \
        const auto& checkEqExpected = (expected);                                                  \
        if (!(checkEqActual == checkEqExpected)) {                                                 \
            std::ostringstream checkEqStream;                                                      \
            checkEqStream << "CHECK_EQ failed: " #actual " == " #expected << " (got "             \
                          << checkEqActual << ", expected " << checkEqExpected << ')';             \
            ::test::fail(__FILE__, __LINE__, checkEqStream.str());                                 \
        }                                                                                          \
    } while (false)

#define TEST_MAIN()                                                                                \
    int main() { return ::test::runAll(); }
