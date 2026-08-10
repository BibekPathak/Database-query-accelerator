"""Pure-Python reference model for the DBQA aggregation semantics.

Implements the same behavior the RTL pipeline exposes: rows 0..num_rows-1 of
the loaded table (0 = all 1024 rows) are filtered by an optional predicate,
then either aggregated classically (COUNT / SUM / MIN / MAX / AVG, with SUM
saturation at 2**ACCUM_W - 1) or grouped per key column.

Only the rows loaded for a test are visible here; the RTL table holds 1024
rows and unloaded rows read back as zero, so ``num_rows`` must not exceed the
number of loaded rows unless the whole table was loaded.
"""

from __future__ import annotations

from . import regs

NUM_ROWS = 1024
SUM_MAX = (1 << regs.ACCUM_W) - 1


def _pred_pass(op: int, a: int, b: int) -> bool:
    if op == 0:
        return a == b
    if op == 1:
        return a != b
    if op == 2:
        return a < b
    if op == 3:
        return a > b
    if op == 4:
        return a <= b
    if op == 5:
        return a >= b
    return False


class AggRef:
    """Classic aggregation result over one query."""

    __slots__ = ("count", "sum", "min", "max")

    def __init__(self, count=0, sum=0, min=0xFFFFFFFF, max=0):
        self.count = count
        self.sum = sum
        self.min = min
        self.max = max


def reference(table, num_rows, agg_col, predicate=None):
    """Classic aggregation reference.

    ``predicate`` is ``(op, imm, column)`` or ``None``.
    Returns an :class:`AggRef` with count/sum/min/max of the passing rows.
    """
    n = NUM_ROWS if num_rows == 0 else min(num_rows, NUM_ROWS)
    r = AggRef()
    for i in range(n):
        row = table[i] if i < len(table) else [0] * regs.NUM_COLS
        if predicate is not None:
            op, imm, col = predicate
            if not _pred_pass(op, row[col], imm):
                continue
        r.count += 1
        v = row[agg_col]
        if r.sum + v > SUM_MAX:
            r.sum = SUM_MAX
        else:
            r.sum += v
        r.min = min(r.min, v)
        r.max = max(r.max, v)
    return r


def groupby_reference(table, num_rows, key_col, value_col):
    """GROUP BY reference: dict key -> AggRef (count/sum/min/max per key)."""
    n = NUM_ROWS if num_rows == 0 else min(num_rows, NUM_ROWS)
    out = {}
    for i in range(n):
        row = table[i] if i < len(table) else [0] * regs.NUM_COLS
        k = row[key_col]
        g = out.get(k)
        v = row[value_col]
        if g is None:
            out[k] = AggRef(1, v, v, v)
        else:
            g.count += 1
            g.sum = min(g.sum + v, SUM_MAX)
            g.min = min(g.min, v)
            g.max = max(g.max, v)
    return out
