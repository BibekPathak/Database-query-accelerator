// ===========================================================================
//  tb_groupby.cpp -- self-checking testbench for groupby_engine.
//
//  Drives a pipeline_data_t stream through the group-by engine and verifies
//  the {key, count, sum, min, max} groups streamed out against a C++ model of
//  the same hash table: 256 BRAM buckets, hash = XOR-fold of the key bytes,
//  linear probing on collision, and the documented drop-on-probe-exhaustion
//  policy. SUM saturates at the ACCUM_W-bit maximum.
//
//  Coverage:
//    * single-row, multi-row folding (count/sum/min/max accumulation)
//    * hash collisions resolved by linear probing
//    * empty table and all-fail tables (no groups)
//    * done/busy/tready/tlast handshakes
//    * constrained-random keys (colliding keyset + random), values and passes
//
//  Requires: rtl/common/db_pkg.sv and rtl/operators/groupby_engine.sv
// ===========================================================================

#include <array>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "Vgroupby_engine.h"
#include "dbqa_test.hpp"
#include "dbqa_trace.hpp"

namespace {

constexpr int BUCKETS = 256;
constexpr uint64_t SUM_MAX = 0x3FFFFFFFFFFull;  // 42-bit all-ones

struct Group {
  uint64_t count;
  uint64_t sum;
  uint32_t mn;  // 0xFFFFFFFF if the group has no row (unused here)
  uint32_t mx;
};

struct Slot {
  bool valid;
  uint32_t key;
  Group g;
};

struct Row {
  uint32_t key;
  uint32_t value;
  bool pass;
};

// Must match groupby_engine.hash_key: XOR-fold the four key bytes into the
// 8-bit bucket address.
uint32_t hash(uint32_t key) {
  return (key & 0xFFu) ^ ((key >> 8) & 0xFFu) ^ ((key >> 16) & 0xFFu) ^
         ((key >> 24) & 0xFFu);
}

// RTL-faithful hash table: 256 slots, hash = XOR-fold of the key bytes,
// linear probing, drop a row when the probe chain is exhausted (all 256
// buckets probed).
struct RefTable {
  std::array<Slot, BUCKETS> slots;

  void reset() {
    for (auto& s : slots) s.valid = false;
  }

  void insert(uint32_t key, uint32_t v) {
    const int h = static_cast<int>(hash(key));
    for (int p = 0; p < BUCKETS; ++p) {
      const int idx = (h + p) & (BUCKETS - 1);
      if (!slots[idx].valid) {
        slots[idx] = Slot{true, key, Group{1, v, v, v}};
        return;
      }
      if (slots[idx].key == key) {
        Group& g = slots[idx].g;
        g.count += 1;
        if (g.sum + v > SUM_MAX) {
          g.sum = SUM_MAX;
        } else {
          g.sum += v;
        }
        if (v < g.mn) g.mn = v;
        if (v > g.mx) g.mx = v;
        return;
      }
    }
    // Probe chain exhausted: drop this row (documented policy).
  }
};

uint32_t bit_of(const Vgroupby_engine& dut, int b) {
  const int word = b / 32;
  const int off = b % 32;
  return (dut.m_axis_tdata[word] >> off) & 1u;
}

uint64_t bits_of(const Vgroupby_engine& dut, int hi, int lo) {
  uint64_t r = 0;
  for (int b = hi; b >= lo; --b) r = (r << 1) | bit_of(dut, b);
  return r;
}

// Push the whole table, drain the groups, and verify them against the
// reference. Returns the number of groups decoded (for extra checks).
int run_query(Vgroupby_engine& dut, auto& tick, const std::vector<Row>& rows,
              int key_col, int value_col) {
  RefTable ref;
  ref.reset();
  for (const Row& r : rows)
    if (r.pass) ref.insert(r.key, r.value);

  dut.key_col = static_cast<uint32_t>(key_col);
  dut.value_col = static_cast<uint32_t>(value_col);
  dut.start = 1;
  tick();
  dut.start = 0;
  dbqa::check(dut.busy, "busy asserted after start");

  size_t pushed = 0;
  for (int cyc = 0; cyc < 16 * static_cast<int>(rows.size()) + 512 &&
                       pushed < rows.size();
       ++cyc) {
    dut.s_axis_tvalid = 1;
    for (int c = 0; c < 4; ++c) dut.s_axis_tdata[c] = 0;
    dut.s_axis_tdata[key_col] = rows[pushed].key;
    dut.s_axis_tdata[value_col] = rows[pushed].value;
    dut.s_axis_tdata[4] = rows[pushed].pass ? 1u : 0u;
    dut.s_axis_tlast = (pushed == rows.size() - 1);
    dut.eval();
    if (dut.s_axis_tready) ++pushed;
    tick();
  }
  dbqa::check(pushed == rows.size(), "all rows accepted");
  dut.s_axis_tvalid = 0;

  std::vector<Group> got;
  std::vector<uint32_t> got_keys;
  bool first_tlast = false;
  int ngroups = 0;
  for (int cyc = 0; cyc < 16 * static_cast<int>(rows.size()) + 4096 &&
                       !dut.done;
       ++cyc) {
    dut.eval();
    if (dut.m_axis_tvalid) {
      Group g;
      g.count = bits_of(dut, 147, 106);
      g.sum = bits_of(dut, 105, 64);
      g.mn = static_cast<uint32_t>(bits_of(dut, 63, 32));
      g.mx = static_cast<uint32_t>(bits_of(dut, 31, 0));
      got.push_back(g);
      got_keys.push_back(static_cast<uint32_t>(bits_of(dut, 179, 148)));
      if (ngroups == 0) first_tlast = dut.m_axis_tlast;
      ++ngroups;
    }
    tick();
  }
  dbqa::check(dut.done, "done asserted");
  dbqa::check(!dut.busy, "busy deasserted after done");
  dbqa::check(!dut.s_axis_tready, "input no longer accepted after done");

  // tlast marks the final group: for a single group it is the first, and for
  // multiple groups it must not be the first.
  dbqa::check(first_tlast == (ngroups == 1),
              "tlast only on the final group");

  // The RTL dumps groups in bucket order; the reference slots do too.
  int nvalid = 0;
  for (const Slot& s : ref.slots) nvalid += s.valid ? 1 : 0;
  dbqa::expect_eq("number of groups", nvalid, ngroups);

  int gi = 0;
  for (const Slot& s : ref.slots) {
    if (!s.valid) continue;
    dbqa::expect_eq("group key", s.key, got_keys[gi]);
    dbqa::expect_eq("group count", s.g.count, got[gi].count);
    dbqa::expect_eq("group sum", s.g.sum, got[gi].sum);
    dbqa::expect_eq("group min", s.g.mn, got[gi].mn);
    dbqa::expect_eq("group max", s.g.mx, got[gi].mx);
    ++gi;
  }

  return ngroups;
}

std::vector<Row> make_rows(size_t n, uint32_t seed, bool all_pass,
                           bool all_keys_collide) {
  std::mt19937 rng(seed);
  std::vector<Row> rows(n);
  const uint32_t keyset[4] = {0, 0x01010101, 0x100, 7};  // 0 and 0x01010101 collide
  for (auto& r : rows) {
    r.key = all_keys_collide
                ? (rng() % 2 == 0 ? 0u : 0x01010101u)  // both fold to bucket 0
                : (rng() % 3 == 0 ? (uint32_t)rng() : keyset[rng() % 4]);
    r.value = rng();
    r.pass = all_pass ? true : (rng() % 2) != 0;
  }
  return rows;
}

}  // namespace

