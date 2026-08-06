// ===========================================================================
//  axis_fifo_multi.sv -- test-only wrapper around axis_fifo.
//
//  Instantiates the FIFO at four DEPTHs so tb_fifo can exercise distributed-
//  RAM edges, block-RAM sizing, and non-power-of-two depths from a single
//  Verilated model.
//
//  Instance map:
//    * u_d1   -> s0_* / m0_*   DEPTH = 1
//    * u_d3   -> s1_* / m1_*   DEPTH = 3
//    * u_d16  -> s2_* / m2_*   DEPTH = 16
//    * u_d257 -> s3_* / m3_*   DEPTH = 257
// ===========================================================================

module axis_fifo_multi #(
    parameter int DATA_W = 32
) (
    input logic clk,
    input logic rst,

    // Instance 0: DEPTH = 1
    input  logic              s0_axis_tvalid,
    output logic              s0_axis_tready,
    input  logic [DATA_W-1:0] s0_axis_tdata,
    input  logic              s0_axis_tlast,
    output logic              m0_axis_tvalid,
    input  logic              m0_axis_tready,
    output logic [DATA_W-1:0] m0_axis_tdata,
    output logic              m0_axis_tlast,

    // Instance 1: DEPTH = 3
    input  logic              s1_axis_tvalid,
    output logic              s1_axis_tready,
    input  logic [DATA_W-1:0] s1_axis_tdata,
    input  logic              s1_axis_tlast,
    output logic              m1_axis_tvalid,
    input  logic              m1_axis_tready,
    output logic [DATA_W-1:0] m1_axis_tdata,
    output logic              m1_axis_tlast,

    // Instance 2: DEPTH = 16
    input  logic              s2_axis_tvalid,
    output logic              s2_axis_tready,
    input  logic [DATA_W-1:0] s2_axis_tdata,
    input  logic              s2_axis_tlast,
    output logic              m2_axis_tvalid,
    input  logic              m2_axis_tready,
    output logic [DATA_W-1:0] m2_axis_tdata,
    output logic              m2_axis_tlast,

    // Instance 3: DEPTH = 257
    input  logic              s3_axis_tvalid,
    output logic              s3_axis_tready,
    input  logic [DATA_W-1:0] s3_axis_tdata,
    input  logic              s3_axis_tlast,
    output logic              m3_axis_tvalid,
    input  logic              m3_axis_tready,
    output logic [DATA_W-1:0] m3_axis_tdata,
    output logic              m3_axis_tlast
);

  initial begin
    if (DATA_W < 1) $error("axis_fifo_multi: DATA_W must be >= 1");
  end

  axis_fifo #(
      .DEPTH (1),
      .DATA_W(DATA_W)
  ) u_d1 (
      .clk(clk),
      .rst(rst),
      .s_axis_tvalid(s0_axis_tvalid),
      .s_axis_tready(s0_axis_tready),
      .s_axis_tdata(s0_axis_tdata),
      .s_axis_tlast(s0_axis_tlast),
      .m_axis_tvalid(m0_axis_tvalid),
      .m_axis_tready(m0_axis_tready),
      .m_axis_tdata(m0_axis_tdata),
      .m_axis_tlast(m0_axis_tlast)
  );

  axis_fifo #(
      .DEPTH (3),
      .DATA_W(DATA_W)
  ) u_d3 (
      .clk(clk),
      .rst(rst),
      .s_axis_tvalid(s1_axis_tvalid),
      .s_axis_tready(s1_axis_tready),
      .s_axis_tdata(s1_axis_tdata),
      .s_axis_tlast(s1_axis_tlast),
      .m_axis_tvalid(m1_axis_tvalid),
      .m_axis_tready(m1_axis_tready),
      .m_axis_tdata(m1_axis_tdata),
      .m_axis_tlast(m1_axis_tlast)
  );

  axis_fifo #(
      .DEPTH (16),
      .DATA_W(DATA_W)
  ) u_d16 (
      .clk(clk),
      .rst(rst),
      .s_axis_tvalid(s2_axis_tvalid),
      .s_axis_tready(s2_axis_tready),
      .s_axis_tdata(s2_axis_tdata),
      .s_axis_tlast(s2_axis_tlast),
      .m_axis_tvalid(m2_axis_tvalid),
      .m_axis_tready(m2_axis_tready),
      .m_axis_tdata(m2_axis_tdata),
      .m_axis_tlast(m2_axis_tlast)
  );

  axis_fifo #(
      .DEPTH (257),
      .DATA_W(DATA_W)
  ) u_d257 (
      .clk(clk),
      .rst(rst),
      .s_axis_tvalid(s3_axis_tvalid),
      .s_axis_tready(s3_axis_tready),
      .s_axis_tdata(s3_axis_tdata),
      .s_axis_tlast(s3_axis_tlast),
      .m_axis_tvalid(m3_axis_tvalid),
      .m_axis_tready(m3_axis_tready),
      .m_axis_tdata(m3_axis_tdata),
      .m_axis_tlast(m3_axis_tlast)
  );

endmodule
