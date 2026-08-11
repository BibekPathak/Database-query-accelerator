// ===========================================================================
//  tb_scheduler.cpp -- self-checking testbench for the query execution core.
//
//  Loads a table through the scheduler's load port, then runs classic and
//  GROUP BY queries, verifying the streamed result and the latched
//  result/count/overflow against a software reference model.
//
//  The reference models the FULL 1024-row table: rows loaded for the test sit
//  at indices 0..n-1 and every remaining row reads back as zero. num_rows = 0
//  therefore scans the whole table, exactly as the RTL row limiter does.
//
//  Coverage:
//    * every classic opcode (COUNT, SUM, MIN, MAX, AVG), with and without a
//      predicate, and with num_rows truncation
//    * empty and all-fail tables, full-table scans
//    * GROUP BY (folding, hash collisions, truncation) via the g_axis stream
//    * start-while-busy error reporting
//    * constrained-random queries against the reference
//
//  Requires: rtl/common/db_pkg.sv and every module in rtl/memory,
//  rtl/operators and rtl/scheduler/scheduler.sv.
// ===========================================================================

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <random>
#include <vector>

#include "Vscheduler.h"
#include "dbqa_test.hpp"
#include "dbqa_trace.hpp"

namespace {

constexpr int OP_COUNT = 1, OP_SUM = 2, OP_MIN = 3, OP_MAX = 4, OP_AVG = 5;
constexpr int PRED_GTE = 5, PRED_LT = 2;
constexpr uint64_t SUM_MAX = 0x3FFFFFFFFFFull;  // 42-bit all-ones
constexpr uint64_t NUM_ROWS = 1024;

struct Row {
  uint32_t cols[4];
};

// Software reference for a classic query. `num_rows` limits the scan to the
// first num_rows rows (0 = the whole 1024-row table).
struct AggRef {
  uint64_t count;
  uint64_t sum;
  uint32_t mn;  // 0xFFFFFFFF if no row passed
  uint32_t mx;  // 0 if no row passed
};

bool pred_pass(int op, uint32_t a, uint32_t b) {
  switch (op) {
    case 0: return a == b;
    case 1: return a != b;
    case 2: return a < b;
    case 3: return a > b;
    case 4: return a <= b;
    case 5: return a >= b;
    default: return false;
  }
}

const Row& row_at(const std::vector<Row>& rows, uint64_t i) {
  static const Row zero = Row{{0, 0, 0, 0}};
  return (i < rows.size()) ? rows[i] : zero;
}

AggRef reference(const std::vector<Row>& rows, uint64_t num_rows, int agg_col,
                 bool pred_en, int pred_op, uint32_t imm, int pred_col) {
  const uint64_t n =
      (num_rows == 0) ? NUM_ROWS : std::min<uint64_t>(num_rows, NUM_ROWS);
  AggRef r{0, 0, 0xFFFFFFFFu, 0u};
  for (uint64_t i = 0; i < n; ++i) {
    const Row& x = row_at(rows, i);
    if (pred_en && !pred_pass(pred_op, x.cols[pred_col], imm)) continue;
    ++r.count;
    const uint32_t v = x.cols[agg_col];
    if (r.sum + v > SUM_MAX) {
      r.sum = SUM_MAX;
    } else {
      r.sum += v;
    }
    if (v < r.mn) r.mn = v;
    if (v > r.mx) r.mx = v;
  }
  return r;
}

void load_table(Vscheduler& dut, auto& tick, const std::vector<Row>& rows) {
  for (size_t r = 0; r < rows.size(); ++r) {
    dut.load_addr = static_cast<uint32_t>(r);
    for (int c = 0; c < 4; ++c) dut.load_data[c] = rows[r].cols[c];
    dut.load_wen = 1;
    tick();
    dut.load_wen = 0;
    tick();
  }
}

// pred_cfg_t packing: enable@46, op@[45:43], combine@42, column@[41:32],
// imm@[31:0].
void set_pred(Vscheduler& dut, bool en, int op, uint32_t imm, int col) {
  dut.pred_cfg[0] = (static_cast<uint64_t>(en) << 46) |
                    (static_cast<uint64_t>(op) << 43) |
                    (static_cast<uint64_t>(col) << 32) | imm;
  dut.pred_cfg[1] = 0;
}

// agg_cfg_t packing: op@[23:21], groupby@20, column@[19:10], gby_key@[9:0].
uint64_t pack_agg(int op, bool groupby, int column, int gby_key) {
  return (static_cast<uint64_t>(op) << 21) |
         (static_cast<uint64_t>(groupby) << 20) |
         (static_cast<uint64_t>(column) << 10) | static_cast<uint64_t>(gby_key);
}

// Runs a classic query and checks result/count/overflow against the model.
void run_classic(Vscheduler& dut, auto& tick, const std::vector<Row>& rows,
                 int op, int agg_col, uint64_t num_rows, bool pred_en,
                 int pred_op, uint32_t imm, int pred_col, const char* tag) {
  const AggRef ref = reference(rows, num_rows, agg_col, pred_en, pred_op, imm,
                               pred_col);
  set_pred(dut, pred_en, pred_op, imm, pred_col);
  dut.query_cfg = num_rows;
  dut.agg_cfg = pack_agg(op, false, agg_col, 0);

  dut.start = 1;
  tick();
  dut.start = 0;
  dbqa::check(dut.busy, "busy asserted after start");
  for (int cyc = 0; cyc < 16 * static_cast<int>(rows.size()) + 16384 &&
                       !dut.done;
       ++cyc)
    tick();
  dbqa::check(dut.done, "done asserted");

  uint64_t expect = 0;
  switch (op) {
    case OP_COUNT: expect = ref.count; break;
    case OP_SUM: expect = ref.sum; break;
    case OP_MIN: expect = ref.mn; break;
    case OP_MAX: expect = ref.mx; break;
    case OP_AVG: expect = ref.sum; break;
  }
  char msg[64];
  std::snprintf(msg, sizeof msg, "%s: result", tag);
  dbqa::expect_eq(msg, expect, dut.result);
  std::snprintf(msg, sizeof msg, "%s: count", tag);
  dbqa::expect_eq(msg, ref.count, dut.count);
  std::snprintf(msg, sizeof msg, "%s: overflow", tag);
  dbqa::check(dut.overflow == (ref.sum == SUM_MAX), msg);
}

// Runs a GROUP BY query and compares the g_axis groups (order-agnostic) to a
// per-key model built over the same full-table view.
void run_groupby(Vscheduler& dut, auto& tick, const std::vector<Row>& rows,
                 int key_col, int value_col, uint64_t num_rows,
                 const std::map<uint32_t, AggRef>& ref, const char* tag) {
  set_pred(dut, false, 0, 0, 0);
  dut.query_cfg = num_rows;
  dut.agg_cfg = pack_agg(OP_SUM, true, value_col, key_col);

  dut.start = 1;
  tick();
  dut.start = 0;
  dbqa::check(dut.busy, "groupby: busy asserted after start");

  std::map<uint32_t, AggRef> got;
  for (int cyc = 0; cyc < 16 * static_cast<int>(rows.size()) + 16384 &&
                       !dut.done;
       ++cyc) {
    dut.eval();
    if (dut.g_axis_tvalid) {
      uint64_t key = 0, cnt = 0, sum = 0;
      for (int b = 179; b >= 148; --b)
        key = (key << 1) | ((dut.g_axis_tdata[b / 32] >> (b % 32)) & 1u);
      for (int b = 147; b >= 106; --b)
        cnt = (cnt << 1) | ((dut.g_axis_tdata[b / 32] >> (b % 32)) & 1u);
      for (int b = 105; b >= 64; --b)
        sum = (sum << 1) | ((dut.g_axis_tdata[b / 32] >> (b % 32)) & 1u);
      got[static_cast<uint32_t>(key)] = AggRef{cnt, sum, 0, 0};
    }
    tick();
  }
  dbqa::check(dut.done, "groupby: done asserted");
  dbqa::check(dut.count == 0 && dut.result == 0, "groupby: classic ports zeroed");

  char msg[64];
  std::snprintf(msg, sizeof msg, "%s: group count", tag);
  dbqa::expect_eq(msg, ref.size(), got.size());
  for (const auto& [k, g] : ref) {
    if (!got.count(k)) continue;
    std::snprintf(msg, sizeof msg, "%s: key 0x%x count", tag, k);
    dbqa::expect_eq(msg, g.count, got[k].count);
    std::snprintf(msg, sizeof msg, "%s: key 0x%x sum", tag, k);
    dbqa::expect_eq(msg, g.sum, got[k].sum);
  }
}

// Builds the GROUP BY reference over the full-table view (unloaded rows are
// zeros: key 0, value 0).
std::map<uint32_t, AggRef> groupby_ref(const std::vector<Row>& rows,
                                       uint64_t num_rows, int key_col,
                                       int value_col) {
  const uint64_t n =
      (num_rows == 0) ? NUM_ROWS : std::min<uint64_t>(num_rows, NUM_ROWS);
  std::map<uint32_t, AggRef> ref;
  for (uint64_t i = 0; i < n; ++i) {
    const Row& x = row_at(rows, i);
    auto& g = ref[x.cols[key_col]];
    if (g.count == 0) {
      g = AggRef{1, x.cols[value_col], 0, 0};
    } else {
      g.count += 1;
      g.sum += x.cols[value_col];
    }
  }
  return ref;
}

}  // namespace

