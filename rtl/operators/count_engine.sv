// ===========================================================================
//  count_engine.sv -- streaming COUNT aggregation.
//
//  Sink that consumes one pipeline_data_t stream per query and counts the
//  rows whose predicate pass bit is set. The result is latched when the final
//  beat (tlast) is consumed and presented on `result` with `done`.
//
//  Control (mirrors the column reader):
//    * start (1-cycle pulse) begins a new aggregation and resets the count
//    * busy is high while beats are being consumed
//    * done pulses (level) once the final beat has been consumed
//    * s_axis_tready is asserted only while busy
//
//  The accumulator is ACCUM_W bits (default db_pkg::ACCUM_WIDTH), which is
//  wider than the maximum count (NUM_ROWS) so the count can never overflow.
//
//  Assertions (SVA) are compiled only when DBQA_ASSERT is defined.
// ===========================================================================

module count_engine #(
    parameter int DATA_W  = db_pkg::AXIS_DATA_W,
    parameter int ACCUM_W = db_pkg::ACCUM_WIDTH
) (
    input logic clk,
    input logic rst,

    input logic start,

    // AXI-Stream slave (input)
    input  logic              s_axis_tvalid,
    output logic              s_axis_tready,
    input  logic [DATA_W-1:0] s_axis_tdata,
    input  logic              s_axis_tlast,

    // Result
    output logic [ACCUM_W-1:0] result,
    output logic               done,
    output logic               busy
);

  import db_pkg::*;

  logic [ACCUM_W-1:0] acc_q;
  logic [ACCUM_W-1:0] result_q;
  logic               busy_q;
  logic               done_q;

  assign s_axis_tready = busy_q;
  assign result = result_q;
  assign busy = busy_q;
  assign done = done_q;

  // The predicate pass bit is the MSB of pipeline_data_t.
  logic pass;
  assign pass = s_axis_tdata[DATA_W-1];

  // Next accumulator value (pass-qualified increment).
  logic [ACCUM_W-1:0] acc_next;
  assign acc_next = acc_q + (pass ? ACCUM_W'(1) : '0);

  always_ff @(posedge clk) begin
    if (rst) begin
      acc_q    <= '0;
      result_q <= '0;
      busy_q   <= 1'b0;
      done_q   <= 1'b0;
    end else if (start) begin
      acc_q    <= '0;
      result_q <= '0;
      busy_q   <= 1'b1;
      done_q   <= 1'b0;
    end else if (busy_q && s_axis_tvalid) begin
      acc_q <= acc_next;
      if (s_axis_tlast) begin
        busy_q   <= 1'b0;
        done_q   <= 1'b1;
        result_q <= acc_next;  // include the final row's contribution
      end
    end
  end

`ifdef DBQA_ASSERT
  // The count never exceeds the number of rows in the table.
  m_count_bounded :
  assert property (@(posedge clk) disable iff (rst) (busy_q |-> (acc_q <= ACCUM_W'(NUM_ROWS))));

  // Once done, the presented result equals the final count.
  m_done_result :
  assert property (@(posedge clk) disable iff (rst) (done_q |-> (result_q == acc_q)));
`endif

endmodule
