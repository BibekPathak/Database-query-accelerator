// ===========================================================================
//  dbqa_reference.hpp -- full-table-aware software reference model shared by
//  the top-level, stress and performance testbenches.
//
//  Models the accelerator exactly as the RTL sees it: rows loaded for a test
//  sit at indices 0..n-1 of a 1024-row table and every remaining row reads
//  back as zero. num_rows = 0 scans the whole table; otherwise only the first
//  num_rows rows are considered. Aggregation and GROUP BY follow the RTL
//  semantics (SUM saturates at the 42-bit maximum).
// ===========================================================================

#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <vector>

#include "dbqa_test.hpp"

namespace dbqa_ref {

constexpr uint64_t NUM_ROWS = 1024;
constexpr uint64_t SUM_MAX = 0x3FFFFFFFFFFull;  // 42-bit all-ones

struct Row {
  uint32_t cols[4];
};

struct AggRef {
  uint64_t count;
  uint64_t sum;
  uint32_t mn;  // 0xFFFFFFFF if no row passed
  uint32_t mx;  // 0 if no row passed
};

// Verilator port types differ (CData/SData/IData/QData); cast to a common
// type for comparison.
template <typename T>
void expect_eq64(const char* what, uint64_t e, T g) {
  dbqa::expect_eq<uint64_t>(what, e, static_cast<uint64_t>(g));
}

inline bool pred_pass(int op, uint32_t a, uint32_t b) {
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

inline const Row& row_at(const std::vector<Row>& rows, uint64_t i) {
  static const Row zero = Row{{0, 0, 0, 0}};
  return (i < rows.size()) ? rows[i] : zero;
}

// Classic aggregation over the first num_rows rows (0 = whole table),
// filtering with an optional predicate.
inline AggRef reference(const std::vector<Row>& rows, uint64_t num_rows,
                        int agg_col, bool pred_en, int pred_op, uint32_t imm,
                        int pred_col) {
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

// GROUP BY over the same view: per-key count/sum/min/max.
inline std::map<uint32_t, AggRef> groupby_ref(const std::vector<Row>& rows,
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

}  // namespace dbqa_ref
