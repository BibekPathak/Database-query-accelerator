// ===========================================================================
//  column_reader_tb_top.sv -- test-only wrapper around column_reader.
//
//  Instantiates the reader at the production configuration plus several edge
//  sizes so tb_reader can exercise the default datapath and the table-size
//  boundaries from a single Verilated model.
//
//  Instance map:
//    * u0 -> default     NUM_COLS=4 COLUMN_WIDTH=32 NUM_ROWS=1024
//    * u1 -> edge        NUM_COLS=2 COLUMN_WIDTH=16 NUM_ROWS=1
//    * u2 -> edge        NUM_COLS=2 COLUMN_WIDTH=16 NUM_ROWS=3
//    * u3 -> edge        NUM_COLS=2 COLUMN_WIDTH=16 NUM_ROWS=16
//    * u4 -> edge        NUM_COLS=2 COLUMN_WIDTH=16 NUM_ROWS=257
// ===========================================================================

module column_reader_tb_top (
    input logic clk,
    input logic rst,

    // Instance 0: default config (4 x 32-bit, 1024 rows)
    input  logic         s0_load_wen,
    input  logic [  9:0] s0_load_addr,
    input  logic [ 31:0] s0_load_data  [4],
    input  logic         s0_start,
    input  logic [  9:0] s0_scan_bound,
    output logic         m0_busy,
    output logic         m0_done,
    output logic         m0_axis_tvalid,
    input  logic         m0_axis_tready,
    output logic [128:0] m0_axis_tdata,
    output logic         m0_axis_tlast,

    // Instance 1: NUM_ROWS = 1
    input  logic        s1_load_wen,
    input  logic [ 0:0] s1_load_addr,
    input  logic [15:0] s1_load_data  [2],
    input  logic        s1_start,
    input  logic [ 0:0] s1_scan_bound,
    output logic        m1_busy,
    output logic        m1_done,
    output logic        m1_axis_tvalid,
    input  logic        m1_axis_tready,
    output logic [32:0] m1_axis_tdata,
    output logic        m1_axis_tlast,

    // Instance 2: NUM_ROWS = 3
    input  logic        s2_load_wen,
    input  logic [ 1:0] s2_load_addr,
    input  logic [15:0] s2_load_data  [2],
    input  logic        s2_start,
    input  logic [ 1:0] s2_scan_bound,
    output logic        m2_busy,
    output logic        m2_done,
    output logic        m2_axis_tvalid,
    input  logic        m2_axis_tready,
    output logic [32:0] m2_axis_tdata,
    output logic        m2_axis_tlast,

    // Instance 3: NUM_ROWS = 16
    input  logic        s3_load_wen,
    input  logic [ 3:0] s3_load_addr,
    input  logic [15:0] s3_load_data  [2],
    input  logic        s3_start,
    input  logic [ 3:0] s3_scan_bound,
    output logic        m3_busy,
    output logic        m3_done,
    output logic        m3_axis_tvalid,
    input  logic        m3_axis_tready,
    output logic [32:0] m3_axis_tdata,
    output logic        m3_axis_tlast,

    // Instance 4: NUM_ROWS = 257
    input  logic        s4_load_wen,
    input  logic [ 8:0] s4_load_addr,
    input  logic [15:0] s4_load_data  [2],
    input  logic        s4_start,
    input  logic [ 8:0] s4_scan_bound,
    output logic        m4_busy,
    output logic        m4_done,
    output logic        m4_axis_tvalid,
    input  logic        m4_axis_tready,
    output logic [32:0] m4_axis_tdata,
    output logic        m4_axis_tlast
);

  column_reader #(
      .NUM_COLS(4),
      .COLUMN_WIDTH(32),
      .NUM_ROWS(1024)
  ) u0 (
      .clk(clk),
      .rst(rst),
      .load_wen(s0_load_wen),
      .load_addr(s0_load_addr),
      .load_data(s0_load_data),
      .start(s0_start),
      .scan_bound(s0_scan_bound),
      .busy(m0_busy),
      .done(m0_done),
      .m_axis_tvalid(m0_axis_tvalid),
      .m_axis_tready(m0_axis_tready),
      .m_axis_tdata(m0_axis_tdata),
      .m_axis_tlast(m0_axis_tlast)
  );

  column_reader #(
      .NUM_COLS(2),
      .COLUMN_WIDTH(16),
      .NUM_ROWS(1)
  ) u1 (
      .clk(clk),
      .rst(rst),
      .load_wen(s1_load_wen),
      .load_addr(s1_load_addr),
      .load_data(s1_load_data),
      .start(s1_start),
      .scan_bound(s1_scan_bound),
      .busy(m1_busy),
      .done(m1_done),
      .m_axis_tvalid(m1_axis_tvalid),
      .m_axis_tready(m1_axis_tready),
      .m_axis_tdata(m1_axis_tdata),
      .m_axis_tlast(m1_axis_tlast)
  );

  column_reader #(
      .NUM_COLS(2),
      .COLUMN_WIDTH(16),
      .NUM_ROWS(3)
  ) u2 (
      .clk(clk),
      .rst(rst),
      .load_wen(s2_load_wen),
      .load_addr(s2_load_addr),
      .load_data(s2_load_data),
      .start(s2_start),
      .scan_bound(s2_scan_bound),
      .busy(m2_busy),
      .done(m2_done),
      .m_axis_tvalid(m2_axis_tvalid),
      .m_axis_tready(m2_axis_tready),
      .m_axis_tdata(m2_axis_tdata),
      .m_axis_tlast(m2_axis_tlast)
  );

  column_reader #(
      .NUM_COLS(2),
      .COLUMN_WIDTH(16),
      .NUM_ROWS(16)
  ) u3 (
      .clk(clk),
      .rst(rst),
      .load_wen(s3_load_wen),
      .load_addr(s3_load_addr),
      .load_data(s3_load_data),
      .start(s3_start),
      .scan_bound(s3_scan_bound),
      .busy(m3_busy),
      .done(m3_done),
      .m_axis_tvalid(m3_axis_tvalid),
      .m_axis_tready(m3_axis_tready),
      .m_axis_tdata(m3_axis_tdata),
      .m_axis_tlast(m3_axis_tlast)
  );

  column_reader #(
      .NUM_COLS(2),
      .COLUMN_WIDTH(16),
      .NUM_ROWS(257)
  ) u4 (
      .clk(clk),
      .rst(rst),
      .load_wen(s4_load_wen),
      .load_addr(s4_load_addr),
      .load_data(s4_load_data),
      .start(s4_start),
      .scan_bound(s4_scan_bound),
      .busy(m4_busy),
      .done(m4_done),
      .m_axis_tvalid(m4_axis_tvalid),
      .m_axis_tready(m4_axis_tready),
      .m_axis_tdata(m4_axis_tdata),
      .m_axis_tlast(m4_axis_tlast)
  );

endmodule
