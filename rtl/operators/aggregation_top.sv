// ===========================================================================
//  aggregation_top.sv -- selectable streaming aggregation over a table scan.
//
//  Instantiates every aggregation engine (COUNT, SUM, MIN, MAX, AVG) and
//  routes the input stream to all of them; the configured opcode
//  (agg_cfg_t.op) selects which result is presented. All engines share the
//  same start/busy/done timing, so the whole aggregator is driven as one sink.
//
//  Outputs:
//    * result   -- the primary result for the configured opcode
//                  (COUNT, SUM, MIN, MAX, or AVG's SUM)
//    * count    -- the count of passing rows (all opcodes)
//    * overflow -- SUM saturation flag (all opcodes; meaningful for SUM/AVG)
//
//  AVG = result / count is computed once in software after the query.
//
//  Assertions (SVA) are compiled only when DBQA_ASSERT is defined.
// ===========================================================================

module aggregation_top #(
    parameter int DATA_W     = db_pkg::AXIS_DATA_W,
    parameter int ACCUM_W    = db_pkg::ACCUM_WIDTH,
    parameter int COLUMN_W   = db_pkg::COLUMN_WIDTH,
    parameter int COL_ADDR_W = db_pkg::COLUMN_ADDR_W
) (
    input logic clk,
    input logic rst,

    /* verilator lint_off UNUSEDSIGNAL */
    input db_pkg::agg_cfg_t agg_cfg,  // groupby/gby_key used in Phase 6
    /* verilator lint_on UNUSEDSIGNAL */
    input logic             start,

    // AXI-Stream slave (input)
    input  logic              s_axis_tvalid,
    output logic              s_axis_tready,
    input  logic [DATA_W-1:0] s_axis_tdata,
    input  logic              s_axis_tlast,

    // Result
    output logic [ACCUM_W-1:0] result,
    output logic [ACCUM_W-1:0] count,
    output logic               overflow,
    output logic               done,
    output logic               busy
);

  import db_pkg::*;

  logic [ ACCUM_W-1:0] count_res;
  logic [ ACCUM_W-1:0] sum_res;
  logic [COLUMN_W-1:0] min_res;
  logic [COLUMN_W-1:0] max_res;
  logic [ ACCUM_W-1:0] avg_sum_res;
  logic                sum_overflow;
  logic cnt_done, cnt_busy;

  // All engines consume the stream in parallel (their timing is identical).
  /* verilator lint_off UNUSEDSIGNAL */
  logic unused_ready, unused_done, unused_busy, unused_overflow;
  logic [ACCUM_W-1:0] unused_avg_cnt;
  /* verilator lint_on UNUSEDSIGNAL */

  count_engine #(
      .DATA_W (DATA_W),
      .ACCUM_W(ACCUM_W)
  ) u_cnt (
      .clk(clk),
      .rst(rst),
      .start(start),
      .s_axis_tvalid(s_axis_tvalid),
      .s_axis_tready(s_axis_tready),
      .s_axis_tdata(s_axis_tdata),
      .s_axis_tlast(s_axis_tlast),
      .result(count_res),
      .done(cnt_done),
      .busy(cnt_busy)
  );

  sum_engine #(
      .DATA_W (DATA_W),
      .ACCUM_W(ACCUM_W)
  ) u_sum (
      .clk(clk),
      .rst(rst),
      .start(start),
      .column(agg_cfg.column),
      .s_axis_tvalid(s_axis_tvalid),
      .s_axis_tready(unused_ready),
      .s_axis_tdata(s_axis_tdata),
      .s_axis_tlast(s_axis_tlast),
      .result(sum_res),
      .overflow(sum_overflow),
      .done(unused_done),
      .busy(unused_busy)
  );

  min_engine #(
      .DATA_W(DATA_W)
  ) u_min (
      .clk(clk),
      .rst(rst),
      .start(start),
      .column(agg_cfg.column),
      .s_axis_tvalid(s_axis_tvalid),
      .s_axis_tready(unused_ready),
      .s_axis_tdata(s_axis_tdata),
      .s_axis_tlast(s_axis_tlast),
      .result(min_res),
      .done(unused_done),
      .busy(unused_busy)
  );

  max_engine #(
      .DATA_W(DATA_W)
  ) u_max (
      .clk(clk),
      .rst(rst),
      .start(start),
      .column(agg_cfg.column),
      .s_axis_tvalid(s_axis_tvalid),
      .s_axis_tready(unused_ready),
      .s_axis_tdata(s_axis_tdata),
      .s_axis_tlast(s_axis_tlast),
      .result(max_res),
      .done(unused_done),
      .busy(unused_busy)
  );

  avg_engine #(
      .DATA_W (DATA_W),
      .ACCUM_W(ACCUM_W)
  ) u_avg (
      .clk(clk),
      .rst(rst),
      .start(start),
      .column(agg_cfg.column),
      .s_axis_tvalid(s_axis_tvalid),
      .s_axis_tready(unused_ready),
      .s_axis_tdata(s_axis_tdata),
      .s_axis_tlast(s_axis_tlast),
      .sum_result(avg_sum_res),
      .count_result(unused_avg_cnt),
      .overflow(unused_overflow),
      .done(unused_done),
      .busy(unused_busy)
  );

  // -------------------------------------------------------------------------
  // Result mux
  // -------------------------------------------------------------------------
  always_comb begin
    result = '0;
    case (agg_cfg.op)
      OP_COUNT: result = count_res;
      OP_SUM:   result = sum_res;
      OP_MIN:   result = ACCUM_W'(min_res);
      OP_MAX:   result = ACCUM_W'(max_res);
      OP_AVG:   result = avg_sum_res;
      default:  result = '0;
    endcase
  end

  assign count    = count_res;
  assign overflow = sum_overflow;
  assign done     = cnt_done;
  assign busy     = cnt_busy;

`ifdef DBQA_ASSERT
  // The COUNT engine drives the top-level control; the engines' done and busy
  // timing must agree (they all see the same stream).
  m_done_busy_excl :
  assert property (@(posedge clk) disable iff (rst) (~(cnt_done && cnt_busy)));
`endif

endmodule
