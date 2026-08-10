// ===========================================================================
//  scheduler.sv -- query execution core for the DBQA accelerator.
//
//  Chains the pipeline stages -- column_reader -> predicate_engine ->
//  projection_engine -> aggregation_top -- into one self-timed query path and
//  owns the start/done/busy control FSM. It is the unit the AXI-Lite slave
//  (Phase 7.5) configures and starts; classic aggregation results are latched
//  here for the register file, and GROUP BY groups stream out g_axis.
//
//  Query flow:
//    * the CPU loads the table through the load port (load only while idle)
//    * on `start` the reader and the aggregation stage begin together; beats
//      flow reader -> predicate -> projection -> aggregation under normal
//      ready/valid backpressure
//    * query_cfg.num_rows (0 = full table) is passed to the reader as its
//      scan bound, so a bounded query actually stops the BRAM traversal early
//      and finishes in ~2*num_rows cycles; the bounded stream carries exactly
//      one end-of-stream (tlast)
//    * completion is signalled when the aggregation stage has finished AND
//      the reader is idle again (guarantees no stale rows in the reader FIFO
//      when a new query starts)
//    * results: classic mode latches result/count/overflow; GROUP BY mode
//      forwards the group stream on g_axis
//
//  Status: busy (S_SCAN), done (S_DONE), error (start-while-busy or SUM
//  overflow) are presented on the control outputs.
//
//  Assertions (SVA) are compiled only when DBQA_ASSERT is defined.
// ===========================================================================

