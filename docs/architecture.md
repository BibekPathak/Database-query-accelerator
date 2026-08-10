# DBQA Architecture

The DBQA accelerator is a fully pipelined, columnar SQL-analytics engine in
SystemVerilog. This document describes the datapath, the control plane, and
the key design decisions.

## Overview

```
        CPU / Python Query Compiler
                    │
                    ▼
              AXI-Lite Control
                    │
  ┌─────────────────┴─────────────────┐
  │         dbqa_top                  │
  │  ┌─────────────────────────────┐  │
  │  │  axi_lite_slave (registers) │  │
  │  └───────┬──────────┬──────────┘  │
  │          │ config   │ status      │
  │  ┌───────▼──────────▼──────────┐  │
  │  │        scheduler            │  │
  │  └───────┬─────────────────────┘  │
  │          │                        │
  │  Column Reader  ──► Predicate  ──►│
  │        │             │            │
  │        ▼             ▼            │
  │  Projection  ──► Aggregation ──►  │
  │        │             │            │
  │        ▼             ▼            │
  │  GROUP BY (opt) ──► Result Buffer │
  └────────────────────────────────────┘
                    │
                    ▼
         AXI-Stream Result Output
```

Classic aggregation results (`COUNT`, `SUM`, `MIN`, `MAX`, `AVG`, overflow)
are read back over AXI-Lite; GROUP BY results stream out the AXI-Stream
master, one beat per group.

## Columnar memory

Columns are stored **independently** — one BRAM bank per column — never
row-wise. `column_memory` is a true dual-port bank (load on port A, streaming
read on port B); `column_reader` scans every row, reading `NUM_COLS` banks in
parallel and emitting one `pipeline_data_t` beat per row:

```
tdata = { pass(1) , col[NUM_COLS-1] ... col[1] , col[0] }   // packed, MSB first
```

The banks use a 1-cycle synchronous read, so the reader sustains **1 row per
2 cycles** (a fetch-ahead design would overrun the single read slot on a
stall). Backpressure is absorbed by a small output FIFO.

### Default parameters (`db_pkg`)

| Parameter           | Value  | Meaning                        |
|---------------------|--------|--------------------------------|
| `COLUMN_WIDTH`      | 32     | bits per column datum          |
| `NUM_ROWS`          | 1024   | rows per table                 |
| `NUM_COLS`          | 4      | columns per table              |
| `NUM_PRED`          | 2      | predicate slots per query      |
| `GROUP_BY_BUCKETS`  | 256    | GROUP BY hash buckets          |
| `ACCUM_WIDTH`       | 42     | `COLUMN_WIDTH + log2(NUM_ROWS)`|

## Streaming pipeline

Every stage connects to the next over AXI-Stream `ready`/`valid` handshakes
with `tlast` marking the end of one query's stream. Beats flow through the
whole chain under normal backpressure.

1. **`column_reader`** — streams the table rows (see above).
2. **`predicate_engine`** — `NUM_PRED` comparator slots combined with
   `AND`/`OR`; the pass bit is set per row (WHERE clause). Six operators
   (`==`, `!=`, `<`, `>`, `<=`, `>=`).
3. **`projection_engine`** — zeroes unselected columns per `proj_mask`
   (SELECT list).
4. **`aggregation_top`** — the aggregation stage. Two modes, selected by
   `agg_cfg.groupby`:
   - **classic** (`groupby = 0`): routes the stream to COUNT / SUM / MIN /
     MAX / AVG engines in parallel; the opcode selects the presented result.
     SUM saturates at the 42-bit maximum with an overflow flag.
   - **GROUP BY** (`groupby = 1`): routes the stream to `groupby_engine`
     instead. Classic result ports are zeroed.
5. **`scheduler`** — the query-execution core. Chains the stages, owns the
   start/done/busy FSM, and latches classic results.

`axis_register` (a one-deep skid buffer) provides a registered pipeline stage
at the predicate and projection outputs; `axis_fifo` (FWFT) absorbs
backpressure at the reader output.

