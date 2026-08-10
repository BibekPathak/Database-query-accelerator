# DBQA Verification

The design is verified at three levels, all self-checking with a CTest/pytest
exit code (no manual inspection):

1. **C++20/Verilator simulation testbenches** (`sim/`) — directed,
   constrained-random, stress and performance, checked against software
   reference models.
2. **Python control plane tests** (`scripts/tests/`) — register packing,
   compiler golden sequences, and end-to-end queries against the Verilated
   model.
3. **SymbiYosys formal proofs** (`formal/`) — bounded model checks of the
   core protocol modules.

## Running everything

```sh
make lint               # Verilator --lint-only over all RTL
make sim                # build + run all 12 CTest testbenches
make python-test        # build axil-server, then pytest
make formal             # SymbiYosys proofs (SBY=yowasp-sby make formal locally)
make synth              # Vivado flow (skips cleanly without Vivado)
make format-check       # Verible formatting gate
```

## Simulation testbenches

Every module has a dedicated TB. Most use a software reference model; the
top-level and stress TBs drive the accelerator entirely over AXI-Lite.

| Testbench          | DUT / scope                                          | Highlights                                  |
|--------------------|------------------------------------------------------|---------------------------------------------|
| `tb_smoke`         | toolchain smoke (no RTL)                             | C++20 + CTest plumbing                      |
| `tb_fifo`          | `axis_fifo`                                          | FWFT, count scoreboard (226k checks)        |
| `tb_reader`        | `column_reader` + banks                              | lossless scan, memory stress                |
| `tb_predicate`     | `predicate_engine`                                   | all ops, AND/OR combining                   |
| `tb_projection`    | `projection_engine`                                  | select masks, zeroing                       |
| `tb_aggregation`   | `aggregation_top`                                    | every opcode, overflow, GROUP BY routing    |
| `tb_groupby`       | `groupby_engine`                                     | collisions, folding, drop policy, buckets   |
| `tb_scheduler`     | `scheduler`                                          | full queries, truncation, errors            |
| `tb_axilite`       | `axi_lite_slave`                                     | register map, backpressure, round-trips     |
| `tb_top`           | `dbqa_top`                                           | end-to-end, backpressure torture            |
| `tb_stress`        | `dbqa_top`                                           | 240-query soak (state-leak checks)          |
| `tb_perf`          | `dbqa_top`                                           | cycle-counted workloads → `results/perf.csv`|

Key techniques:

- **Full-table-aware references** (`sim/dbqa_reference.hpp`): loaded rows sit
  at indices 0..n-1 of the 1024-row table and unloaded rows read back as
  zero, exactly matching the RTL.
- **Backpressure torture**: AXI-Lite `bready`/`rready` and the GROUP BY AXIS
  `tready` are randomly gated; results must still match (handshakes hold).
- **Soak**: hundreds of randomized classic + GROUP BY queries in one session
  catch cross-query state leaks.

## Python control plane tests

`scripts/dbqa/` (register map + query compiler) is tested by pytest
(`make python-test`, 20 tests):

- `test_regs.py` — packing helpers match the documented bit layouts.
- `test_query.py` — `Query.execute()` produces the exact golden register-write
  sequences from `docs/register_map.md`.
- `test_e2e.py` — real queries against the Verilated model via the
  `axil_server` stdio harness, verified against `scripts/dbqa/reference.py`
  (skipped automatically if `axil-server` hasn't been built).

## Formal verification

`make formal` runs SymbiYosys bounded model checks (`mode bmc`, `depth 24`,
`smtbmc z3`) from `formal/*.sby`:

| Proof             | Properties proven                                        |
|-------------------|-----------------------------------------------------------|
| `axis_fifo`       | occupancy conservation; `count ∈ [0, DEPTH]`; no push-into-full / pop-from-empty |
| `axis_register`   | ≤ 1 beat in flight; a held beat is presented unchanged     |
| `count_engine`    | latched result equals an independent pass-count shadow at completion |
| `sum_engine`      | latched result and overflow match a saturating shadow at completion |

All properties are plain current-cycle booleans checked against independent
shadow models, so they run on both the native yosys frontend (CI) and the
WASM builds. A deliberately-broken FIFO is caught by the `axis_fifo` proof,
confirming the properties are non-vacuous.

**Toolchain note — why formal uses a Yosys-compatible abstraction.** The
production RTL is verified with Verilator. For formal verification, a
Yosys-compatible abstraction of selected modules is maintained because the
installed yosys SystemVerilog frontend does not support several constructs
used by the production RTL (`$bits()`, package `import`, `@(posedge clk)`,
`$past`). Specifically:

- `formal/db_pkg_formal.sv` — a replica of `db_pkg` with the `$bits()`
  width hardcoded.
- `formal/count_engine_f.sv` / `formal/sum_engine_f.sv` — copies of the two
  engines with the package `import` made explicit via qualified references.

These files mirror the RTL (see their headers) and must be kept in sync; the
abstraction is limited to the modules under formal proof, and every other RTL
module is verified solely by Verilator simulation.

## What is verified where

| Behavior                       | Simulation | Formal |
|--------------------------------|:----------:|:------:|
| FIFO occupancy / protocol      | tb_fifo    | axis_fifo |
| Skid-register ready/valid      | tb_predicate/projection | axis_register |
| COUNT/SUM accumulation         | tb_aggregation + tb_top | count/sum_engine |
| Predicate/projection semantics | tb_predicate/tb_projection | — |
| GROUP BY hashing/collisions    | tb_groupby | — |
| Scheduler FSM / truncation     | tb_scheduler | — |
| AXI-Lite register interface    | tb_axilite + tb_top | — (SVA in RTL) |
| End-to-end queries             | tb_top, tb_stress | — |
| Performance                    | tb_perf | — |
