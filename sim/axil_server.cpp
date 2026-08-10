// ===========================================================================
//  axil_server.cpp -- stdio co-simulation server for dbqa_top.
//
//  Exposes the Verilated accelerator to the Python control plane over a
//  simple line-based stdio protocol. Each command drives the AXI-Lite or
//  AXI-Stream interfaces with the same timing as tb_top.cpp:
//
//    W <word> <data>   AXI-Lite write to register <word> (hex) with <data>
//                      -> replies "OK"
//    R <word>          AXI-Lite read of register <word> (hex)
//                      -> replies "<data>" (hex)
//    G                 Sample the GROUP BY result stream (m_axis). If a group
//                      beat is valid, consume it and reply
//                      "<key> <count> <sum> <min> <max> <last>" (hex);
//                      otherwise reply "NONE"
//    Q                 Quit (replies "BYE")
//
//  Values are 32-bit registers on the AXI-Lite bus and hex on the wire.
//  One command is processed per line of stdin; results are one line of
//  stdout. The simulation advances by however many clock cycles each
//  handshake requires, so the Python side needs no clock control.
// ===========================================================================

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "Vdbqa_top.h"

namespace {

// Advance the DUT by one clock cycle.
template <typename Dut>
void tick(Dut& dut) {
  dut.clk = 1;
  dut.eval();
  dut.clk = 0;
  dut.eval();
}

// AXI-Lite write to a word offset, mirroring tb_top's helper.
template <typename Dut>
void axil_write(Dut& dut, uint32_t word, uint32_t data) {
  dut.s_axil_bready = 1;
  dut.s_axil_awvalid = 1;
  dut.s_axil_awaddr = word << 2;
  dut.s_axil_wvalid = 1;
  dut.s_axil_wdata = data;
  dut.eval();
  tick(dut);
  dut.s_axil_awvalid = 0;
  dut.s_axil_wvalid = 0;
  int cyc = 0;
  while (!dut.s_axil_bvalid && cyc++ < 1024) {
    dut.eval();
    tick(dut);
  }
  dut.eval();
  while (dut.s_axil_bvalid) {
    dut.eval();
    tick(dut);
  }
  dut.eval();
}

// AXI-Lite read of a word offset; returns the data.
template <typename Dut>
uint32_t axil_read(Dut& dut, uint32_t word) {
  dut.s_axil_rready = 1;
  dut.s_axil_arvalid = 1;
  dut.s_axil_araddr = word << 2;
  dut.eval();
  tick(dut);
  dut.s_axil_arvalid = 0;
  int cyc = 0;
  while (!dut.s_axil_rvalid && cyc++ < 1024) {
    dut.eval();
    tick(dut);
  }
  dut.eval();
  uint32_t d = dut.s_axil_rvalid ? dut.s_axil_rdata : 0xDEADBEEFu;
  while (dut.s_axil_rvalid) {
    dut.eval();
    tick(dut);
  }
  dut.eval();
  return d;
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
  for (int i = 0; i < 2; ++i) tick(dut);
  dut.rst = 0;
  tick(dut);

  std::string line;
  char buf[256];
  while (std::fgets(buf, sizeof buf, stdin) != nullptr) {
    line = buf;
    // Trim trailing whitespace.
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
      line.pop_back();
    }
    if (line.empty()) continue;

    if (line == "Q") {
      std::printf("BYE\n");
      std::fflush(stdout);
      return 0;
    }

    if (line.size() >= 2 && (line[0] == 'W' || line[0] == 'R') &&
        line[1] == ' ') {
      char op = line[0];
      uint32_t word = 0;
      if (op == 'W') {
        uint32_t data = 0;
        if (std::sscanf(line.c_str() + 2, "%x %x", &word, &data) == 2) {
          axil_write(dut, word, data);
          std::printf("OK\n");
        } else {
          std::printf("ERR\n");
        }
      } else {
        if (std::sscanf(line.c_str() + 2, "%x", &word) == 1) {
          std::printf("%08x\n", axil_read(dut, word));
        } else {
          std::printf("ERR\n");
        }
      }
      std::fflush(stdout);
      continue;
    }

    if (line == "G") {
      // Advance the simulation one cycle on every call so the Python side
      // can poll until the GROUP BY dump phase produces groups.
      dut.eval();
      if (dut.m_axis_tvalid) {
        uint64_t v[6] = {0, 0, 0, 0, 0, 0};
        for (int b = 179; b >= 148; --b)
          v[0] = (v[0] << 1) | ((dut.m_axis_tdata[b / 32] >> (b % 32)) & 1u);
        for (int b = 147; b >= 106; --b)
          v[1] = (v[1] << 1) | ((dut.m_axis_tdata[b / 32] >> (b % 32)) & 1u);
        for (int b = 105; b >= 64; --b)
          v[2] = (v[2] << 1) | ((dut.m_axis_tdata[b / 32] >> (b % 32)) & 1u);
        for (int b = 63; b >= 32; --b)
          v[3] = (v[3] << 1) | ((dut.m_axis_tdata[b / 32] >> (b % 32)) & 1u);
        for (int b = 31; b >= 0; --b)
          v[4] = (v[4] << 1) | ((dut.m_axis_tdata[b / 32] >> (b % 32)) & 1u);
        const bool last = dut.m_axis_tlast;
        tick(dut);  // accept the beat (m_axis_tready is held high)
        std::printf("%08llx %010llx %010llx %08llx %08llx %d\n",
                    static_cast<unsigned long long>(v[0]),
                    static_cast<unsigned long long>(v[1]),
                    static_cast<unsigned long long>(v[2]),
                    static_cast<unsigned long long>(v[3]),
                    static_cast<unsigned long long>(v[4]), last ? 1 : 0);
      } else {
        tick(dut);  // still advance the simulation
        std::printf("NONE\n");
      }
      std::fflush(stdout);
      continue;
    }

    std::printf("ERR\n");
    std::fflush(stdout);
  }
  return 0;
}
