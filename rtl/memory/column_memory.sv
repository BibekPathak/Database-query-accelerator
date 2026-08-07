// ===========================================================================
//  column_memory.sv -- true dual-port column bank (BRAM).
//
//  One column of the columnar store. Storage is a single always_ff RAM array;
//  both ports are independently writable and readable in the same cycle
//  (true dual-port), which maps directly to a Xilinx 7-series BRAM.
//
//  Port roles used by the accelerator:
//    * port A -- write (data load) and read (used by the column reader)
//    * port B -- write (alternative load / GROUP BY update) and read
//
//  Semantics (standard synchronous BRAM):
//    * synchronous read: rdata reflects mem[raddr] one cycle after the clock
//    * read-during-write: reading an address written in the same cycle
//      returns the OLD value (non-blocking update)
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

    // Port A
    input  logic              a_wen,
    input  logic [ADDR_W-1:0] a_waddr,
    input  logic [DATA_W-1:0] a_wdata,
    input  logic [ADDR_W-1:0] a_raddr,
    output logic [DATA_W-1:0] a_rdata,

    // Port B
    input  logic              b_wen,
    input  logic [ADDR_W-1:0] b_waddr,
    input  logic [DATA_W-1:0] b_wdata,
    input  logic [ADDR_W-1:0] b_raddr,
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
    if (a_wen) mem[a_waddr] <= a_wdata;
    a_rdata <= mem[a_raddr];
  end

  // Port B: write + synchronous read.
  always_ff @(posedge clk) begin
    if (b_wen) mem[b_waddr] <= b_wdata;
    b_rdata <= mem[b_raddr];
  end

`ifdef DBQA_ASSERT
  // Addresses must stay within the physical depth.
  a_addr_in_range :
  assert property (@(posedge clk) (a_raddr <= ADDR_W'(DEPTH - 1)));
  a_waddr_in_range :
  assert property (@(posedge clk) (a_wen |-> (a_waddr <= ADDR_W'(DEPTH - 1))));
  b_addr_in_range :
  assert property (@(posedge clk) (b_raddr <= ADDR_W'(DEPTH - 1)));
  b_waddr_in_range :
  assert property (@(posedge clk) (b_wen |-> (b_waddr <= ADDR_W'(DEPTH - 1))));
`endif

endmodule
