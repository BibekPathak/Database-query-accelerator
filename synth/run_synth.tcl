# ===========================================================================
#  run_synth.tcl -- non-project Vivado synthesis flow for the DBQA
#  accelerator (dbqa_top).
#
#  Target: AMD/Xilinx Artix-7 XC7A35T (xc7a35tcsg324-1).
#
#  Run from the repository root:
#      vivado -mode batch -source synth/run_synth.tcl -nojournal
#  or, when Vivado is on PATH:
#      make synth
#
#  The flow is a plain non-project run (no .xpr): read the SystemVerilog
#  sources in dependency order, synthesize, place, route, and emit
#  utilization / timing / power reports plus a routed checkpoint into
#  results/. It is intended to be run on a machine with Vivado installed
#  (not available on GitHub-hosted CI runners).
# ===========================================================================

set part xc7a35tcsg324-1
set top  dbqa_top
set out  results
file mkdir $out

# ---------------------------------------------------------------------------
# Sources in dependency order (package and interfaces before consumers).
# ---------------------------------------------------------------------------
read_verilog -sv \
    rtl/common/db_pkg.sv \
    rtl/interfaces/axis_fifo.sv \
    rtl/interfaces/axis_register.sv \
    rtl/memory/column_memory.sv \
    rtl/memory/column_reader.sv \
    rtl/operators/predicate_engine.sv \
    rtl/operators/projection_engine.sv \
    rtl/operators/count_engine.sv \
    rtl/operators/sum_engine.sv \
    rtl/operators/min_engine.sv \
    rtl/operators/max_engine.sv \
    rtl/operators/avg_engine.sv \
    rtl/operators/aggregation_top.sv \
    rtl/operators/groupby_engine.sv \
    rtl/scheduler/scheduler.sv \
    rtl/top/axi_lite_slave.sv \
    rtl/top/dbqa_top.sv

read_xdc synth/constraints.xdc

# ---------------------------------------------------------------------------
# Synthesis, placement, routing.
# ---------------------------------------------------------------------------
synth_design -top $top -part $part
opt_design
place_design
route_design

# ---------------------------------------------------------------------------
# Reports.
# ---------------------------------------------------------------------------
report_utilization -file $out/synth_utilization.rpt
report_timing_summary -file $out/synth_timing.rpt
report_power -file $out/synth_power.rpt

# Timed path summary (WNS) for the printed log.
set clk [get_clocks -quiet sys_clk]
if {$clk ne ""} {
    catch {
        set paths [get_timing_paths -max_paths 1 -nworst 1 -quiet]
        if {[llength $paths] > 0} {
            puts "SYNTH WNS: [get_property SLACK [lindex $paths 0]] ns"
        }
    }
}

write_checkpoint -force $out/dbqa_top_routed.dcp

# ---------------------------------------------------------------------------
# Summary CSV: key resource usage and timing (best-effort parse; "n/a" if a
# metric cannot be extracted from the report text).
# ---------------------------------------------------------------------------
proc extract_num {text pat} {
    if {[regexp $pat $text -> num]} { return $num }
    return "n/a"
}

set util [report_utilization -return_string]
set luts [extract_num $util {Slice LUTs[ \t]+([0-9]+)}]
set regs [extract_num $util {Register[ \t]+([0-9]+)}]
set bram [extract_num $util {Block RAM Tile[ \t]+([0-9]+)}]
set csv  [open $out/synth_summary.csv w]
puts $csv "metric,value"
puts $csv "part,$part"
puts $csv "top,$top"
puts $csv "slice_luts,$luts"
puts $csv "registers,$regs"
puts $csv "bram36,$bram"
puts $csv "target_clk_mhz,100"
if {[info exists wns]} { puts $csv "wns_ns,$wns" }
close $csv

puts "SYNTH OK: reports in $out/"
