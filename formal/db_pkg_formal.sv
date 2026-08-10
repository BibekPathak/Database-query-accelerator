// ===========================================================================
//  db_pkg_formal.sv -- formal-only replica of rtl/common/db_pkg.sv.
//
//  The yosys SystemVerilog frontend used by the SymbiYosys flow rejects
//  `$bits()`, which the real package uses to derive AXIS_DATA_W. This replica
//  hardcodes the width of the default configuration instead.
//
//  IMPORTANT: keep every value in lockstep with rtl/common/db_pkg.sv. It is
//  only read by the formal wrappers in this directory; simulation uses the
//  real package.
// ===========================================================================

package db_pkg;

  localparam int COLUMN_WIDTH = 32;  // bits per column datum
  localparam int NUM_ROWS = 1024;  // rows per table
  localparam int NUM_COLS = 4;  // columns per table
  localparam int NUM_PRED = 2;  // predicate slots per query
  localparam int GROUP_BY_BUCKETS = 256;  // GROUP BY hash bucket count

  localparam int COLUMN_ADDR_W = (NUM_ROWS <= 2) ? 1 : $clog2(NUM_ROWS);
  localparam int GROUP_BY_ADDR_W = (GROUP_BY_BUCKETS <= 2) ? 1 : $clog2(GROUP_BY_BUCKETS);
  localparam int ACCUM_WIDTH = COLUMN_WIDTH + COLUMN_ADDR_W;
  localparam int COLUMN_VECTOR_W = NUM_COLS * COLUMN_WIDTH;

  // $bits(pipeline_data_t) of the default configuration: 1 + 4 * 32.
  localparam int AXIS_DATA_W = COLUMN_VECTOR_W + 1;

  typedef enum logic [2:0] {
    OP_SELECT  = 3'd0,
    OP_COUNT   = 3'd1,
    OP_SUM     = 3'd2,
    OP_MIN     = 3'd3,
    OP_MAX     = 3'd4,
    OP_AVG     = 3'd5,
    OP_GROUPBY = 3'd6
  } opcode_e;

  typedef enum logic [2:0] {
    PRED_EQ  = 3'd0,
    PRED_NEQ = 3'd1,
    PRED_LT  = 3'd2,
    PRED_GT  = 3'd3,
    PRED_LTE = 3'd4,
    PRED_GTE = 3'd5
  } pred_op_e;

  typedef enum logic {
    LOGIC_AND = 1'b0,
    LOGIC_OR  = 1'b1
  } logic_op_e;

  typedef logic [NUM_COLS-1:0][COLUMN_WIDTH-1:0] column_vector_t;

  typedef struct packed {
    logic           pass;
    column_vector_t columns;
  } pipeline_data_t;

  typedef logic [NUM_COLS-1:0] proj_mask_t;

  typedef struct packed {
    logic                     enable;
    pred_op_e                 op;
    logic_op_e                combine;
    logic [COLUMN_ADDR_W-1:0] column;
    logic [COLUMN_WIDTH-1:0]  imm;
  } pred_cfg_t;

  typedef struct packed {
    opcode_e                  op;
    logic                     groupby;
    logic [COLUMN_ADDR_W-1:0] column;
    logic [COLUMN_ADDR_W-1:0] gby_key;
  } agg_cfg_t;

  typedef struct packed {
    logic [COLUMN_ADDR_W-1:0] num_rows;
  } query_cfg_t;

  typedef struct packed {
    logic busy;
    logic done;
    logic error;
  } status_t;

  typedef enum logic [1:0] {
    ERR_NONE         = 2'd0,
    ERR_START_BUSY   = 2'd1,
    ERR_AGG_OVERFLOW = 2'd2
  } error_e;

  localparam int REG_CTRL       = 0;
  localparam int REG_STATUS     = 1;
  localparam int REG_QUERY      = 2;
  localparam int REG_AGG_CFG    = 3;
  localparam int REG_PROJ_MASK  = 4;
  localparam int REG_PRED_BASE  = 8;
  localparam int REG_PRED_WORDS = 2;
  localparam int REG_LOAD_ADDR  = 32'h20;
  localparam int REG_LOAD_DATA0 = 32'h21;
  localparam int REG_LOAD_ROW   = REG_LOAD_DATA0 + NUM_COLS;
  localparam int REG_RESULT     = 32'h30;
  localparam int REG_RESULT_HI  = 32'h31;
  localparam int REG_COUNT      = 32'h32;
  localparam int REG_COUNT_HI   = 32'h33;
  localparam int REG_OVERFLOW   = 32'h34;

  localparam int CTRL_START     = 0;
  localparam int CTRL_ABORT     = 1;

  localparam int STATUS_BUSY    = 0;
  localparam int STATUS_DONE    = 1;
  localparam int STATUS_ERROR   = 2;

endpackage
