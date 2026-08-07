// ===========================================================================
//  column_memory.sv -- true dual-port column bank (BRAM).
//
//  One column of the columnar store. Storage is a single always_ff RAM array
//  with two independent ports; each port has one address bus plus a write
//  enable (standard dual-port BRAM template, e.g. Xilinx UG901). This maps
//  to a single 7-series BRAM. Separate read and write addresses per port are
//  intentionally NOT supported: a BRAM port has a single address, so the
//  writer and reader of a column must use different ports (or the same port
//  in non-overlapping phases).
//
//  Port roles used by the accelerator:
//    * port A -- data load (write), e.g. from the control plane
//    * port B -- streaming read (used by the column reader)
//
//  Semantics (standard synchronous BRAM):
//    * synchronous read: rdata reflects mem[addr] one cycle after the clock
//    * read-during-write: reading an address written on the same port in the
//      same cycle returns the OLD value (non-blocking update)
//    * dual-port write collision: writing the same address on both ports in
//      one cycle is undefined -- the design must not do that
//
//  No reset: BRAM storage is not reset at power-on / sync reset; control
//  state that needs resetting lives in the surrounding modules.
//
//  Assertions (SVA) are compiled only when DBQA_ASSERT is defined.
// ===========================================================================

module column_memory #(
    parameter int DATA_W = 32,
    parameter int DEPTH  = 1024
) (
    input logic clk,

    // Port A (load)
    input  logic              a_wen,
    input  logic [ADDR_W-1:0] a_addr,
    input  logic [DATA_W-1:0] a_wdata,
    output logic [DATA_W-1:0] a_rdata,

    // Port B (stream)
    input  logic              b_wen,
    input  logic [ADDR_W-1:0] b_addr,
    input  logic [DATA_W-1:0] b_wdata,
    output logic [DATA_W-1:0] b_rdata
);

  localparam int ADDR_W = (DEPTH <= 1) ? 1 : $clog2(DEPTH);

  initial begin
    if (DATA_W < 1) $error("column_memory: DATA_W must be >= 1");
    if (DEPTH < 1) $error("column_memory: DEPTH must be >= 1");
  end

  logic [DATA_W-1:0] mem[0:DEPTH-1];

  // Port A: write + synchronous read.
  always_ff @(posedge clk) begin
    if (a_wen) mem[a_addr] <= a_wdata;
    a_rdata <= mem[a_addr];
  end

  // Port B: write + synchronous read.
  always_ff @(posedge clk) begin
    if (b_wen) mem[b_addr] <= b_wdata;
    b_rdata <= mem[b_addr];
  end

`ifdef DBQA_ASSERT
  // Addresses must stay within the physical depth.
  a_addr_in_range :
  assert property (@(posedge clk) (a_addr <= ADDR_W'(DEPTH - 1)));
  a_waddr_in_range :
  assert property (@(posedge clk) (a_wen |-> (a_addr <= ADDR_W'(DEPTH - 1))));
  b_addr_in_range :
  assert property (@(posedge clk) (b_addr <= ADDR_W'(DEPTH - 1)));
  b_waddr_in_range :
  assert property (@(posedge clk) (b_wen |-> (b_addr <= ADDR_W'(DEPTH - 1))));
`endif

endmodule
