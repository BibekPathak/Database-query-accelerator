// ===========================================================================
//  tb_fifo.cpp -- self-checking testbench for axis_fifo.
//
//  Covers the FIFO at DEPTH = {1, 3, 16, 257} via the axis_fifo_multi
//  wrapper. Verification is two-pronged:
//
//  1. Directed tests on a known-good instance (DEPTH = 16):
//       * reset behavior
//       * single-beat first-word-fall-through
//       * fill-to-full (s_axis_tready deasserts exactly at capacity)
//       * overflow push is rejected
//       * drain-to-empty with strict output ordering
//       * output stability while backpressured
//       * simultaneous push+pop (including the count==1 drain+fill path)
//       * tlast ordering
//  2. Fill-to-full / drain-to-empty on every depth (1, 3, 16, 257).
//  3. Constrained-random scoreboard on all four instances simultaneously:
//       * each instance has an independent std::queue reference model
//       * random push/pop with random backpressure
//       * per-cycle cross-check of tvalid/tready against occupancy
//       * ordering + tlast + lossless conservation across a forced drain
//
//  Requires: rtl/interfaces/axis_fifo.sv, sim/axis_fifo_multi.sv
// ===========================================================================

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <queue>
#include <random>
#include <utility>
#include <vector>

#include "Vaxis_fifo_multi.h"
#include "dbqa_test.hpp"

namespace {

constexpr int N_INST = 4;
constexpr int DEPTHS[N_INST] = {1, 3, 16, 257};

// ---------------------------------------------------------------------------
// One FIFO instance: pointers into the Verilated model plus a reference model.
// ---------------------------------------------------------------------------
struct Inst {
    int depth = 0;
    uint8_t*  s_valid = nullptr;
    uint8_t*  s_ready = nullptr;
    uint8_t*  s_last  = nullptr;
    uint32_t* s_data  = nullptr;
    uint8_t*  m_valid = nullptr;
    uint8_t*  m_ready = nullptr;
    uint8_t*  m_last  = nullptr;
    uint32_t* m_data  = nullptr;

    std::queue<std::pair<uint32_t, bool>> ref;
    long pushed = 0;
    long popped = 0;
};

Inst make_inst(int depth, uint8_t* sv, uint8_t* sr, uint8_t* sl, uint32_t* sd,
               uint8_t* mv, uint8_t* mr, uint8_t* ml, uint32_t* md) {
    Inst i;
    i.depth   = depth;
    i.s_valid = sv;
    i.s_ready = sr;
    i.s_last  = sl;
    i.s_data  = sd;
    i.m_valid = mv;
    i.m_ready = mr;
    i.m_last  = ml;
    i.m_data  = md;
    return i;
}

void bind_instances(Vaxis_fifo_multi& dut, std::vector<Inst>& insts) {
    insts.clear();
    insts.push_back(make_inst(
        DEPTHS[0], &dut.s0_axis_tvalid, &dut.s0_axis_tready, &dut.s0_axis_tlast,
        &dut.s0_axis_tdata, &dut.m0_axis_tvalid, &dut.m0_axis_tready,
        &dut.m0_axis_tlast, &dut.m0_axis_tdata));
    insts.push_back(make_inst(
        DEPTHS[1], &dut.s1_axis_tvalid, &dut.s1_axis_tready, &dut.s1_axis_tlast,
        &dut.s1_axis_tdata, &dut.m1_axis_tvalid, &dut.m1_axis_tready,
        &dut.m1_axis_tlast, &dut.m1_axis_tdata));
    insts.push_back(make_inst(
        DEPTHS[2], &dut.s2_axis_tvalid, &dut.s2_axis_tready, &dut.s2_axis_tlast,
        &dut.s2_axis_tdata, &dut.m2_axis_tvalid, &dut.m2_axis_tready,
        &dut.m2_axis_tlast, &dut.m2_axis_tdata));
    insts.push_back(make_inst(
        DEPTHS[3], &dut.s3_axis_tvalid, &dut.s3_axis_tready, &dut.s3_axis_tlast,
        &dut.s3_axis_tdata, &dut.m3_axis_tvalid, &dut.m3_axis_tready,
        &dut.m3_axis_tlast, &dut.m3_axis_tdata));
}

// ---------------------------------------------------------------------------
// Directed-test harness bound to a single instance.
// ---------------------------------------------------------------------------
class FifoHarness {
public:
    Vaxis_fifo_multi& dut;
    std::vector<Inst>& insts;
    Inst& i;

    FifoHarness(Vaxis_fifo_multi& d, std::vector<Inst>& is, size_t idx)
        : dut(d), insts(is), i(is[idx]) {}