## GROUP BY engine

`groupby_engine` is a fixed-size hash table in BRAM:

- `GROUP_BY_BUCKETS` (256) buckets, each storing
  `{valid, key, count, sum, min, max}`.
- Hash = the key's low 8 bits; **linear probing** resolves collisions.
- If the probe chain is exhausted (all 256 buckets probed), the row is
  dropped — a documented policy for a fixed-size hash table.
- At `start` the engine **clears every bucket** (a 256-cycle sweep), then
  aggregates each row (read-modify-write through a shared issue/wait/process
  pipeline), then dumps valid groups in bucket order as one AXIS beat each,
  with `tlast` on the final group.

Every access costs a few cycles (2-cycle BRAM read + write), so a GROUP BY
query is slower than a classic scan — and the cost grows sharply once the
number of distinct keys exceeds `GROUP_BY_BUCKETS` (see
`docs/performance.md`).

## Query execution (control flow)

1. **Load** the table over AXI-Lite: write `REG_LOAD_ADDR`, the per-column
   `REG_LOAD_DATA0..3`, then `REG_LOAD_ROW` to commit one row. (Must happen
   while idle.)
2. **Configure**: predicate slots, `REG_PROJ_MASK`, `REG_AGG_CFG`,
   `REG_QUERY` (`num_rows`; 0 = full table).
3. **Start**: write `REG_CTRL` bit 0. The scheduler asserts `start` on the
   reader and aggregation stage together.
4. **Poll** `REG_STATUS` until `done`.
5. **Collect**: classic results from `REG_RESULT`/`REG_COUNT`/`REG_OVERFLOW`
   (AVG = `sum / count` in software); GROUP BY groups from the AXI-Stream
   master.

The `num_rows` limiter truncates the *forwarded* stream (after `num_rows`
rows it forces `tlast` and swallows the rest while the reader drains) — it
does **not** make the scan faster, because the reader always walks all
`NUM_ROWS` rows.

Abort (`REG_CTRL` bit 1) pulses a synchronous reset of the scheduler core,
cancelling an in-flight query while keeping the register file and loaded
table intact.

## Module map

```
rtl/common/db_pkg.sv        types, parameters, register map (single source of truth)
rtl/interfaces/axis_fifo.sv      FWFT AXI-Stream FIFO with count
rtl/interfaces/axis_register.sv  one-deep skid-buffer pipeline stage
rtl/memory/column_memory.sv      true dual-port BRAM column bank
rtl/memory/column_reader.sv      streaming columnar reader
rtl/operators/predicate_engine.sv   WHERE comparator slots
rtl/operators/projection_engine.sv  SELECT mask
rtl/operators/count_engine.sv       COUNT sink
rtl/operators/sum_engine.sv         SUM sink (saturating)
rtl/operators/min_engine.sv         MIN sink
rtl/operators/max_engine.sv         MAX sink
rtl/operators/avg_engine.sv         AVG (SUM + COUNT)
rtl/operators/aggregation_top.sv    classic vs GROUP BY selector
rtl/operators/groupby_engine.sv     hash-based GROUP BY
rtl/scheduler/scheduler.sv          query execution core + row limiter
rtl/top/axi_lite_slave.sv           AXI-Lite register file
rtl/top/dbqa_top.sv                 accelerator top level
```

## Design decisions worth knowing

- **`tlast` from day one** — every stream carries end-of-query, which the
  aggregation engines latch on; the scheduler relies on it to detect
  completion.
- **Saturating, not wrapping, SUM** — keeps the result defined and provable
  (the 42-bit accumulator can hold the worst case at default sizes, so
  overflow is a safety net).
- **Classic results over AXI-Lite, GROUP BY over AXIS** — chosen to keep the
  register file small while giving the group stream a natural, lossless
  transport.
- **Bounded memory** — the whole design uses fixed BRAM (table banks, GROUP BY
  buckets); there is no unbounded buffering, so resource usage is predictable
  (see `docs/synthesis.md`).
