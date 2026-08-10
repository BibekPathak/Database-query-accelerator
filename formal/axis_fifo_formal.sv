// ===========================================================================
//  axis_fifo_formal.sv -- SymbiYosys formal wrapper for axis_fifo.
//
//  Bounded model check of the FIFO's occupancy accounting and ready/valid
//  protocol. All properties are plain current-cycle booleans (the yosys SVA
//  subset used here rejects clocking/`$past`), so transition behavior is
//  checked with an independent shadow model of the occupancy counter:
//    * count matches an independently-computed count (conservation)
//    * occupancy stays within [0, DEPTH]
//    * no push into a full FIFO, no pop from an empty FIFO
//
//  A deterministic two-cycle reset is generated internally (seeded by an
//  `initial` block, honored by prep without -nordff), and the input sides are
//  constrained quiet during reset so no handshake happens while the FIFO is
//  being cleared.
// ===========================================================================

module axis_fifo_formal #(
    parameter int DEPTH  = 4,
    parameter int DATA_W = 8
) (
    input logic clk,
    input  logic              s_axis_tvalid,
    input  logic [DATA_W-1:0] s_axis_tdata,
    input  logic              s_axis_tlast,
    input  logic              m_axis_tready
);

  localparam int COUNT_W = (DEPTH <= 1) ? 1 : $clog2(DEPTH + 1);

  // Deterministic reset: asserted for the first two cycles, then released.
  logic rst;
  logic [1:0] rst_s;
  initial rst_s = 2'b11;
  always @(posedge clk) rst_s <= {1'b0, rst_s[1]};
  assign rst = rst_s[1];

  logic              s_axis_tready;
  logic              m_axis_tvalid;
  logic [DATA_W-1:0] m_axis_tdata;
  logic              m_axis_tlast;
  logic [COUNT_W-1:0] count;

  axis_fifo #(
      .DEPTH (DEPTH),
      .DATA_W(DATA_W)
  ) u_fifo (
      .clk          (clk),
      .rst          (rst),
      .s_axis_tvalid(s_axis_tvalid),
      .s_axis_tready(s_axis_tready),
      .s_axis_tdata (s_axis_tdata),
      .s_axis_tlast (s_axis_tlast),
      .m_axis_tvalid(m_axis_tvalid),
      .m_axis_tready(m_axis_tready),
      .m_axis_tdata (m_axis_tdata),
      .m_axis_tlast (m_axis_tlast),
      .count        (count)
  );

  // Inputs are free, but the source/drain are quiet during reset so no
  // handshake happens while the FIFO is being cleared.
  assume property (!rst || !s_axis_tvalid);
  assume property (!rst || !m_axis_tready);

  // Recomputed handshake events (identical to the DUT's internal signals).
  logic push, pop;
  assign push = s_axis_tvalid & s_axis_tready;
  assign pop  = m_axis_tvalid & m_axis_tready;

  // -------------------------------------------------------------------------
  // Shadow occupancy model: the obvious independent update
  //     count' = count + push - pop
  // The DUT's count must always match it (its push&&pop special case is
  // equivalent, so a divergence indicates a bug in the DUT's accounting).
  // -------------------------------------------------------------------------
  logic [COUNT_W:0] count_shadow;
  always @(posedge clk) begin
    if (rst) begin
      count_shadow <= '0;
    end else begin
      count_shadow <= count_shadow + (push ? 1'b1 : 1'b0) - (pop ? 1'b1 : 1'b0);
    end
  end

  // Conservation: the DUT occupancy tracks pushes minus pops.
  assert property (rst || (count == count_shadow));

  // Occupancy never exceeds the physical depth.
  assert property (rst || (count <= DEPTH));

  // Never push into a full FIFO.
  assert property (rst || !(push && (count == DEPTH)));

  // Never pop from an empty FIFO.
  assert property (rst || !(pop && (count == '0)));

endmodule
