// ===========================================================================
//  dbqa_test.hpp -- shared self-checking helpers for every DBQA testbench.
//
//  Provides a tiny assertion framework with check counting and a pass/fail
//  summary used to derive the CTest exit code, plus a uniform trace gate.
//
//  Trace policy: VCD/FST dumping is OFF by default. Enable at runtime with
//  the DBQA_TRACE environment variable, or at compile time with -DDBQA_TRACE.
// ===========================================================================

#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <type_traits>

namespace dbqa {

// ---------------------------------------------------------------------------
// Test statistics
// ---------------------------------------------------------------------------
struct TestStats {
    std::size_t checks = 0;
    std::size_t failures = 0;
};

inline TestStats& stats() {
    static TestStats s;
    return s;
}

// ---------------------------------------------------------------------------
// Checks
// ---------------------------------------------------------------------------
inline bool check(bool condition, const char* msg) {
    ++stats().checks;
    if (!condition) {
        ++stats().failures;
        std::printf("  [FAIL] %s\n", msg);
    }
    return condition;
}

template <typename T>
inline bool expect_eq(const char* what, T expected, T actual) {
    static_assert(std::is_integral_v<T>,
                  "dbqa::expect_eq supports integral types");
    ++stats().checks;
    if (expected != actual) {
        ++stats().failures;
        std::printf("  [FAIL] %s: expected %lld, got %lld\n", what,
                    static_cast<long long>(expected),
                    static_cast<long long>(actual));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Trace policy: enabled via DBQA_TRACE env var or -DDBQA_TRACE=1
// ---------------------------------------------------------------------------
inline bool trace_enabled() {
#if defined(DBQA_TRACE)
    return true;
#else
    static const bool enabled = std::getenv("DBQA_TRACE") != nullptr;
    return enabled;
#endif
}

// ---------------------------------------------------------------------------
// Test summary. Returns the CTest exit code.
// ---------------------------------------------------------------------------
inline int summary(const char* tb_name) {
    const TestStats& s = stats();
    std::printf("\n[%s] %zu checks, %zu failures\n", tb_name, s.checks,
                s.failures);
    return s.failures == 0 ? 0 : 1;
}

}  // namespace dbqa
