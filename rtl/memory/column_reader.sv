// ===========================================================================
//  column_reader.sv -- streaming columnar table reader.
//
//  Streams every row of a columnar table out as one AXI-Stream beat per row:
//  tdata = {pass=1, col[NUM_COLS-1], ..., col[0]} with tlast on the final
//  row. NUM_COLS column banks are read in parallel (NUM_COLS words/cycle of
//  memory bandwidth).
//
//  Read timing:
//    * the column banks use a synchronous read (1-cycle latency): bank_rdata
//      returns mem[raddr] one cycle after raddr is applied
//    * raddr_q tracks the row currently on bank_rdata. After a fetch
//      (raddr advances) the RAM re-reads the previous address for one cycle,
//      so bank_rdata is "stale" every other cycle; the pending_q gate pushes
//      only the fresh rows. Sustained throughput is therefore 1 row / 2
//      cycles -- the correct, lossless trade-off for a 1-cycle-latency BRAM
//      with arbitrary backpressure (a fetch-ahead design would overrun the
//      single bank_rdata slot on a stall)
//
//  Backpressure: rows are pushed into an output FIFO (axis_fifo). When it is
//  full the reader holds raddr_q and bank_rdata, so no data is lost or
//  duplicated under any ready/valid pattern.
//
//  Control:
//    * start (1-cycle pulse) begins streaming from row 0
//    * busy is high while rows are being pushed
//    * done pulses (level) once the final row has been pushed into the FIFO
//    * queries are serialized: assert start only after done, and consume the
//      previous query's output before starting the next (the FIFO must not be
//      full when a new query starts)
//
//  The load and stream phases never overlap: load the table (load_wen=1)
//  with start deasserted, then assert start.
//
//  Assertions (SVA) are compiled only when DBQA_ASSERT is defined.
// ===========================================================================

