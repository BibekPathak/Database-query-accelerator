// ===========================================================================
//  tb_projection.cpp -- self-checking testbench for projection_engine.
//
//  Drives pipeline_data_t beats with a projection mask and verifies that
//  projected columns pass through unchanged, masked-out columns are zeroed,
//  and the predicate pass bit and tlast are preserved.
//
//  Coverage:
//    * directed masks: single column, multi-column, all columns, no columns
//    * constrained-random masks and beats, with random backpressure
//
//  Requires: rtl/common/db_pkg.sv, rtl/interfaces/axis_register.sv,
//  rtl/operators/projection_engine.sv
// ===========================================================================

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>
#include <vector>

#include "Vprojection_engine.h"
#include "dbqa_test.hpp"

namespace {

struct RefBeat {
  uint32_t cols[4];
  bool pass;
  bool last;
};

// Drive `beats` through the engine with a projection mask and verify the
// output stream, optionally with random backpressure. Uses the skid-register
// timing model validated in tb_predicate: the output always presents the next
// unconsumed beat, so it is verified against `beats[expected]` every valid
// cycle and `expected` advances only when a beat is consumed.
void run_beats(Vprojection_engine& dut, std::function<void()> tick,
               const std::vector<RefBeat>& beats, uint32_t mask, uint32_t seed,
               bool allow_stall) {
  std::mt19937 rng(seed);
  size_t presented = 0;
  size_t expected = 0;

  for (int cyc = 0;
       cyc < 8 * static_cast<int>(beats.size()) + 64 &&
       (presented < beats.size() || expected < beats.size());
       ++cyc) {
    // Drive the input first (the empty skid register passes it through).
    const bool push = (presented < beats.size());
    dut.s_axis_tvalid = push;
    if (push) {
      for (int c = 0; c < 4; ++c) dut.s_axis_tdata[c] = beats[presented].cols[c];
      dut.s_axis_tdata[4] = beats[presented].pass ? 1u : 0u;
      dut.s_axis_tlast = beats[presented].last;
    }
    dut.eval();

    // Verify the current output against the next unconsumed beat.
    const bool valid = dut.m_axis_tvalid;
    if (valid && expected < beats.size()) {
      const RefBeat& f = beats[expected];
      for (int c = 0; c < 4; ++c) {
        const uint32_t expect = ((mask >> c) & 1u) ? f.cols[c] : 0u;
        char msg[64];
        std::snprintf(msg, sizeof msg, "mask 0x%x column %d", mask, c);
        dbqa::expect_eq(msg, expect, dut.m_axis_tdata[c]);
      }
      const bool got_pass = ((dut.m_axis_tdata[4] >> 0) & 1u) != 0;
      dbqa::check(got_pass == f.pass, "pass bit passthrough");
      dbqa::check(dut.m_axis_tlast == f.last, "tlast passthrough");
    }

    const bool ready = allow_stall ? (rng() % 3 != 0) : true;
    dut.m_axis_tready = ready;
    if (valid && ready && expected < beats.size()) ++expected;

    if (push && dut.s_axis_tready) ++presented;

    tick();
  }
  dbqa::check(expected == beats.size(), "all beats consumed");
}

std::vector<RefBeat> make_beats(int n, uint32_t seed) {
  std::mt19937 rng(seed);
  std::vector<RefBeat> beats(n);
  for (int b = 0; b < n; ++b) {
    for (int c = 0; c < 4; ++c) beats[b].cols[c] = rng();
    beats[b].pass = (rng() % 2) != 0;
    beats[b].last = false;
  }
  beats.back().last = true;
  return beats;
}

}  // namespace

int main() {
  Vprojection_engine dut;
  dut.clk = 0;
  dut.rst = 1;
  dut.proj_mask = 0;
  dut.s_axis_tvalid = 0;
  dut.s_axis_tlast = 0;
  dut.m_axis_tready = 0;
  for (int i = 0; i < 5; ++i) dut.s_axis_tdata[i] = 0;
  dut.eval();

  auto tick = [&]() {
    dut.clk = 1;
    dut.eval();
    dut.clk = 0;
    dut.eval();
  };
  for (int i = 0; i < 2; ++i) tick();
  dut.rst = 0;
  tick();

  std::printf("DBQA projection_engine testbench\n");

  // Directed masks: single column, multi-column, all, none.
  const uint32_t masks[4] = {0x1, 0xA, 0xF, 0x0};
  for (int m = 0; m < 4; ++m) {
    dut.proj_mask = masks[m];
    auto beats = make_beats(48, 0x600 + m);
    std::printf("  mask 0x%x (no stall)\n", masks[m]);
    run_beats(dut, tick, beats, masks[m], 0x600 + m, false);
    std::printf("  mask 0x%x (backpressure)\n", masks[m]);
    run_beats(dut, tick, beats, masks[m], 0x700 + m, true);
  }

  // Constrained-random masks + beats + backpressure.
  std::mt19937 rng(0xFEED);
  for (int run = 0; run < 30; ++run) {
    const uint32_t mask = rng() & 0xF;
    dut.proj_mask = mask;
    auto beats = make_beats(64, 0x800 + run);
    run_beats(dut, tick, beats, mask, 0x800 + run, true);
  }
  std::printf("  constrained-random complete\n");

  return dbqa::summary("tb_projection");
}
