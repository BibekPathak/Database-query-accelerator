// ===========================================================================
//  tb_stress.cpp -- long-session soak test for dbqa_top.
//
//  Loads a full deterministic 1024-row table once, then runs hundreds of
//  randomized classic and GROUP BY queries in one session, verifying every
//  result against the software reference. Running many queries back-to-back
//  against the same hardware instance catches cross-query state leaks (the
//  accelerator must be stateless between queries beyond the loaded table).
//
//  Requires: every RTL module in rtl/common, rtl/interfaces, rtl/memory,
//  rtl/operators, rtl/scheduler and rtl/top.
// ===========================================================================

#include <cstdint>
#include <cstdio>
#include <map>
#include <random>
#include <vector>

#include "Vdbqa_top.h"
#include "dbqa_reference.hpp"
#include "dbqa_test.hpp"

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
  while (!dut.s_axil_bvalid && cyc++ < 1024) {
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
  while (!dut.s_axil_rvalid && cyc++ < 1024) {
    dut.eval();
    tick();
  }
  dut.eval();
  const uint32_t d = dut.s_axil_rvalid ? dut.s_axil_rdata : 0xDEADBEEFu;
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
  for (int i = 0; i < max_polls; ++i)
    if (axil_read(dut, tick, REG_STATUS) & 2) return true;
  return false;
}

// Classic query: configure, start, wait, read and verify result/count.
void run_classic(Vdbqa_top& dut, auto& tick, const std::vector<Row>& full,
                 int op, int agg_col, uint64_t num_rows, bool pred_en,
                 int pred_op, uint32_t imm, int pred_col, int run) {
  const AggRef ref =
      reference(full, num_rows, agg_col, pred_en, pred_op, imm, pred_col);
  set_pred(dut, tick, pred_en, pred_op, imm, pred_col);
  axil_write(dut, tick, REG_QUERY, static_cast<uint32_t>(num_rows));
  axil_write(dut, tick, REG_AGG_CFG, pack_agg(op, false, agg_col, 0));
  axil_write(dut, tick, REG_PROJ_MASK, 0xF);
  axil_write(dut, tick, REG_CTRL, 1);  // start

  if (!poll_done(dut, tick, 4096)) {
    dbqa::check(false, "soak: query did not complete");
    return;
  }
  const uint64_t result =
      (static_cast<uint64_t>(axil_read(dut, tick, REG_RESULT_HI)) << 32) |
      axil_read(dut, tick, REG_RESULT);
  const uint64_t count =
      (static_cast<uint64_t>(axil_read(dut, tick, REG_COUNT_HI)) << 32) |
      axil_read(dut, tick, REG_COUNT);

  uint64_t expect = 0;
  switch (op) {
    case OP_COUNT: expect = ref.count; break;
    case OP_SUM: expect = ref.sum; break;
    case OP_MIN: expect = ref.mn; break;
    case OP_MAX: expect = ref.mx; break;
    case OP_AVG: expect = ref.sum; break;
  }
  char msg[64];
  std::snprintf(msg, sizeof msg, "soak %d: result", run);
  dbqa::expect_eq(msg, expect, result);
  std::snprintf(msg, sizeof msg, "soak %d: count", run);
  dbqa::expect_eq(msg, ref.count, count);
}

uint32_t gbit(const Vdbqa_top& dut, int b) {
  return (dut.m_axis_tdata[b / 32] >> (b % 32)) & 1u;
}

uint64_t gbits(const Vdbqa_top& dut, int hi, int lo) {
  uint64_t r = 0;
  for (int b = hi; b >= lo; --b) r = (r << 1) | gbit(dut, b);
  return r;
}

// GROUP BY query: drain the group stream and verify per-key aggregates.
void run_groupby(Vdbqa_top& dut, auto& tick, const std::vector<Row>& full,
                 int key_col, int value_col, uint64_t num_rows, int run) {
  const auto ref = groupby_ref(full, num_rows, key_col, value_col);
  set_pred(dut, tick, false, 0, 0, 0);
  axil_write(dut, tick, REG_QUERY, static_cast<uint32_t>(num_rows));
  axil_write(dut, tick, REG_AGG_CFG,
             pack_agg(OP_SUM, true, value_col, key_col));
  axil_write(dut, tick, REG_PROJ_MASK, 0xF);
  axil_write(dut, tick, REG_CTRL, 1);  // start

  std::map<uint32_t, AggRef> got;
  bool saw_done = false;
  for (int cyc = 0; cyc < 8 * static_cast<int>(NUM_ROWS) + 65536 &&
                       !saw_done;
       ++cyc) {
    dut.m_axis_tready = 1;
    dut.eval();
    if (dut.m_axis_tvalid) {
      const uint32_t key = static_cast<uint32_t>(gbits(dut, 179, 148));
      const uint64_t cnt = gbits(dut, 147, 106);
      const uint64_t sum = gbits(dut, 105, 64);
      got[key] = AggRef{cnt, sum, 0, 0};
    }
    tick();
    if ((cyc & 0x3FF) == 0x3FF) {
      if (axil_read(dut, tick, REG_STATUS) & 2) saw_done = true;
    }
  }
  dbqa::check(saw_done, "soak: groupby completed");

  char msg[64];
  std::snprintf(msg, sizeof msg, "soak gb %d: group count", run);
  dbqa::expect_eq(msg, ref.size(), got.size());
  for (const auto& [k, g] : ref) {
    if (!got.count(k)) continue;
    std::snprintf(msg, sizeof msg, "soak gb %d: key 0x%x count", run, k);
    dbqa::expect_eq(msg, g.count, got[k].count);
    std::snprintf(msg, sizeof msg, "soak gb %d: key 0x%x sum", run, k);
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

  auto tick = [&]() {
    dut.clk = 1;
    dut.eval();
    dut.clk = 0;
    dut.eval();
  };
  for (int i = 0; i < 2; ++i) tick();
  dut.rst = 0;
  tick();

  std::printf("DBQA soak test\n");

  // Full deterministic table: id, value, keypool index, extra.
  std::vector<Row> full(1024);
  for (int i = 0; i < 1024; ++i) {
    full[i].cols[0] = i;                    // id
    full[i].cols[1] = (i * 37) % 1000;      // value
    full[i].cols[2] = (i * 11) % 13;        // small key set (13 groups)
    full[i].cols[3] = 0;
  }
  load_table(dut, tick, full);

  std::mt19937 rng(0x50A3u);

  int classic_runs = 0, gb_runs = 0;
  for (int run = 0; run < 240; ++run) {
    if (run % 6 == 5) {
      // GROUP BY: keys from a small set plus forced low-byte collisions.
      const int key_col = 2;
      const uint64_t num_rows = (rng() % 3 == 0) ? 0 : 1 + rng() % 1024;
      run_groupby(dut, tick, full, key_col, 1, num_rows, gb_runs++);
    } else {
      const int op = OP_COUNT + rng() % 5;
      const int agg_col = rng() % 3;
      const uint64_t num_rows = (rng() % 3 == 0) ? 0 : 1 + rng() % 1024;
      const bool pred_en = (rng() % 2) != 0;
      const int pred_op = rng() % 6;
      const uint32_t imm = rng() % 1100;
      const int pred_col = rng() % 3;
      run_classic(dut, tick, full, op, agg_col, num_rows, pred_en, pred_op,
                  imm, pred_col, classic_runs++);
    }
  }
  std::printf("  soak complete: %d classic, %d groupby queries\n",
              classic_runs, gb_runs);

  return dbqa::summary("tb_stress");
}
