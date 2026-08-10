# DBQA AXI-Lite Register Map

The accelerator is controlled exclusively over a 32-bit AXI-Lite slave
interface. This document is the contract between the register file RTL
(`rtl/top/axi_lite_slave.sv`), the query scheduler (`rtl/scheduler/`) and the
software control plane (Python query compiler).

Offsets are **32-bit word offsets**. The byte address on the AXI-Lite bus is
`offset << 2`. The authoritative source for every constant below is
`rtl/common/db_pkg.sv` (`REG_*`, `CTRL_*`, `STATUS_*`); do not hardcode the
numbers elsewhere.

## Register summary

| Word offset | Name            | Access | Description                                  |
|------------:|-----------------|:------:|----------------------------------------------|
| `0x00`      | `REG_CTRL`      | W      | `[0]` start, `[1]` abort (1-cycle pulses)    |
| `0x01`      | `REG_STATUS`    | R      | `[0]` busy, `[1]` done, `[3:2]` error        |
| `0x02`      | `REG_QUERY`     | W/R    | `query_cfg_t` (num_rows)                     |
| `0x03`      | `REG_AGG_CFG`   | W/R    | `agg_cfg_t` (op / groupby / column / key)    |
| `0x04`      | `REG_PROJ_MASK` | W/R    | `proj_mask_t` (bit *c* = project column *c*) |
| `0x08..0x0B`| `REG_PRED_BASE` | W/R    | 2 predicate slots, 2 words each              |
| `0x20`      | `REG_LOAD_ADDR` | W/R    | row address to load                          |
| `0x21..0x24`| `REG_LOAD_DATA0`| W/R    | `REG_LOAD_DATA0 + c` = column *c* datum      |
| `0x25`      | `REG_LOAD_ROW`  | W      | commit strobe: pulses `load_wen`             |
| `0x30`      | `REG_RESULT`    | R      | aggregation result, bits `[31:0]`            |
| `0x31`      | `REG_RESULT_HI` | R      | aggregation result, bits `[ACCUM_W-1:32]`    |
| `0x32`      | `REG_COUNT`     | R      | aggregate row count, bits `[31:0]`           |
| `0x33`      | `REG_COUNT_HI`  | R      | aggregate row count, bits `[ACCUM_W-1:32]`   |
| `0x34`      | `REG_OVERFLOW`  | R      | `[0]` SUM saturation                         |

Write-only registers (CTRL, LOAD_ROW) respond with OKAY but have no read
value; unmapped reads return `0`.

## Control register (`REG_CTRL`)

| Bit | Name     | Semantics                                             |
|----:|----------|-------------------------------------------------------|
| `0` | start    | Pulses the scheduler to run the configured query.     |
| `1` | abort    | Cancels the in-flight query (resets the scheduler core only). |

The strobes are single-cycle registered pulses: write the register, and one
cycle later the scheduler samples `start` with the already-latched
configuration. Abort leaves the register file and the loaded table intact.

## Status register (`REG_STATUS`)

| Bit  | Name  | Semantics                                                   |
|-----:|-------|-------------------------------------------------------------|
| `0`  | busy  | A query is running.                                         |
| `1`  | done  | The last query completed; results are valid.                |
| `3:2`| error | `db_pkg::error_e`: `0` none, `1` start-while-busy, `2` SUM overflow. |

`busy` and `done` are mutually exclusive. The control plane writes `start`
(possibly after clearing `done` by starting the next query), polls `STATUS`
until `done` is set, then reads `RESULT`/`COUNT`.

## Query configuration

### `REG_QUERY` — `query_cfg_t`

| Bits     | Field     | Semantics                                              |
|---------:|-----------|--------------------------------------------------------|
| `9:0`    | num_rows  | Rows to scan from row 0; `0` = the full table (1024).  |

### `REG_AGG_CFG` — `agg_cfg_t`

