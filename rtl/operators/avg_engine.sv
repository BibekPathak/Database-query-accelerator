// ===========================================================================
//  avg_engine.sv -- streaming AVG aggregation (SUM and COUNT).
//
//  Sink that consumes one pipeline_data_t stream per query and accumulates
//  the SUM of the configured column and the COUNT of rows whose pass bit is
//  set. The average itself is computed once after the query in software
//  (avg = sum / count), keeping the hardware free of a divider.
//
//  Overflow policy (same as sum_engine): the SUM saturates at all-ones and
//  `overflow` is asserted if the ACCUM_W-bit range would be exceeded. The
//  COUNT is bounded by NUM_ROWS and can never overflow.
//
//  Control (mirrors the other aggregation engines):
//    * start (1-cycle pulse) begins a new aggregation and resets state
//    * busy is high while beats are being consumed
//    * done pulses (level) once the final beat has been consumed
//    * s_axis_tready is asserted only while busy
//
//  Assertions (SVA) are compiled only when DBQA_ASSERT is defined.
// ===========================================================================

module avg_engine #(
    parameter int DATA_W     = db_pkg::AXIS_DATA_W,
    parameter int ACCUM_W    = db_pkg::ACCUM_WIDTH,
    parameter int COLUMN_W   = db_pkg::COLUMN_WIDTH,
    parameter int COL_ADDR_W = db_pkg::COLUMN_ADDR_W
) (
    input logic clk,
    input logic rst,

    input logic start,
    input logic [COL_ADDR_W-1:0] column,  // column to average

    // AXI-Stream slave (input)
    input  logic              s_axis_tvalid,
    output logic              s_axis_tready,
    input  logic [DATA_W-1:0] s_axis_tdata,
    input  logic              s_axis_tlast,

    // Result
    output logic [ACCUM_W-1:0] sum_result,    // SUM of passing rows
    output logic [ACCUM_W-1:0] count_result,  // COUNT of passing rows
    output logic               overflow,
    output logic               done,
    output logic               busy
);

  import db_pkg::*;

  logic [ACCUM_W-1:0] count_q;
  logic [ACCUM_W-1:0] sum_q;
  logic [ACCUM_W-1:0] sum_result_q;
  logic [ACCUM_W-1:0] count_result_q;
  logic               overflow_q;
  logic               busy_q;
  logic               done_q;

  assign s_axis_tready = busy_q;
  assign sum_result    = sum_result_q;
  assign count_result  = count_result_q;
  assign overflow      = overflow_q;
  assign busy          = busy_q;
  assign done          = done_q;

  pipeline_data_t beat;
  assign beat = pipeline_data_t'(s_axis_tdata);

  // Extended sum (ACCUM_W+1 bits) so overflow is a single carry-out bit.
  logic [ACCUM_W:0] sum_ext;
  assign sum_ext = {1'b0, sum_q} + {{(ACCUM_W - COLUMN_W) {1'b0}}, beat.columns[column]};

  // Next accumulator values (pass-qualified).
  logic [ACCUM_W-1:0] count_next;
  logic [ACCUM_W-1:0] sum_next;
  logic               of_next;
  always_comb begin
    count_next = count_q;
    sum_next   = sum_q;
    of_next    = overflow_q;
    if (beat.pass) begin
      count_next = count_q + ACCUM_W'(1);
      if (sum_ext[ACCUM_W]) begin
        sum_next = {ACCUM_W{1'b1}};  // saturate
        of_next  = 1'b1;
      end else begin
        sum_next = sum_ext[ACCUM_W-1:0];
      end
    end
  end

  always_ff @(posedge clk) begin
    if (rst) begin
      count_q        <= '0;
      sum_q          <= '0;
      sum_result_q   <= '0;
      count_result_q <= '0;
      overflow_q     <= 1'b0;
      busy_q         <= 1'b0;
      done_q         <= 1'b0;
    end else if (start) begin
      count_q        <= '0;
      sum_q          <= '0;
      sum_result_q   <= '0;
      count_result_q <= '0;
      overflow_q     <= 1'b0;
      busy_q         <= 1'b1;
      done_q         <= 1'b0;
    end else if (busy_q && s_axis_tvalid) begin
      count_q <= count_next;
      sum_q <= sum_next;
      overflow_q <= of_next;
      if (s_axis_tlast) begin
        busy_q         <= 1'b0;
        done_q         <= 1'b1;
        sum_result_q   <= sum_next;  // include the final row's contribution
        count_result_q <= count_next;
      end
    end
  end

`ifdef DBQA_ASSERT
  // The count never exceeds the number of rows in the table.
  m_count_bounded :
  assert property (@(posedge clk) disable iff (rst) (busy_q |-> (count_q <= ACCUM_W'(NUM_ROWS))));

  // If overflow is flagged, the sum is saturated.
  m_overflow_saturates :
  assert property (@(posedge clk) disable iff (rst) (overflow_q |-> (sum_q == {ACCUM_W{1'b1}})));
`endif

endmodule
