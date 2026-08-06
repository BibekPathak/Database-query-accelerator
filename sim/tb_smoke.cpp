// ===========================================================================
//  tb_smoke.cpp -- toolchain smoke test.
//
//  Runs before any RTL exists. Verifies that the build environment is sane:
//    * the C++ compiler actually supports C++20 (concepts, constexpr)
//    * the dbqa_test.hpp self-checking harness compiles and reports correctly
//    * the CTest plumbing round-trips the exit code
//
//  Serves as the template that every later Verilator testbench is modelled on.
// ===========================================================================

#include <concepts>
#include <type_traits>

#include "dbqa_test.hpp"

namespace {

template <typename T>
concept Arithmetic = std::is_arithmetic_v<T>;

template <Arithmetic T>
constexpr T fibonacci(T n) {
    T a = 0, b = 1;
    for (T i = 0; i < n; ++i) {
        T next = a + b;
        a = b;
        b = next;
    }
    return a;
}

static_assert(fibonacci(10) == 55, "constexpr evaluation must work");

}  // namespace

int main() {
    std::printf("DBQA toolchain smoke test\n");

    dbqa::check(sizeof(std::int64_t) == 8, "int64_t is 8 bytes");
    dbqa::check(fibonacci(1) == 1, "fibonacci(1) == 1");
    dbqa::expect_eq("fibonacci(10)", 55, fibonacci(10));
    dbqa::expect_eq("fibonacci(20)", 6765, fibonacci(20));
    dbqa::check(dbqa::trace_enabled() == false,
                "trace is disabled by default (no DBQA_TRACE)");

    return dbqa::summary("tb_smoke");
}
