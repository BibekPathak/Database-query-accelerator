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
| SUM, first 256 rows   |  256 |   2052 |      0.125 |          12.5 M  |
| SUM, first 1024 rows  | 1024 |   2052 |      0.499 |          49.9 M  |
| GROUP BY, 13 keys     | 1024 |   5396 |      0.190 |          19.0 M  |
| GROUP BY, 1024 keys   | 1024 | 131076 |      0.008 |           0.78 M |

`rows/s @ 100 MHz` assumes the 100 MHz clock from `synth/constraints.xdc`;
scale linearly with the achieved Fmax.

## Two findings worth understanding

### 1. The reader always walks the whole table (~0.5 rows/cycle)

`SUM first 256 rows` and `SUM full` take the **same** 2052 cycles. `num_rows`
truncates the stream that reaches the aggregation (fewer rows are forwarded)
but the reader still scans all 1024 rows — the truncated tail is swallowed,
not skipped. This is a deliberate simplicity/safety trade-off (the reader has
no stop-row address). If per-query scan ranges matter, a bounded "scan until
row N" reader would cut latency for short queries at the cost of extra
control logic.

Classic scans run at the reader's intrinsic rate of **1 row / 2 cycles**
(0.5 rows/cycle) because of the 1-cycle BRAM read latency. The 2052-cycle
baseline is ~2048 scan cycles plus pipeline fill/drain.

### 2. GROUP BY degrades sharply above 256 distinct keys

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

This is the expected cost of a bounded hash table. Mitigations, in order of
effort:

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
