// ===========================================================================
//  predicate_engine.sv -- streaming WHERE-clause evaluation.
//
//  Consumes pipeline_data_t beats and rewrites the pass bit according to the
//  configured predicates: a list of (column, operator, immediate) triples
//  combined with AND/OR. Column data passes through untouched, so the engine
//  is a transparent filter on the streaming datapath.
//
//  Combination semantics (see db_pkg::pred_cfg_t):
//    * the first *enabled* slot seeds the result (its combine field is
//      ignored); each later enabled slot folds in via its combine operator
//    * disabled slots are skipped
//    * no enabled slots -> every row passes (pass = 1)
//
//  Architecture:
//    * the pass is computed combinationally from the input beat and the
//      configuration (single source of comparator semantics:
//      db_pkg::pred_apply / db_pkg::pred_combine)
//    * an output skid register (axis_register) provides one registered
//      pipeline stage and full ready/valid backpressure
//
//  Assertions (SVA) are compiled only when DBQA_ASSERT is defined.
// ===========================================================================

module predicate_engine #(
    parameter int NUM_PRED = 2,
    parameter int DATA_W   = db_pkg::AXIS_DATA_W
) (
    input logic clk,
    input logic rst,

    input db_pkg::pred_cfg_t pred_cfg[NUM_PRED],

    // AXI-Stream slave (input)
    input  logic              s_axis_tvalid,
    output logic              s_axis_tready,
    input  logic [DATA_W-1:0] s_axis_tdata,
    input  logic              s_axis_tlast,

    // AXI-Stream master (output)
    output logic              m_axis_tvalid,
    input  logic              m_axis_tready,
    output logic [DATA_W-1:0] m_axis_tdata,
    output logic              m_axis_tlast
);

  import db_pkg::*;

  initial begin
    if (NUM_PRED < 1) $error("predicate_engine: NUM_PRED must be >= 1");
  end

  // -------------------------------------------------------------------------
  // Combination logic: the first enabled slot seeds the mask, later enabled
  // slots fold in via their combine operator; disabled slots are skipped.
  // Only the configured columns are read, so unselected beat bits are unused.
  // -------------------------------------------------------------------------
  pipeline_data_t beat_in;
  assign beat_in = pipeline_data_t'(s_axis_tdata);

  pipeline_data_t beat_out;
  always_comb begin
    logic [NUM_PRED-1:0] slot_result;
    logic found;

    beat_out = beat_in;
    for (int i = 0; i < NUM_PRED; i++)
    slot_result[i] =
        db_pkg::pred_apply(pred_cfg[i].op, beat_in.columns[pred_cfg[i].column], pred_cfg[i].imm);

    beat_out.pass = 1'b1;
    found         = 1'b0;
    for (int i = 0; i < NUM_PRED; i++) begin
      if (pred_cfg[i].enable) begin
        if (!found) begin
          beat_out.pass = slot_result[i];
          found         = 1'b1;
        end else begin
          beat_out.pass = db_pkg::pred_combine(beat_out.pass, pred_cfg[i].combine, slot_result[i]);
        end
      end
    end
  end

  // -------------------------------------------------------------------------
  // Registered output stage (skid register) with ready/valid backpressure.
  // -------------------------------------------------------------------------
  axis_register #(
      .DATA_W(DATA_W)
  ) u_out_reg (
      .clk(clk),
      .rst(rst),
      .s_axis_tvalid(s_axis_tvalid),
      .s_axis_tready(s_axis_tready),
      .s_axis_tdata(DATA_W'(beat_out)),
      .s_axis_tlast(s_axis_tlast),
      .m_axis_tvalid(m_axis_tvalid),
      .m_axis_tready(m_axis_tready),
      .m_axis_tdata(m_axis_tdata),
      .m_axis_tlast(m_axis_tlast)
  );

`ifdef DBQA_ASSERT
  // The registered pass bit is self-consistent with the registered columns
  // and the current configuration (all sampled signals are registered/stable).
  pipeline_data_t m_beat;
  assign m_beat = pipeline_data_t'(m_axis_tdata);

  logic pass_check;
  always_comb begin
    logic [NUM_PRED-1:0] slot_result;
    logic found;

    pass_check = 1'b1;
    found      = 1'b0;
    for (int i = 0; i < NUM_PRED; i++) begin
      slot_result[i] =
          db_pkg::pred_apply(pred_cfg[i].op, m_beat.columns[pred_cfg[i].column], pred_cfg[i].imm);
      if (pred_cfg[i].enable) begin
        if (!found) begin
          pass_check = slot_result[i];
          found      = 1'b1;
        end else begin
          pass_check = db_pkg::pred_combine(pass_check, pred_cfg[i].combine, slot_result[i]);
        end
      end
    end
  end

  m_pass_consistent :
  assert property (@(posedge clk) disable iff (rst)
      (m_axis_tvalid |-> (m_beat.pass == pass_check)));
`endif

endmodule
