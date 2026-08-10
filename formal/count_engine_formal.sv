// ===========================================================================
//  count_engine_formal.sv -- SymbiYosys formal wrapper for count_engine.
//
//  Checks that the streaming COUNT engine's latched result equals an
//  independent shadow model of the accumulator at completion: the result must
//  equal the number of passing beats (s_axis_tdata MSB = pass) consumed.
//
//  Reads the formal db_pkg replica (formal/db_pkg_formal.sv) instead of the
//  real package because the yosys frontend rejects `$bits()`.
// ===========================================================================

module count_engine_formal #(
    parameter int DATA_W  = db_pkg::AXIS_DATA_W,
    parameter int ACCUM_W = db_pkg::ACCUM_WIDTH
) (
    input logic clk,
    input logic start,
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
  logic              done, busy;
  logic [ACCUM_W-1:0] result;

  count_engine #(
      .DATA_W (DATA_W),
      .ACCUM_W(ACCUM_W)
  ) u_cnt (
      .clk          (clk),
      .rst          (rst),
      .start        (start),
      .s_axis_tvalid(s_axis_tvalid),
      .s_axis_tready(s_axis_tready),
      .s_axis_tdata (s_axis_tdata),
      .s_axis_tlast (s_axis_tlast),
      .result       (result),
      .done         (done),
      .busy         (busy)
  );

  // Inputs are free, but the source is quiet during reset.
  assume property (!rst || !s_axis_tvalid);

  logic pass;
  assign pass = s_axis_tdata[DATA_W-1];

  // Shadow accumulator: increments on every accepted passing beat.
  logic [ACCUM_W:0] shadow;
  always @(posedge clk) begin
    if (rst) begin
      shadow <= '0;
    end else if (start) begin
      shadow <= '0;
    end else if (busy && s_axis_tvalid) begin
      shadow <= shadow + (pass ? 1'b1 : 1'b0);
    end
  end

  // At completion the latched result equals the accumulated pass count.
  assert property (rst || !done || (result == shadow));

endmodule
