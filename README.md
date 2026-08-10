# DBQA — FPGA Database Query Accelerator

A fully pipelined, configurable **SQL analytics accelerator** implemented in
SystemVerilog, with a Python control plane, a C++20 reference model, Verilator
verification, SymbiYosys formal proofs, and a Vivado synthesis flow targeting
the AMD/Xilinx Artix-7 XC7A35T.

The engine reads **columnar** memory, pushes predicates down to the first
pipeline stage, projects the requested columns, and streams them through
dedicated aggregation operators — all without software involvement after the
query is configured over AXI-Lite.

---

## Highlights

- **Columnar memory architecture** — BRAM-based banks, streaming address
  generators, parallel multi-column reads, configurable width / depth.
- **Streaming execution pipeline** — one independent operator per stage,
  connected by AXI-Stream `ready`/`valid` handshakes with backpressure.
- **Predicate pushdown** — 6 comparison operators (`==`, `!=`, `<`, `>`,
  `<=`, `>=`) composable with `AND` / `OR`.
- **Projection** — arbitrary column subsets driven by a select mask.
- **Aggregation** — `COUNT`, `SUM`, `MIN`, `MAX`, `AVG` as standalone
  pipelined engines with overflow detection.
- **GROUP BY** — optional hash-based bucketing in BRAM
  (`GROUP_BY_BUCKETS`, default 256).
- **AXI-Lite control plane** — documented register map: query type,
  predicates, column select, aggregation, start / status / result.
  See [`docs/register_map.md`](docs/register_map.md).
- **Python query compiler** — `query.select("salary").where("age",">",30)
  .sum().execute()` compiles to register writes; `avg = sum / count`.
- **Verification** — C++20 self-checking testbenches with scoreboards against
  a software reference model, constrained-random and stress tests.
- **Formal verification** — SymbiYosys properties on the FIFO, ready/valid
  protocol, counters, aggregators and register interface.
- **Synthesis** — Vivado flow producing utilization, timing, Fmax and power
  reports.

## Architecture

```
        CPU / Python Query Compiler
                    │
                    ▼
              AXI-Lite Control
                    │
  ┌─────────────────┴─────────────────┐
  │         Query Accelerator         │
  │  ┌─────────────────────────────┐  │
  │  │  AXI-Lite slave (registers) │  │
  │  └───────┬──────────┬──────────┘  │
  │          │ config   │ status      │
  │  ┌───────▼──────────▼──────────┐  │
  │  │       Query scheduler       │  │
  │  └───────┬─────────────────────┘  │
  │          │                        │
  │  Column Reader  ──► Predicate  ──►│
  │        │             │            │
  │        ▼             ▼            │
  │  Projection  ──► Aggregation ──►  │
  │        │             │            │
  │        ▼             ▼            │
  │  GROUP BY (opt) ──► Result Buffer │
  │                                    │
  └────────────────────────────────────┘
                    │
                    ▼
         AXI-Stream Result Output
```

Classic aggregation results (`COUNT`, `SUM`, `MIN`, `MAX`, `AVG`, overflow)
are read back over AXI-Lite; GROUP BY results stream out the AXI-Stream
master, one beat per group.

Columns are stored independently (`age`, `salary`, `department`, …) — never
row-wise. Each stage consumes and produces one element per cycle when not
backpressured.

## Repository layout

```
├── rtl/                  SystemVerilog RTL
│   ├── common/           shared package (types, enums, packed structs)
│   ├── interfaces/       AXI-Stream FIFO and pipeline registers
│   ├── memory/           column memory banks + column reader
│   ├── operators/        predicate / projection / count / sum / min / max /
│   │                     avg / group-by engines
│   ├── scheduler/        query execution FSM
│   └── top/              AXI-Lite register interface + pipeline top
├── sim/                  C++20 self-checking testbenches (Verilator + CTest)
├── scripts/              Python control plane (query compiler + AXI-Lite driver)
├── formal/               SymbiYosys property files and .sby scripts
├── synth/                Vivado Tcl synthesis flow for XC7A35T
├── docs/                 architecture, register map, verification and
│                         performance reports
├── results/              generated benchmark CSVs and reports (gitignored)
└── .github/workflows/    CI: format → lint → build → test → formal → synth
```

## Getting started

Requirements:

- **Verilator** ≥ 5.x
- **g++ / clang++** with C++20 support
- **CMake** ≥ 3.20
- Python ≥ 3.10 (control plane)
- Optional: SymbiYosys (formal), Vivado (synthesis)

Build and run every testbench:

```sh
make configure        # or: cmake -B build -DCMAKE_BUILD_TYPE=Release
make build            # or: cmake --build build
make sim              # or: ctest --test-dir build --output-on-failure
```

Run a single testbench (every module has its own self-checking TB):

```sh
make tb_top            # full accelerator over AXI-Lite
make tb_scheduler      # query execution core
make tb_axilite        # AXI-Lite register file
```

Lint all RTL:

```sh
make lint
```

Waveform capture is planned but not yet wired into the Verilator testbenches
(Phases 8/12); the `dbqa_test.hpp` trace gate (`DBQA_TRACE`) is reserved for
it.

## Engineering standards

SystemVerilog:

- IEEE 1800-2012, packages with `typedef struct packed` and `enum`
- parameterized, single-purpose, composable modules
- `always_ff` / `always_comb` only; no latches; registered outputs where
  practical
- ready/valid handshakes on every cross-module datapath
- SVA assertions on critical interfaces

Verification:

- every RTL module has a dedicated self-checking testbench
- golden reference model + scoreboard, directed + constrained-random + stress
- each testbench returns a CTest exit code; no manual inspection

## Status

Phases 0–7 are complete: the accelerator is a working, verified end-to-end
pipeline. The remaining phases add scale and tooling.

| Area                    | Status                                  |
| ----------------------- | --------------------------------------- |
| Repository skeleton     | ✅ Phase 0                              |
| Core RTL + primitives   | ✅ Phase 1 (db_pkg, AXI-Stream)         |
| Columnar memory         | ✅ Phase 2                              |
| Operators               | ✅ Phases 3–6                           |
| Scheduler + top         | ✅ Phase 7 (AXI-Lite, full queries)     |
| Random / perf TBs       | 🔜 Phase 8                              |
| Python control plane    | 🔜 Phase 9                              |
| Formal verification     | 🔜 Phase 10                             |
| Synthesis (XC7A35T)     | 🔜 Phase 11                             |
| Documentation           | 🔜 Phase 12                             |

Verilator testbenches run in CI: `make sim` builds and runs all ten TBs
(`tb_smoke`…`tb_top`) with a CTest exit code per test.

## License

MIT — see `LICENSE`.
