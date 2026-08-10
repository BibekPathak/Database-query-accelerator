"""Unit tests for the register-map packing helpers."""

from dbqa import regs


def test_register_offsets():
    assert regs.REG_CTRL == 0x00
    assert regs.REG_STATUS == 0x01
    assert regs.REG_QUERY == 0x02
    assert regs.REG_AGG_CFG == 0x03
    assert regs.REG_PROJ_MASK == 0x04
    assert regs.REG_PRED_BASE == 0x08
    assert regs.REG_LOAD_ADDR == 0x20
    assert regs.REG_LOAD_DATA0 == 0x21
    assert regs.REG_LOAD_ROW == 0x25
    assert regs.REG_RESULT == 0x30
    assert regs.REG_COUNT == 0x32
    assert regs.REG_OVERFLOW == 0x34


def test_pack_agg():
    # SUM over column 1, no GROUP BY.
    assert regs.pack_agg(regs.OP_SUM, False, 1, 0) == 0x400400
    # SUM with GROUP BY on key column 0.
    assert regs.pack_agg(regs.OP_SUM, True, 1, 0) == 0x500400
    assert regs.pack_agg(regs.OP_COUNT, False, 0, 0) == 0x200000


def test_pack_query():
    assert regs.pack_query(0) == 0  # full table
    assert regs.pack_query(1024) == 0  # wraps to full table
    assert regs.pack_query(5) == 5
    assert regs.pack_query(2048) == 0  # masked to 10 bits


def test_pack_pred():
    # enable=1, op=GTE(5), combine=AND, column=1, imm=30.
    low, high = regs.pack_pred(True, regs.PRED_OP[">="], 30, 1)
    assert low == 30
    assert high == 0x6801  # (1<<14) | (5<<11) | (0<<10) | 1


def test_pack_pred_disabled():
    low, high = regs.pack_pred(False, regs.PRED_OP["=="], 0, 0)
    assert low == 0
    assert high == 0


def test_pack_proj_mask():
    assert regs.pack_proj_mask([]) == 0
    assert regs.pack_proj_mask([0, 1, 2, 3]) == 0xF
    assert regs.pack_proj_mask([1, 2]) == 0b0110
