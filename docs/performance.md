# DBQA Performance

Cycle-accurate measurements come from the `tb_perf` harness (`make tb_perf`),
which drives `dbqa_top` over AXI-Lite, counts clock cycles from the `start`
write to the first `done` status read, and writes `results/perf.csv` for fixed
workloads over a full 1024-row table.

## Measured workloads

| Workload              | Rows | Cycles | Rows/cycle | ~Rows/s @ 100 MHz |
|-----------------------|-----:|-------:|-----------:|------------------:|
| COUNT, full table     | 1024 |   2052 |      0.499 |          49.9 M  |
| SUM, full table       | 1024 |   2052 |      0.499 |          49.9 M  |
| SUM, first 64 rows    |   64 |    132 |      0.485 |          48.5 M  |
| SUM, first 256 rows   |  256 |    516 |      0.496 |          49.6 M  |
| SUM, first 512 rows   |  512 |   1028 |      0.498 |          49.8 M  |
| SUM, first 1024 rows  | 1024 |   2052 |      0.499 |          49.9 M  |
| GROUP BY, 13 keys     | 1024 |   5396 |      0.190 |          19.0 M  |
| GROUP BY, 1024 keys   | 1024 | 131076 |      0.008 |           0.78 M |

`rows/s @ 100 MHz` assumes the 100 MHz clock from `synth/constraints.xdc`;
scale linearly with the achieved Fmax.

Classic scans run at the reader's intrinsic rate of **1 row / 2 cycles**
(0.5 rows/cycle) because of the 1-cycle BRAM read latency: the 2052-cycle
full-scan baseline is ~2048 scan cycles plus pipeline fill/drain, and the
short-scan workloads (132 / 516 / 1028 cycles for 64 / 256 / 512 rows) scale
linearly with the requested scan size.

## Why a bounded scan was needed (a resolved trade-off)

**Before** the reader was parameterized, *every* query cost 2052 cycles: the
column reader always walked all 1024 rows, and `num_rows` only truncated the
stream that reached the aggregation (the tail was swallowed, not skipped).
`SUM first 256 rows` and `SUM full` therefore took the **same** time.

That is a good question to be asked in an interview: *"Why doesn't `LIMIT 256`
reduce your execution time?"* The honest answer is that the original reader
was a fixed 1024-row scan — `num_rows` limited the *output semantics* but not
the *memory traversal*.

**After** the fix, the reader takes a `scan_bound` (0 = full table) and stops
the BRAM traversal at row `scan_bound-1`, carrying `tlast` on that row. Query
latency now scales with the requested scan:

| Scan size | Before (cycles) | After (cycles) |
|----------:|----------------:|---------------:|
|        64 |            2052 |             132 |
|       256 |            2052 |             516 |
|       512 |            2052 |            1028 |
|      1024 |            2052 |            2052 |

This changed the reader (`scan_bound` port, early termination + `tlast`/`done`
at the bound) and removed the scheduler's now-redundant row limiter, which
existed only to truncate a stream the reader used to always over-produce.

## GROUP BY degrades sharply above 256 distinct keys

- 13 distinct keys: 5396 cycles (~0.19 rows/cycle).
- 1024 distinct keys: 131076 cycles (~0.008 rows/cycle) — **24× slower**.

The GROUP BY engine is a fixed-size hash table with `GROUP_BY_BUCKETS = 256`
buckets and linear probing. Hash = the key's low 8 bits, so with more than 256
distinct keys the table overfills:

- Every key whose low byte collides with occupied buckets walks a longer probe
  chain (each probe is a 2-cycle BRAM read plus a process step).
- Once all 256 buckets are occupied, remaining keys probe the whole table
  before the documented **drop-on-exhaustion** policy discards them — the
  worst case is ~256 probes per dropped row.

**Bounded-accelerator policy:** GROUP BY supports up to 256 resident groups
per query; additional groups are rejected when the hash table is exhausted.
This is a deliberate trade-off for a fixed-resource BRAM hash table — the
accelerator never silently overflows into unbounded memory.

Mitigations, in order of effort:

1. Increase `GROUP_BY_BUCKETS` (BRAM usage grows accordingly — see
   `docs/synthesis.md`).
2. Use a better hash than the low byte to spread keys (e.g., XOR-fold the key
   into the bucket address).
3. Switch to a two-pass / external sort-based GROUP BY for large key counts.

## How to reproduce

```sh
make tb_perf          # prints the table and writes results/perf.csv
```

## Planned additions

Once the Vivado flow has run (`make synth` on a Xilinx host), fold the
reported Fmax and utilization into this document and update the
`rows/s @ 100 MHz` figures for the achieved frequency.
