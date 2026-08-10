// ===========================================================================
//  sum_engine_formal.sv -- SymbiYosys formal wrapper for sum_engine.
//
//  Checks that the streaming SUM engine's latched result and overflow flag
//  equal an independent shadow model at completion: the result must equal the
//  saturated sum of the configured column over passing beats, and overflow
//  must be asserted exactly when the accumulator saturates.
//
//  Reads the formal db_pkg replica (formal/db_pkg_formal.sv) instead of the
//  real package because the yosys frontend rejects `$bits()`.
// ===========================================================================

module sum_engine_formal #(
    parameter int DATA_W     = db_pkg::AXIS_DATA_W,
    parameter int ACCUM_W    = db_pkg::ACCUM_WIDTH,
    parameter int COLUMN_W   = db_pkg::COLUMN_WIDTH,
    parameter int COL_ADDR_W = db_pkg::COLUMN_ADDR_W
) (
    input logic clk,
    input logic start,
    input logic [COL_ADDR_W-1:0] column,
    input  logic              s_axis_tvalid,
    input  logic [DATA_W-1:0] s_axis_tdata,
    input  logic              s_axis_tlast
);

  // Deterministic two-cycle reset.
  logic rst;
  logic [1:0] rst_s;
  initial rst_s = 2'b11;
  always @(posedge clk) rst_s <= {1'b0, rst_s[1]};
  assign rst = rst_s[1];

  logic              s_axis_tready;
  logic              done, busy, overflow;
  logic [ACCUM_W-1:0] result;

  sum_engine #(
      .DATA_W    (DATA_W),
      .ACCUM_W   (ACCUM_W),
      .COLUMN_W  (COLUMN_W),
      .COL_ADDR_W(COL_ADDR_W)
  ) u_sum (
      .clk          (clk),
      .rst          (rst),
      .start        (start),
      .column       (column),
      .s_axis_tvalid(s_axis_tvalid),
      .s_axis_tready(s_axis_tready),
      .s_axis_tdata (s_axis_tdata),
      .s_axis_tlast (s_axis_tlast),
      .result       (result),
      .overflow     (overflow),
      .done         (done),
      .busy         (busy)
  );

  // Inputs are free, but the source is quiet during reset.
  assume property (!rst || !s_axis_tvalid);

  db_pkg::pipeline_data_t beat;
  assign beat = db_pkg::pipeline_data_t'(s_axis_tdata);

  logic pass;
  assign pass = beat.pass;

  logic [COLUMN_W-1:0] value;
  assign value = beat.columns[column];

  // Shadow model: pass-qualified accumulating SUM with saturation.
  logic [ACCUM_W:0]   shadow;
  logic               shadow_of;
  always @(posedge clk) begin
    if (rst) begin
      shadow   <= '0;
      shadow_of <= 1'b0;
    end else if (start) begin
      shadow   <= '0;
      shadow_of <= 1'b0;
    end else if (busy && s_axis_tvalid) begin
      if (pass) begin
        if (shadow + value > {ACCUM_W{1'b1}}) begin
          shadow   <= {ACCUM_W{1'b1}};
          shadow_of <= 1'b1;
        end else begin
          shadow <= shadow + value;
        end
      end
    end
  end

  // At completion the latched result equals the shadow sum.
  assert property (rst || !done || (result == shadow[ACCUM_W-1:0]));

  // At completion the overflow flag agrees with the shadow's saturation.
  assert property (rst || !done || (overflow == shadow_of));

endmodule