module column_reader #(
    parameter int NUM_COLS     = db_pkg::NUM_COLS,
    parameter int COLUMN_WIDTH = db_pkg::COLUMN_WIDTH,
    parameter int NUM_ROWS     = db_pkg::NUM_ROWS,
    parameter int ADDR_W       = (NUM_ROWS <= 2) ? 1 : $clog2(NUM_ROWS),
    parameter int FIFO_DEPTH   = 4
) (
    input logic clk,
    input logic rst,

    // Load port: writes one whole row across all banks.
    input logic                    load_wen,
    input logic [      ADDR_W-1:0] load_addr,
    input logic [COLUMN_WIDTH-1:0] load_data[NUM_COLS],

    // Control
    input  logic start,
    output logic busy,
    output logic done,

    // AXI-Stream master
    output logic                  m_axis_tvalid,
    input  logic                  m_axis_tready,
    output logic [OUT_DATA_W-1:0] m_axis_tdata,
    output logic                  m_axis_tlast
);

  localparam int COL_VEC_W = NUM_COLS * COLUMN_WIDTH;
  localparam int OUT_DATA_W = COL_VEC_W + 1;
  localparam int LADDR = NUM_ROWS - 1;

  initial begin
    if (NUM_COLS < 1) $error("column_reader: NUM_COLS must be >= 1");
    if (NUM_ROWS < 1) $error("column_reader: NUM_ROWS must be >= 1");
    if (FIFO_DEPTH < 2) $error("column_reader: FIFO_DEPTH must be >= 2");
  end

  // -------------------------------------------------------------------------
  // Column banks. Port A: load (write). Port B: streaming read.
  // -------------------------------------------------------------------------
  logic [COLUMN_WIDTH-1:0] bank_rdata[NUM_COLS];
  logic [COLUMN_WIDTH-1:0] load_rdata_unused[NUM_COLS];

  genvar g;
  generate
    for (g = 0; g < NUM_COLS; g++) begin : gen_bank
      column_memory #(
          .DATA_W(COLUMN_WIDTH),
          .DEPTH (NUM_ROWS)
      ) u_bank (
          .clk(clk),
          .a_wen(load_wen),
          .a_addr(load_addr),
          .a_wdata(load_data[g]),
          .a_rdata(load_rdata_unused[g]),
          .b_wen(1'b0),
          .b_addr(raddr_q),
          .b_wdata('0),
          .b_rdata(bank_rdata[g])
      );
    end
  endgenerate

  // -------------------------------------------------------------------------
  // Reader FSM. raddr_q is the row currently on bank_rdata; pending_q gates
  // the push onto the fresh (non-stale) cycles.
  // -------------------------------------------------------------------------
  logic [ADDR_W-1:0] raddr_q;
  logic [ADDR_W-1:0] ret_row_q;  // row whose data is on bank_rdata
  logic              pending_q;  // bank_rdata holds a fresh, un-pushed row
  logic              busy_q;
  logic              done_q;

  logic              fifo_s_ready;
  logic              fifo_tvalid;

  assign fifo_tvalid = pending_q && busy_q;

  always_ff @(posedge clk) begin
    if (rst) begin
      raddr_q   <= '0;
      ret_row_q <= '0;
      pending_q <= 1'b0;
      busy_q    <= 1'b0;
      done_q    <= 1'b0;
    end else if (start) begin
      busy_q    <= 1'b1;
      done_q    <= 1'b0;
      raddr_q   <= '0;
      ret_row_q <= '0;
      pending_q <= 1'b1;  // row 0 is on bank_rdata (idle read of address 0)
    end else if (busy_q) begin
      ret_row_q <= raddr_q;
      if (pending_q && fifo_s_ready) begin
        // A fresh row (bank_rdata, row ret_row_q) was accepted by the FIFO.
        if (ret_row_q == ADDR_W'(LADDR)) begin
          busy_q    <= 1'b0;
          done_q    <= 1'b1;
          raddr_q   <= '0;
          ret_row_q <= '0;
          pending_q <= 1'b0;
        end else begin
          raddr_q   <= raddr_q + 1'b1;  // fetch the next row
          pending_q <= 1'b0;  // the next cycle's read is stale
        end
      end else begin
        pending_q <= 1'b1;  // the next (fresh) read is ready
      end
    end
  end

  assign busy = busy_q;
  assign done = done_q;

  // -------------------------------------------------------------------------
  // Output FIFO: absorbs downstream backpressure.
  // -------------------------------------------------------------------------
  logic [COL_VEC_W-1:0] columns_packed;

  always_comb begin
    columns_packed = '0;
    for (int c = 0; c < NUM_COLS; c++) columns_packed[c*COLUMN_WIDTH+:COLUMN_WIDTH] = bank_rdata[c];
  end

  localparam int FCOUNT_W = (FIFO_DEPTH <= 1) ? 1 : $clog2(FIFO_DEPTH + 1);
  /* verilator lint_off UNUSEDSIGNAL */
  logic [FCOUNT_W-1:0] fifo_count_unused;  // FIFO fill level (not used here)
  /* verilator lint_on UNUSEDSIGNAL */

  axis_fifo #(
      .DEPTH (FIFO_DEPTH),
      .DATA_W(OUT_DATA_W)
  ) u_out_fifo (
      .clk          (clk),
      .rst          (rst),
      .s_axis_tvalid(fifo_tvalid),
      .s_axis_tready(fifo_s_ready),
      .s_axis_tdata ({1'b1, columns_packed}),       // pass=1 in the MSB
      .s_axis_tlast (ret_row_q == ADDR_W'(LADDR)),
      .m_axis_tvalid(m_axis_tvalid),
      .m_axis_tready(m_axis_tready),
      .m_axis_tdata (m_axis_tdata),
      .m_axis_tlast (m_axis_tlast),
      .count        (fifo_count_unused)
  );

`ifdef DBQA_ASSERT
  // busy and done are mutually exclusive.
  m_busy_done_excl :
  assert property (@(posedge clk) disable iff (rst) (~(busy_q && done_q)));

  // Row indices stay within the table.
  m_row_in_range :
  assert property (@(posedge clk) disable iff (rst) (busy_q |-> (ret_row_q <= ADDR_W'(LADDR))));
  m_raddr_in_range :
  assert property (@(posedge clk) disable iff (rst) (busy_q |-> (raddr_q <= ADDR_W'(LADDR))));
`endif

endmodule
