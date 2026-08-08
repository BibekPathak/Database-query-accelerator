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

// ---------------------------------------------------------------------------
// GROUP BY mode (agg_cfg.groupby = 1): the stream routes to the group-by
// engine; groups come out g_axis and the classic result ports are zeroed.
// ---------------------------------------------------------------------------
uint32_t gbit(const Vaggregation_top& dut, int b) {
  const int word = b / 32;
  const int off = b % 32;
  return (dut.g_axis_tdata[word] >> off) & 1u;
}

uint64_t gbits(const Vaggregation_top& dut, int hi, int lo) {
  uint64_t r = 0;
  for (int b = hi; b >= lo; --b) r = (r << 1) | gbit(dut, b);
  return r;
}

struct GroupOut {
  uint64_t count;
  uint64_t sum;
};

// Runs one GROUP BY query; expected groups are keyed by key in bucket order.
void run_groupby_query(Vaggregation_top& dut, auto& tick,
                       const std::vector<Beat>& beats, int key_col,
                       int value_col,
                       const std::vector<std::pair<uint32_t, GroupOut>>& expect,
                       uint32_t seed) {
  dut.agg_cfg = (static_cast<uint64_t>(1) << 20) |   // groupby = 1
                (static_cast<uint64_t>(value_col) << 10) |
                static_cast<uint64_t>(key_col);

  dut.start = 1;
  tick();
  dut.start = 0;
  dbqa::check(dut.busy, "groupby: busy asserted after start");

  size_t pushed = 0;
  for (int cyc = 0;
       cyc < 16 * static_cast<int>(beats.size()) + 512 && pushed < beats.size();
       ++cyc) {
    dut.s_axis_tvalid = 1;
    for (int c = 0; c < 4; ++c) dut.s_axis_tdata[c] = 0;
    dut.s_axis_tdata[key_col] = beats[pushed].cols[key_col];
    dut.s_axis_tdata[value_col] = beats[pushed].cols[value_col];
    dut.s_axis_tdata[4] = beats[pushed].pass ? 1u : 0u;
    dut.s_axis_tlast = (pushed == beats.size() - 1);
    dut.eval();
    if (dut.s_axis_tready) ++pushed;
    tick();
  }
  dbqa::check(pushed == beats.size(), "groupby: all rows accepted");
  dut.s_axis_tvalid = 0;

  std::vector<GroupOut> got;
  std::vector<uint32_t> got_keys;
  for (int cyc = 0; cyc < 4096 && !dut.done; ++cyc) {
    dut.eval();
    if (dut.g_axis_tvalid) {
      got.push_back(GroupOut{gbits(dut, 147, 106), gbits(dut, 105, 64)});
      got_keys.push_back(static_cast<uint32_t>(gbits(dut, 179, 148)));
    }
    tick();
  }

  dbqa::check(dut.done, "groupby: done asserted");
  dbqa::check(!dut.busy, "groupby: busy deasserted after done");
  dbqa::check(dut.count == 0, "groupby: count port zeroed");
  dbqa::check(dut.result == 0, "groupby: result port zeroed");
  dbqa::check(!dut.overflow, "groupby: overflow port zeroed");

  dbqa::expect_eq("groupby: number of groups", expect.size(), got.size());
  for (size_t i = 0; i < expect.size() && i < got.size(); ++i) {
    dbqa::expect_eq("groupby: group key", expect[i].first, got_keys[i]);
    dbqa::expect_eq("groupby: group count", expect[i].second.count,
                    got[i].count);
    dbqa::expect_eq("groupby: group sum", expect[i].second.sum, got[i].sum);
  }
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
  dut.g_axis_tready = 1;
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
  // GROUP BY mode: stream routes to the group-by engine; result ports zeroed.
  // -------------------------------------------------------------------------
  {
    // Folding + collision: keys 0 and 0x100 both hash to bucket 0 (linear
    // probe to bucket 1); key 7 folds in its own bucket.
    std::vector<Beat> beats(5);
    const uint32_t key0[5] = {0, 0, 7, 0x100, 0};
    const uint32_t val0[5] = {5, 15, 10, 20, 25};
    for (int i = 0; i < 5; ++i) {
      for (int c = 0; c < 4; ++c) beats[i].cols[c] = 0;
      beats[i].cols[0] = key0[i];
      beats[i].cols[1] = val0[i];
      beats[i].pass = true;
    }
    // Bucket order: key 0 -> bucket 0, key 0x100 (hash 0) -> bucket 1 via
    // linear probe, key 7 -> bucket 7.
    run_groupby_query(
        dut, tick, beats, 0, 1,
        {{0, GroupOut{3, 45}}, {0x100, GroupOut{1, 20}}, {7, GroupOut{1, 10}}},
        0x600);
  }

  {
    // No rows pass: no groups, still completes.
    auto beats = make_beats(8, 0x601u);
    for (auto& b : beats) b.pass = false;
    run_groupby_query(dut, tick, beats, 0, 1, {}, 0x601);
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
