// ===========================================================================
//  db_pkg.sv -- global types, parameters and configuration records for the
//  DBQA database query accelerator.
//
//  This package is the single source of truth for datapath widths, operation
//  encodings and the configuration structures that the AXI-Lite control plane
//  programs. Every pipeline module defaults its parameters from here, so a
//  width change (e.g. 16 -> 64 bit columns) propagates through the design
//  without editing RTL.
//
//  AXI-Stream conventions used across the pipeline:
//    * tvalid / tready handshake, one beat per row
//    * tdata  = $bits(pipeline_data_t)
//    * tlast  = end-of-stream marker for one query (triggers result capture)
//
//  Note: interface modules (axis_fifo, axis_register) stay fully generic and
//  take DATA_W as a parameter -- they intentionally do not depend on this
//  package so they can be reused as-is for any AXI-Stream payload.
// ===========================================================================

package db_pkg;

  // -----------------------------------------------------------------------
  // Global defaults (overridable per-instance through parameter defaults,
  // e.g. `parameter COLUMN_WIDTH = db_pkg::COLUMN_WIDTH`).
  // -----------------------------------------------------------------------
  localparam int COLUMN_WIDTH = 32;  // bits per column datum
  localparam int NUM_ROWS = 1024;  // rows per table
  localparam int NUM_COLS = 4;  // columns per table
  localparam int GROUP_BY_BUCKETS = 256;  // GROUP BY hash bucket count

  // -----------------------------------------------------------------------
  // Derived widths
  // -----------------------------------------------------------------------
  // Address width for NUM_ROWS rows.
  localparam int COLUMN_ADDR_W = (NUM_ROWS <= 2) ? 1 : $clog2(NUM_ROWS);

  // Address width for GROUP_BY_BUCKETS hash buckets.
  localparam int GROUP_BY_ADDR_W = (GROUP_BY_BUCKETS <= 2) ? 1 : $clog2(GROUP_BY_BUCKETS);

  // Accumulator width: COLUMN_WIDTH + log2(NUM_ROWS) bits are enough to hold
  // the worst-case SUM over every row (all 0xFFFF..FFFF). Exposes the
  // configurable accumulator widths required by the design.
  localparam int ACCUM_WIDTH = COLUMN_WIDTH + COLUMN_ADDR_W;

  // Width of a beat carrying every column of one row.
  localparam int COLUMN_VECTOR_W = NUM_COLS * COLUMN_WIDTH;

  // -----------------------------------------------------------------------
  // Operations
  // -----------------------------------------------------------------------

  // High-level query opcode, programmed by the CPU over AXI-Lite.
  typedef enum logic [2:0] {
    OP_SELECT  = 3'd0,
    OP_COUNT   = 3'd1,
    OP_SUM     = 3'd2,
    OP_MIN     = 3'd3,
    OP_MAX     = 3'd4,
    OP_AVG     = 3'd5,
    OP_GROUPBY = 3'd6
  } opcode_e;

  // Predicate comparison operators (WHERE clause).
  typedef enum logic [2:0] {
    PRED_EQ  = 3'd0,
    PRED_NEQ = 3'd1,
    PRED_LT  = 3'd2,
    PRED_GT  = 3'd3,
    PRED_LTE = 3'd4,
    PRED_GTE = 3'd5
  } pred_op_e;

  // Boolean combination of multiple predicate slots.
  typedef enum logic {
    LOGIC_AND = 1'b0,
    LOGIC_OR  = 1'b1
  } logic_op_e;

  // -----------------------------------------------------------------------
  // Packed datapath types
  // -----------------------------------------------------------------------

  // All NUM_COLS columns of one row on one bus. `vec[i]` selects the i-th
  // column and occupies the least-significant bits of the vector.
  typedef logic [NUM_COLS-1:0][COLUMN_WIDTH-1:0] column_vector_t;

  // Pipeline beat: one row's columns plus the predicate pass/fail mask.
  // Flows between stages over AXI-Stream.
  typedef struct packed {
    column_vector_t columns;
    logic           pass;     // 1 = row satisfied the WHERE clause
  } pipeline_data_t;

  // Projection mask: bit i set => column i is projected (SELECT list).
  typedef logic [NUM_COLS-1:0] proj_mask_t;

  // AXI-Stream tdata width used by every pipeline stage.
  localparam int AXIS_DATA_W = $bits(pipeline_data_t);

  // -----------------------------------------------------------------------
  // Configuration records (programmed through the AXI-Lite register file)
  // -----------------------------------------------------------------------

  // One predicate slot. Slots combine left to right: the first *enabled*
  // slot seeds the result (its `combine` field is ignored); every later
  // enabled slot folds in with `combine`; disabled slots are skipped. This
  // makes (p0) AND (p1) OR (p2) expressible with enable = {1,1,1} and
  // combine = {-, AND, OR}.
  typedef struct packed {
    logic                     enable;   // 1 = slot active
    pred_op_e                 op;       // comparator
    logic_op_e                combine;  // fold with previous active slot
    logic [COLUMN_ADDR_W-1:0] column;   // column under test
    logic [COLUMN_WIDTH-1:0]  imm;      // immediate operand
  } pred_cfg_t;

  // Aggregation configuration.
  typedef struct packed {
    opcode_e                  op;       // COUNT / SUM / MIN / MAX / AVG
    logic                     groupby;  // 1 = group by key column
    logic [COLUMN_ADDR_W-1:0] column;   // column to aggregate
    logic [COLUMN_ADDR_W-1:0] gby_key;  // GROUP BY key column
  } agg_cfg_t;

  // -----------------------------------------------------------------------
  // Combinational helpers (single source of comparator semantics, shared by
  // every predicate slot and the software reference model).
  // -----------------------------------------------------------------------

  // Evaluate one comparison: `a op b`.
  function automatic logic pred_apply(input pred_op_e op, input logic [COLUMN_WIDTH-1:0] a,
                                      input logic [COLUMN_WIDTH-1:0] b);
    case (op)
      PRED_EQ:  return (a == b);
      PRED_NEQ: return (a != b);
      PRED_LT:  return (a < b);
      PRED_GT:  return (a > b);
      PRED_LTE: return (a <= b);
      PRED_GTE: return (a >= b);
      default:  return 1'b0;
    endcase
  endfunction

  // Fold two predicate results with the requested logic operator.
  function automatic logic pred_combine(input logic lhs, input logic_op_e op, input logic rhs);
    return (op == LOGIC_AND) ? (lhs & rhs) : (lhs | rhs);
  endfunction

endpackage
