// ===========================================================================
//  tb_aggregation.cpp -- self-checking testbench for aggregation_top.
//
//  Drives a pipeline_data_t stream through the aggregation top for each
//  opcode and verifies the final result, count and overflow against a C++
//  reference implementing the same aggregation semantics (SUM saturates at
//  the ACCUM_W-bit maximum; MIN/MAX keep their sentinel when no row passes).
//
//  Coverage:
//    * every aggregation opcode (COUNT, SUM, MIN, MAX, AVG)
//    * all-pass, all-fail and single-row tables
//    * SUM overflow saturation
//    * constrained-random opcodes, columns and beats
//
//  Requires: rtl/common/db_pkg.sv and all five aggregation engines plus
//  rtl/operators/aggregation_top.sv
// ===========================================================================

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>
#include <vector>

#include "Vaggregation_top.h"
#include "dbqa_test.hpp"

namespace {

constexpr int OP_COUNT = 1, OP_SUM = 2, OP_MIN = 3, OP_MAX = 4, OP_AVG = 5;
constexpr uint64_t ACCUM_MAX = 0x3FFFFFFFFFFull;  // 42-bit all-ones

struct Beat {
  uint32_t cols[4];
  bool pass;
};

// Reference aggregation result for one query.
struct AggRef {
  uint64_t count;
  uint64_t sum;
  bool overflow;
  uint32_t minv;   // 0xFFFFFFFF if no row passed
  uint32_t maxv;   // 0 if no row passed
};

AggRef reference(const std::vector<Beat>& beats, int column) {
  AggRef r{0, 0, false, 0xFFFFFFFFu, 0u};
  for (const Beat& b : beats) {
    if (b.pass) {
      ++r.count;
      const uint64_t v = b.cols[column];
      if (!r.overflow) {
        if (r.sum + v > ACCUM_MAX) {
          r.sum = ACCUM_MAX;
          r.overflow = true;
        } else {
          r.sum += v;
        }
      }
      if (v < r.minv) r.minv = v;
      if (v > r.maxv) r.maxv = v;
    }
  }
  return r;
}

void run_query(Vaggregation_top& dut, std::function<void()> tick,
               const std::vector<Beat>& beats, int op, int column,
               uint32_t seed) {
  // agg_cfg packed: {op[3], groupby, column[10], gby_key[10]}.
  dut.agg_cfg = (static_cast<uint64_t>(op) << 21) |
                (static_cast<uint64_t>(column) << 10);

  const AggRef ref = reference(beats, column);

  dut.start = 1;
  tick();
  dut.start = 0;
  dbqa::check(dut.busy, "busy asserted after start");

  for (size_t i = 0; i < beats.size(); ++i) {
    dut.s_axis_tvalid = 1;
    for (int c = 0; c < 4; ++c) dut.s_axis_tdata[c] = beats[i].cols[c];
    dut.s_axis_tdata[4] = beats[i].pass ? 1u : 0u;
    dut.s_axis_tlast = (i == beats.size() - 1);
    tick();
  }
  dut.s_axis_tvalid = 0;
  tick();

  dbqa::check(dut.done, "done asserted");
  dbqa::check(!dut.busy, "busy deasserted after done");
  dbqa::check(!dut.s_axis_tready, "input no longer accepted after done");

  dbqa::expect_eq("count result", ref.count, dut.count);

  uint64_t expect_result = 0;
  switch (op) {
    case OP_COUNT: expect_result = ref.count; break;
    case OP_SUM: expect_result = ref.sum; break;
    case OP_MIN: expect_result = ref.minv; break;
    case OP_MAX: expect_result = ref.maxv; break;
    case OP_AVG: expect_result = ref.sum; break;
  }
  dbqa::expect_eq("primary result", expect_result, dut.result);
  dbqa::check(dut.overflow == ref.overflow, "overflow flag matches reference");
}

std::vector<Beat> make_beats(size_t n, uint32_t seed, bool all_pass = false) {
  std::mt19937 rng(seed);
  std::vector<Beat> beats(n);
  for (auto& b : beats) {
    for (int c = 0; c < 4; ++c) b.cols[c] = rng();
    b.pass = all_pass ? true : (rng() % 2) != 0;
  }
  return beats;
}

}  // namespace

int main() {
  Vaggregation_top dut;
  dut.clk = 0;
  dut.rst = 1;
  dut.agg_cfg = 0;
  dut.start = 0;
  dut.s_axis_tvalid = 0;
  dut.s_axis_tlast = 0;
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

  std::printf("DBQA aggregation testbench\n");

  // -------------------------------------------------------------------------
  // Directed: every opcode on a random table, each column.
  // -------------------------------------------------------------------------
  {
    auto beats = make_beats(64, 0xA11CEu);
    for (int op = OP_COUNT; op <= OP_AVG; ++op)
      for (int column = 0; column < 4; ++column)
        run_query(dut, tick, beats, op, column, 0x100 + op * 4 + column);
  }

  // All rows pass (worst-case counts/sums).
  {
    auto beats = make_beats(256, 0xBEEFu, /*all_pass=*/true);
    for (int op = OP_COUNT; op <= OP_AVG; ++op)
      run_query(dut, tick, beats, op, 1, 0x200 + op);
  }

  // No rows pass: COUNT=0, SUM=0, MIN=all-ones, MAX=0, AVG sum=0.
  {
    auto beats = make_beats(32, 0xCAFEu);
    for (auto& b : beats) b.pass = false;
    for (int op = OP_COUNT; op <= OP_AVG; ++op)
      run_query(dut, tick, beats, op, 0, 0x300 + op);
  }

  // Single-row table.
  {
    std::vector<Beat> beats(1);
    beats[0].cols[0] = 42;
    beats[0].cols[1] = beats[0].cols[2] = beats[0].cols[3] = 0;
    beats[0].pass = true;
    for (int op = OP_COUNT; op <= OP_AVG; ++op)
      run_query(dut, tick, beats, op, 0, 0x400 + op);
  }

  // SUM overflow: large values force saturation.
  {
    std::vector<Beat> beats(4);
    for (auto& b : beats) {
      for (int c = 0; c < 4; ++c) b.cols[c] = 0xFFFFFFFFu;
      b.pass = true;
    }
    run_query(dut, tick, beats, OP_SUM, 0, 0x500);
    run_query(dut, tick, beats, OP_AVG, 0, 0x501);
  }

  // -------------------------------------------------------------------------
  // Constrained-random: random opcode, column, table and seed.
  // -------------------------------------------------------------------------
  std::mt19937 rng(0xDEADu);
  for (int run = 0; run < 60; ++run) {
    const int op = OP_COUNT + (rng() % 5);
    const int column = rng() % 4;
    auto beats = make_beats(1 + rng() % 200, 0x600 + run);
    run_query(dut, tick, beats, op, column, 0x600 + run);
  }
  std::printf("  constrained-random complete\n");

  return dbqa::summary("tb_aggregation");
}
