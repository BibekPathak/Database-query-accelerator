// ===========================================================================
//  axi_lite_slave.sv -- AXI-Lite slave and register file for the DBQA
//  accelerator.
//
//  Implements the AXI-Lite register map defined in db_pkg (word offsets;
//  byte address = offset << 2). It is the bridge between the CPU / Python
//  control plane and the query scheduler:
//
//    writes   REG_CTRL         [0] start pulse, [1] abort pulse
//             REG_QUERY        query_cfg_t (num_rows)
//             REG_AGG_CFG      agg_cfg_t
//             REG_PROJ_MASK    proj_mask_t
//             REG_PRED_BASE    predicate slots, two words each
//             REG_LOAD_ADDR    row address to load
//             REG_LOAD_DATA0   +c for column c
//             REG_LOAD_ROW     commit strobe (load_wen pulse)
//    reads    REG_STATUS       {error, done, busy}
//             REG_RESULT / HI  classic aggregation result
//             REG_COUNT / HI   aggregate row count
//             REG_OVERFLOW     [0] = SUM overflow
//
//  The control and load strobes are single-cycle registered pulses: write the
//  register, and one cycle later the scheduler/reader samples the strobe with
//  the already-latched configuration/address/data.
//
//  AXI-Lite notes: 32-bit data, full-word writes only (wstrb is ignored),
//  and the write channel accepts AW and W in the same cycle.
//
//  Assertions (SVA) are compiled only when DBQA_ASSERT is defined.
// ===========================================================================

