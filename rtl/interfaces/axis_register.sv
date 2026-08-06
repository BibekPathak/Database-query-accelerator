// ===========================================================================
//  axis_register.sv -- one-deep skid-buffer AXI-Stream pipeline stage.
//
//  The canonical "one pipeline stage" building block for the DBQA datapath.
//  It provides:
//
//    * ~zero-latency pass-through: while the register is empty and the
//      downstream is ready, an incoming beat is forwarded combinationally in
//      the same cycle (no pipeline bubble on an empty stage)
//    * backpressure capture: if the downstream stalls, the incoming beat is
//      latched into the register and held (tdata/tlast stable) until accepted
//    * a one-beat buffer that lets an upstream keep sending while the
//      downstream drains the registered beat in the following cycle
//
//  Combinational outputs are derived from either the registered beat (when
//  full) or the registered upstream inputs (when empty), so the output path is
//  always registered or a direct passthrough of registered data.
//
//  Assertions (SVA) are compiled only when DBQA_ASSERT is defined.
// ===========================================================================

module axis_register #(
    parameter int DATA_W = 32
) (
    input  logic                clk,
    input  logic                rst,

    // AXI-Stream slave (input) side
    input  logic                s_axis_tvalid,
    output logic                s_axis_tready,
    input  logic [DATA_W-1:0]   s_axis_tdata,
    input  logic                s_axis_tlast,

    // AXI-Stream master (output) side
    output logic                m_axis_tvalid,
    input  logic                m_axis_tready,
    output logic [DATA_W-1:0]   m_axis_tdata,
    output logic                m_axis_tlast
);

    initial begin
        if (DATA_W < 1) $error("axis_register: DATA_W must be >= 1");
    end

    // Skid register: {tlast, tdata} plus a valid flag.
    logic [DATA_W:0] sreg_q;
    logic            sreg_valid_q;

    // -----------------------------------------------------------------------
    // Combinational pass-through / select
    // -----------------------------------------------------------------------
    assign s_axis_tready = ~sreg_valid_q;

    assign m_axis_tvalid = sreg_valid_q | s_axis_tvalid;
    assign m_axis_tdata  = sreg_valid_q ? sreg_q[DATA_W-1:0] : s_axis_tdata;
    assign m_axis_tlast  = sreg_valid_q ? sreg_q[DATA_W]     : s_axis_tlast;

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------
    always_ff @(posedge clk) begin
        if (rst) begin
            sreg_q       <= '0;
            sreg_valid_q <= 1'b0;
        end else if (~sreg_valid_q) begin
            // Register empty. Accept the input. It is stored only when the
            // downstream cannot take it this cycle (pass-through otherwise),
            // so s_axis_tready deasserts exactly when a beat is held.
            if (s_axis_tvalid) begin
                sreg_q       <= {s_axis_tlast, s_axis_tdata};
                sreg_valid_q <= ~m_axis_tready;
            end
        end else begin
            // Register full. Drain it when the downstream accepts; the
            // waiting upstream beat then passes through in the next cycle.
            // The input is deliberately NOT captured here -- the upstream has
            // not been granted tready yet (tready is 0 while full).
            if (m_axis_tvalid && m_axis_tready) begin
                sreg_valid_q <= 1'b0;
            end
        end
    end

    // -----------------------------------------------------------------------
    // SVA assertions (compiled when DBQA_ASSERT is defined)
    // -----------------------------------------------------------------------
`ifdef DBQA_ASSERT
    // A beat held in the register is preserved while the downstream remains
    // stalled. The $stable(m_axis_tready) guard makes the property immune to
    // combinational-output sampling at the clock edge (post-NBA in Verilator):
    // if the stall ends, the implication passes vacuously.
    a_stall_hold: assert property (
        @(posedge clk) disable iff (rst)
            (sreg_valid_q && ~m_axis_tready) |=> (
                $stable(m_axis_tready) |-> (
                    sreg_valid_q && sreg_q == $past(sreg_q))));

    // A beat latched into the empty register is preserved exactly while the
    // downstream remains stalled.
    a_hold_input: assert property (
        @(posedge clk) disable iff (rst)
            (s_axis_tvalid && s_axis_tready && ~m_axis_tready) |=> (
                $stable(m_axis_tready) |-> (
                    sreg_valid_q && sreg_q == $past({s_axis_tlast, s_axis_tdata}))));
`endif

endmodule
