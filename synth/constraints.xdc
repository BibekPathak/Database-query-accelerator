# ===========================================================================
#  constraints.xdc -- timing constraints for the DBQA accelerator.
#
#  The accelerator exposes one clocked domain (all synchronous logic is
#  clocked by `clk`) and is controlled over AXI-Lite. These are minimal
#  constraints sufficient for a timing-closure sanity check; adjust the clock
#  frequency and tighten I/O timing for the real board interface.
# ===========================================================================

# 100 MHz system clock.
create_clock -period 10.000 -name sys_clk [get_ports clk]

# Keep the asynchronous reset/control inputs lightly constrained.
set_input_delay -clock sys_clk 3.0 [get_ports rst]
set_input_delay -clock sys_clk 3.0 [all_inputs -filter {NAME != clk}]
set_output_delay -clock sys_clk 3.0 [all_outputs]
