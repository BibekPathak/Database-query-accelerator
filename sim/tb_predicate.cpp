// ===========================================================================
//  tb_predicate.cpp -- self-checking testbench for predicate_engine.
//
//  Drives pipeline_data_t beats with a configured predicate list and verifies
//  the output pass bit against a software reference implementing the same
//  combination semantics (first enabled slot seeds; later enabled slots fold
//  via AND/OR; disabled slots skipped). Also verifies column and tlast
//  passthrough.
//
//  Coverage:
//    * every operator (==, !=, <, >, <=, >=) against boundary values
//    * AND / OR truth tables over two slots
//    * disabled slots and mixed AND/OR chains
//    * constrained-random configurations and beats, with random backpressure
//
//  Requires: rtl/common/db_pkg.sv, rtl/interfaces/axis_register.sv,
//  rtl/operators/predicate_engine.sv
// ===========================================================================

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <queue>
#include <random>
#include <vector>

#include "Vpredicate_engine.h"
#include "dbqa_test.hpp"
#include "dbqa_trace.hpp"

namespace {

constexpr int PRED_EQ = 0, PRED_NEQ = 1, PRED_LT = 2, PRED_GT = 3;
constexpr int PRED_LTE = 4, PRED_GTE = 5;
constexpr int LOGIC_AND = 0, LOGIC_OR = 1;

// pred_cfg_t is packed {enable, op, combine, column, imm} with enable at
// bit 46 (packed structs are most-significant-first).
uint64_t pack_cfg(int en, int op, int cmb, int col, uint32_t imm) {
  return (static_cast<uint64_t>(en) << 46) | (static_cast<uint64_t>(op) << 43) |
         (static_cast<uint64_t>(cmb) << 42) |
         (static_cast<uint64_t>(col) << 32) | imm;
}

// Reference combination (matches the RTL).
bool ref_pass(const int en[2], const int op[2], const int cmb[2],
              const int col[2], const uint32_t imm[2], const uint32_t cols[4]) {
  auto apply = [&](int o, uint32_t a, uint32_t b) {
    switch (o) {
      case PRED_EQ: return a == b;
      case PRED_NEQ: return a != b;
      case PRED_LT: return a < b;
      case PRED_GT: return a > b;
      case PRED_LTE: return a <= b;
      default: return a >= b;
    }
  };
  bool pass = true;
  bool found = false;
  for (int i = 0; i < 2; ++i) {
    if (en[i]) {
      const bool r = apply(op[i], cols[col[i]], imm[i]);
      if (!found) {
        pass = r;
        found = true;
      } else {
        pass = (cmb[i] == LOGIC_AND) ? (pass && r) : (pass || r);
      }
    }
  }
  return pass;
}

struct RefBeat {
  uint32_t cols[4];
  bool pass;
  bool last;
};

// Drive `beats` through the engine and verify the output stream against the
// reference, optionally with random backpressure.
//
// Timing model: the output skid register always presents the next
// unconsumed beat (either held in its buffer or passed through combinationally
// from the input), so the output is verified against `beats[expected]` every
// valid cycle and `expected` advances only when a beat is consumed.
void run_beats(Vpredicate_engine& dut, std::function<void()> tick,
               const std::vector<RefBeat>& beats, const int en[2],
               const int op[2], const int cmb[2], const int col[2],
               const uint32_t imm[2], uint32_t seed, bool allow_stall) {
  std::mt19937 rng(seed);
  size_t presented = 0;
  size_t expected = 0;

  for (int cyc = 0;
       cyc < 8 * static_cast<int>(beats.size()) + 64 &&
       (presented < beats.size() || expected < beats.size());
       ++cyc) {
    // Drive the input first: the output skid register passes the input
    // through combinationally while empty, so the output reflects the newly
    // driven beat.
    const bool push = (presented < beats.size());
    dut.s_axis_tvalid = push;
    if (push) {
      for (int c = 0; c < 4; ++c) dut.s_axis_tdata[c] = beats[presented].cols[c];
      dut.s_axis_tdata[4] = 0;  // input pass bit is ignored/overwritten
      dut.s_axis_tlast = beats[presented].last;
    }
    dut.eval();

    // The output always holds the next unconsumed beat: either a beat held in
    // the register or the input beat passed through. Verify and consume it.
    const bool valid = dut.m_axis_tvalid;
    if (valid && expected < beats.size()) {
      const RefBeat& f = beats[expected];
      for (int c = 0; c < 4; ++c) {
        char msg[64];
        std::snprintf(msg, sizeof msg, "column %d passthrough", c);
        dbqa::expect_eq(msg, f.cols[c], dut.m_axis_tdata[c]);
      }
      const bool got_pass = ((dut.m_axis_tdata[4] >> 0) & 1u) != 0;
      dbqa::check(got_pass == f.pass, "pass matches reference");
      dbqa::check(dut.m_axis_tlast == f.last, "tlast passthrough");
    }

    const bool ready = allow_stall ? (rng() % 3 != 0) : true;
    dut.m_axis_tready = ready;
    if (valid && ready && expected < beats.size()) ++expected;

    // The input beat is accepted by the register only when it is empty
    // (tready is combinational on the pre-posedge register state).
    if (push && dut.s_axis_tready) ++presented;

    tick();
  }
  dbqa::check(expected == beats.size(), "all beats consumed");
}

std::vector<RefBeat> make_beats(int n, const int en[2], const int op[2],
                                const int cmb[2], const int col[2],
                                const uint32_t imm[2], uint32_t seed) {
  std::mt19937 rng(seed);
  std::vector<RefBeat> beats(n);
  for (int b = 0; b < n; ++b) {
    for (int c = 0; c < 4; ++c) beats[b].cols[c] = rng();
    beats[b].pass = ref_pass(en, op, cmb, col, imm, beats[b].cols);
    beats[b].last = (b == n - 1);
  }
  return beats;
}

}  // namespace

