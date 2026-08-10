// ===========================================================================
//  sum_engine.sv -- streaming SUM aggregation with overflow saturation.
//
//  Sink that consumes one pipeline_data_t stream per query and accumulates the
//  configured column's values over the rows whose pass bit is set. The result
//  is latched when the final beat (tlast) is consumed.
//
//  Overflow policy: if the accumulator would exceed its ACCUM_W-bit range,
//  it saturates at all-ones and `overflow` is asserted (and stays asserted
//  until the next start). Saturating, not wrapping, keeps the result defined
//  and provable.
//
//  Control (mirrors the column reader):
//    * start (1-cycle pulse) begins a new aggregation and resets state
//    * busy is high while beats are being consumed
//    * done pulses (level) once the final beat has been consumed
//    * s_axis_tready is asserted only while busy
//
//  Assertions (SVA) are compiled only when DBQA_ASSERT is defined.
// ===========================================================================

module sum_engine #(
    parameter int DATA_W    = db_pkg::AXIS_DATA_W,
    parameter int ACCUM_W   = db_pkg::ACCUM_WIDTH,
    parameter int COLUMN_W  = db_pkg::COLUMN_WIDTH,
    parameter int COL_ADDR_W = db_pkg::COLUMN_ADDR_W
) (
    input logic clk,
    input logic rst,

    input logic start,
    input logic [COL_ADDR_W-1:0] column,  // column to sum

    // AXI-Stream slave (input)
    input  logic              s_axis_tvalid,
    output logic              s_axis_tready,
    input  logic [DATA_W-1:0] s_axis_tdata,
    input  logic              s_axis_tlast,

    // Result
    output logic [ACCUM_W-1:0] result,
    output logic               overflow,
    output logic               done,
    output logic               busy
);


  logic [ACCUM_W-1:0] acc_q;
  logic [ACCUM_W-1:0] result_q;
  logic               overflow_q;
  logic               busy_q;
  logic               done_q;

  assign s_axis_tready = busy_q;
  assign result        = result_q;
  assign overflow      = overflow_q;
  assign busy          = busy_q;
  assign done          = done_q;

  db_pkg::pipeline_data_t beat;
  assign beat = db_pkg::pipeline_data_t'(s_axis_tdata);

  // Extended sum (ACCUM_W+1 bits) so overflow is a single carry-out bit.
  logic [ACCUM_W:0] sum_ext;
  assign sum_ext = {1'b0, acc_q} + {{(ACCUM_W - COLUMN_W) {1'b0}}, beat.columns[column]};

  // Next accumulator / overflow value (pass-qualified).
  logic [ACCUM_W-1:0] acc_next;
  logic               of_next;
  always_comb begin
    acc_next = acc_q;
    of_next  = overflow_q;
    if (beat.pass) begin
      if (sum_ext[ACCUM_W]) begin
        acc_next = {ACCUM_W{1'b1}};  // saturate
        of_next  = 1'b1;
      end else begin
        acc_next = sum_ext[ACCUM_W-1:0];
      end
    end
  end

  always_ff @(posedge clk) begin
    if (rst) begin
      acc_q      <= '0;
      result_q   <= '0;
      overflow_q <= 1'b0;
      busy_q     <= 1'b0;
      done_q     <= 1'b0;
    end else if (start) begin
      acc_q      <= '0;
      result_q   <= '0;
      overflow_q <= 1'b0;
      busy_q     <= 1'b1;
      done_q     <= 1'b0;
    end else if (busy_q && s_axis_tvalid) begin
      acc_q      <= acc_next;
      overflow_q <= of_next;
      if (s_axis_tlast) begin
        busy_q   <= 1'b0;
        done_q   <= 1'b1;
        result_q <= acc_next;  // include the final row's contribution
      end
    end
  end

`ifdef DBQA_ASSERT
  // If overflow is flagged, the accumulator is saturated.
  m_overflow_saturates :
  assert property (@(posedge clk) disable iff (rst) (overflow_q |-> (acc_q == {ACCUM_W{1'b1}})));
`endif

endmodule
