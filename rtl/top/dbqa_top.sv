// ===========================================================================
//  dbqa_top.sv -- DBQA accelerator top level.
//
//  Instantiates the AXI-Lite register file and the query scheduler, wiring
//  the control plane to the execution core:
//
//    CPU / Python                    dbqa_top
//    -----------                     --------
//    AXI-Lite writes  ---------------> axi_lite_slave --query_cfg/agg_cfg/
//                                      pred_cfg/proj_mask/load/start--> scheduler
//    AXI-Lite reads   <--------------- axi_lite_slave <-status/result/count/
//                                      overflow
//    GROUP BY result stream  <-------- scheduler g_axis --> m_axis master
//
//  The GROUP BY result is the only streaming output; classic aggregation
//  results are read back over AXI-Lite. `abort_req` pulses a synchronous
//  reset of the scheduler (cancelling an in-flight query) while leaving the
//  register file intact.
//
//  Assertions (SVA) are compiled only when DBQA_ASSERT is defined.
// ===========================================================================

module dbqa_top #(
    parameter int NUM_PRED   = db_pkg::NUM_PRED,
    parameter int NUM_COLS   = db_pkg::NUM_COLS,
    parameter int ACCUM_W    = db_pkg::ACCUM_WIDTH,
    parameter int COL_ADDR_W = db_pkg::COLUMN_ADDR_W,
    parameter int COLUMN_W   = db_pkg::COLUMN_WIDTH,
    parameter int DATA_W     = db_pkg::AXIS_DATA_W
) (
    input logic clk,
    input logic rst,

    // AXI-Lite slave (control plane)
    input  logic        s_axil_awvalid,
    output logic        s_axil_awready,
    input  logic [31:0] s_axil_awaddr,
    input  logic        s_axil_wvalid,
    output logic        s_axil_wready,
    input  logic [31:0] s_axil_wdata,
    input  logic [ 3:0] s_axil_wstrb,
    output logic        s_axil_bvalid,
    input  logic        s_axil_bready,
    output logic [ 1:0] s_axil_bresp,
    input  logic        s_axil_arvalid,
    output logic        s_axil_arready,
    input  logic [31:0] s_axil_araddr,
    output logic        s_axil_rvalid,
    input  logic        s_axil_rready,
    output logic [31:0] s_axil_rdata,
    output logic [ 1:0] s_axil_rresp,

    // AXI-Stream master (GROUP BY group results; inactive in classic mode)
    output logic                   m_axis_tvalid,
    input  logic                   m_axis_tready,
    output logic [GB_RESULT_W-1:0] m_axis_tdata,
    output logic                   m_axis_tlast
);

  import db_pkg::*;

  localparam int GB_RESULT_W = 2 * ACCUM_W + 3 * COLUMN_W;

  logic sched_rst;
  logic start, abort_req;
  logic                                load_wen;
  logic               [COL_ADDR_W-1:0] load_addr;
  logic               [  COLUMN_W-1:0] load_data [NUM_COLS];
  db_pkg::query_cfg_t                  query_cfg;
  db_pkg::agg_cfg_t                    agg_cfg;
  db_pkg::pred_cfg_t                   pred_cfg  [NUM_PRED];
  db_pkg::proj_mask_t                  proj_mask;
  logic busy, done, overflow;
  db_pkg::error_e               error;
  logic           [ACCUM_W-1:0] result;
  logic           [ACCUM_W-1:0] count;

  // Abort cancels an in-flight query: it resets only the scheduler core.
  assign sched_rst = rst || abort_req;

  axi_lite_slave #(
      .NUM_PRED  (NUM_PRED),
      .NUM_COLS  (NUM_COLS),
      .ACCUM_W   (ACCUM_W),
      .COL_ADDR_W(COL_ADDR_W)
  ) u_axil (
      .clk           (clk),
      .rst           (rst),
      .s_axil_awvalid(s_axil_awvalid),
      .s_axil_awready(s_axil_awready),
      .s_axil_awaddr (s_axil_awaddr),
      .s_axil_wvalid (s_axil_wvalid),
      .s_axil_wready (s_axil_wready),
      .s_axil_wdata  (s_axil_wdata),
      .s_axil_wstrb  (s_axil_wstrb),
      .s_axil_bvalid (s_axil_bvalid),
      .s_axil_bready (s_axil_bready),
      .s_axil_bresp  (s_axil_bresp),
      .s_axil_arvalid(s_axil_arvalid),
      .s_axil_arready(s_axil_arready),
      .s_axil_araddr (s_axil_araddr),
      .s_axil_rvalid (s_axil_rvalid),
      .s_axil_rready (s_axil_rready),
      .s_axil_rdata  (s_axil_rdata),
      .s_axil_rresp  (s_axil_rresp),
      .query_cfg     (query_cfg),
      .agg_cfg       (agg_cfg),
      .pred_cfg      (pred_cfg),
      .proj_mask     (proj_mask),
      .start         (start),
      .abort_req     (abort_req),
      .load_wen      (load_wen),
      .load_addr     (load_addr),
      .load_data     (load_data),
      .error         (error),
      .busy          (busy),
      .done          (done),
      .result        (result),
      .count         (count),
      .overflow      (overflow)
  );

  scheduler #(
      .DATA_W    (DATA_W),
      .ACCUM_W   (ACCUM_W),
      .COLUMN_W  (COLUMN_W),
      .COL_ADDR_W(COL_ADDR_W),
      .NUM_PRED  (NUM_PRED),
      .NUM_COLS  (NUM_COLS)
  ) u_sched (
      .clk          (clk),
      .rst          (sched_rst),
      .load_wen     (load_wen),
      .load_addr    (load_addr),
      .load_data    (load_data),
      .query_cfg    (query_cfg),
      .agg_cfg      (agg_cfg),
      .pred_cfg     (pred_cfg),
      .proj_mask    (proj_mask),
      .start        (start),
      .done         (done),
      .busy         (busy),
      .error        (error),
      .result       (result),
      .count        (count),
      .overflow     (overflow),
      .g_axis_tvalid(m_axis_tvalid),
      .g_axis_tready(m_axis_tready),
      .g_axis_tdata (m_axis_tdata),
      .g_axis_tlast (m_axis_tlast)
  );

endmodule