int main() {
  Vpredicate_engine dut;
  dut.clk = 0;
  dut.rst = 1;
  dut.s_axis_tvalid = 0;
  dut.s_axis_tlast = 0;
  dut.m_axis_tready = 0;
  for (int i = 0; i < 5; ++i) dut.s_axis_tdata[i] = 0;
  dut.pred_cfg[0] = 0;
  dut.pred_cfg[1] = 0;
  dut.eval();

  vluint64_t trace_cycle = 0;
  auto tfp = dbqa::init_trace(dut, "results/tb_predicate.fst");
  auto tick = [&]() {
    dut.clk = 1;
    dut.eval();
    dut.clk = 0;
    dut.eval();
    dbqa::trace_dump(tfp.get(), ++trace_cycle);
  };
  for (int i = 0; i < 2; ++i) tick();
  dut.rst = 0;
  tick();

  std::printf("DBQA predicate_engine testbench\n");

  int en[2], op[2], cmb[2], col[2];
  uint32_t imm[2];

  // -------------------------------------------------------------------------
  // Directed: every operator against boundary values (col0 OP imm).
  // -------------------------------------------------------------------------
  const int ops[6] = {PRED_EQ, PRED_NEQ, PRED_LT, PRED_GT, PRED_LTE, PRED_GTE};
  const char* opnames[6] = {"EQ", "NEQ", "LT", "GT", "LTE", "GTE"};
  for (int o = 0; o < 6; ++o) {
    en[0] = 1; op[0] = ops[o]; cmb[0] = LOGIC_AND; col[0] = 0; imm[0] = 100;
    en[1] = 0; op[1] = PRED_EQ; cmb[1] = LOGIC_AND; col[1] = 1; imm[1] = 0;
    dut.pred_cfg[0] = pack_cfg(1, ops[o], LOGIC_AND, 0, 100);
    dut.pred_cfg[1] = 0;
    std::vector<RefBeat> beats;
    const uint32_t probes[6] = {0, 99, 100, 101, 0xFFFFFFFFu, 42};
    for (uint32_t v : probes) {
      RefBeat b;
      b.cols[0] = v;
      b.cols[1] = b.cols[2] = b.cols[3] = 0;
      b.pass = ref_pass(en, op, cmb, col, imm, b.cols);
      b.last = false;
      beats.push_back(b);
    }
    beats.back().last = true;
    std::printf("  operator %-3s\n", opnames[o]);
    run_beats(dut, tick, beats, en, op, cmb, col, imm, 0x100 + o, false);
  }

  // -------------------------------------------------------------------------
  // Directed: AND and OR truth tables (p0 = (col0==5), p1 = (col1==7)).
  // -------------------------------------------------------------------------
  for (int c = 0; c < 2; ++c) {  // c = 0 -> AND, c = 1 -> OR
    en[0] = 1; op[0] = PRED_EQ; cmb[0] = LOGIC_AND; col[0] = 0; imm[0] = 5;
    en[1] = 1; op[1] = PRED_EQ; cmb[1] = c; col[1] = 1; imm[1] = 7;
    dut.pred_cfg[0] = pack_cfg(1, PRED_EQ, LOGIC_AND, 0, 5);
    dut.pred_cfg[1] = pack_cfg(1, PRED_EQ, c, 1, 7);
    std::vector<RefBeat> beats;
    const uint32_t a[2] = {0, 5};
    const uint32_t b[2] = {0, 7};
    for (uint32_t va : a)
      for (uint32_t vb : b) {
        RefBeat x;
        x.cols[0] = va; x.cols[1] = vb; x.cols[2] = x.cols[3] = 0;
        x.pass = ref_pass(en, op, cmb, col, imm, x.cols);
        x.last = false;
        beats.push_back(x);
      }
    beats.back().last = true;
    std::printf("  truth table %s\n", c ? "OR" : "AND");
    run_beats(dut, tick, beats, en, op, cmb, col, imm, 0x200 + c, false);
  }

  // -------------------------------------------------------------------------
  // Directed: disabled slots.
  // -------------------------------------------------------------------------
  {  // both disabled -> every row passes
    en[0] = 0; op[0] = PRED_EQ; cmb[0] = LOGIC_AND; col[0] = 0; imm[0] = 5;
    en[1] = 0; op[1] = PRED_EQ; cmb[1] = LOGIC_AND; col[1] = 1; imm[1] = 7;
    dut.pred_cfg[0] = 0;
    dut.pred_cfg[1] = 0;
    auto beats = make_beats(32, en, op, cmb, col, imm, 0x300);
    for (auto& b : beats) {
      if (!b.pass) std::printf("  [FAIL] all-pass expected\n");
      b.pass = true;
    }
    beats.back().last = true;
    std::printf("  both slots disabled (all pass)\n");
    run_beats(dut, tick, beats, en, op, cmb, col, imm, 0x300, false);
  }
  {  // only slot 1 enabled (slot 0 disabled): OR on a single predicate
    en[0] = 0; op[0] = PRED_EQ; cmb[0] = LOGIC_AND; col[0] = 0; imm[0] = 5;
    en[1] = 1; op[1] = PRED_NEQ; cmb[1] = LOGIC_OR; col[1] = 3; imm[1] = 9;
    dut.pred_cfg[0] = 0;
    dut.pred_cfg[1] = pack_cfg(1, PRED_NEQ, LOGIC_OR, 3, 9);
    auto beats = make_beats(48, en, op, cmb, col, imm, 0x301);
    beats.back().last = true;
    std::printf("  only slot 1 enabled\n");
    run_beats(dut, tick, beats, en, op, cmb, col, imm, 0x301, true);
  }

  // -------------------------------------------------------------------------
  // Constrained-random: random configs + beats + backpressure.
  // -------------------------------------------------------------------------
  std::mt19937 rng(0xDEADBEEF);
  for (int run = 0; run < 40; ++run) {
    en[0] = rng() % 2; en[1] = rng() % 2;
    op[0] = rng() % 6; op[1] = rng() % 6;
    cmb[0] = rng() % 2; cmb[1] = rng() % 2;
    col[0] = rng() % 4; col[1] = rng() % 4;
    imm[0] = rng(); imm[1] = rng();
    dut.pred_cfg[0] = pack_cfg(en[0], op[0], cmb[0], col[0], imm[0]);
    dut.pred_cfg[1] = pack_cfg(en[1], op[1], cmb[1], col[1], imm[1]);
    auto beats = make_beats(80, en, op, cmb, col, imm, 0x400 + run);
    beats.back().last = true;
    run_beats(dut, tick, beats, en, op, cmb, col, imm, 0x400 + run, true);
  }
  std::printf("  constrained-random complete\n");

  return dbqa::summary("tb_predicate");
}