int main() {
  Vscheduler dut;
  dut.clk = 0;
  dut.rst = 1;
  dut.start = 0;
  dut.load_wen = 0;
  dut.load_addr = 0;
  for (int i = 0; i < 4; ++i) dut.load_data[i] = 0;
  dut.query_cfg = 0;
  dut.agg_cfg = 0;
  dut.pred_cfg[0] = 0;
  dut.pred_cfg[1] = 0;
  dut.proj_mask = 0xF;
  dut.g_axis_tready = 1;
  dut.eval();

  vluint64_t trace_cycle = 0;
  auto tfp = dbqa::init_trace(dut, "results/tb_scheduler.fst");
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

  std::printf("DBQA scheduler testbench\n");

  // A fixed 8-row table: col0 = id, col1 = value, col2/3 = 0.
  std::vector<Row> rows(8);
  for (int i = 0; i < 8; ++i) {
    rows[i].cols[0] = i;
    rows[i].cols[1] = 10u * (i + 1);
    rows[i].cols[2] = 0;
    rows[i].cols[3] = 0;
  }
  load_table(dut, tick, rows);

  // -------------------------------------------------------------------------
  // Directed classic queries over the 8-row table.
  // -------------------------------------------------------------------------
  run_classic(dut, tick, rows, OP_COUNT, 1, 8, false, 0, 0, 0, "count all");
  run_classic(dut, tick, rows, OP_SUM, 1, 8, false, 0, 0, 0, "sum all");
  run_classic(dut, tick, rows, OP_MIN, 1, 8, false, 0, 0, 0, "min all");
  run_classic(dut, tick, rows, OP_MAX, 1, 8, false, 0, 0, 0, "max all");
  run_classic(dut, tick, rows, OP_AVG, 1, 8, false, 0, 0, 0, "avg all");
  // num_rows truncation: first 3 rows -> 10+20+30 = 60.
  run_classic(dut, tick, rows, OP_SUM, 1, 3, false, 0, 0, 0, "sum first3");
  // Predicate id >= 5: rows 5..7 -> 60+70+80 = 210, count 3.
  run_classic(dut, tick, rows, OP_SUM, 1, 8, true, PRED_GTE, 5, 0,
              "sum pred gte5");
  run_classic(dut, tick, rows, OP_COUNT, 1, 8, true, PRED_GTE, 5, 0,
              "count pred gte5");
  // Predicate + truncation: first 6 rows, id < 3 -> rows 0..2 -> 60.
  run_classic(dut, tick, rows, OP_SUM, 1, 6, true, PRED_LT, 3, 0,
              "sum pred+trunc");
  // Empty: num_rows=1, id >= 5 filters the only scanned row.
  run_classic(dut, tick, rows, OP_COUNT, 1, 1, true, PRED_GTE, 5, 0,
              "count empty");
  // All fail: id >= 100.
  run_classic(dut, tick, rows, OP_SUM, 1, 8, true, PRED_GTE, 100, 0,
              "sum all-fail");
  // Full-table scans: load all 1024 rows with a deterministic pattern so the
  // memory contents are exactly the reference view (num_rows = 0 = all).
  {
    std::vector<Row> full(1024);
    for (int i = 0; i < 1024; ++i) {
      full[i].cols[0] = i % 5;
      full[i].cols[1] = (i * 3) % 1000;
      full[i].cols[2] = full[i].cols[3] = 0;
    }
    load_table(dut, tick, full);
    run_classic(dut, tick, full, OP_COUNT, 1, 0, false, 0, 0, 0,
                "count full");
    run_classic(dut, tick, full, OP_SUM, 1, 0, false, 0, 0, 0, "sum full");
    run_groupby(dut, tick, full, 0, 1, 0, groupby_ref(full, 0, 0, 1),
                "groupby full");
  }
  load_table(dut, tick, rows);  // reload the base table

  // -------------------------------------------------------------------------
  // Directed GROUP BY.
  // -------------------------------------------------------------------------
  {
    // Folding: ids 0 and 1 repeated, num_rows covers only the loaded rows.
    std::vector<Row> gb(6);
    const uint32_t id[6] = {0, 1, 0, 1, 0, 1};
    const uint32_t v[6] = {5, 10, 15, 20, 25, 30};
    for (int i = 0; i < 6; ++i) {
      gb[i].cols[0] = id[i];
      gb[i].cols[1] = v[i];
      gb[i].cols[2] = gb[i].cols[3] = 0;
    }
    load_table(dut, tick, gb);
    run_groupby(dut, tick, gb, 0, 1, 6, groupby_ref(gb, 6, 0, 1),
                "groupby folding");
  }
  {
    // Hash collision: keys 0 and 0x100 both hash to bucket 0.
    std::vector<Row> gb(3);
    const uint32_t id[3] = {0, 0x100, 0};
    const uint32_t v[3] = {7, 11, 13};
    for (int i = 0; i < 3; ++i) {
      gb[i].cols[0] = id[i];
      gb[i].cols[1] = v[i];
      gb[i].cols[2] = gb[i].cols[3] = 0;
    }
    load_table(dut, tick, gb);
    run_groupby(dut, tick, gb, 0, 1, 3, groupby_ref(gb, 3, 0, 1),
                "groupby collision");
  }
  load_table(dut, tick, rows);

  // -------------------------------------------------------------------------
  // Start-while-busy: an illegal start latches ERR_START_BUSY but the query
  // still completes.
  // -------------------------------------------------------------------------
  set_pred(dut, false, 0, 0, 0);
  dut.query_cfg = 8;
  dut.agg_cfg = pack_agg(OP_SUM, false, 1, 0);
  dut.start = 1;
  tick();
  dut.start = 0;
  dbqa::check(dut.busy, "busy while running");
  dut.start = 1;
  tick();
  dut.start = 0;  // illegal start
  for (int cyc = 0; cyc < 16384 && !dut.done; ++cyc) tick();
  dbqa::check(dut.done, "query completes despite illegal start");
  dbqa::check(dut.error == 1, "error = ERR_START_BUSY");

  // -------------------------------------------------------------------------
  // Constrained-random classic queries.
  // -------------------------------------------------------------------------
  std::mt19937 rng(0x57DUL);
  for (int run = 0; run < 60; ++run) {
    std::vector<Row> r(1 + rng() % 64);
    for (auto& x : r)
      for (int c = 0; c < 4; ++c) x.cols[c] = rng() % 200;
    load_table(dut, tick, r);
    const int op = OP_COUNT + rng() % 5;
    const int agg_col = rng() % 4;
    const uint64_t num_rows = 1 + rng() % r.size();
    const bool pred_en = (rng() % 2) != 0;
    const int pred_op = rng() % 6;
    const uint32_t imm = rng() % 300;
    const int pred_col = rng() % 4;
    char tag[48];
    std::snprintf(tag, sizeof tag, "rand %d", run);
    run_classic(dut, tick, r, op, agg_col, num_rows, pred_en, pred_op, imm,
                pred_col, tag);
  }
  std::printf("  constrained-random classic complete\n");

  // Constrained-random GROUP BY.
  for (int run = 0; run < 20; ++run) {
    std::vector<Row> r(1 + rng() % 48);
    const uint32_t keypool[6] = {0, 1, 7, 0x100, 0x1FF, 3};
    for (auto& x : r) {
      x.cols[0] = keypool[rng() % 6];
      x.cols[1] = rng() % 1000;
      x.cols[2] = x.cols[3] = 0;
    }
    load_table(dut, tick, r);
    const uint64_t num_rows = 1 + rng() % r.size();
    char tag[48];
    std::snprintf(tag, sizeof tag, "gb rand %d", run);
    run_groupby(dut, tick, r, 0, 1, num_rows, groupby_ref(r, num_rows, 0, 1),
                tag);
  }
  std::printf("  constrained-random groupby complete\n");

  return dbqa::summary("tb_scheduler");
}
