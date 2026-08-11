// ===========================================================================
//  groupby_engine.sv -- hash-based streaming GROUP BY.
//
//  Groups the passing rows of a table scan by a key column and aggregates a
//  value column per group: COUNT, SUM (saturating), MIN and MAX. The per-
//  group results are streamed out after the scan as one beat per group.
//
//  Buckets:
//    * GROUP_BY_BUCKETS (default 256) fixed-size hash table in BRAM; each
//      bucket stores {valid, key, count, sum, min, max}
//    * hash = XOR-fold of the key's BUCKET_ADDR_W-bit chunks (see hash_key);
//      linear probing resolves
//      collisions (probe the next bucket on a valid key mismatch)
//    * if the probe chain is exhausted (all buckets probed), the row is
//      dropped -- a documented policy for a fixed-size hash table
//
//  The bucket read is a synchronous BRAM read (1-cycle latency, plus a
//  register on the returned data), so every access goes through a shared
//  issue / wait / process pipeline (rd_stage). Rows therefore cost a few
//  cycles each; the dump takes two passes over the buckets (a scan to find
//  the last valid bucket, then a present pass). This is the expected cost of
//  hashing and is the stretch feature of the design.
//
//  Control:
//    * start (1-cycle pulse) begins a new grouping (buckets are NOT cleared;
//      the caller must have consumed the previous query's groups)
//    * busy is high while aggregating or dumping
//    * done pulses (level) once every group has been streamed out
//    * s_axis_tready is asserted only while a new row can be accepted
//    * m_axis streams {key, count, sum, min, max} per group with tlast on
//      the final group
//
//  Assertions (SVA) are compiled only when DBQA_ASSERT is defined.
// ===========================================================================

