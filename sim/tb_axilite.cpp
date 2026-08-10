// ===========================================================================
//  tb_axilite.cpp -- self-checking testbench for the AXI-Lite register file.
//
//  Exercises every register in the db_pkg map over the AXI-Lite channels:
//  config writes and read-backs, the load commit strobe, the start/abort
//  control pulses, status/result/count/overflow reads, channel backpressure
//  and constrained-random round-trips.
//
//  Requires: rtl/common/db_pkg.sv and rtl/top/axi_lite_slave.sv
// ===========================================================================

#include <cstdint>
#include <cstdio>
#include <random>

#include "Vaxi_lite_slave.h"
#include "dbqa_test.hpp"

namespace {

constexpr int COL_ADDR_W = 10;
constexpr int ACCUM_W = 42;

// Verilator port types differ (CData/SData/IData/QData); cast to a common
// type for comparison.
template <typename T>
void expect_eq64(const char* what, uint64_t e, T g) {
  dbqa::expect_eq<uint64_t>(what, e, static_cast<uint64_t>(g));
}

void axil_write(Vaxi_lite_slave& dut, auto& tick, uint32_t word,
                uint32_t data, bool drive_bready = true) {
  dut.s_axil_bready = drive_bready ? 1 : 0;
  dut.s_axil_awvalid = 1;
  dut.s_axil_awaddr = word << 2;
  dut.s_axil_wvalid = 1;
  dut.s_axil_wdata = data;
  dut.eval();
  tick();
  dut.s_axil_awvalid = 0;
  dut.s_axil_wvalid = 0;
  int cyc = 0;
  while (!dut.s_axil_bvalid && cyc++ < 16) {
    dut.eval();
    tick();
  }
  dut.eval();
  dbqa::check(dut.s_axil_bvalid, "write: bvalid asserted");
  while (dut.s_axil_bvalid) {
    dut.eval();
    tick();
  }
  dut.eval();
}

uint32_t axil_read(Vaxi_lite_slave& dut, auto& tick, uint32_t word) {
  dut.s_axil_arvalid = 1;
  dut.s_axil_araddr = word << 2;
  dut.eval();
  tick();
  dut.s_axil_arvalid = 0;
  int cyc = 0;
  while (!dut.s_axil_rvalid && cyc++ < 16) {
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

// Backpressure-torture variants: randomly gate bready / rready. The same
// register must still be written / read because handshakes hold until ready.
void gated_write(Vaxi_lite_slave& dut, auto& tick, std::mt19937& rng,
                 uint32_t word, uint32_t data) {
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

uint32_t gated_read(Vaxi_lite_slave& dut, auto& tick, std::mt19937& rng,
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

}  // namespace

int main() {
  Vaxi_lite_slave dut;
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
  dut.error = 0;
  dut.busy = 0;
  dut.done = 0;
  dut.result = 0;
  dut.count = 0;
  dut.overflow = 0;
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

  std::printf("DBQA AXI-Lite testbench\n");

  // -------------------------------------------------------------------------
  // Config writes and read-backs.
  // -------------------------------------------------------------------------
  axil_write(dut, tick, 2, 10);  // REG_QUERY: num_rows
  expect_eq64("query_cfg num_rows", 10ull, dut.query_cfg);
  expect_eq64("query readback", 10u, axil_read(dut, tick, 2));

  axil_write(dut, tick, 3, 0x2C0015u);  // REG_AGG_CFG
  expect_eq64("agg_cfg", 0x2C0015ull, dut.agg_cfg);
  expect_eq64("agg readback", 0x2C0015u, axil_read(dut, tick, 3));

  axil_write(dut, tick, 4, 0b0101u);  // REG_PROJ_MASK
  expect_eq64("proj_mask", 0b0101u, dut.proj_mask);
  expect_eq64("proj readback", 0b0101u, axil_read(dut, tick, 4));

  // Predicate slot 0: low word then high word.
  axil_write(dut, tick, 8, 0x12345678u);
  expect_eq64("pred0 imm", 0x12345678ull, dut.pred_cfg[0] & 0xFFFFFFFFull);
  axil_write(dut, tick, 9, 0x6800u);  // {enable=1, op=5, combine=0, column=0}
  expect_eq64("pred0 enable", 1ull, (dut.pred_cfg[0] >> 46) & 1);
  expect_eq64("pred0 op", 5ull, (dut.pred_cfg[0] >> 43) & 7);
  expect_eq64("pred0 combine", 0ull, (dut.pred_cfg[0] >> 42) & 1);
  expect_eq64("pred0 column", 0ull, (dut.pred_cfg[0] >> 32) & 0x3FF);

  // Predicate slot 1 untouched.
  expect_eq64("pred1 untouched", 0ull, dut.pred_cfg[1]);

  // -------------------------------------------------------------------------
  // Load commit strobe.
  // -------------------------------------------------------------------------
  axil_write(dut, tick, 0x20, 7);
  axil_write(dut, tick, 0x21, 0x1111u);
  axil_write(dut, tick, 0x22, 0x2222u);
  axil_write(dut, tick, 0x23, 0x3333u);
  axil_write(dut, tick, 0x24, 0x4444u);
  expect_eq64("load_addr pre", 7ull, dut.load_addr);
  dut.s_axil_awvalid = 1;
  dut.s_axil_awaddr = 0x25 << 2;
  dut.s_axil_wvalid = 1;
  dut.s_axil_wdata = 0;
  dut.eval();
  tick();
  dut.s_axil_awvalid = 0;
  dut.s_axil_wvalid = 0;
  dbqa::check(dut.load_wen == 1, "load_wen pulses after commit");
  expect_eq64("load_addr", 7ull, dut.load_addr);
  expect_eq64("load_data0", 0x1111ull, dut.load_data[0]);
  expect_eq64("load_data1", 0x2222ull, dut.load_data[1]);
  expect_eq64("load_data2", 0x3333ull, dut.load_data[2]);
  expect_eq64("load_data3", 0x4444ull, dut.load_data[3]);
  tick();
  dbqa::check(dut.load_wen == 0, "load_wen clears next cycle");
  while (dut.s_axil_bvalid) {
    dut.eval();
    tick();
  }
  dut.eval();

  // -------------------------------------------------------------------------
  // Control strobes.
  // -------------------------------------------------------------------------
  dut.s_axil_awvalid = 1;
  dut.s_axil_awaddr = 0;
  dut.s_axil_wvalid = 1;
  dut.s_axil_wdata = 1;  // start
  dut.eval();
  tick();
  dut.s_axil_awvalid = 0;
  dut.s_axil_wvalid = 0;
  dbqa::check(dut.start == 1, "start pulses after CTRL write");
  tick();
  dbqa::check(dut.start == 0, "start clears next cycle");
  while (dut.s_axil_bvalid) {
    dut.eval();
    tick();
  }
  dut.eval();

  dut.s_axil_awvalid = 1;
  dut.s_axil_awaddr = 0;
  dut.s_axil_wvalid = 1;
  dut.s_axil_wdata = 2;  // abort
  dut.eval();
  tick();
  dut.s_axil_awvalid = 0;
  dut.s_axil_wvalid = 0;
  dbqa::check(dut.abort_req == 1, "abort_req pulses after CTRL write");
  tick();
  dbqa::check(dut.abort_req == 0, "abort_req clears next cycle");
  while (dut.s_axil_bvalid) {
    dut.eval();
    tick();
  }
  dut.eval();

  // -------------------------------------------------------------------------
  // Status, result, count and overflow reads.
  // -------------------------------------------------------------------------
  dut.busy = 1;
  dut.done = 0;
  dut.error = 1;
  dut.eval();
  tick();
  dut.eval();
  expect_eq64("status busy|error", (1u | (1u << 2)), axil_read(dut, tick, 1));
  dut.busy = 0;
  dut.done = 1;
  dut.error = 0;
  dut.eval();
  tick();
  dut.eval();
  expect_eq64("status done", 2u, axil_read(dut, tick, 1));

  dut.result = 0x123456789ABull;
  dut.count = 0x2AA00000000ull;
  dut.overflow = 1;
  dut.eval();
  tick();
  dut.eval();
  expect_eq64("result lo", 0x456789ABu, axil_read(dut, tick, 0x30));
  expect_eq64("result hi", 0x123u, axil_read(dut, tick, 0x31));
  expect_eq64("count lo", 0u, axil_read(dut, tick, 0x32));
  expect_eq64("count hi", 0x2AAu, axil_read(dut, tick, 0x33));
  expect_eq64("overflow", 1u, axil_read(dut, tick, 0x34));
  expect_eq64("unmapped read", 0u, axil_read(dut, tick, 0x40));

  // -------------------------------------------------------------------------
  // Backpressure.
  // -------------------------------------------------------------------------
  // Write channel: without BREADY the write is held, then applied on release.
  dut.s_axil_bready = 0;
  dut.s_axil_awvalid = 1;
  dut.s_axil_awaddr = 2 << 2;
  dut.s_axil_wvalid = 1;
  dut.s_axil_wdata = 99;
  dut.eval();
  tick();
  dut.s_axil_awvalid = 0;
  dut.s_axil_wvalid = 0;
  int cyc = 0;
  while (!dut.s_axil_bvalid && cyc++ < 16) {
    dut.eval();
    tick();
  }
  dut.eval();
  dbqa::check(dut.s_axil_bvalid, "bp: bvalid held");
  dbqa::check(!dut.s_axil_awready, "bp: awready deasserted while B pending");
  dut.s_axil_bready = 1;
  dut.eval();
  tick();
  dut.eval();
  dbqa::check(!dut.s_axil_bvalid, "bp: bvalid clears after bready");
  expect_eq64("bp: write applied", 99ull, dut.query_cfg);

  // Read channel: without RREADY the response is held, then released.
  dut.s_axil_rready = 0;
  dut.s_axil_arvalid = 1;
  dut.s_axil_araddr = 4 << 2;
  dut.eval();
  tick();
  dut.s_axil_arvalid = 0;
  cyc = 0;
  while (!dut.s_axil_rvalid && cyc++ < 16) {
    dut.eval();
    tick();
  }
  dut.eval();
  dbqa::check(dut.s_axil_rvalid, "bp: rvalid held");
  dbqa::check(!dut.s_axil_arready, "bp: arready deasserted while R pending");
  expect_eq64("bp: read data", 0b0101u, dut.s_axil_rdata);
  dut.s_axil_rready = 1;
  dut.eval();
  tick();
  dut.eval();
  dbqa::check(!dut.s_axil_rvalid, "bp: rvalid clears after rready");

  // -------------------------------------------------------------------------
  // Constrained-random register round-trips.
  // -------------------------------------------------------------------------
  std::mt19937 rng(0x21Eu);
  for (int run = 0; run < 100; ++run) {
    const uint32_t num_rows = rng() & 0x3FF;
    const uint32_t agg = rng() & 0xFFFFFF;
    const uint32_t mask = rng() & 0xF;
    axil_write(dut, tick, 2, num_rows);
    axil_write(dut, tick, 3, agg);
    axil_write(dut, tick, 4, mask);
    expect_eq64("rand query", num_rows, dut.query_cfg);
    expect_eq64("rand agg", agg, dut.agg_cfg);
    expect_eq64("rand proj", mask, dut.proj_mask);

    const uint32_t imm = rng();
    const uint32_t hi = (rng() & 0x3FF);  // {enable, op, combine, column}
    axil_write(dut, tick, 8, imm);
    axil_write(dut, tick, 9, hi);
    expect_eq64("rand pred imm", imm, dut.pred_cfg[0] & 0xFFFFFFFFull);
    expect_eq64("rand pred hi", hi, (dut.pred_cfg[0] >> 32) & 0x3FFF);
  }
  std::printf("  constrained-random round-trips complete\n");

  // -------------------------------------------------------------------------
  // Backpressure torture: random bready/rready gating while writing and
  // reading the same registers; values must round-trip unchanged.
  // -------------------------------------------------------------------------
  {
    std::mt19937 trng(0xBA0Fu);
    for (int run = 0; run < 60; ++run) {
      const uint32_t num_rows = trng() & 0x3FF;
      const uint32_t agg = trng() & 0xFFFFFF;
      const uint32_t mask = trng() & 0xF;
      const uint32_t imm = trng();
      const uint32_t hi = trng() & 0x3FFF;
      gated_write(dut, tick, trng, 2, num_rows);
      gated_write(dut, tick, trng, 3, agg);
      gated_write(dut, tick, trng, 4, mask);
      gated_write(dut, tick, trng, 8, imm);
      gated_write(dut, tick, trng, 9, hi);
      expect_eq64("torture query", num_rows, dut.query_cfg);
      expect_eq64("torture agg", agg, dut.agg_cfg);
      expect_eq64("torture proj", mask, dut.proj_mask);
      expect_eq64("torture pred imm", imm,
                  dut.pred_cfg[0] & 0xFFFFFFFFull);
      expect_eq64("torture pred hi", hi, (dut.pred_cfg[0] >> 32) & 0x3FFF);
      expect_eq64("torture read query", num_rows, gated_read(dut, tick, trng, 2));
      expect_eq64("torture read agg", agg, gated_read(dut, tick, trng, 3));
      expect_eq64("torture read proj", mask, gated_read(dut, tick, trng, 4));
      expect_eq64("torture read pred lo", imm,
                  gated_read(dut, tick, trng, 8));
    }
  }
  std::printf("  backpressure torture complete\n");

  return dbqa::summary("tb_axilite");
}
