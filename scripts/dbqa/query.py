"""Fluent query API for the DBQA accelerator.

Mirrors the README example::

    q = Query(backend, schema={"id": 0, "age": 1, "salary": 2, "extra": 3})
    result = q.where("age", ">", 30).sum("salary").execute()          # classic
    groups = q.group_by("id").sum("salary").execute()                 # GROUP BY

``execute()`` compiles the query to register writes (documented in
``docs/register_map.md``), starts it, waits for completion, and returns either
a :class:`Result` (classic aggregation) or a ``list`` of :class:`Group`
(GROUP BY, one per key, streamed out of the accelerator).
"""

from __future__ import annotations

from . import regs


class Result:
    """Classic aggregation result. ``avg`` is computed in software as
    ``sum / count`` (the RTL reports AVG as its SUM)."""

    __slots__ = ("count", "result", "overflow")

    def __init__(self, count, result, overflow):
        self.count = count
        self.result = result
        self.overflow = overflow

    @property
    def avg(self):
        return (self.result / self.count) if self.count else 0

    def __repr__(self):
        return (f"Result(count={self.count}, result={self.result}, "
                f"overflow={self.overflow})")


class Group:
    """One GROUP BY group: key with count/sum/min/max of the value column."""

    __slots__ = ("key", "count", "sum", "min", "max")

    def __init__(self, key, count, sum, min, max):
        self.key = key
        self.count = count
        self.sum = sum
        self.min = min
        self.max = max

    @property
    def avg(self):
        return (self.sum / self.count) if self.count else 0

    def __repr__(self):
        return (f"Group(key={self.key:#x}, count={self.count}, "
                f"sum={self.sum}, min={self.min}, max={self.max})")


class Query:
    """Builds and executes one query against a backend."""

    def __init__(self, backend, schema, num_rows=1024):
        if len(schema) != regs.NUM_COLS:
            raise ValueError(
                f"schema must map exactly {regs.NUM_COLS} columns, got {len(schema)}"
            )
        self._backend = backend
        self._schema = dict(schema)
        self._select = list(schema)
        self._predicates = []  # (column, op, imm)
        self._agg = None  # (op, column)
        self._groupby = None  # key column
        self._num_rows = num_rows or 0  # 0 = full table

    # -- column helpers ------------------------------------------------------
    def _col(self, col):
        if isinstance(col, str):
            if col not in self._schema:
                raise ValueError(f"unknown column {col!r}")
            return self._schema[col]
        return col

    # -- query clauses -------------------------------------------------------
    def select(self, *cols):
        self._select = list(cols)
        return self

    def where(self, col, op, value):
        if len(self._predicates) >= regs.REG_PRED_WORDS:
            raise ValueError("at most two predicate slots are supported")
        if op not in regs.PRED_OP:
            raise ValueError(f"unknown comparison operator {op!r}")
        self._predicates.append((self._col(col), regs.PRED_OP[op],
                                 value & 0xFFFFFFFF))
        return self

    def group_by(self, col):
        self._groupby = self._col(col)
        return self

    def limit(self, n):
        self._num_rows = n
        return self

    def count(self, col=None):
        self._agg = (regs.OP_COUNT, self._col(col) if col is not None else 0)
        return self

    def sum(self, col):
        self._agg = (regs.OP_SUM, self._col(col))
        return self

    def min(self, col):
        self._agg = (regs.OP_MIN, self._col(col))
        return self

    def max(self, col):
        self._agg = (regs.OP_MAX, self._col(col))
        return self

    def avg(self, col):
        self._agg = (regs.OP_AVG, self._col(col))
        return self

    # -- execution -----------------------------------------------------------
    def execute(self):
        if self._agg is None:
            raise ValueError("no aggregate selected (use .count()/.sum()/...)")
        op, agg_col = self._agg
        groupby = self._groupby is not None

        # Predicate slots.
        for i in range(regs.REG_PRED_WORDS):
            if i < len(self._predicates):
                col, pop, imm = self._predicates[i]
                low, high = regs.pack_pred(True, pop, imm, col)
            else:
                low, high = 0, 0
            self._backend.write_word(regs.REG_PRED_BASE + 2 * i, low)
            self._backend.write_word(regs.REG_PRED_BASE + 2 * i + 1, high)

        self._backend.write_word(regs.REG_PROJ_MASK,
                                 regs.pack_proj_mask([self._col(c) for c in self._select]))
        self._backend.write_word(
            regs.REG_AGG_CFG,
            regs.pack_agg(op, groupby, agg_col,
                          self._groupby if self._groupby is not None else 0),
        )
        self._backend.write_word(regs.REG_QUERY,
                                 regs.pack_query(self._num_rows))
        self._backend.write_word(regs.REG_CTRL, 1 << regs.CTRL_START)

        if groupby:
            return self._collect_groups()
        return self._collect_classic()

    def _collect_classic(self):
        if not _wait_done(self._backend):
            raise RuntimeError("query did not complete")
        count = self._read_accum(regs.REG_COUNT, regs.REG_COUNT_HI)
        result = self._read_accum(regs.REG_RESULT, regs.REG_RESULT_HI)
        overflow = self._backend.read_word(regs.REG_OVERFLOW) & 1
        return Result(count, result, overflow)

    def _collect_groups(self):
        groups = []
        seen_last = False
        for _ in range(30000):
            g = self._backend.next_group()
            if g is not None:
                key, count, sum, mn, mx, last = g
                groups.append(Group(key, count, sum, mn, mx))
                if last:
                    seen_last = True
                    break
        if not seen_last:
            raise RuntimeError("GROUP BY stream did not terminate")
        if not _wait_done(self._backend):
            raise RuntimeError("GROUP BY query did not complete")
        return groups

    def _read_accum(self, low_word, high_word):
        low = self._backend.read_word(low_word)
        high = self._backend.read_word(high_word)
        return (high << 32) | low


def _wait_done(backend, max_polls=30000):
    """Poll REG_STATUS until the done bit is set."""
    for _ in range(max_polls):
        status = backend.read_word(regs.REG_STATUS)
        if status & (1 << regs.STATUS_DONE):
            return True
    return False
