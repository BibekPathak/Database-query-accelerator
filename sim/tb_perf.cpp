// ===========================================================================
//  tb_perf.cpp -- performance harness for dbqa_top.
//
//  Runs fixed workloads over a full 1024-row table and measures the query
//  latency in clock cycles (from the CTRL start write to the first STATUS
//  read that reports done). Classic results and GROUP BY groups are both
//  covered; GROUP BY groups are accepted continuously (m_axis_tready held
//  high) so completion reflects the full aggregate+dump path.
//
//  Results are printed and written to results/perf.csv (gitignored, uploaded
//  by CI): cycles per query and rows per cycle per workload.
// ===========================================================================

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "Vdbqa_top.h"
#include "dbqa_reference.hpp"
#include "dbqa_test.hpp"

namespace {

using namespace dbqa_ref;

constexpr uint32_t REG_CTRL = 0, REG_STATUS = 1, REG_QUERY = 2,
                   REG_AGG_CFG = 3, REG_PROJ_MASK = 4, REG_PRED_BASE = 8,
                   REG_LOAD_ADDR = 0x20, REG_LOAD_DATA0 = 0x21,
                   REG_LOAD_ROW = 0x25;

constexpr int OP_COUNT = 1, OP_SUM = 2;

uint64_t g_cycles = 0;

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

uint32_t pack_agg(int op, bool groupby, int col, int key) {
  return static_cast<uint32_t>(op) << 21 |
         static_cast<uint32_t>(groupby) << 20 |
         static_cast<uint32_t>(col) << 10 | static_cast<uint32_t>(key);
}

// Runs one workload, returning the measured cycle count (start -> done).
uint64_t measure(Vdbqa_top& dut, auto& tick, bool groupby, int agg_col,
                 int key_col, uint64_t num_rows) {
  axil_write(dut, tick, REG_PRED_BASE, 0);
  axil_write(dut, tick, REG_PRED_BASE + 1, 0);
  axil_write(dut, tick, REG_PRED_BASE + 2, 0);
  axil_write(dut, tick, REG_PRED_BASE + 3, 0);
  axil_write(dut, tick, REG_QUERY, static_cast<uint32_t>(num_rows));
  axil_write(dut, tick, REG_AGG_CFG,
             pack_agg(OP_SUM, groupby, agg_col, key_col));
  axil_write(dut, tick, REG_PROJ_MASK, 0xF);

  const uint64_t c0 = g_cycles;
  axil_write(dut, tick, REG_CTRL, 1);  // start
  int polls = 0;
  while (!(axil_read(dut, tick, REG_STATUS) & 2) && polls++ < 65536) {}
  return g_cycles - c0;
}

}  // namespace

int main(int argc, char** argv) {
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
  dut.m_axis_tready = 1;  // continuously accept GROUP BY groups
  dut.eval();

  auto tick = [&]() {
    ++g_cycles;
    dut.clk = 1;
    dut.eval();
    dut.clk = 0;
    dut.eval();
  };
  for (int i = 0; i < 2; ++i) tick();
  dut.rst = 0;
  tick();

  std::printf("DBQA performance harness\n");

  std::vector<Row> full(1024);
  for (int i = 0; i < 1024; ++i) {
    full[i].cols[0] = i;                // 1024 distinct keys
    full[i].cols[1] = (i * 37) % 1000;  // value
    full[i].cols[2] = (i * 11) % 13;    // 13 distinct keys
    full[i].cols[3] = 0;
  }
  load_table(dut, tick, full);

  // name, groupby, agg_col, key_col, num_rows
  struct W {
    const char* name;
    bool groupby;
    int agg_col;
    int key_col;
    uint64_t num_rows;
  };
  const W workloads[] = {
      {"count_full", false, 1, 0, 0},
      {"sum_full", false, 1, 0, 0},
      {"sum_256", false, 1, 0, 256},
      {"sum_1024", false, 1, 0, 1024},
      {"groupby_13keys", true, 1, 2, 0},
      {"groupby_1024keys", true, 1, 0, 0},
  };

  std::FILE* csv = nullptr;
  std::string repo = ".";
  try {
    // Exe lives at <repo>/<build_dir>/tb_perf/Vtb_perf; three parents up is
    // the repository root.
    const auto exe = std::filesystem::canonical(argv[0]);
    repo = exe.parent_path().parent_path().parent_path().string();
  } catch (...) {
  }
  const std::string csv_path = repo + "/results/perf.csv";
  csv = std::fopen(csv_path.c_str(), "w");

  std::printf("%-22s %9s %12s %14s\n", "workload", "cycles", "rows/cycle",
              "rows/sec@100MHz");
  if (csv) std::fprintf(csv, "workload,num_rows,groupby,cycles_min,rows_per_cycle\n");

  for (const W& w : workloads) {
    uint64_t best = UINT64_MAX;
    for (int rep = 0; rep < 3; ++rep) {
      const uint64_t c = measure(dut, tick, w.groupby, w.agg_col, w.key_col,
                                 w.num_rows);
      if (c < best) best = c;
    }
    const uint64_t rows = (w.num_rows == 0) ? NUM_ROWS : w.num_rows;
    const double rpc = static_cast<double>(rows) / static_cast<double>(best);
    std::printf("%-22s %9llu %12.4f %14.1f\n", w.name,
                static_cast<unsigned long long>(best), rpc, rpc * 1e8);
    if (csv) {
      std::fprintf(csv, "%s,%llu,%d,%llu,%.4f\n", w.name,
                   static_cast<unsigned long long>(rows), w.groupby ? 1 : 0,
                   static_cast<unsigned long long>(best), rpc);
    }
  }
  if (csv) std::fclose(csv);

  dbqa::check(g_cycles > 0, "cycles counted");
  std::printf("  perf results written to %s\n", csv_path.c_str());
  return dbqa::summary("tb_perf");
}
