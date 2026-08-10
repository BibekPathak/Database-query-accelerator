"""Golden register-write sequences for the query compiler.

Validates that ``Query.execute()`` compiles to the exact register writes
documented in ``docs/register_map.md``.
"""

from dbqa import Query, RecordingBackend, regs

SCHEMA = {"id": 0, "age": 1, "salary": 2, "extra": 3}


def test_sum_with_predicate_sequence():
    b = RecordingBackend()
    (Query(b, SCHEMA)
     .where("age", ">=", 30)
     .sum("salary")
     .execute())

    assert b.writes == [
        (regs.REG_PRED_BASE, 30),               # slot 0 imm
        (regs.REG_PRED_BASE + 1, 0x6801),       # slot 0 cfg
        (regs.REG_PRED_BASE + 2, 0),            # slot 1 disabled
        (regs.REG_PRED_BASE + 3, 0),
        (regs.REG_PROJ_MASK, 0xF),              # all columns projected
        (regs.REG_AGG_CFG, regs.pack_agg(regs.OP_SUM, False, 2, 0)),
        (regs.REG_QUERY, 0),                    # num_rows = 1024 -> full table
        (regs.REG_CTRL, 1),                     # start
    ]


def test_groupby_sequence():
    b = RecordingBackend()
    (Query(b, SCHEMA)
     .group_by("id")
     .sum("salary")
     .limit(64)
     .execute())

    assert b.writes == [
        (regs.REG_PRED_BASE, 0),
        (regs.REG_PRED_BASE + 1, 0),
        (regs.REG_PRED_BASE + 2, 0),
        (regs.REG_PRED_BASE + 3, 0),
        (regs.REG_PROJ_MASK, 0xF),
        (regs.REG_AGG_CFG, regs.pack_agg(regs.OP_SUM, True, 2, 0)),
        (regs.REG_QUERY, 64),
        (regs.REG_CTRL, 1),
    ]


def test_two_predicate_slots():
    b = RecordingBackend()
    (Query(b, SCHEMA)
     .where("age", ">=", 30)
     .where("salary", "<", 1000)
     .count("id")
     .execute())

    slot0_low, slot0_high = regs.pack_pred(True, regs.PRED_OP[">="], 30, 1)
    slot1_low, slot1_high = regs.pack_pred(True, regs.PRED_OP["<"], 1000, 2)
    writes = dict()  # word -> data (later predicate writes overwrite here)
    pred_writes = [w for w in b.writes if regs.REG_PRED_BASE <= w[0] <= regs.REG_PRED_BASE + 3]
    assert pred_writes == [
        (regs.REG_PRED_BASE, slot0_low),
        (regs.REG_PRED_BASE + 1, slot0_high),
        (regs.REG_PRED_BASE + 2, slot1_low),
        (regs.REG_PRED_BASE + 3, slot1_high),
    ]


def test_projection_mask_follows_select():
    b = RecordingBackend()
    (Query(b, SCHEMA)
     .select("age", "salary")
     .sum("salary")
     .execute())
    proj_writes = [w for w in b.writes if w[0] == regs.REG_PROJ_MASK]
    assert proj_writes == [(regs.REG_PROJ_MASK, 0b0110)]


def test_unknown_column_rejected():
    b = RecordingBackend()
    try:
        Query(b, SCHEMA).sum("nonexistent")  # raises at build time
        assert False, "expected ValueError"
    except ValueError:
        pass


def test_no_aggregate_rejected():
    b = RecordingBackend()
    try:
        Query(b, SCHEMA).execute()
        assert False, "expected ValueError"
    except ValueError:
        pass