module scheduler #(
    parameter int DATA_W     = db_pkg::AXIS_DATA_W,
    parameter int ACCUM_W    = db_pkg::ACCUM_WIDTH,
    parameter int COLUMN_W   = db_pkg::COLUMN_WIDTH,
    parameter int COL_ADDR_W = db_pkg::COLUMN_ADDR_W,
    parameter int NUM_PRED   = db_pkg::NUM_PRED,
    parameter int NUM_COLS   = db_pkg::NUM_COLS,
    parameter int NUM_ROWS   = db_pkg::NUM_ROWS
) (
    input logic clk,
    input logic rst,

    // Table load port (CPU-driven; load only while idle)
    input logic                  load_wen,
    input logic [COL_ADDR_W-1:0] load_addr,
    input logic [  COLUMN_W-1:0] load_data[NUM_COLS],

    // Query configuration
    input db_pkg::query_cfg_t query_cfg,
    input db_pkg::agg_cfg_t   agg_cfg,
    input db_pkg::pred_cfg_t  pred_cfg [NUM_PRED],
    input db_pkg::proj_mask_t proj_mask,

    // Control / status
    input  logic                         start,
    output logic                         done,
    output logic                         busy,
    output db_pkg::error_e               error,
    output logic           [ACCUM_W-1:0] result,
    output logic           [ACCUM_W-1:0] count,
    output logic                         overflow,

    // AXI-Stream master (GROUP BY group results; inactive in classic mode)
    output logic                   g_axis_tvalid,
    input  logic                   g_axis_tready,
    output logic [GB_RESULT_W-1:0] g_axis_tdata,
    output logic                   g_axis_tlast
);

  import db_pkg::*;

  localparam int GB_RESULT_W = 2 * ACCUM_W + 3 * COLUMN_W;

  typedef enum logic [1:0] {
    S_IDLE,  // awaiting start
    S_SCAN,  // query running
    S_DONE   // completed; results latched
  } state_e;

  // -------------------------------------------------------------------------
  // Column reader
  // -------------------------------------------------------------------------
  logic rd_tvalid, rd_tlast;
  logic [DATA_W-1:0] rd_tdata;
  /* verilator lint_off UNUSEDSIGNAL */
  logic reader_start, reader_busy, reader_done;
  /* verilator lint_on UNUSEDSIGNAL */
  column_reader #(
      .NUM_COLS    (NUM_COLS),
      .COLUMN_WIDTH(COLUMN_W),
      .NUM_ROWS    (NUM_ROWS),
      .ADDR_W      (COL_ADDR_W)
  ) u_reader (
      .clk          (clk),
      .rst          (rst),
      .load_wen     (load_wen),
      .load_addr    (load_addr),
      .load_data    (load_data),
      .start        (reader_start),
      .scan_bound   (query_cfg.num_rows),
      .busy         (reader_busy),
      .done         (reader_done),
      .m_axis_tvalid(rd_tvalid),
      .m_axis_tready(pred_s_ready),
      .m_axis_tdata (rd_tdata),
      .m_axis_tlast (rd_tlast)
  );

  // -------------------------------------------------------------------------
  // Predicate, projection and aggregation stages. The reader bounds the scan
  // itself (query_cfg.num_rows), so its stream feeds the predicate directly.
  // -------------------------------------------------------------------------
  logic pred_s_ready;
  logic pred_tvalid, pred_tready, pred_tlast;
  logic [DATA_W-1:0] pred_tdata;
  logic proj_tvalid, proj_tready, proj_tlast;
  logic [DATA_W-1:0] proj_tdata;
  logic agg_start, agg_done, agg_overflow;
  /* verilator lint_off UNUSEDSIGNAL */
  logic agg_busy;
  /* verilator lint_on UNUSEDSIGNAL */
  logic [ACCUM_W-1:0] agg_result, agg_count;

  predicate_engine #(
      .NUM_PRED(NUM_PRED),
      .DATA_W  (DATA_W)
  ) u_pred (
      .clk          (clk),
      .rst          (rst),
      .pred_cfg     (pred_cfg),
      .s_axis_tvalid(rd_tvalid),
      .s_axis_tready(pred_s_ready),
      .s_axis_tdata (rd_tdata),
      .s_axis_tlast (rd_tlast),
      .m_axis_tvalid(pred_tvalid),
      .m_axis_tready(pred_tready),
      .m_axis_tdata (pred_tdata),
      .m_axis_tlast (pred_tlast)
  );

  projection_engine #(
      .NUM_COLS    (NUM_COLS),
      .COLUMN_WIDTH(COLUMN_W),
      .DATA_W      (DATA_W)
  ) u_proj (
      .clk          (clk),
      .rst          (rst),
      .proj_mask    (proj_mask),
      .s_axis_tvalid(pred_tvalid),
      .s_axis_tready(pred_tready),
      .s_axis_tdata (pred_tdata),
      .s_axis_tlast (pred_tlast),
      .m_axis_tvalid(proj_tvalid),
      .m_axis_tready(proj_tready),
      .m_axis_tdata (proj_tdata),
      .m_axis_tlast (proj_tlast)
  );

  aggregation_top #(
      .DATA_W    (DATA_W),
      .ACCUM_W   (ACCUM_W),
      .COLUMN_W  (COLUMN_W),
      .COL_ADDR_W(COL_ADDR_W)
  ) u_agg (
      .clk          (clk),
      .rst          (rst),
      .agg_cfg      (agg_cfg),
      .start        (agg_start),
      .s_axis_tvalid(proj_tvalid),
      .s_axis_tready(proj_tready),
      .s_axis_tdata (proj_tdata),
      .s_axis_tlast (proj_tlast),
      .result       (agg_result),
      .count        (agg_count),
      .overflow     (agg_overflow),
      .done         (agg_done),
      .busy         (agg_busy),
      .g_axis_tvalid(g_axis_tvalid),
      .g_axis_tready(g_axis_tready),
      .g_axis_tdata (g_axis_tdata),
      .g_axis_tlast (g_axis_tlast)
  );

  // -------------------------------------------------------------------------
  // Control FSM
  // -------------------------------------------------------------------------
  state_e                       state_q;
  logic           [ACCUM_W-1:0] result_q;
  logic           [ACCUM_W-1:0] count_q;
  logic                         overflow_q;
  db_pkg::error_e               error_q;

  // Start pulse: launches the reader and the aggregation stage together.
  logic                         qstart;
  assign qstart = start && ((state_q == S_IDLE) || (state_q == S_DONE));
  assign reader_start = qstart;
  assign agg_start    = qstart;

  always_ff @(posedge clk) begin
    if (rst) begin
      state_q    <= S_IDLE;
      result_q   <= '0;
      count_q    <= '0;
      overflow_q <= 1'b0;
      error_q    <= db_pkg::ERR_NONE;
    end else begin
      // Control FSM.
      case (state_q)
        S_IDLE: begin
          error_q <= db_pkg::ERR_NONE;
          if (start) state_q <= S_SCAN;
        end
        S_SCAN: begin
          if (start) error_q <= db_pkg::ERR_START_BUSY;
          if (agg_done && reader_done) begin
            result_q   <= agg_result;
            count_q    <= agg_count;
            overflow_q <= agg_overflow;
            if (agg_overflow) error_q <= db_pkg::ERR_AGG_OVERFLOW;
            state_q <= S_DONE;
          end
        end
        S_DONE: begin
          if (start) begin
            error_q <= db_pkg::ERR_NONE;
            state_q <= S_SCAN;
          end
        end
        default: state_q <= S_IDLE;
      endcase
    end
  end

  assign busy     = (state_q == S_SCAN);
  assign done     = (state_q == S_DONE);
  assign error    = error_q;
  assign result   = result_q;
  assign count    = count_q;
  assign overflow = overflow_q;

`ifdef DBQA_ASSERT
  // done and busy are mutually exclusive.
  m_done_busy_excl :
  assert property (@(posedge clk) disable iff (rst) (~(done && busy)));

  // The reader and aggregation stage must both be idle once done.
  m_done_reader_idle :
  assert property (@(posedge clk) disable iff (rst) (done |-> (!reader_busy && !agg_busy)));
`endif

endmodule