    void reset_all(int cycles = 2) {
        dut.clk = 0;
        dut.eval();
        dut.rst = 1;
        for (auto& x : insts) {
            *x.s_valid = 0;
            *x.s_data  = 0;
            *x.s_last  = 0;
            *x.m_ready = 0;
        }
        for (int c = 0; c < cycles; ++c) {
            dut.clk = 1;
            dut.eval();
            dut.clk = 0;
            dut.eval();
        }
        dut.rst = 0;
    }

    void set_input(bool svalid, uint32_t sdata, bool slast, bool mready) {
        *i.s_valid = svalid;
        *i.s_data  = sdata;
        *i.s_last  = slast;
        *i.m_ready = mready;
    }

    void cycle() {
        dut.clk = 1;
        dut.eval();
        dut.clk = 0;
        dut.eval();
    }

    bool m_valid() const { return *i.m_valid; }
    uint32_t m_data() const { return *i.m_data; }
    bool m_last() const { return *i.m_last; }
    bool s_ready() const { return *i.s_ready; }
};

// Fill to capacity, reject overflow, drain in order.
void fill_and_drain(FifoHarness& h, uint32_t base) {
    const int depth = h.i.depth;

    // Fill to capacity.
    for (int k = 0; k < depth; ++k) {
        h.set_input(true, base + static_cast<uint32_t>(k), false, false);
        h.cycle();
    }
    dbqa::check(h.s_ready() == false, "full: s_axis_tready deasserted");

    // A further push while full must be rejected (no transfer).
    h.set_input(true, 0xDEADBEEFu, false, false);
    h.cycle();
    dbqa::check(h.s_ready() == false, "full: overflow push rejected");

    // Drain in order; each pop advances the output by one beat.
    for (int k = 0; k < depth; ++k) {
        dbqa::check(h.m_valid() == true, "drain: output valid");
        dbqa::expect_eq("drain: data order", base + static_cast<uint32_t>(k),
                        h.m_data());
        h.set_input(false, 0, false, true);
        h.cycle();
    }
    dbqa::check(h.m_valid() == false, "drained: output invalid");
    dbqa::check(h.s_ready() == true, "drained: s_axis_tready reasserted");

    h.set_input(false, 0, false, true);
    h.cycle();
    dbqa::check(h.m_valid() == false, "drained: still invalid");
}

// ---------------------------------------------------------------------------
// Constrained-random scoreboard across all instances.
// ---------------------------------------------------------------------------
void random_phase(Vaxis_fifo_multi& dut, std::vector<Inst>& insts,
                  uint32_t seed, int cycles) {
    std::mt19937 rng(seed);
    uint32_t ctr = 1;

    for (int cyc = 0; cyc < cycles; ++cyc) {
        dut.clk = 0;
        dut.eval();
        const bool rst = (cyc < 2);
        dut.rst = rst;

        for (auto& x : insts) {
            const bool tready = *x.s_ready;   // settled, combinational on count
            const bool tvalid = *x.m_valid;   // settled, combinational on count

            const bool do_push = !rst && tready && (rng() % 4 != 0);
            const bool do_pop  = !rst && tvalid && (rng() % 2 == 0);

            uint32_t data = ctr++;
            if ((rng() % 64) == 0) data = (rng() % 2) ? 0u : 0xFFFFFFFFu;
            const bool last = (data % 11 == 0);

            *x.s_valid = do_push;
            *x.s_data  = data;
            *x.s_last  = last;
            *x.m_ready = do_pop;

            if (do_push) {
                x.ref.push({data, last});
                ++x.pushed;
            }
            if (do_pop) {
                const auto want = x.ref.front();
                x.ref.pop();
                dbqa::expect_eq("pop: data (ordered)", want.first, *x.m_data);
                dbqa::check((*x.m_last) == want.second, "pop: tlast (ordered)");
                ++x.popped;
            }
        }

        dut.clk = 1;
        dut.eval();

        for (auto& x : insts) {
            dbqa::check((*x.m_valid) == !x.ref.empty(),
                        "m_axis_tvalid matches occupancy");
            if (!x.ref.empty()) {
                dbqa::check((*x.m_data) == x.ref.front().first,
                            "out data == reference front");
                dbqa::check((*x.m_last) == x.ref.front().second,
                            "out tlast == reference front");
            }
            dbqa::check((*x.s_ready) == (x.ref.size() < (size_t)x.depth),
                        "s_axis_tready matches not-full");
        }
    }

    // Force-drain everything, checking order and conservation.
    for (int cyc = 0; cyc < 520; ++cyc) {
        dut.clk = 0;
        dut.eval();
        bool all_drained = true;
        for (auto& x : insts) {
            const bool tvalid = *x.m_valid;
            *x.s_valid = 0;
            *x.m_ready = tvalid;
            if (tvalid && !x.ref.empty()) {
                const auto want = x.ref.front();
                x.ref.pop();
                dbqa::expect_eq("drain: data (ordered)", want.first, *x.m_data);
                dbqa::check((*x.m_last) == want.second, "drain: tlast (ordered)");
                ++x.popped;
            }
            all_drained &= x.ref.empty();
        }
        dut.clk = 1;
        dut.eval();
        if (all_drained) break;
    }

    for (auto& x : insts) {
        dbqa::check(x.ref.empty(), "instance fully drained");
        dbqa::expect_eq("lossless: pushed == popped", x.pushed, x.popped);
    }
}

}  // namespace