module groupby_engine #(
    parameter int DATA_W        = db_pkg::AXIS_DATA_W,
    parameter int COLUMN_W      = db_pkg::COLUMN_WIDTH,
    parameter int ACCUM_W       = db_pkg::ACCUM_WIDTH,
    parameter int COL_ADDR_W    = db_pkg::COLUMN_ADDR_W,
    parameter int BUCKETS       = db_pkg::GROUP_BY_BUCKETS,
    parameter int BUCKET_ADDR_W = db_pkg::GROUP_BY_ADDR_W
) (
    input logic clk,
    input logic rst,

    input logic start,
    input logic [COL_ADDR_W-1:0] key_col,
    input logic [COL_ADDR_W-1:0] value_col,

    // AXI-Stream slave (input)
    input  logic              s_axis_tvalid,
    output logic              s_axis_tready,
    input  logic [DATA_W-1:0] s_axis_tdata,
    input  logic              s_axis_tlast,

    // AXI-Stream master (group results)
    output logic                   m_axis_tvalid,
    input  logic                   m_axis_tready,
    output logic [GB_RESULT_W-1:0] m_axis_tdata,
    output logic                   m_axis_tlast,

    // Control
    output logic done,
    output logic busy
);

  import db_pkg::*;

  typedef struct packed {
    logic                valid;
    logic [COLUMN_W-1:0] key;
    logic [ACCUM_W-1:0]  count;
    logic [ACCUM_W-1:0]  sum;
    logic [COLUMN_W-1:0] min;
    logic [COLUMN_W-1:0] max;
  } gb_bucket_t;

  typedef struct packed {
    logic [COLUMN_W-1:0] key;
    logic [ACCUM_W-1:0]  count;
    logic [ACCUM_W-1:0]  sum;
    logic [COLUMN_W-1:0] min;
    logic [COLUMN_W-1:0] max;
  } gb_result_t;

  localparam int GB_W        = $bits(gb_bucket_t);
  localparam int GB_RESULT_W = $bits(gb_result_t);

  typedef enum logic [2:0] {
    S_CLEAR,      // zero every bucket before the scan
    S_AGG_IDLE,   // accepting rows
    S_AGG_RD,     // bucket read for the current row (issue/wait/process)
    S_AGG_WRITE,  // bucket update write completing
    S_SCAN,       // pass 1: find the last valid bucket
    S_PRESENT     // pass 2: stream the valid groups out
  } state_e;

  // -------------------------------------------------------------------------
  // Bucket RAM (simple dual-port: read port + write port).
  // -------------------------------------------------------------------------
  logic       [         GB_W-1:0] bucket_mem[0:BUCKETS-1];
  gb_bucket_t                     rd_bucket;
  logic       [BUCKET_ADDR_W-1:0] raddr_q;
  logic                           wen_q;
  logic       [BUCKET_ADDR_W-1:0] waddr_q;
  logic       [         GB_W-1:0] wdata_q;

  always_ff @(posedge clk) begin
    if (rst) rd_bucket <= '0;
    else rd_bucket <= bucket_mem[raddr_q];
  end

  always_ff @(posedge clk) begin
    if (wen_q) bucket_mem[waddr_q] <= wdata_q;
  end

  // -------------------------------------------------------------------------
  // Control and pipeline state
  // -------------------------------------------------------------------------
  state_e                         state_q;
  logic                           busy_q;
  logic                           done_q;
  logic       [              1:0] rd_stage_q;  // 0 = issue, 1 = wait, 2 = data ready

  logic       [     COLUMN_W-1:0] row_key_q;
  logic       [     COLUMN_W-1:0] row_val_q;
  logic                           row_last_q;

  logic       [BUCKET_ADDR_W-1:0] probe_idx_q;
  logic       [  BUCKET_ADDR_W:0] probe_count_q;  // bounds the probe chain
  logic       [BUCKET_ADDR_W-1:0] dump_idx_q;
  logic       [BUCKET_ADDR_W-1:0] last_valid_q;
  logic       [BUCKET_ADDR_W-1:0] clear_idx_q;

  logic                           present_hold_q;
  logic                           present_is_last_q;
  gb_result_t                     present_out_q;

  assign s_axis_tready = busy_q && (state_q == S_AGG_IDLE);

  pipeline_data_t beat;
  assign beat = pipeline_data_t'(s_axis_tdata);

  // Hash: XOR-fold the key's four BUCKET_ADDR_W-bit chunks into the bucket
  // address (valid for the default COLUMN_W == 4*BUCKET_ADDR_W). Folding
  // rather than taking the low bits removes the low-byte bias so keys that
  // share a low byte land in different buckets.
  function automatic logic [BUCKET_ADDR_W-1:0] hash_key(input logic [COLUMN_W-1:0] k);
    return k[BUCKET_ADDR_W-1:0]
         ^ k[2*BUCKET_ADDR_W-1:BUCKET_ADDR_W]
         ^ k[3*BUCKET_ADDR_W-1:2*BUCKET_ADDR_W]
         ^ k[4*BUCKET_ADDR_W-1:3*BUCKET_ADDR_W];
  endfunction

  // -------------------------------------------------------------------------
  // Bucket update for the row currently being read (place or fold).
  // -------------------------------------------------------------------------
  logic [ACCUM_W:0] sum_ext;
  assign sum_ext = {1'b0, rd_bucket.sum} + {{(ACCUM_W - COLUMN_W) {1'b0}}, row_val_q};

  gb_bucket_t updated_bucket;
  always_comb begin
    if (!rd_bucket.valid) begin
      updated_bucket.valid = 1'b1;
      updated_bucket.key   = row_key_q;
      updated_bucket.count = ACCUM_W'(1);
      updated_bucket.sum   = {{(ACCUM_W - COLUMN_W) {1'b0}}, row_val_q};
      updated_bucket.min   = row_val_q;
      updated_bucket.max   = row_val_q;
    end else begin
      updated_bucket = rd_bucket;
      updated_bucket.count = rd_bucket.count + ACCUM_W'(1);
      if (sum_ext[ACCUM_W]) begin
        updated_bucket.sum = {ACCUM_W{1'b1}};  // saturate
      end else begin
        updated_bucket.sum = sum_ext[ACCUM_W-1:0];
      end
      if (row_val_q < updated_bucket.min) updated_bucket.min = row_val_q;
      if (row_val_q > updated_bucket.max) updated_bucket.max = row_val_q;
    end
  end

  // -------------------------------------------------------------------------
  // Main state machine
  // -------------------------------------------------------------------------
  always_ff @(posedge clk) begin
    if (rst) begin
      state_q           <= S_AGG_IDLE;
      busy_q            <= 1'b0;
      done_q            <= 1'b0;
      rd_stage_q        <= 2'd0;
      wen_q             <= 1'b0;
      waddr_q           <= '0;
      wdata_q           <= '0;
      row_key_q         <= '0;
      row_val_q         <= '0;
      row_last_q        <= 1'b0;
      probe_idx_q       <= '0;
      probe_count_q     <= '0;
      dump_idx_q        <= '0;
      last_valid_q      <= '0;
      clear_idx_q       <= '0;
      present_hold_q    <= 1'b0;
      present_is_last_q <= 1'b0;
      present_out_q     <= '0;
    end else if (start) begin
      state_q        <= S_CLEAR;
      busy_q         <= 1'b1;
      done_q         <= 1'b0;
      rd_stage_q     <= 2'd0;
      wen_q          <= 1'b0;
      clear_idx_q    <= '0;
      present_hold_q <= 1'b0;
    end else if (busy_q) begin
      case (state_q)
        // ---------------------------------------------------------------
        S_CLEAR: begin
          wen_q   <= 1'b1;
          waddr_q <= clear_idx_q;
          wdata_q <= '0;
          if (clear_idx_q == BUCKET_ADDR_W'(BUCKETS - 1)) begin
            state_q <= S_AGG_IDLE;
          end else begin
            clear_idx_q <= clear_idx_q + 1'b1;
          end
        end

        // ---------------------------------------------------------------
        S_AGG_IDLE: begin
          wen_q <= 1'b0;
          if (s_axis_tvalid) begin
            row_last_q <= s_axis_tlast;
            if (beat.pass) begin
              row_key_q     <= beat.columns[key_col];
              row_val_q     <= beat.columns[value_col];
              probe_idx_q   <= hash_key(beat.columns[key_col]);
              probe_count_q <= '1;
              raddr_q       <= hash_key(beat.columns[key_col]);
              rd_stage_q    <= 2'd1;  // issue; data ready 2 cycles later
              state_q       <= S_AGG_RD;
            end else if (s_axis_tlast) begin
              state_q    <= S_SCAN;
              dump_idx_q <= '0;
              rd_stage_q <= 2'd0;
            end
          end
        end

        // ---------------------------------------------------------------
        S_AGG_RD: begin
          if (rd_stage_q == 2'd2) begin
            // rd_bucket holds bucket_mem[probe_idx].
            if (rd_bucket.valid && (rd_bucket.key != row_key_q)) begin
              if (probe_count_q == (BUCKET_ADDR_W + 1)'(BUCKETS)) begin
                // Probe chain exhausted: drop this row (documented policy).
                if (row_last_q) begin
                  state_q    <= S_SCAN;
                  dump_idx_q <= '0;
                  rd_stage_q <= 2'd0;
                end else begin
                  state_q <= S_AGG_IDLE;
                end
              end else begin
                probe_idx_q   <= probe_idx_q + 1'b1;
                probe_count_q <= probe_count_q + 1'b1;
                raddr_q       <= probe_idx_q + 1'b1;
                rd_stage_q    <= 2'd1;  // re-issue the next probe
              end
            end else begin
              wen_q   <= 1'b1;
              waddr_q <= probe_idx_q;
              wdata_q <= GB_W'(updated_bucket);
              state_q <= S_AGG_WRITE;
            end
          end else begin
            rd_stage_q <= rd_stage_q + 1'b1;
          end
        end

        // ---------------------------------------------------------------
        S_AGG_WRITE: begin
          wen_q <= 1'b0;
          if (row_last_q) begin
            state_q    <= S_SCAN;
            dump_idx_q <= '0;
            rd_stage_q <= 2'd0;
          end else begin
            state_q <= S_AGG_IDLE;
          end
        end

        // ---------------------------------------------------------------
        S_SCAN: begin
          if (rd_stage_q == 2'd2) begin
            // rd_bucket holds bucket_mem[dump_idx].
            if (rd_bucket.valid) last_valid_q <= dump_idx_q;
            if (dump_idx_q == BUCKET_ADDR_W'(BUCKETS - 1)) begin
              state_q    <= S_PRESENT;
              dump_idx_q <= '0;
              rd_stage_q <= 2'd0;
            end else begin
              dump_idx_q <= dump_idx_q + 1'b1;
              raddr_q    <= dump_idx_q + 1'b1;
              rd_stage_q <= 2'd1;  // issue the next bucket read
            end
          end else begin
            if (rd_stage_q == 2'd0) raddr_q <= dump_idx_q;
            rd_stage_q <= rd_stage_q + 1'b1;
          end
        end

        // ---------------------------------------------------------------
        S_PRESENT: begin
          if (present_hold_q) begin
            if (m_axis_tready) begin
              present_hold_q <= 1'b0;
              if (dump_idx_q == BUCKET_ADDR_W'(BUCKETS - 1)) begin
                busy_q <= 1'b0;
                done_q <= 1'b1;
              end else begin
                dump_idx_q <= dump_idx_q + 1'b1;
                raddr_q    <= dump_idx_q + 1'b1;
                rd_stage_q <= 2'd1;  // issue the next bucket read
              end
            end
          end else if (rd_stage_q == 2'd2) begin
            // rd_bucket holds bucket_mem[dump_idx].
            if (rd_bucket.valid) begin
              present_hold_q <= 1'b1;
              present_is_last_q <= (dump_idx_q == last_valid_q);
              present_out_q <= '{
                  rd_bucket.key,
                  rd_bucket.count,
                  rd_bucket.sum,
                  rd_bucket.min,
                  rd_bucket.max
              };
            end else if (dump_idx_q == BUCKET_ADDR_W'(BUCKETS - 1)) begin
              busy_q <= 1'b0;
              done_q <= 1'b1;
            end else begin
              dump_idx_q <= dump_idx_q + 1'b1;
              raddr_q    <= dump_idx_q + 1'b1;
              rd_stage_q <= 2'd1;
            end
          end else begin
            if (rd_stage_q == 2'd0) raddr_q <= dump_idx_q;
            rd_stage_q <= rd_stage_q + 1'b1;
          end
        end

        default: state_q <= state_q;  // unreachable (all states covered)
      endcase
    end
  end

  assign m_axis_tvalid = (state_q == S_PRESENT) && present_hold_q;
  assign m_axis_tdata  = present_out_q;
  assign m_axis_tlast  = present_is_last_q;
  assign busy          = busy_q;
  assign done          = done_q;

`ifdef DBQA_ASSERT
  // The probe index stays within the bucket table.
  m_probe_in_range :
  assert property (@(posedge clk) disable iff (rst)
      (state_q == S_AGG_RD) |-> (probe_idx_q <= BUCKET_ADDR_W'(BUCKETS - 1)));

  // A bucket's count never exceeds the number of rows in the table.
  /* verilator lint_off UNUSEDSIGNAL */
  gb_bucket_t wr_bucket;
  assign wr_bucket = gb_bucket_t'(wdata_q);
  /* verilator lint_on UNUSEDSIGNAL */
  m_bucket_count_bounded :
  assert property (@(posedge clk) disable iff (rst)
      (wen_q |-> (wr_bucket.count <= ACCUM_W'(NUM_ROWS))));
`endif

endmodule
