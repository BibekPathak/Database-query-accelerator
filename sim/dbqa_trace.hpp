// ===========================================================================
//  dbqa_trace.hpp -- opt-in FST waveform capture shared by the Verilated
//  testbenches.
//
//  Tracing is OFF by default. Enable it at runtime with the DBQA_TRACE
//  environment variable (or at compile time with -DDBQA_TRACE). When enabled,
//  a testbench opens "results/<tb>.fst" and dumps one timestamp per clock
//  edge, e.g.:
//
//      auto tfp = dbqa::init_trace(dut, "results/tb_top.fst");
//      // ... inside tick(): dbqa::trace_dump(tfp.get(), ++cycle);
//
//  Only the Verilated testbenches include this header (tb_smoke is a pure
//  C++ toolchain smoke test with no DUT).
// ===========================================================================

#pragma once

#include <memory>

#include "verilated_fst_c.h"

#include "dbqa_test.hpp"

namespace dbqa {

// Opens an FST trace for `dut` if tracing is enabled, otherwise returns
// nullptr (a no-op). The trace target is written to `name`.
template <typename Dut>
inline std::unique_ptr<VerilatedFstC> init_trace(Dut& dut, const char* name) {
  if (!trace_enabled()) return nullptr;
  Verilated::traceEverOn(true);
  auto tfp = std::make_unique<VerilatedFstC>();
  dut.trace(tfp.get(), 99);
  tfp->open(name);
  return tfp;
}

// Writes the current signal state to the trace at `cycle` (no-op when tracing
// is disabled).
inline void trace_dump(VerilatedFstC* tfp, vluint64_t cycle) {
  if (tfp) tfp->dump(cycle);
}

}  // namespace dbqa