int main() {
  Vgroupby_engine dut;
  dut.clk = 0;
  dut.rst = 1;
  dut.start = 0;
  dut.key_col = 0;
  dut.value_col = 1;
  dut.s_axis_tvalid = 0;
  dut.s_axis_tlast = 0;
  dut.m_axis_tready = 1;
  for (int i = 0; i < 5; ++i) dut.s_axis_tdata[i] = 0;
  dut.eval();

  vluint64_t trace_cycle = 0;
  auto tfp = dbqa::init_trace(dut, "results/tb_groupby.fst");
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

  std::printf("DBQA group-by testbench\n");

  // -------------------------------------------------------------------------
  // Directed: single row, collision probing, folding, empty and all-fail.
  // -------------------------------------------------------------------------
  {
    std::vector<Row> rows = {{5, 42, true}};
    dbqa::check(run_query(dut, tick, rows, 0, 1) == 1,
                "single row: one group");
  }

  {
    // keys 0 and 0x01010101 both fold to bucket 0 -> linear probe to bucket 1.
    std::vector<Row> rows = {{0, 5, true}, {0x01010101, 10, true}, {0, 15, true}};
    dbqa::check(run_query(dut, tick, rows, 0, 1) == 2,
                "collision: two groups");
  }

  {
    // Folding: same key, count/sum/min/max accumulate.
    std::vector<Row> rows = {{7, 3, true}, {7, 9, true}, {7, 3, true}};
    dbqa::check(run_query(dut, tick, rows, 0, 1) == 1,
                "folding: one group");
  }

  {
    // Empty table: a single failing row carrying tlast.
    std::vector<Row> rows = {{0, 0, false}};
    dbqa::check(run_query(dut, tick, rows, 0, 1) == 0, "empty table: no groups");
  }

  {
    // All rows fail, multiple rows.
    std::vector<Row> rows = {{1, 2, false}, {2, 3, false}, {3, 4, false}};
    dbqa::check(run_query(dut, tick, rows, 0, 1) == 0,
                "all-fail table: no groups");
  }

  // -------------------------------------------------------------------------
  // Constrained-random: size, keyset, pass pattern and column selection.
  // -------------------------------------------------------------------------
  std::mt19937 rng(0x6B77u);
  for (int run = 0; run < 40; ++run) {
    const int n = 1 + static_cast<int>(rng() % 180);
    const bool all_pass = (run % 8) == 0;
    const bool collide = (run % 5) == 0;
    auto rows = make_rows(n, 0x700 + run, all_pass, collide);
    run_query(dut, tick, rows, 0, 1);
    run_query(dut, tick, rows, 0, 2);  // aggregate a different value column
  }
  std::printf("  constrained-random complete\n");

  return dbqa::summary("tb_groupby");
}
