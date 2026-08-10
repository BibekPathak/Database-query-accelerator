// ===========================================================================
//  axis_register_formal.sv -- SymbiYosys formal wrapper for axis_register.
//
//  Bounded model check of the one-deep skid-buffer pipeline stage:
//    * at most one beat is ever in flight (input beats minus output beats is
//      always 0 or 1)
//    * a beat held in the skid register is presented unchanged on the output
//
//  Like the FIFO wrapper, all properties are plain current-cycle booleans;
//  history is captured with shadow registers and compared against the DUT.
// ===========================================================================

module axis_register_formal #(
    parameter int DATA_W = 8
) (
    input logic clk,
    input  logic              s_axis_tvalid,
    input  logic [DATA_W-1:0] s_axis_tdata,
    input  logic              s_axis_tlast,
    input  logic              m_axis_tready
);

  // Deterministic two-cycle reset.
  logic rst;
  logic [1:0] rst_s;
  initial rst_s = 2'b11;
  always @(posedge clk) rst_s <= {1'b0, rst_s[1]};
  assign rst = rst_s[1];

  logic              s_axis_tready;
  logic              m_axis_tvalid;
  logic [DATA_W-1:0] m_axis_tdata;
  logic              m_axis_tlast;

  axis_register #(
      .DATA_W(DATA_W)
  ) u_reg (
      .clk          (clk),
      .rst          (rst),
      .s_axis_tvalid(s_axis_tvalid),
      .s_axis_tready(s_axis_tready),
      .s_axis_tdata (s_axis_tdata),
      .s_axis_tlast (s_axis_tlast),
      .m_axis_tvalid(m_axis_tvalid),
      .m_axis_tready(m_axis_tready),
      .m_axis_tdata (m_axis_tdata),
      .m_axis_tlast (m_axis_tlast)
  );

  // Inputs are free, but the source is quiet during reset.
  assume property (!rst || !s_axis_tvalid);

  // Accepted input beats and accepted output beats (shadow counters).
  logic push, pop;
  assign push = s_axis_tvalid & s_axis_tready;
  assign pop  = m_axis_tvalid & m_axis_tready;

  logic [3:0] in_count;
  logic [3:0] out_count;
  always @(posedge clk) begin
    if (rst) begin
      in_count  <= '0;
      out_count <= '0;
    end else begin
      if (push) in_count <= in_count + 1'b1;
      if (pop) out_count <= out_count + 1'b1;
    end
  end

  // Conservation: at most one beat in flight (0 or 1 un-emitted beats).
  assert property (rst || (in_count == out_count) || (in_count == out_count + 1'b1));

  // -------------------------------------------------------------------------
  // Held-beat shadow: a beat accepted while the downstream stalls is latched
  // and must be presented unchanged on the output.
  // -------------------------------------------------------------------------
  logic [DATA_W:0] held;
  logic            held_valid;
  always @(posedge clk) begin
    if (rst) begin
      held       <= '0;
      held_valid <= 1'b0;
    end else if (push) begin
      held       <= {s_axis_tlast, s_axis_tdata};
      held_valid <= ~m_axis_tready;  // stored only when the drain stalls
    end else if (pop) begin
      held_valid <= 1'b0;
    end
  end

  // When a beat is held, the output presents that exact beat.
  assert property (rst || !held_valid
      || (m_axis_tvalid && m_axis_tdata == held[DATA_W-1:0] && m_axis_tlast == held[DATA_W]));

endmodule