| Bits     | Field   | Semantics                                                    |
|---------:|---------|--------------------------------------------------------------|
| `23:21`  | op      | `1` COUNT, `2` SUM, `3` MIN, `4` MAX, `5` AVG.               |
| `20`     | groupby | `0` classic engine, `1` GROUP BY engine.                     |
| `19:10`  | column  | Column to aggregate.                                         |
| `9:0`    | gby_key | Column hashed as the GROUP BY key (when `groupby = 1`).      |

AVG is reported as its SUM; the software computes `avg = sum / count`.

### `REG_PROJ_MASK` — `proj_mask_t`

Bit *c* set selects (projects) column *c*; unselected columns are zeroed in
the stream.

### Predicate slots — `REG_PRED_BASE`

Each of the `NUM_PRED` (default 2) slots occupies two words. Slot *i*:

| Word offset                 | Contents                                        |
|-----------------------------|-------------------------------------------------|
| `REG_PRED_BASE + 2*i`       | Low word: `imm` (32-bit comparison operand).    |
| `REG_PRED_BASE + 2*i + 1`   | High word: `{enable, op, combine, column}`.     |

High-word bit layout (`pred_cfg_t` bits `[46:32]`):

| Bits     | Field   | Semantics                                            |
|---------:|---------|------------------------------------------------------|
| `14`     | enable  | `1` activates the slot.                              |
| `13:11`  | op      | `0` EQ, `1` NEQ, `2` LT, `3` GT, `4` LTE, `5` GTE.   |
| `10`     | combine | `0` AND, `1` OR (fold with the previous active slot).|
| `9:0`    | column  | Column under test.                                   |

Enabled slots combine left to right: the first enabled slot seeds the
predicate result (its `combine` is ignored); every later enabled slot folds in
with its `combine`. This expresses `(p0) AND (p1) OR (p2)` with
`enable = {1,1,1}`, `combine = {-, AND, OR}`.

## Table load

Rows are loaded one at a time, one whole row per commit:

1. Write `REG_LOAD_ADDR` = row index.
2. Write `REG_LOAD_DATA0 + c` = the datum for each column *c*.
3. Write `REG_LOAD_ROW` (any value) to pulse `load_wen` for one cycle.

The load port writes all `NUM_COLS` column banks at `REG_LOAD_ADDR`
simultaneously. Loading must happen while the accelerator is idle.

## Example: `SELECT SUM(salary) WHERE age >= 30`

Assume `NUM_COLS = 4`, `COLUMN_ADDR_W = 10`, columns are
`(id, age, salary, extra)`.

```
# Configure the predicate: slot 0 enabled, op = GTE (5), column 1, imm = 30
write REG_PRED_BASE     = 30                 # imm
write REG_PRED_BASE + 1 = (1 << 14) | (5 << 11) | (1)   # enable, op, column
write REG_PRED_BASE + 2 = 0                  # slot 1 disabled
write REG_PRED_BASE + 3 = 0
write REG_PROJ_MASK     = 0b1111             # keep all columns
write REG_AGG_CFG       = (2 << 21) | (2 << 10)   # op = SUM, column = 2
write REG_QUERY         = 1024               # scan the whole table

write REG_CTRL          = 1                  # start
loop: read REG_STATUS until bit 1 (done) is set
read REG_RESULT and REG_RESULT_HI            # SUM(salary) = (hi << 32) | lo
read REG_COUNT  and REG_COUNT_HI             # number of passing rows
read REG_OVERFLOW                            # SUM saturation
```

## Example: `SELECT id, SUM(salary) ... GROUP BY id`

```
write REG_AGG_CFG       = (2 << 21) | (1 << 20) | (2 << 10) | (0)
                          # op = SUM, groupby = 1, column = 2, gby_key = 0
write REG_QUERY         = 1024
write REG_CTRL          = 1                  # start

# Groups stream out the AXI-Stream master, one beat per group:
#   tdata = {key, count, sum, min, max}, tlast on the final group.
loop: accept m_axis until tlast, then read REG_STATUS for done
```

In GROUP BY mode `REG_RESULT`/`REG_COUNT`/`REG_OVERFLOW` read `0`.
