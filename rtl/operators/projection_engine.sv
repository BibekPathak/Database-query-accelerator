// ===========================================================================
//  projection_engine.sv -- streaming SELECT-list masking.
//
//  Consumes pipeline_data_t beats and zeroes every column that is not in the
//  projection mask (SELECT list). Projected columns, the predicate pass bit
//  and tlast pass through untouched.
//
//  The datapath width stays constant (AXIS_DATA_W): unselected columns are
//  masked to zero rather than the bus being narrowed, which keeps every
//  downstream stage reading the same fixed layout. The projection mask is the
//  register-visible `proj_mask_t` from db_pkg.
//
//  Architecture: the mask is applied combinationally, then an output skid
//  register (axis_register) provides one registered pipeline stage and full
//  ready/valid backpressure.
//
//  Assertions (SVA) are compiled only when DBQA_ASSERT is defined.
// ===========================================================================

module projection_engine #(
    parameter int NUM_COLS     = db_pkg::NUM_COLS,
    parameter int COLUMN_WIDTH = db_pkg::COLUMN_WIDTH,
    parameter int DATA_W       = db_pkg::AXIS_DATA_W
) (
    input logic clk,
    input logic rst,

    input db_pkg::proj_mask_t proj_mask,

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

  pipeline_data_t beat_in;
  assign beat_in = pipeline_data_t'(s_axis_tdata);

  // -------------------------------------------------------------------------
  // Mask unselected columns to zero; keep pass and projected columns.
  // -------------------------------------------------------------------------
  pipeline_data_t beat_out;
  always_comb begin
    beat_out = beat_in;
    for (int c = 0; c < NUM_COLS; c++) if (!proj_mask[c]) beat_out.columns[c] = '0;
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
  // Unprojected (masked-out) columns are zeroed in the output; projected
  // column passthrough and the pass bit are verified by the testbench.
  /* verilator lint_off UNUSEDSIGNAL */
  pipeline_data_t m_out;
  assign m_out = pipeline_data_t'(m_axis_tdata);
  /* verilator lint_on UNUSEDSIGNAL */

  logic zero_ok;
  always_comb begin
    zero_ok = 1'b1;
    for (int c = 0; c < NUM_COLS; c++) if (!proj_mask[c]) zero_ok &= (m_out.columns[c] == '0);
  end

  m_masked_zero :
  assert property (@(posedge clk) disable iff (rst) (m_axis_tvalid |-> zero_ok));
`endif

endmodule
