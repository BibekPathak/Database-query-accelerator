# DBQA Synthesis Flow (Artix-7 XC7A35T)

The accelerator is synthesized with a Vivado **non-project** batch flow (no
`.xpr`), targeting the AMD/Xilinx Artix-7 XC7A35T. The flow and constraints
live in `synth/`; the actual run requires a machine with Vivado installed
(it is not available on GitHub-hosted CI runners).

## Running

```sh
make synth                     # if vivado is on PATH
# or explicitly:
vivado -mode batch -source synth/run_synth.tcl -nojournal \
       -log results/synth_vivado.log
```

`make synth` degrades gracefully when Vivado is missing: it prints a note and
exits 0. The design's elaboration is already gated locally by `make lint`
(which flattens `dbqa_top` and would flag unsynthesizable constructs), so a
clean `make lint` is the no-Vivado "the design synthesizes" check.

## What the flow does

1. Reads every SystemVerilog source in dependency order
   (`db_pkg` → interfaces → memory → operators → scheduler → `dbqa_top`).
2. Reads `synth/constraints.xdc` (100 MHz system clock, light I/O timing).
3. Runs `synth_design` / `opt_design` / `place_design` / `route_design` for
   `dbqa_top` on `xc7a35tcsg324-1`.
4. Emits reports and a routed checkpoint into `results/`:
   - `synth_utilization.rpt` — LUTs, FFs, BRAM, DSP, Slices
   - `synth_timing.rpt` — WNS/TNS, Fmax estimate
   - `synth_power.rpt`
   - `synth_summary.csv` — key metrics in machine-readable form
   - `dbqa_top_routed.dcp` — routed design checkpoint

`results/` is gitignored and uploaded by CI when the job runs.

## Expected resource picture (estimation)

The design is small for a 35T (≈20.8K LUTs, 50 × 36Kb BRAM):

- **Columnar memory** — 4 banks × 1024 × 32-bit (32Kb each) → 4 BRAMs.
- **GROUP BY hash table** — 256 buckets × ~180-bit entries (~46Kb) → 1–2 BRAMs.
- **Datapath logic** — the streaming engines are mostly LUT/FF; the whole
  accelerator should fit comfortably with a large margin. The 100 MHz clock is
  conservative for this design size.

These are estimates pending an actual run.

## Target metrics to report after a Vivado run

Fill these in from the `results/` reports once `make synth` has run on a
Xilinx host (`synth_utilization.rpt`, `synth_timing.rpt`, `synth_power.rpt`,
`synth_utilization_hier.rpt`):

| Metric         | Value                        | Source                                |
|----------------|------------------------------|---------------------------------------|
| Device         | XC7A35T-1CPG236              | `synth_summary.csv` (part)            |
| LUTs           | —                            | `synth_utilization.rpt`               |
| FFs            | —                            | `synth_utilization.rpt`               |
| BRAMs          | —                            | `synth_utilization.rpt`               |
| DSPs           | —                            | `synth_utilization.rpt`               |
| Fmax           | —                            | `synth_timing.rpt` (1/WNS + period)   |
| WNS            | —                            | `synth_timing.rpt`                    |
| TNS            | —                            | `synth_timing.rpt`                    |
| Power          | —                            | `synth_power.rpt`                     |

Per-module resource breakdown (from `synth_utilization_hier.rpt`):

| Module                  | LUT | FF | BRAM | Fmax |
|-------------------------|----:|---:|-----:|-----:|
| column_memory ×4       |     |    |      |      |
| predicate_engine        |     |    |      |      |
| projection_engine       |     |    |      |      |
| aggregation_top         |     |    |      |      |
| groupby_engine          |     |    |      |      |
| scheduler               |     |    |      |      |
| axi_lite_slave          |     |    |      |      |

Once populated, update `docs/performance.md` with the achieved Fmax and
recompute the `rows/s @ 100 MHz` figures.

## Interpreting the reports

- **Utilization**: compare LUT/FF/BRAM against the 35T totals; watch for BRAM
  fragmentation in the group-by table if many small memories are inferred.
- **Timing**: WNS ≥ 0 at 100 MHz is the closure target; if WNS < 0, the
  critical path is typically in the group-by probe chain (see the performance
  notes in `docs/performance.md`).
- **Power**: the design is idle-dominated (no dynamic workload during a query
  beyond the streaming scan); expect low static power on the 35T.

## Changing the target

Edit `synth/run_synth.tcl` (`set part ...`) for a different Artix-7 part and
adjust the clock in `synth/constraints.xdc`. The RTL is fully parameterized
(`NUM_ROWS`, `GROUP_BY_BUCKETS`, `NUM_COLS`, …) from `db_pkg`, so resource
usage scales with those parameters.
