// ===========================================================================
//  tb_reader.cpp -- self-checking testbench for column_reader.
//
//  Exercises the reader at the production configuration (4 x 32-bit columns,
//  1024 rows) plus edge table sizes (1 / 3 / 16 / 257 rows) through the
//  column_reader_tb_top wrapper.
//
//  For each configuration:
//    * load a random table through the load port (one whole row per cycle)
//    * start the query and stream the rows out with random backpressure
//    * verify every row's columns, the pass bit, tlast on the final row, and
//      the busy/done handshake
//    * drain the output FIFO and check it empties
//    * repeat across several seeds to exercise stalls at every phase
//
//  Requires: rtl/memory/column_memory.sv, rtl/memory/column_reader.sv,
//  rtl/interfaces/axis_fifo.sv, rtl/interfaces/axis_register.sv,
//  sim/column_reader_tb_top.sv
// ===========================================================================

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>
#include <vector>

#include "Vcolumn_reader_tb_top.h"
#include "dbqa_test.hpp"

namespace {

// ---------------------------------------------------------------------------
// Per-instance port accessors, bound via lambdas to the Verilated model.
// ---------------------------------------------------------------------------
struct ReaderPorts {
    int num_rows;
    int num_cols;
    int col_width;

    std::function<void(uint32_t)>       set_load_wen;
    std::function<void(uint32_t)>       set_load_addr;
    std::function<void(int, uint32_t)>  set_load_data;   // (column, value)
    std::function<void(uint32_t)>       set_start;
    std::function<void(uint32_t)>       set_scan_bound;
    std::function<void(uint32_t)>       set_m_ready;
    std::function<bool()>               get_busy;
    std::function<bool()>               get_done;
    std::function<bool()>               get_m_valid;
    std::function<bool()>               get_m_last;
    std::function<uint32_t(int)>        get_m_col;       // column value
    std::function<bool()>               get_m_pass;
};

// ---------------------------------------------------------------------------
// Load a random table, run a query, verify the full stream under random
// backpressure. `tick` advances the shared clock by one cycle.
// ---------------------------------------------------------------------------
void run_query(ReaderPorts& p, std::function<void()> tick, uint32_t seed,
               bool allow_stall) {
    std::mt19937 rng(seed);
    const uint32_t mask =
        (p.col_width >= 32) ? 0xFFFFFFFFu : ((1u << p.col_width) - 1);

    std::vector<std::vector<uint32_t>> table(
        p.num_rows, std::vector<uint32_t>(p.num_cols));
    for (int r = 0; r < p.num_rows; ++r)
        for (int c = 0; c < p.num_cols; ++c) table[r][c] = rng() & mask;

    // Load the table.
    p.set_start(0);
    p.set_scan_bound(0);  // full scan by default
    for (int r = 0; r < p.num_rows; ++r) {
        p.set_load_wen(1);
        p.set_load_addr(static_cast<uint32_t>(r));
        for (int c = 0; c < p.num_cols; ++c)
            p.set_load_data(c, table[r][c]);
        tick();
    }
    p.set_load_wen(0);
    tick();

    // Start the query.
    p.set_start(1);
    tick();
    p.set_start(0);
    dbqa::check(p.get_busy(), "busy asserted after start");

    // Stream out and verify every row.
    int expected = 0;
    for (int cyc = 0; cyc < 8 * p.num_rows + 64 && expected < p.num_rows; ++cyc) {
        const bool valid = p.get_m_valid();
        const bool ready = allow_stall ? (rng() % 3 != 0) : true;
        p.set_m_ready(valid && ready);
        if (valid && ready) {
            for (int c = 0; c < p.num_cols; ++c) {
                char msg[64];
                std::snprintf(msg, sizeof msg, "row %d col %d data", expected, c);
                dbqa::expect_eq(msg, table[expected][c], p.get_m_col(c));
            }
            dbqa::check(p.get_m_pass(), "pass bit set");
            dbqa::check(p.get_m_last() == (expected == p.num_rows - 1),
                        "tlast on the final row");
            ++expected;
        }
        tick();
    }
    dbqa::expect_eq("all rows received", p.num_rows, expected);
    dbqa::check(p.get_done(), "done asserted");
    dbqa::check(!p.get_busy(), "busy deasserted after done");

    // Drain the output FIFO.
    for (int cyc = 0; cyc < 32; ++cyc) {
        p.set_m_ready(1);
        tick();
        if (!p.get_m_valid()) break;
    }
    dbqa::check(!p.get_m_valid(), "output FIFO drained");
}

// ---------------------------------------------------------------------------
// Bounded scan: scan_bound stops the BRAM traversal early. Exactly `bound`
// rows (or the whole table if bound == 0 / bound >= num_rows) are emitted,
// with tlast on the final emitted row.
// ---------------------------------------------------------------------------
void run_bounded_query(ReaderPorts& p, std::function<void()> tick,
                       uint32_t seed, uint32_t bound) {
    std::mt19937 rng(seed);
    const uint32_t mask =
        (p.col_width >= 32) ? 0xFFFFFFFFu : ((1u << p.col_width) - 1);

    std::vector<std::vector<uint32_t>> table(
        p.num_rows, std::vector<uint32_t>(p.num_cols));
    for (int r = 0; r < p.num_rows; ++r)
        for (int c = 0; c < p.num_cols; ++c) table[r][c] = rng() & mask;

    p.set_start(0);
    for (int r = 0; r < p.num_rows; ++r) {
        p.set_load_wen(1);
        p.set_load_addr(static_cast<uint32_t>(r));
        for (int c = 0; c < p.num_cols; ++c)
            p.set_load_data(c, table[r][c]);
        tick();
    }
    p.set_load_wen(0);
    tick();

    const int expect =
        (bound == 0) ? p.num_rows : std::min<int>(bound, p.num_rows);

    p.set_scan_bound(bound);
    p.set_start(1);
    tick();
    p.set_start(0);
    dbqa::check(p.get_busy(), "bounded: busy asserted after start");

    int got = 0;
    for (int cyc = 0; cyc < 8 * p.num_rows + 64 && got < expect; ++cyc) {
        const bool valid = p.get_m_valid();
        p.set_m_ready(1);
        if (valid) {
            for (int c = 0; c < p.num_cols; ++c) {
                char msg[64];
                std::snprintf(msg, sizeof msg, "bounded row %d col %d data", got,
                              c);
                dbqa::expect_eq(msg, table[got][c], p.get_m_col(c));
            }
            dbqa::check(p.get_m_last() == (got == expect - 1),
                        "bounded: tlast on the final emitted row");
            ++got;
        }
        tick();
    }
    dbqa::expect_eq("bounded: rows received", expect, got);
    dbqa::check(p.get_done(), "bounded: done asserted");
    dbqa::check(!p.get_busy(), "bounded: busy deasserted after done");

    for (int cyc = 0; cyc < 32; ++cyc) {
        p.set_m_ready(1);
        tick();
        if (!p.get_m_valid()) break;
    }
    dbqa::check(!p.get_m_valid(), "bounded: output FIFO drained");
}

// ---------------------------------------------------------------------------
// Memory stress: controlled stall profiles plus tready-gating correctness.
//
//   stall_start : hold m_axis_tready low for N cycles once the output first
//                 becomes valid (long backpressure burst at the head)
//   stall_mid   : after the first half of the rows, hold m_axis_tready low
//                 for N cycles (mid-stream stall), then resume at 1/2 random
//
// While a beat is held (m_axis_tvalid=1, m_axis_tready=0) its data must stay
// stable and correct -- this is checked every held cycle against the row that
// will be consumed next.
// ---------------------------------------------------------------------------
void run_stress(ReaderPorts& p, std::function<void()> tick, uint32_t seed,
                int stall_start, int stall_mid) {
    std::mt19937 rng(seed);
    const uint32_t mask =
        (p.col_width >= 32) ? 0xFFFFFFFFu : ((1u << p.col_width) - 1);

    std::vector<std::vector<uint32_t>> table(
        p.num_rows, std::vector<uint32_t>(p.num_cols));
    for (int r = 0; r < p.num_rows; ++r)
        for (int c = 0; c < p.num_cols; ++c) table[r][c] = rng() & mask;

    // Load the table.
    p.set_start(0);
    p.set_scan_bound(0);  // full scan by default
    for (int r = 0; r < p.num_rows; ++r) {
        p.set_load_wen(1);
        p.set_load_addr(static_cast<uint32_t>(r));
        for (int c = 0; c < p.num_cols; ++c) p.set_load_data(c, table[r][c]);
        tick();
    }
    p.set_load_wen(0);
    tick();

    p.set_start(1);
    tick();
    p.set_start(0);
    dbqa::check(p.get_busy(), "stress: busy asserted after start");

    int expected = 0;
    int start_stall_left = stall_start;
    int mid_stall_left = 0;
    bool mid_started = false;

    for (int cyc = 0; cyc < 16 * p.num_rows + 512 && expected < p.num_rows; ++cyc) {
        const bool valid = p.get_m_valid();

        // Stall profile: head burst, then random, then a mid-stream burst,
        // then random again. While stalling, the held beat must be stable.
        bool ready;
        if (valid && start_stall_left > 0) {
            ready = false;
            --start_stall_left;
        } else if (valid && !mid_started && expected >= p.num_rows / 2) {
            ready = false;
            mid_started = true;
            mid_stall_left = stall_mid;
        } else if (valid && mid_stall_left > 0) {
            ready = false;
            --mid_stall_left;
        } else {
            ready = (rng() % 3 != 0);
        }
        p.set_m_ready(ready);

        if (valid && !ready) {
            // tready gating: the held beat must equal the next expected row.
            bool stable = true;
            for (int c = 0; c < p.num_cols; ++c)
                stable &= (p.get_m_col(c) == table[expected][c]);
            stable &= (p.get_m_last() == (expected == p.num_rows - 1));
            dbqa::check(stable, "stress: held beat stable and correct");
        }

        if (valid && ready) {
            for (int c = 0; c < p.num_cols; ++c) {
                char msg[64];
                std::snprintf(msg, sizeof msg, "stress: row %d col %d data", expected, c);
                dbqa::expect_eq(msg, table[expected][c], p.get_m_col(c));
            }
            dbqa::check(p.get_m_pass(), "stress: pass bit set");
            dbqa::check(p.get_m_last() == (expected == p.num_rows - 1),
                        "stress: tlast on the final row");
            ++expected;
        }
        tick();
    }

    dbqa::expect_eq("stress: all rows received", p.num_rows, expected);
    dbqa::check(p.get_done(), "stress: done asserted");
    dbqa::check(!p.get_busy(), "stress: busy deasserted after done");

    // Drain the output FIFO.
    for (int cyc = 0; cyc < 64; ++cyc) {
        p.set_m_ready(1);
        tick();
        if (!p.get_m_valid()) break;
    }
    dbqa::check(!p.get_m_valid(), "stress: output FIFO drained");
}

}  // namespace