module axi_lite_slave #(
    parameter int NUM_PRED   = db_pkg::NUM_PRED,
    parameter int NUM_COLS   = db_pkg::NUM_COLS,
    parameter int ACCUM_W    = db_pkg::ACCUM_WIDTH,
    parameter int COL_ADDR_W = db_pkg::COLUMN_ADDR_W
) (
    input logic clk,
    input logic rst,

    // AXI-Lite write channel
    input  logic        s_axil_awvalid,
    output logic        s_axil_awready,
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [31:0] s_axil_awaddr,
    input  logic        s_axil_wvalid,
    output logic        s_axil_wready,
    input  logic [31:0] s_axil_wdata,
    input  logic [ 3:0] s_axil_wstrb,    // full-word writes only
    /* verilator lint_on UNUSEDSIGNAL */
    output logic        s_axil_bvalid,
    input  logic        s_axil_bready,
    output logic [ 1:0] s_axil_bresp,

    // AXI-Lite read channel
    input  logic        s_axil_arvalid,
    output logic        s_axil_arready,
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [31:0] s_axil_araddr,
    /* verilator lint_on UNUSEDSIGNAL */
    output logic        s_axil_rvalid,
    input  logic        s_axil_rready,
    output logic [31:0] s_axil_rdata,
    output logic [ 1:0] s_axil_rresp,

    // Query configuration (registered, exposed to the scheduler)
    output db_pkg::query_cfg_t query_cfg,
    output db_pkg::agg_cfg_t   agg_cfg,
    output db_pkg::pred_cfg_t  pred_cfg [NUM_PRED],
    output db_pkg::proj_mask_t proj_mask,

    // Control
    output logic start,  // 1-cycle pulse: begin the configured query
    output logic abort_req,  // 1-cycle pulse: abort request (top-level action)

    // Table load strobe (registered pulse on REG_LOAD_ROW)
    output logic                    load_wen,
    output logic [  COL_ADDR_W-1:0] load_addr,
    output logic [COLUMN_WIDTH-1:0] load_data[NUM_COLS],

    // Status from the scheduler (read-only registers)
    input db_pkg::error_e               error,
    input logic                         busy,
    input logic                         done,
    input logic           [ACCUM_W-1:0] result,
    input logic           [ACCUM_W-1:0] count,
    input logic                         overflow
);

  import db_pkg::*;

  localparam int PRED_CFG_W = $bits(pred_cfg_t);  // 47

  // Word offsets as 32-bit values so all comparisons stay width-clean.
  logic [31:0] aw_addr_word;
  logic [31:0] ar_addr_word;
  assign aw_addr_word = {18'd0, s_axil_awaddr[15:2]};
  assign ar_addr_word = {18'd0, araddr_q[15:2]};

  // -------------------------------------------------------------------------
  // AXI-Lite handshake state
  // -------------------------------------------------------------------------
  logic awready_q, wready_q, bvalid_q, bresp_q;
  logic arready_q, rvalid_q, rresp_q;
  /* verilator lint_off UNUSEDSIGNAL */
  logic [31:0] araddr_q;  // only [15:2] is decoded
  /* verilator lint_on UNUSEDSIGNAL */

  logic wr_cap;  // both AW and W accepted this cycle
  assign wr_cap = s_axil_awvalid && awready_q && s_axil_wvalid && wready_q;

  always_ff @(posedge clk) begin
    if (rst) begin
      awready_q <= 1'b1;
      wready_q  <= 1'b1;
      bvalid_q  <= 1'b0;
      bresp_q   <= 1'b0;
      arready_q <= 1'b1;
      rvalid_q  <= 1'b0;
      rresp_q   <= 1'b0;
      araddr_q  <= '0;
    end else begin
      // Write: accept AW+W together, then drive B until BREADY.
      if (wr_cap) begin
        awready_q <= 1'b0;
        wready_q  <= 1'b0;
        bvalid_q  <= 1'b1;
        bresp_q   <= 1'b0;  // OKAY
      end
      if (s_axil_bvalid && s_axil_bready) begin
        bvalid_q  <= 1'b0;
        awready_q <= 1'b1;
        wready_q  <= 1'b1;
      end

      // Read: accept AR, then drive R until RREADY.
      if (s_axil_arvalid && arready_q) begin
        arready_q <= 1'b0;
        araddr_q  <= s_axil_araddr;
        rvalid_q  <= 1'b1;
        rresp_q   <= 1'b0;  // OKAY
      end
      if (s_axil_rvalid && s_axil_rready) begin
        rvalid_q  <= 1'b0;
        arready_q <= 1'b1;
      end
    end
  end

  assign s_axil_awready = awready_q;
  assign s_axil_wready  = wready_q;
  assign s_axil_bvalid  = bvalid_q;
  assign s_axil_bresp   = {1'b0, bresp_q};
  assign s_axil_arready = arready_q;
  assign s_axil_rvalid  = rvalid_q;
  assign s_axil_rresp   = {1'b0, rresp_q};

  // -------------------------------------------------------------------------
  // Register file
  // -------------------------------------------------------------------------
  db_pkg::query_cfg_t                    query_cfg_q;
  db_pkg::agg_cfg_t                      agg_cfg_q;
  db_pkg::pred_cfg_t                     pred_cfg_q  [NUM_PRED];
  db_pkg::proj_mask_t                    proj_mask_q;
  logic               [  COL_ADDR_W-1:0] load_addr_q;
  logic               [COLUMN_WIDTH-1:0] load_data_q [NUM_COLS];
  logic                                  start_q;
  logic                                  abort_req_q;
  logic                                  load_wen_q;

  always_ff @(posedge clk) begin
    if (rst) begin
      query_cfg_q <= '0;
      agg_cfg_q   <= '0;
      proj_mask_q <= '0;
      load_addr_q <= '0;
      start_q     <= 1'b0;
      abort_req_q <= 1'b0;
      load_wen_q  <= 1'b0;
      for (int i = 0; i < NUM_PRED; i++) pred_cfg_q[i] <= '0;
      for (int c = 0; c < NUM_COLS; c++) load_data_q[c] <= '0;
    end else begin
      // Strobes are single-cycle registered pulses.
      start_q    <= 1'b0;
      abort_req_q <= 1'b0;
      load_wen_q <= 1'b0;

      if (wr_cap) begin
        case (aw_addr_word)
          REG_CTRL: begin
            if (s_axil_wdata[CTRL_START]) start_q <= 1'b1;
            if (s_axil_wdata[CTRL_ABORT]) abort_req_q <= 1'b1;
          end
          REG_QUERY:     query_cfg_q <= db_pkg::query_cfg_t'(s_axil_wdata);
          REG_AGG_CFG:   agg_cfg_q <= db_pkg::agg_cfg_t'(s_axil_wdata);
          REG_PROJ_MASK: proj_mask_q <= db_pkg::proj_mask_t'(s_axil_wdata);
          REG_LOAD_ADDR: load_addr_q <= s_axil_wdata[COL_ADDR_W-1:0];
          default:       ;
        endcase

        // Predicate slots: low word = imm, high word = {enable, op, combine,
        // column}.
        if (aw_addr_word >= REG_PRED_BASE && aw_addr_word < REG_PRED_BASE + 2 * NUM_PRED) begin
          if (aw_addr_word[0]) begin
            pred_cfg_q[(aw_addr_word - REG_PRED_BASE) >> 1][PRED_CFG_W-1:32] <= s_axil_wdata[PRED_CFG_W-1-32:0];
          end else begin
            pred_cfg_q[(aw_addr_word-REG_PRED_BASE)>>1][31:0] <= s_axil_wdata;
          end
        end

        // Load data words and the commit strobe.
        if (aw_addr_word >= REG_LOAD_DATA0 && aw_addr_word < REG_LOAD_DATA0 + NUM_COLS) begin
          load_data_q[aw_addr_word-REG_LOAD_DATA0] <= s_axil_wdata;
        end
        if (aw_addr_word == REG_LOAD_ROW) load_wen_q <= 1'b1;
      end
    end
  end

  assign query_cfg = query_cfg_q;
  assign agg_cfg   = agg_cfg_q;
  assign proj_mask = proj_mask_q;
  assign start     = start_q;
  assign abort_req = abort_req_q;
  assign load_wen  = load_wen_q;
  assign load_addr = load_addr_q;
  for (genvar i = 0; i < NUM_PRED; i++) begin : gen_pred_cfg_out
    assign pred_cfg[i] = pred_cfg_q[i];
  end
  for (genvar c = 0; c < NUM_COLS; c++) begin : gen_load_data_out
    assign load_data[c] = load_data_q[c];
  end

  // -------------------------------------------------------------------------
  // Read data mux
  // -------------------------------------------------------------------------
  logic [31:0] rdata;
  always_comb begin
    rdata = '0;
    case (ar_addr_word)
      REG_STATUS: rdata = {28'd0, error, done, busy};  // [0] busy [1] done [2:3] error
      REG_QUERY: rdata = {22'd0, query_cfg_q};
      REG_AGG_CFG: rdata = {8'd0, agg_cfg_q};
      REG_PROJ_MASK: rdata = {28'd0, proj_mask_q};
      REG_RESULT: rdata = result[31:0];
      REG_RESULT_HI: rdata = {{32 - (ACCUM_W - 32) {1'b0}}, result[ACCUM_W-1:32]};
      REG_COUNT: rdata = count[31:0];
      REG_COUNT_HI: rdata = {{32 - (ACCUM_W - 32) {1'b0}}, count[ACCUM_W-1:32]};
      REG_OVERFLOW: rdata = {31'd0, overflow};
      default: rdata = '0;
    endcase

    // Predicate slots (read-back): low word = imm, high word = cfg bits.
    if (ar_addr_word >= REG_PRED_BASE && ar_addr_word < REG_PRED_BASE + 2 * NUM_PRED) begin
      if (ar_addr_word[0]) begin
        rdata = {17'd0, pred_cfg_q[(ar_addr_word-REG_PRED_BASE)>>1][PRED_CFG_W-1:32]};
      end else begin
        rdata = pred_cfg_q[(ar_addr_word-REG_PRED_BASE)>>1][31:0];
      end
    end

    // Load registers (read-back).
    if (ar_addr_word == REG_LOAD_ADDR) rdata = {22'd0, load_addr_q};
    if (ar_addr_word >= REG_LOAD_DATA0 && ar_addr_word < REG_LOAD_DATA0 + NUM_COLS)
      rdata = load_data_q[ar_addr_word-REG_LOAD_DATA0];
  end
  assign s_axil_rdata = rdata;

`ifdef DBQA_ASSERT
  // Responses are always OKAY.
  m_bresp_okay :
  assert property (@(posedge clk) disable iff (rst) (bvalid_q |-> (bresp_q == 1'b0)));
  m_rresp_okay :
  assert property (@(posedge clk) disable iff (rst) (rvalid_q |-> (rresp_q == 1'b0)));

  // The register file is never overwritten out of order: a new AW+W pair is
  // only accepted once the previous B handshake has completed.
  m_write_serialized :
  assert property (@(posedge clk) disable iff (rst) (wr_cap |-> !bvalid_q));
`endif

endmodule
