// ===========================================================================
//  tb_top.cpp -- end-to-end self-checking testbench for dbqa_top.
//
//  Drives the accelerator entirely over AXI-Lite: loads a table through the
//  load registers, configures and starts classic and GROUP BY queries, polls
//  the status register, and verifies results against a software reference
//  model of the full 1024-row table.
//
//  Coverage:
//    * classic COUNT/SUM/MIN/MAX/AVG with/without predicates and num_rows
//      truncation, plus empty/all-fail and full-table scans
//    * GROUP BY (folding, collisions, truncation) over the m_axis stream
//    * abort cancels an in-flight query, then a clean query still works
//    * constrained-random classic and GROUP BY queries
//
//  Requires: every RTL module in rtl/common, rtl/interfaces, rtl/memory,
//  rtl/operators, rtl/scheduler and rtl/top.
// ===========================================================================

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <random>
#include <vector>

#include "Vdbqa_top.h"
#include "dbqa_reference.hpp"
#include "dbqa_test.hpp"
#include "dbqa_trace.hpp"

namespace {

using namespace dbqa_ref;

// db_pkg register map (word offsets).
constexpr uint32_t REG_CTRL = 0, REG_STATUS = 1, REG_QUERY = 2,
                   REG_AGG_CFG = 3, REG_PROJ_MASK = 4, REG_PRED_BASE = 8,
                   REG_LOAD_ADDR = 0x20, REG_LOAD_DATA0 = 0x21,
                   REG_LOAD_ROW = 0x25, REG_RESULT = 0x30, REG_RESULT_HI = 0x31,
                   REG_COUNT = 0x32, REG_COUNT_HI = 0x33, REG_OVERFLOW = 0x34;

constexpr int OP_COUNT = 1, OP_SUM = 2, OP_MIN = 3, OP_MAX = 4, OP_AVG = 5;

void axil_write(Vdbqa_top& dut, auto& tick, uint32_t word, uint32_t data) {
  dut.s_axil_bready = 1;
  dut.s_axil_awvalid = 1;
  dut.s_axil_awaddr = word << 2;
  dut.s_axil_wvalid = 1;
  dut.s_axil_wdata = data;
  dut.eval();
  tick();
  dut.s_axil_awvalid = 0;
  dut.s_axil_wvalid = 0;
  int cyc = 0;
  while (!dut.s_axil_bvalid && cyc++ < 32) {
    dut.eval();
    tick();
  }
  dut.eval();
  while (dut.s_axil_bvalid) {
    dut.eval();
    tick();
  }
  dut.eval();
}

uint32_t axil_read(Vdbqa_top& dut, auto& tick, uint32_t word) {
  dut.s_axil_rready = 1;
  dut.s_axil_arvalid = 1;
  dut.s_axil_araddr = word << 2;
  dut.eval();
  tick();
  dut.s_axil_arvalid = 0;
  int cyc = 0;
  while (!dut.s_axil_rvalid && cyc++ < 32) {
    dut.eval();
    tick();
  }
  dut.eval();
  uint32_t d = dut.s_axil_rvalid ? dut.s_axil_rdata : 0xDEADBEEFu;
  while (dut.s_axil_rvalid) {
    dut.eval();
    tick();
  }
  dut.eval();
  return d;
}

void load_table(Vdbqa_top& dut, auto& tick, const std::vector<Row>& rows) {
  for (size_t r = 0; r < rows.size(); ++r) {
    axil_write(dut, tick, REG_LOAD_ADDR, static_cast<uint32_t>(r));
    for (int c = 0; c < 4; ++c)
      axil_write(dut, tick, REG_LOAD_DATA0 + c, rows[r].cols[c]);
    axil_write(dut, tick, REG_LOAD_ROW, 0);
  }
}

void set_pred(Vdbqa_top& dut, auto& tick, bool en, int op, uint32_t imm,
              int col) {
  axil_write(dut, tick, REG_PRED_BASE, imm);
  axil_write(dut, tick, REG_PRED_BASE + 1,
             (en ? 1u : 0u) << 14 | static_cast<uint32_t>(op & 7) << 11 |
                 static_cast<uint32_t>(col & 0x3FF));
  axil_write(dut, tick, REG_PRED_BASE + 2, 0);
  axil_write(dut, tick, REG_PRED_BASE + 3, 0);
}

uint32_t pack_agg(int op, bool groupby, int col, int key) {
  return static_cast<uint32_t>(op) << 21 |
         static_cast<uint32_t>(groupby) << 20 |
         static_cast<uint32_t>(col) << 10 | static_cast<uint32_t>(key);
}

bool poll_done(Vdbqa_top& dut, auto& tick, int max_polls) {
  for (int i = 0; i < max_polls; ++i) {
    if (axil_read(dut, tick, REG_STATUS) & 2) return true;
  }
  return false;
}

void run_classic(Vdbqa_top& dut, auto& tick, const std::vector<Row>& rows,
                 int op, int agg_col, uint64_t num_rows, bool pred_en,
                 int pred_op, uint32_t imm, int pred_col, const char* tag) {
  const AggRef ref =
      reference(rows, num_rows, agg_col, pred_en, pred_op, imm, pred_col);
  set_pred(dut, tick, pred_en, pred_op, imm, pred_col);
  axil_write(dut, tick, REG_QUERY, static_cast<uint32_t>(num_rows));
  axil_write(dut, tick, REG_AGG_CFG, pack_agg(op, false, agg_col, 0));
  axil_write(dut, tick, REG_PROJ_MASK, 0xF);
  axil_write(dut, tick, REG_CTRL, 1);  // start

  dbqa::check(poll_done(dut, tick, 8192), "query completes");

  uint64_t result =
      (static_cast<uint64_t>(axil_read(dut, tick, REG_RESULT_HI)) << 32) |
      axil_read(dut, tick, REG_RESULT);
  uint64_t count =
      (static_cast<uint64_t>(axil_read(dut, tick, REG_COUNT_HI)) << 32) |
      axil_read(dut, tick, REG_COUNT);
  const uint32_t overflow = axil_read(dut, tick, REG_OVERFLOW);

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
  dbqa::expect_eq(msg, expect, result);
  std::snprintf(msg, sizeof msg, "%s: count", tag);
  dbqa::expect_eq(msg, ref.count, count);
  std::snprintf(msg, sizeof msg, "%s: overflow", tag);
  dbqa::check(overflow == (ref.sum == SUM_MAX), msg);
}

uint32_t gbit(const Vdbqa_top& dut, int b) {
  return (dut.m_axis_tdata[b / 32] >> (b % 32)) & 1u;
}

uint64_t gbits(const Vdbqa_top& dut, int hi, int lo) {
  uint64_t r = 0;
  for (int b = hi; b >= lo; --b) r = (r << 1) | gbit(dut, b);
  return r;
}

void run_groupby(Vdbqa_top& dut, auto& tick, const std::vector<Row>& rows,
                 int key_col, int value_col, uint64_t num_rows,
                 const std::map<uint32_t, AggRef>& ref, const char* tag) {
  set_pred(dut, tick, false, 0, 0, 0);
  axil_write(dut, tick, REG_QUERY, static_cast<uint32_t>(num_rows));
  axil_write(dut, tick, REG_AGG_CFG, pack_agg(OP_SUM, true, value_col, key_col));
  axil_write(dut, tick, REG_PROJ_MASK, 0xF);
  axil_write(dut, tick, REG_CTRL, 1);  // start

  std::map<uint32_t, AggRef> got;
  bool saw_done = false;
  for (int cyc = 0; cyc < 8 * static_cast<int>(rows.size()) + 32768 &&
                       !saw_done;
       ++cyc) {
    dut.eval();
    if (dut.m_axis_tvalid) {
      uint32_t key = static_cast<uint32_t>(gbits(dut, 179, 148));
      const uint64_t cnt = gbits(dut, 147, 106);
      const uint64_t sum = gbits(dut, 105, 64);
      got[key] = AggRef{cnt, sum, 0, 0};
    }
    tick();
    if ((cyc & 0x3FF) == 0x3FF) {
      if (axil_read(dut, tick, REG_STATUS) & 2) saw_done = true;
    }
  }
  dbqa::check(saw_done, "groupby: done asserted");

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

// ---------------------------------------------------------------------------
// Backpressure torture: every AXI-Lite and AXI-Stream handshake is randomly
// gated (ready deasserted ~50% of the time); the same results must still come
// out because handshakes hold until ready.
// ---------------------------------------------------------------------------
uint32_t gated_read(Vdbqa_top& dut, auto& tick, std::mt19937& rng,
                    uint32_t word) {
  dut.s_axil_rready = rng() & 1;
  dut.s_axil_arvalid = 1;
  dut.s_axil_araddr = word << 2;
  dut.eval();
  tick();
  dut.s_axil_arvalid = 0;
  int cyc = 0;
  while (!dut.s_axil_rvalid && cyc++ < 1024) {
    dut.s_axil_rready = rng() & 1;
    dut.eval();
    tick();
  }
  dut.eval();
  const uint32_t d = dut.s_axil_rvalid ? dut.s_axil_rdata : 0xDEADBEEFu;
  while (dut.s_axil_rvalid) {
    dut.s_axil_rready = rng() & 1;
    dut.eval();
    tick();
  }
  dut.eval();
  return d;
}

void gated_write(Vdbqa_top& dut, auto& tick, std::mt19937& rng, uint32_t word,
                 uint32_t data) {
  dut.s_axil_bready = rng() & 1;
  dut.s_axil_awvalid = 1;
  dut.s_axil_awaddr = word << 2;
  dut.s_axil_wvalid = 1;
  dut.s_axil_wdata = data;
  dut.eval();
  tick();
  dut.s_axil_awvalid = 0;
  dut.s_axil_wvalid = 0;
  int cyc = 0;
  while (!dut.s_axil_bvalid && cyc++ < 1024) {
    dut.s_axil_bready = rng() & 1;
    dut.eval();
    tick();
  }
  dut.eval();
  while (dut.s_axil_bvalid) {
    dut.s_axil_bready = rng() & 1;
    dut.eval();
    tick();
  }
  dut.eval();
}

void set_pred_torture(Vdbqa_top& dut, auto& tick, std::mt19937& rng, bool en,
                      int op, uint32_t imm, int col) {
  gated_write(dut, tick, rng, REG_PRED_BASE, imm);
  gated_write(dut, tick, rng, REG_PRED_BASE + 1,
              (en ? 1u : 0u) << 14 | static_cast<uint32_t>(op & 7) << 11 |
                  static_cast<uint32_t>(col & 0x3FF));
  gated_write(dut, tick, rng, REG_PRED_BASE + 2, 0);
  gated_write(dut, tick, rng, REG_PRED_BASE + 3, 0);
}

void run_classic_torture(Vdbqa_top& dut, auto& tick, std::mt19937& rng,
                         const std::vector<Row>& rows, int op, int agg_col,
                         uint64_t num_rows, bool pred_en, int pred_op,
                         uint32_t imm, int pred_col, const char* tag) {
  const AggRef ref =
      reference(rows, num_rows, agg_col, pred_en, pred_op, imm, pred_col);
  set_pred_torture(dut, tick, rng, pred_en, pred_op, imm, pred_col);
  gated_write(dut, tick, rng, REG_QUERY, static_cast<uint32_t>(num_rows));
  gated_write(dut, tick, rng, REG_AGG_CFG, pack_agg(op, false, agg_col, 0));
  gated_write(dut, tick, rng, REG_PROJ_MASK, 0xF);
  gated_write(dut, tick, rng, REG_CTRL, 1);  // start

  bool done = false;
  for (int i = 0; i < 8192 && !done; ++i)
    if (gated_read(dut, tick, rng, REG_STATUS) & 2) done = true;
  dbqa::check(done, "torture: query completes");

  uint64_t result =
      (static_cast<uint64_t>(gated_read(dut, tick, rng, REG_RESULT_HI)) << 32) |
      gated_read(dut, tick, rng, REG_RESULT);
  uint64_t count =
      (static_cast<uint64_t>(gated_read(dut, tick, rng, REG_COUNT_HI)) << 32) |
      gated_read(dut, tick, rng, REG_COUNT);
  const uint32_t overflow = gated_read(dut, tick, rng, REG_OVERFLOW);

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
  dbqa::expect_eq(msg, expect, result);
  std::snprintf(msg, sizeof msg, "%s: count", tag);
  dbqa::expect_eq(msg, ref.count, count);
  std::snprintf(msg, sizeof msg, "%s: overflow", tag);
  dbqa::check(overflow == (ref.sum == SUM_MAX), msg);
}

void run_groupby_torture(Vdbqa_top& dut, auto& tick, std::mt19937& rng,
                         const std::vector<Row>& rows, int key_col,
                         int value_col, uint64_t num_rows,
                         const std::map<uint32_t, AggRef>& ref,
                         const char* tag) {
  set_pred_torture(dut, tick, rng, false, 0, 0, 0);
  gated_write(dut, tick, rng, REG_QUERY, static_cast<uint32_t>(num_rows));
  gated_write(dut, tick, rng, REG_AGG_CFG,
              pack_agg(OP_SUM, true, value_col, key_col));
  gated_write(dut, tick, rng, REG_PROJ_MASK, 0xF);
  gated_write(dut, tick, rng, REG_CTRL, 1);  // start

  std::map<uint32_t, AggRef> got;
  bool saw_done = false;
  for (int cyc = 0; cyc < 8 * static_cast<int>(rows.size()) + 65536 &&
                       !saw_done;
       ++cyc) {
    dut.m_axis_tready = rng() & 1;  // gate the group stream
    dut.eval();
    if (dut.m_axis_tvalid && dut.m_axis_tready) {
      uint32_t key = static_cast<uint32_t>(gbits(dut, 179, 148));
      const uint64_t cnt = gbits(dut, 147, 106);
      const uint64_t sum = gbits(dut, 105, 64);
      got[key] = AggRef{cnt, sum, 0, 0};
    }
    tick();
    if ((cyc & 0x3FF) == 0x3FF) {
      if (gated_read(dut, tick, rng, REG_STATUS) & 2) saw_done = true;
    }
  }
  dbqa::check(saw_done, "torture: groupby done asserted");

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

}  // namespace

int main() {
  Vdbqa_top dut;
  dut.clk = 0;
  dut.rst = 1;
  dut.s_axil_awvalid = 0;
  dut.s_axil_awaddr = 0;
  dut.s_axil_wvalid = 0;
  dut.s_axil_wdata = 0;
  dut.s_axil_wstrb = 0xF;
  dut.s_axil_bready = 1;
  dut.s_axil_arvalid = 0;
  dut.s_axil_araddr = 0;
  dut.s_axil_rready = 1;
  dut.m_axis_tready = 1;
  dut.eval();

  vluint64_t trace_cycle = 0;
  auto tfp = dbqa::init_trace(dut, "results/tb_top.fst");
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

  std::printf("DBQA top-level testbench\n");

  // Base 8-row table: col0 = id, col1 = value.
  std::vector<Row> rows(8);
  for (int i = 0; i < 8; ++i) {
    rows[i].cols[0] = i;
    rows[i].cols[1] = 10u * (i + 1);
    rows[i].cols[2] = rows[i].cols[3] = 0;
  }
  load_table(dut, tick, rows);

  // -------------------------------------------------------------------------
  // Directed classic queries.
  // -------------------------------------------------------------------------
  run_classic(dut, tick, rows, OP_COUNT, 1, 8, false, 0, 0, 0, "count all");
  run_classic(dut, tick, rows, OP_SUM, 1, 8, false, 0, 0, 0, "sum all");
  run_classic(dut, tick, rows, OP_MIN, 1, 8, false, 0, 0, 0, "min all");
  run_classic(dut, tick, rows, OP_MAX, 1, 8, false, 0, 0, 0, "max all");
  run_classic(dut, tick, rows, OP_AVG, 1, 8, false, 0, 0, 0, "avg all");
  run_classic(dut, tick, rows, OP_SUM, 1, 3, false, 0, 0, 0, "sum first3");
  run_classic(dut, tick, rows, OP_SUM, 1, 8, true, 5, 5, 0, "sum pred gte5");
  run_classic(dut, tick, rows, OP_COUNT, 1, 8, true, 5, 5, 0, "count pred");
  run_classic(dut, tick, rows, OP_SUM, 1, 6, true, 2, 3, 0, "sum pred+trunc");
  run_classic(dut, tick, rows, OP_COUNT, 1, 1, true, 5, 5, 0, "count empty");
  run_classic(dut, tick, rows, OP_SUM, 1, 8, true, 5, 100, 0, "sum all-fail");

  // Full-table scan: load all 1024 rows deterministically.
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
  load_table(dut, tick, rows);

  // -------------------------------------------------------------------------
  // Directed GROUP BY.
  // -------------------------------------------------------------------------
  {
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
  // Abort: cancel an in-flight query, then a clean query still works.
  // -------------------------------------------------------------------------
  {
    set_pred(dut, tick, false, 0, 0, 0);
    axil_write(dut, tick, REG_QUERY, 0);  // full-table scan (long)
    axil_write(dut, tick, REG_AGG_CFG, pack_agg(OP_SUM, false, 1, 0));
    axil_write(dut, tick, REG_PROJ_MASK, 0xF);
    axil_write(dut, tick, REG_CTRL, 1);  // start
    for (int cyc = 0; cyc < 16; ++cyc) tick();
    const uint32_t busy = axil_read(dut, tick, REG_STATUS) & 1;
    dbqa::check(busy, "query running before abort");
    axil_write(dut, tick, REG_CTRL, 2);  // abort
    for (int cyc = 0; cyc < 8; ++cyc) tick();
    const uint32_t st = axil_read(dut, tick, REG_STATUS);
    dbqa::check(!(st & 1), "busy cleared after abort");

    // A clean query after the abort must still complete.
    axil_write(dut, tick, REG_QUERY, 8);
    axil_write(dut, tick, REG_AGG_CFG, pack_agg(OP_SUM, false, 1, 0));
    axil_write(dut, tick, REG_CTRL, 1);
    dbqa::check(poll_done(dut, tick, 8192), "query after abort completes");
  }

  // -------------------------------------------------------------------------
  // Constrained-random classic queries.
  // -------------------------------------------------------------------------
  std::mt19937 rng(0x70Fu);
  for (int run = 0; run < 40; ++run) {
    std::vector<Row> r(1 + rng() % 48);
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
  for (int run = 0; run < 12; ++run) {
    std::vector<Row> r(1 + rng() % 32);
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

  // -------------------------------------------------------------------------
  // Backpressure torture: randomly gate every AXI-Lite / AXI-Stream
  // handshake and re-verify the same queries against the reference.
  // -------------------------------------------------------------------------
  {
    std::mt19937 trng(0x7155u);
    load_table(dut, tick, rows);
    run_classic_torture(dut, tick, trng, rows, OP_COUNT, 1, 8, false, 0, 0, 0,
                        "torture count");
    run_classic_torture(dut, tick, trng, rows, OP_SUM, 1, 8, true, 5, 5, 0,
                        "torture sum pred");
    run_classic_torture(dut, tick, trng, rows, OP_SUM, 1, 3, false, 0, 0, 0,
                        "torture trunc");
    run_classic_torture(dut, tick, trng, rows, OP_MIN, 2, 8, false, 0, 0, 0,
                        "torture min");
    {
      std::vector<Row> gb(6);
      const uint32_t id[6] = {0, 1, 0, 1, 0, 1};
      const uint32_t v[6] = {5, 10, 15, 20, 25, 30};
      for (int i = 0; i < 6; ++i) {
        gb[i].cols[0] = id[i];
        gb[i].cols[1] = v[i];
        gb[i].cols[2] = gb[i].cols[3] = 0;
      }
      load_table(dut, tick, gb);
      run_groupby_torture(dut, tick, trng, gb, 0, 1, 6,
                          groupby_ref(gb, 6, 0, 1), "torture gb");
    }
    dut.m_axis_tready = 1;
  }
  std::printf("  backpressure torture complete\n");

  return dbqa::summary("tb_top");
}