int main() {
    Vcolumn_reader_tb_top dut;
    dut.clk = 0;
    dut.rst = 1;
    dut.s0_scan_bound = 0;
    dut.s1_scan_bound = 0;
    dut.s2_scan_bound = 0;
    dut.s3_scan_bound = 0;
    dut.s4_scan_bound = 0;
    dut.eval();

    auto tick = [&]() {
        dut.clk = 1;
        dut.eval();
        dut.clk = 0;
        dut.eval();
    };

    // Bind all instances.
    ReaderPorts p0;
    p0.num_rows = 1024;
    p0.num_cols = 4;
    p0.col_width = 32;
    p0.set_load_wen = [&](uint32_t v) { dut.s0_load_wen = v; };
    p0.set_load_addr = [&](uint32_t v) { dut.s0_load_addr = v; };
    p0.set_load_data = [&](int c, uint32_t v) { dut.s0_load_data[c] = v; };
    p0.set_start = [&](uint32_t v) { dut.s0_start = v; };
    p0.set_scan_bound = [&](uint32_t v) { dut.s0_scan_bound = v; };
    p0.set_m_ready = [&](uint32_t v) { dut.m0_axis_tready = v; };
    p0.get_busy = [&]() { return dut.m0_busy; };
    p0.get_done = [&]() { return dut.m0_done; };
    p0.get_m_valid = [&]() { return dut.m0_axis_tvalid; };
    p0.get_m_last = [&]() { return dut.m0_axis_tlast; };
    p0.get_m_col = [&](int c) { return dut.m0_axis_tdata[c]; };
    p0.get_m_pass = [&]() { return (dut.m0_axis_tdata[4] >> 0) & 1u; };

    auto make_edge = [&](ReaderPorts& p, int rows, int idx) {
        p.num_rows = rows;
        p.num_cols = 2;
        p.col_width = 16;
        // Bind the port accessors for the selected edge instance.
        if (idx == 1) {
            p.set_load_wen  = [&](uint32_t v) { dut.s1_load_wen = v; };
            p.set_load_addr = [&](uint32_t v) { dut.s1_load_addr = v; };
            p.set_load_data = [&](int c, uint32_t v) { dut.s1_load_data[c] = v; };
            p.set_start     = [&](uint32_t v) { dut.s1_start = v; };
            p.set_scan_bound = [&](uint32_t v) { dut.s1_scan_bound = v; };
            p.set_m_ready   = [&](uint32_t v) { dut.m1_axis_tready = v; };
            p.get_busy      = [&]() { return dut.m1_busy; };
            p.get_done      = [&]() { return dut.m1_done; };
            p.get_m_valid   = [&]() { return dut.m1_axis_tvalid; };
            p.get_m_last    = [&]() { return dut.m1_axis_tlast; };
            p.get_m_col     = [&](int c) {
                const uint64_t d = static_cast<uint64_t>(dut.m1_axis_tdata);
                return static_cast<uint32_t>((d >> (c * 16)) & 0xFFFF);
            };
            p.get_m_pass    = [&]() {
                return ((static_cast<uint64_t>(dut.m1_axis_tdata) >> 32) & 1u) != 0;
            };
        } else if (idx == 2) {
            p.set_load_wen  = [&](uint32_t v) { dut.s2_load_wen = v; };
            p.set_load_addr = [&](uint32_t v) { dut.s2_load_addr = v; };
            p.set_load_data = [&](int c, uint32_t v) { dut.s2_load_data[c] = v; };
            p.set_start     = [&](uint32_t v) { dut.s2_start = v; };
            p.set_scan_bound = [&](uint32_t v) { dut.s2_scan_bound = v; };
            p.set_m_ready   = [&](uint32_t v) { dut.m2_axis_tready = v; };
            p.get_busy      = [&]() { return dut.m2_busy; };
            p.get_done      = [&]() { return dut.m2_done; };
            p.get_m_valid   = [&]() { return dut.m2_axis_tvalid; };
            p.get_m_last    = [&]() { return dut.m2_axis_tlast; };
            p.get_m_col     = [&](int c) {
                const uint64_t d = static_cast<uint64_t>(dut.m2_axis_tdata);
                return static_cast<uint32_t>((d >> (c * 16)) & 0xFFFF);
            };
            p.get_m_pass    = [&]() {
                return ((static_cast<uint64_t>(dut.m2_axis_tdata) >> 32) & 1u) != 0;
            };
        } else if (idx == 3) {
            p.set_load_wen  = [&](uint32_t v) { dut.s3_load_wen = v; };
            p.set_load_addr = [&](uint32_t v) { dut.s3_load_addr = v; };
            p.set_load_data = [&](int c, uint32_t v) { dut.s3_load_data[c] = v; };
            p.set_start     = [&](uint32_t v) { dut.s3_start = v; };
            p.set_scan_bound = [&](uint32_t v) { dut.s3_scan_bound = v; };
            p.set_m_ready   = [&](uint32_t v) { dut.m3_axis_tready = v; };
            p.get_busy      = [&]() { return dut.m3_busy; };
            p.get_done      = [&]() { return dut.m3_done; };
            p.get_m_valid   = [&]() { return dut.m3_axis_tvalid; };
            p.get_m_last    = [&]() { return dut.m3_axis_tlast; };
            p.get_m_col     = [&](int c) {
                const uint64_t d = static_cast<uint64_t>(dut.m3_axis_tdata);
                return static_cast<uint32_t>((d >> (c * 16)) & 0xFFFF);
            };
            p.get_m_pass    = [&]() {
                return ((static_cast<uint64_t>(dut.m3_axis_tdata) >> 32) & 1u) != 0;
            };
        } else {
            p.set_load_wen  = [&](uint32_t v) { dut.s4_load_wen = v; };
            p.set_load_addr = [&](uint32_t v) { dut.s4_load_addr = v; };
            p.set_load_data = [&](int c, uint32_t v) { dut.s4_load_data[c] = v; };
            p.set_start     = [&](uint32_t v) { dut.s4_start = v; };
            p.set_scan_bound = [&](uint32_t v) { dut.s4_scan_bound = v; };
            p.set_m_ready   = [&](uint32_t v) { dut.m4_axis_tready = v; };
            p.get_busy      = [&]() { return dut.m4_busy; };
            p.get_done      = [&]() { return dut.m4_done; };
            p.get_m_valid   = [&]() { return dut.m4_axis_tvalid; };
            p.get_m_last    = [&]() { return dut.m4_axis_tlast; };
            p.get_m_col     = [&](int c) {
                const uint64_t d = static_cast<uint64_t>(dut.m4_axis_tdata);
                return static_cast<uint32_t>((d >> (c * 16)) & 0xFFFF);
            };
            p.get_m_pass    = [&]() {
                return ((static_cast<uint64_t>(dut.m4_axis_tdata) >> 32) & 1u) != 0;
            };
        }
    };

    ReaderPorts p1, p2, p3, p4;
    make_edge(p1, 1, 1);
    make_edge(p2, 3, 2);
    make_edge(p3, 16, 3);
    make_edge(p4, 257, 4);

    // Reset.
    for (int i = 0; i < 2; ++i) tick();
    dut.rst = 0;
    tick();

    std::printf("DBQA column_reader testbench\n");

    // Production config: directed (no stall) then constrained-random.
    run_query(p0, tick, 0xA11CEu, false);
    run_query(p0, tick, 0xB00Bu, true);
    run_query(p0, tick, 0xC0FFEEu, true);

    // Edge sizes: directed then constrained-random each.
    for (auto* p : {&p1, &p2, &p3, &p4}) {
        run_query(*p, tick, 0xD1CEu, false);
        run_query(*p, tick, 0xED6Eu, true);
    }

    // Memory stress (2.4).
    //  - full-capacity multi-query: heavy random backpressure, many runs
    for (int q = 0; q < 12; ++q) run_query(p0, tick, 0xF00Du + q, true);
    //  - controlled stall profiles with tready-gating stability checks
    run_stress(p0, tick, 0x5EEDu, 48, 96);
    run_stress(p0, tick, 0xBEEFu, 1, 0);
    run_stress(p0, tick, 0xCAFEu, 0, 200);
    //  - back-to-back single-row queries
    for (int q = 0; q < 24; ++q) run_query(p1, tick, 0x5000u + q, true);

    // Bounded scans: scan_bound stops the traversal early.
    run_bounded_query(p0, tick, 0xBA5Eu, 1);    // single row
    run_bounded_query(p0, tick, 0xBA5Fu, 64);   // 64 of 1024 rows
    run_bounded_query(p0, tick, 0xBA60u, 256);  // 256 of 1024 rows
    run_bounded_query(p0, tick, 0xBA61u, 1023); // largest representable bound
    run_bounded_query(p0, tick, 0xBA62u, 0);    // 0 = full table
    run_bounded_query(p2, tick, 0xBA63u, 2);    // 2 of 3 rows
    run_bounded_query(p2, tick, 0xBA64u, 3);    // bound == table size

    return dbqa::summary("tb_reader");
}
