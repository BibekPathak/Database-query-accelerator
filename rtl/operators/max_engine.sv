// ===========================================================================
//  max_engine.sv -- streaming MAX aggregation.
//
//  Sink that consumes one pipeline_data_t stream per query and keeps the
//  maximum value of the configured column over the rows whose pass bit is
//  set. The result is latched when the final beat (tlast) is consumed.
//
//  The accumulator is initialized to zero (the smallest representable value),
//  so it is updated by the first passing row. If no row passes, the result
//  remains zero (documented sentinel).
//
//  Control (mirrors the column reader and the other aggregation engines):
//    * start (1-cycle pulse) begins a new aggregation and resets state
//    * busy is high while beats are being consumed
//    * done pulses (level) once the final beat has been consumed
//    * s_axis_tready is asserted only while busy
//
//  Assertions (SVA) are compiled only when DBQA_ASSERT is defined.
// ===========================================================================

module max_engine #(
    parameter int DATA_W    = db_pkg::AXIS_DATA_W,
    parameter int COLUMN_W  = db_pkg::COLUMN_WIDTH,
    parameter int COL_ADDR_W = db_pkg::COLUMN_ADDR_W
) (
    input logic clk,
    input logic rst,

    input logic start,
    input logic [COL_ADDR_W-1:0] column,  // column to take the maximum of

    // AXI-Stream slave (input)
    input  logic              s_axis_tvalid,
    output logic              s_axis_tready,
    input  logic [DATA_W-1:0] s_axis_tdata,
    input  logic              s_axis_tlast,

    // Result
    output logic [COLUMN_W-1:0] result,
    output logic                done,
    output logic                busy
);

  import db_pkg::*;

  logic [COLUMN_W-1:0] acc_q;
  logic [COLUMN_W-1:0] result_q;
  logic                busy_q;
  logic                done_q;

  assign s_axis_tready = busy_q;
  assign result = result_q;
  assign busy = busy_q;
  assign done = done_q;

  pipeline_data_t beat;
  assign beat = pipeline_data_t'(s_axis_tdata);

  // Next accumulator value (pass-qualified maximum).
  logic [COLUMN_W-1:0] acc_next;
  assign acc_next = beat.pass ? ((beat.columns[column] > acc_q) ?
                                 beat.columns[column] : acc_q) : acc_q;

  always_ff @(posedge clk) begin
    if (rst) begin
      acc_q    <= '0;
      result_q <= '0;
      busy_q   <= 1'b0;
      done_q   <= 1'b0;
    end else if (start) begin
      acc_q    <= '0;   // identity for MAX
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
  // The running maximum never drops below the last presented row's value.
  m_max_monotonic :
  assert property (@(posedge clk) disable iff (rst)
      (busy_q && s_axis_tvalid && beat.pass) |=> (acc_q >= $past(
      acc_q
  )));

  // The accumulator is stable when the engine is idle.
  m_idle_stable :
  assert property (@(posedge clk) disable iff (rst) (~busy_q |=> $stable(acc_q)));
`endif

endmodule
