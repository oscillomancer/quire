// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// A test harness small enough to read in a minute, with no dependency.
//
//   TEST(filter_attenuates_below_cutoff) {
//       CHECK(level < -20.0);
//       CHECK_NEAR(gain, 1.0, 1e-9);
//   }
//   SUPPLE_TEST_MAIN
//
// Every check runs (a failure does not stop the test), each failure prints
// its file, line and values, and the process exits non-zero if any failed.
// Pass a substring as the first argument to run only matching tests.
#pragma once

#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace supple::test {

inline double finiteDifference(double a, double b) {
    return std::isfinite(a) && std::isfinite(b) ? std::fabs(a - b) : INFINITY;
}

struct Case {
    const char* name;
    void (*run)();
};

inline std::vector<Case>& cases() {
    static std::vector<Case> all;
    return all;
}
inline int& failures() {
    static int count = 0;
    return count;
}

struct Register {
    Register(const char* name, void (*run)()) { cases().push_back({name, run}); }
};

inline void fail(const char* file, int line, const std::string& what) {
    ++failures();
    std::printf("      FAIL %s:%d: %s\n", file, line, what.c_str());
}

inline const char*& running() {
    static const char* name = nullptr;
    return name;
}

inline int run(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
#ifdef _WIN32
    // Windows says only that the process faulted: name the test. (Elsewhere
    // the sanitizers' own handlers say more.)
    std::signal(SIGSEGV, [](int) {
        std::fprintf(stderr, "\n  %s crashed\n", running() ? running() : "(no test)");
        std::fflush(stderr);
        std::_Exit(3);
    });
#endif
    int ran = 0, failed = 0;
    for (const Case& c : cases()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        const int before = failures();
        running() = c.name;
        c.run();
        ++ran;
        const bool ok = failures() == before;
        failed += ok ? 0 : 1;
        std::printf("  %-64s %s\n", c.name, ok ? "ok" : "FAIL");
        std::fflush(stdout);  // a crash in the next test keeps this one's line
    }
    std::printf("\n%d tests, %d failed\n", ran, failed);
    return failed ? 1 : 0;
}

}  // namespace supple::test

#define SUPPLE_TEST_CAT2(a, b) a##b
#define SUPPLE_TEST_CAT(a, b) SUPPLE_TEST_CAT2(a, b)
#define TEST(name)                                                                       \
    static void name();                                                                  \
    static const ::supple::test::Register SUPPLE_TEST_CAT(register_, name)(#name, name); \
    static void name()

#define CHECK(condition)                                                         \
    do {                                                                         \
        if (!(condition)) ::supple::test::fail(__FILE__, __LINE__, #condition);  \
    } while (0)

#define CHECK_MSG(condition, ...)                                        \
    do {                                                                 \
        if (!(condition)) {                                              \
            char message_[768];                                          \
            std::snprintf(message_, sizeof(message_), __VA_ARGS__);      \
            ::supple::test::fail(__FILE__, __LINE__, message_);          \
        }                                                                \
    } while (0)

#define CHECK_NEAR(a, b, tolerance)                                                                   \
    do {                                                                                              \
        const double a_ = (a), b_ = (b);                                                              \
        if (!(std::fabs(a_ - b_) <= (tolerance))) {                                                   \
            char message_[384];                                                                       \
            std::snprintf(message_, sizeof(message_), "%s = %.12g, %s = %.12g, tolerance %g", #a, a_, \
                          #b, b_, static_cast<double>(tolerance));                                    \
            ::supple::test::fail(__FILE__, __LINE__, message_);                                       \
        }                                                                                             \
    } while (0)

#define SUPPLE_TEST_MAIN \
    int main(int argc, char** argv) { return ::supple::test::run(argc, argv); }
