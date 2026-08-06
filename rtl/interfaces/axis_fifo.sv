// ===========================================================================
//  axis_fifo.sv -- parameterized AXI-Stream FIFO with end-of-stream marker.
//
//  Ready/valid first-word-fall-through (FWFT) FIFO:
//    * the first word written to an empty FIFO is presented on the output
//      register without a preceding read pulse (no first-beat bubble)
//    * occupancy is tracked with an explicit counter; full/empty are
//      combinational, derived from that counter
//    * DEPTH does not need to be a power of two (pointers wrap explicitly)
//    * storage is a single always_ff RAM; synthesis tools infer distributed
//      RAM for shallow FIFOs and block RAM (BRAM) for DEPTH >= 64
//    * tlast is stored alongside each word and preserves ordering
//
//  Protocol (AXI-Stream):
//    * a beat transfers on every cycle where s_axis_tvalid && s_axis_tready
//      (and likewise on the master side)
//    * s_axis_tready is asserted only while the FIFO is not full
//    * m_axis_tvalid is asserted only while the FIFO is not empty
//
//  Assertions (SVA) are compiled only when DBQA_ASSERT is defined.
// ===========================================================================

module axis_fifo #(
    parameter int DEPTH  = 16,
    parameter int DATA_W = 32
) (
    input  logic                clk,
    input  logic                rst,

    // AXI-Stream slave (write) side
    input  logic                s_axis_tvalid,
    output logic                s_axis_tready,
    input  logic [DATA_W-1:0]   s_axis_tdata,
    input  logic                s_axis_tlast,

    // AXI-Stream master (read) side
    output logic                m_axis_tvalid,
    input  logic                m_axis_tready,
    output logic [DATA_W-1:0]   m_axis_tdata,
    output logic                m_axis_tlast
);

    localparam int ADDR_W   = (DEPTH <= 1) ? 1 : $clog2(DEPTH);
    localparam int COUNT_W  = (DEPTH <= 1) ? 1 : $clog2(DEPTH + 1);

    initial begin
        if (DEPTH < 1) $error("axis_fifo: DEPTH must be >= 1");
        if (DATA_W < 1) $error("axis_fifo: DATA_W must be >= 1");
    end

    // Storage: {tlast, tdata} packed into DATA_W+1 bits.
    logic [DATA_W:0]  mem [0:DEPTH-1];

    logic [ADDR_W-1:0]  wptr;
    logic [ADDR_W-1:0]  rptr;
    logic [COUNT_W-1:0] count;
    logic [DATA_W:0]    dout_q;   // output register (FWFT)

    // -----------------------------------------------------------------------
    // Handshake events and status
    // -----------------------------------------------------------------------
    logic push, pop, full, empty;

    assign push  = s_axis_tvalid & s_axis_tready;
    assign pop   = m_axis_tvalid & m_axis_tready;

    assign full  = (count == COUNT_W'(DEPTH));
    assign empty = (count == {COUNT_W{1'b0}});

    // Forward the incoming word straight to the output when the push is the
    // event that (re)populates the FIFO: it was empty, or the same cycle's pop
    // drained the last remaining word (count drops to 0 then back to 1).
    logic fw_forward;
    assign fw_forward = push && (count == (pop ? COUNT_W'(1) : '0));

    assign s_axis_tready = ~full;
    assign m_axis_tvalid = ~empty;
    assign m_axis_tdata  = dout_q[DATA_W-1:0];
    assign m_axis_tlast  = dout_q[DATA_W];

    // -----------------------------------------------------------------------
    // Core logic
    // -----------------------------------------------------------------------
    always_ff @(posedge clk) begin
        if (rst) begin
            wptr   <= '0;
            rptr   <= '0;
            count  <= '0;
            dout_q <= '0;
        end else begin
            // Write path: store the incoming word.
            if (push) begin
                mem[wptr] <= {s_axis_tlast, s_axis_tdata};
                wptr      <= (wptr == ADDR_W'(DEPTH-1)) ? '0 : wptr + 1'b1;
            end

            // Read path: on pop, present the next word.
            if (pop) begin
                rptr   <= (rptr == ADDR_W'(DEPTH-1)) ? '0 : rptr + 1'b1;
                dout_q <= mem[rptr];
            end

            // Direct forwarding: the push is the event that (re)populates the
            // output register -- either the FIFO was empty, or the pop just
            // drained the last word. In both cases the incoming word must be
            // presented instead of reading a stale memory slot, and the read
            // pointer must skip the slot it was just written to.
            if (fw_forward) begin
                dout_q <= {s_axis_tlast, s_axis_tdata};
                rptr   <= (wptr == ADDR_W'(DEPTH-1)) ? '0 : wptr + 1'b1;
            end

            // Occupancy tracking.
            if (push && pop) begin
                count <= count;
            end else if (push) begin
                count <= count + 1'b1;
            end else if (pop) begin
                count <= count - 1'b1;
            end
        end
    end

    // -----------------------------------------------------------------------
    // SVA assertions (compiled when DBQA_ASSERT is defined)
    // -----------------------------------------------------------------------
`ifdef DBQA_ASSERT
    // Occupancy never exceeds the physical depth.
    m_count_in_range: assert property (
        @(posedge clk) disable iff (rst) (count <= COUNT_W'(DEPTH)));

    // A transfer only ever happens when the FIFO can accept / provide a word.
    m_push_never_full: assert property (
        @(posedge clk) disable iff (rst) (push |-> ~full));
    m_pop_never_empty: assert property (
        @(posedge clk) disable iff (rst) (pop |-> ~empty));

    // Count is monotone with respect to the push/pop events.
    m_count_stable_no_transfer: assert property (
        @(posedge clk) disable iff (rst)
            ((push & pop) | ~(push | pop)) |-> $stable(count));
    m_count_up_on_push: assert property (
        @(posedge clk) disable iff (rst)
            (push & ~pop) |-> (count == $past(count) + 1'b1));
    m_count_down_on_pop: assert property (
        @(posedge clk) disable iff (rst)
            (~push & pop) |-> (count == $past(count) - 1'b1));

    // Handshake validity matches the observed occupancy.
    m_valid_iff_not_empty: assert property (
        @(posedge clk) disable iff (rst)
            (m_axis_tvalid == ~empty));
    m_ready_iff_not_full: assert property (
        @(posedge clk) disable iff (rst)
            (s_axis_tready == ~full));
`endif

endmodule
