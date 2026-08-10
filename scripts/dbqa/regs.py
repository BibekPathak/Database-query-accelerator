"""DBQA accelerator register map and configuration packing.

Mirrors ``rtl/common/db_pkg.sv``. Offsets are 32-bit word offsets (the AXI-Lite
byte address is ``offset << 2``); these constants are the single source of
truth for the software control plane. Keep in lockstep with the RTL package.
"""

# ---------------------------------------------------------------------------
# Register map (word offsets)
# ---------------------------------------------------------------------------
REG_CTRL = 0x00
REG_STATUS = 0x01
REG_QUERY = 0x02
REG_AGG_CFG = 0x03
REG_PROJ_MASK = 0x04
REG_PRED_BASE = 0x08
REG_PRED_WORDS = 2  # words per predicate slot (47-bit cfg)
REG_LOAD_ADDR = 0x20
REG_LOAD_DATA0 = 0x21  # +c for column c
NUM_COLS = 4
REG_LOAD_ROW = REG_LOAD_DATA0 + NUM_COLS
REG_RESULT = 0x30
REG_RESULT_HI = 0x31
REG_COUNT = 0x32
REG_COUNT_HI = 0x33
REG_OVERFLOW = 0x34

# Control register bits.
CTRL_START = 0
CTRL_ABORT = 1

# Status register bits.
STATUS_BUSY = 0
STATUS_DONE = 1
STATUS_ERROR = 2

# Field widths.
COLUMN_ADDR_W = 10  # $clog2(1024) rows
COLUMN_WIDTH = 32
ACCUM_W = 42  # COLUMN_WIDTH + COLUMN_ADDR_W

# Error codes (db_pkg::error_e).
ERR_NONE = 0
ERR_START_BUSY = 1
ERR_AGG_OVERFLOW = 2

# ---------------------------------------------------------------------------
# Operation encodings
# ---------------------------------------------------------------------------
OP_SELECT = 0
OP_COUNT = 1
OP_SUM = 2
OP_MIN = 3
OP_MAX = 4
OP_AVG = 5
OP_GROUPBY = 6

# Predicate operators and the "WHERE" syntax they map to.
PRED_OP = {
    "==": 0,
    "!=": 1,
    "<": 2,
    ">": 3,
    "<=": 4,
    ">=": 5,
}

# Boolean combine operators (fold with the previous active predicate slot).
LOGIC_AND = 0
LOGIC_OR = 1


# ---------------------------------------------------------------------------
# Packing helpers. Field layouts follow the packed structs in db_pkg (MSB
# first, so the first struct member occupies the most significant bits).
# ---------------------------------------------------------------------------
def pack_agg(op, groupby=False, column=0, gby_key=0):
    """Pack an ``agg_cfg_t``: op[23:21], groupby[20], column[19:10],
    gby_key[9:0]."""
    return (op << 21) | (int(groupby) << 20) | (column << 10) | gby_key


def pack_query(num_rows):
    """Pack a ``query_cfg_t``: num_rows[9:0] (0 = full table)."""
    return num_rows & ((1 << COLUMN_ADDR_W) - 1)


def pack_pred(enable, op, imm, column, combine=LOGIC_AND):
    """Pack one predicate slot into its two 32-bit words.

    ``pred_cfg_t`` is 47 bits: imm[31:0], column[41:32], combine[42],
    op[45:43], enable[46]. Word 0 is ``imm``; word 1 is
    {enable, op, combine, column} in bits [14:0].
    """
    high = (int(enable) << 14) | ((op & 0x7) << 11) | (combine << 10) | (
        column & ((1 << COLUMN_ADDR_W) - 1)
    )
    return imm, high


def pack_proj_mask(columns, num_columns=NUM_COLS):
    """Pack a projection mask from a list of column indices (bit *c* set)."""
    mask = 0
    for c in columns:
        mask |= 1 << c
    return mask & ((1 << num_columns) - 1)
