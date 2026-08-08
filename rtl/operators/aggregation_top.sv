// ===========================================================================
//  aggregation_top.sv -- selectable streaming aggregation over a table scan.
//
//  Two modes, selected by agg_cfg.groupby:
//    * groupby = 0 -- classic aggregation. Instantiates every aggregation
//      engine (COUNT, SUM, MIN, MAX, AVG) and routes the input stream to all
//      of them; the configured opcode (agg_cfg_t.op) selects which result is
//      presented. All engines share the same start/busy/done timing, so the
//      whole aggregator is driven as one sink.
//    * groupby = 1 -- GROUP BY. Routes the input stream to the group-by
//      engine instead, which hashes on agg_cfg.gby_key and aggregates
//      agg_cfg.column per group. The groups stream out the g_axis master
//      (one beat per group, tlast on the final group); result/count/overflow
//      are zeroed.
//
//  Classic outputs:
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

    input db_pkg::agg_cfg_t agg_cfg,
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
    output logic               busy,

    // AXI-Stream master (GROUP BY group results; inactive in classic mode)
    output logic                   g_axis_tvalid,
    input  logic                   g_axis_tready,
    output logic [GB_RESULT_W-1:0] g_axis_tdata,
    output logic                   g_axis_tlast
);

  import db_pkg::*;

  // Width of one group result {key, count, sum, min, max}.
  localparam int GB_RESULT_W = 2 * ACCUM_W + 3 * COLUMN_W;

  // -------------------------------------------------------------------------
  // Routing: classic engines or the group-by engine, never both.
  // -------------------------------------------------------------------------
  logic classic_start, classic_tvalid, classic_tready;
  logic gb_start, gb_tvalid, gb_tready, gb_done, gb_busy;

  assign classic_start  = start && !agg_cfg.groupby;
  assign classic_tvalid = s_axis_tvalid && !agg_cfg.groupby;
  assign gb_start       = start && agg_cfg.groupby;
  assign gb_tvalid      = s_axis_tvalid && agg_cfg.groupby;

  assign s_axis_tready  = agg_cfg.groupby ? gb_tready : classic_tready;

  logic [ ACCUM_W-1:0] count_res;
  logic [ ACCUM_W-1:0] sum_res;
  logic [COLUMN_W-1:0] min_res;
  logic [COLUMN_W-1:0] max_res;
  logic [ ACCUM_W-1:0] avg_sum_res;
  logic                sum_overflow;
  logic cnt_done, cnt_busy;

  // The classic engines consume the stream in parallel (their timing is
  // identical); COUNT drives the sink ready.
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
      .start(classic_start),
      .s_axis_tvalid(classic_tvalid),
      .s_axis_tready(classic_tready),
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
      .start(classic_start),
      .column(agg_cfg.column),
      .s_axis_tvalid(classic_tvalid),
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
      .start(classic_start),
      .column(agg_cfg.column),
      .s_axis_tvalid(classic_tvalid),
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
      .start(classic_start),
      .column(agg_cfg.column),
      .s_axis_tvalid(classic_tvalid),
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
      .start(classic_start),
      .column(agg_cfg.column),
      .s_axis_tvalid(classic_tvalid),
      .s_axis_tready(unused_ready),
      .s_axis_tdata(s_axis_tdata),
      .s_axis_tlast(s_axis_tlast),
      .sum_result(avg_sum_res),
      .count_result(unused_avg_cnt),
      .overflow(unused_overflow),
      .done(unused_done),
      .busy(unused_busy)
  );

  groupby_engine #(
      .DATA_W    (DATA_W),
      .COLUMN_W  (COLUMN_W),
      .ACCUM_W   (ACCUM_W),
      .COL_ADDR_W(COL_ADDR_W)
  ) u_gb (
      .clk(clk),
      .rst(rst),
      .start(gb_start),
      .key_col(agg_cfg.gby_key),
      .value_col(agg_cfg.column),
      .s_axis_tvalid(gb_tvalid),
      .s_axis_tready(gb_tready),
      .s_axis_tdata(s_axis_tdata),
      .s_axis_tlast(s_axis_tlast),
      .m_axis_tvalid(g_axis_tvalid),
      .m_axis_tready(g_axis_tready),
      .m_axis_tdata(g_axis_tdata),
      .m_axis_tlast(g_axis_tlast),
      .done(gb_done),
      .busy(gb_busy)
  );

  // -------------------------------------------------------------------------
  // Result mux
  // -------------------------------------------------------------------------
  always_comb begin
    result = '0;
    if (!agg_cfg.groupby) begin
      case (agg_cfg.op)
        OP_COUNT: result = count_res;
        OP_SUM:   result = sum_res;
        OP_MIN:   result = ACCUM_W'(min_res);
        OP_MAX:   result = ACCUM_W'(max_res);
        OP_AVG:   result = avg_sum_res;
        default:  result = '0;
      endcase
    end
  end

  assign count    = agg_cfg.groupby ? '0 : count_res;
  assign overflow = agg_cfg.groupby ? 1'b0 : sum_overflow;
  assign done     = agg_cfg.groupby ? gb_done : cnt_done;
  assign busy     = agg_cfg.groupby ? gb_busy : cnt_busy;

`ifdef DBQA_ASSERT
  // The COUNT engine drives the classic control; its done and busy timing
  // must agree (all classic engines see the same stream).
  m_done_busy_excl :
  assert property (@(posedge clk) disable iff (rst) (~(cnt_done && cnt_busy)));
`endif

endmodule