int main() {
    Vaxis_fifo_multi dut;
    dut.clk = 0;
    dut.rst = 1;
    dut.eval();

    std::vector<Inst> insts;
    bind_instances(dut, insts);

    std::printf("DBQA axis_fifo testbench\n");

    // -----------------------------------------------------------------------
    // Directed: instance 2 (DEPTH = 16)
    // -----------------------------------------------------------------------
    {
        FifoHarness h(dut, insts, 2);
        h.reset_all();
        dbqa::check(h.m_valid() == false, "reset: output invalid");
        dbqa::check(h.s_ready() == true, "reset: input ready");

        // Single-beat fall-through.
        h.set_input(true, 0xAAAAu, true, false);
        h.cycle();
        dbqa::check(h.m_valid() == true, "fwft: output valid");
        dbqa::expect_eq("fwft: data", 0xAAAAu, h.m_data());
        dbqa::check(h.m_last() == true, "fwft: tlast");
        h.set_input(false, 0, false, true);
        h.cycle();
        dbqa::check(h.m_valid() == false, "fwft: drained");

        // Fill / drain with ordering.
        fill_and_drain(h, 0x1000u);

        // Output stability while backpressured.
        h.set_input(true, 0x55u, false, false);
        h.cycle();
        for (int c = 0; c < 5; ++c) {
            h.set_input(false, 0, false, false);
            h.cycle();
            dbqa::check(h.m_valid() && h.m_data() == 0x55u,
                        "stall: beat held stable");
        }
        h.set_input(false, 0, false, true);
        h.cycle();
        dbqa::check(h.m_valid() == false, "stall: released");

        // Simultaneous push + pop, including drain-and-refill in one cycle.
        h.set_input(true, 0x100u, false, false);
        h.cycle();
        h.set_input(true, 0x200u, false, false);
        h.cycle();
        dbqa::expect_eq("simul: head is A", 0x100u, h.m_data());
        h.set_input(true, 0x300u, false, true);
        h.cycle();
        dbqa::expect_eq("simul: pop A, head is B", 0x200u, h.m_data());
        h.set_input(true, 0x400u, false, true);
        h.cycle();
        dbqa::expect_eq("simul: pop B, head is C", 0x300u, h.m_data());
        h.set_input(false, 0, false, true);
        h.cycle();
        dbqa::expect_eq("simul: pop C, head is D", 0x400u, h.m_data());
        h.set_input(false, 0, false, true);
        h.cycle();
        dbqa::check(h.m_valid() == false, "simul: drained");

        // tlast ordering through the FIFO.
        const bool last_seq[4] = {false, false, true, false};
        h.set_input(true, 10u, false, false);
        h.cycle();
        h.set_input(true, 11u, false, false);
        h.cycle();
        h.set_input(true, 12u, true, false);
        h.cycle();
        h.set_input(true, 13u, false, false);
        h.cycle();
        for (int k = 0; k < 4; ++k) {
            dbqa::check(h.m_valid() == true, "tlast: output valid");
            dbqa::check(h.m_last() == last_seq[k], "tlast: ordering");
            h.set_input(false, 0, false, true);
            h.cycle();
        }
        dbqa::check(h.m_valid() == false, "tlast: drained");
    }

    // -----------------------------------------------------------------------
    // Directed fill/drain on every depth (1, 3, 16, 257)
    // -----------------------------------------------------------------------
    for (size_t idx = 0; idx < insts.size(); ++idx) {
        FifoHarness h(dut, insts, idx);
        h.reset_all();
        fill_and_drain(h, 0x2000u + static_cast<uint32_t>(idx) * 0x1000u);
    }

    // -----------------------------------------------------------------------
    // Constrained-random scoreboard on all instances.
    // -----------------------------------------------------------------------
    random_phase(dut, insts, 0xCAFEu, 6000);
    random_phase(dut, insts, 0x5EEDu, 6000);

    return dbqa::summary("tb_fifo");
}
