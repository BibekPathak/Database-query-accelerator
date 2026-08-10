"""End-to-end tests: Python control plane against the Verilator model.

Skipped automatically when ``make axil-server`` has not been run (or
``DBQA_AXIL_SERVER`` points elsewhere).
"""

import os

import pytest

from dbqa import Query, VerilatorBackend, regs, reference

SCHEMA = {"id": 0, "age": 1, "salary": 2, "extra": 3}


def _default_server():
    path = os.environ.get("DBQA_AXIL_SERVER")
    if not path:
        repo = os.path.dirname(os.path.dirname(os.path.dirname(
            os.path.abspath(__file__))))
        path = os.path.join(repo, "build", "axil_server", "Vaxil_server")
    return path


def _server_available():
    return os.path.exists(_default_server())


pytestmark = pytest.mark.skipif(
    not _server_available(), reason="run `make axil-server` first"
)


@pytest.fixture
def backend():
    with VerilatorBackend() as b:
        yield b


def _rows():
    # id, age, salary, extra
    return [[i, 10 * (i + 1), 3 * i, 0] for i in range(8)]


def test_count_all(backend):
    backend.load_table(_rows())
    r = Query(backend, SCHEMA).limit(8).count().execute()
    assert r.count == 8
    assert r.result == 8


def test_sum_min_max_avg(backend):
    backend.load_table(_rows())
    ref = reference.reference(_rows(), 8, 2)
    r = Query(backend, SCHEMA).limit(8).sum("salary").execute()
    assert r.count == ref.count and r.result == ref.sum
    r = Query(backend, SCHEMA).limit(8).min("salary").execute()
    assert r.result == ref.min
    r = Query(backend, SCHEMA).limit(8).max("salary").execute()
    assert r.result == ref.max
    r = Query(backend, SCHEMA).limit(8).avg("salary").execute()
    assert r.result == ref.sum
    assert r.avg == ref.sum / ref.count


def test_predicate(backend):
    backend.load_table(_rows())
    ref = reference.reference(_rows(), 8, 2, (regs.PRED_OP[">="], 30, 1))
    r = Query(backend, SCHEMA).where("age", ">=", 30).limit(8).sum("salary").execute()
    assert r.count == ref.count
    assert r.result == ref.sum


def test_two_predicates(backend):
    backend.load_table(_rows())
    ref = reference.reference(
        _rows(), 8, 2,
        (regs.PRED_OP[">="], 30, 1),
    )
    # count of rows with age >= 30 AND salary < 12 (salaries are multiples of 3)
    passing = 0
    for row in _rows():
        if row[1] >= 30 and row[2] < 12:
            passing += 1
    r = (Query(backend, SCHEMA)
         .where("age", ">=", 30)
         .where("salary", "<", 12)
         .count("id")
         .execute())
    assert r.count == passing


def test_limit(backend):
    backend.load_table(_rows())
    ref = reference.reference(_rows(), 3, 2)
    r = Query(backend, SCHEMA).limit(3).sum("salary").execute()
    assert r.count == 3
    assert r.result == ref.sum


def test_groupby(backend):
    backend.load_table(_rows())
    ref = reference.groupby_reference(_rows(), 8, 0, 2)
    groups = (Query(backend, SCHEMA)
              .group_by("id")
              .sum("salary")
              .limit(8)
              .execute())
    got = {g.key: g for g in groups}
    assert len(got) == len(ref)
    for k, g in ref.items():
        assert k in got
        assert got[k].count == g.count
        assert got[k].sum == g.sum
        assert got[k].min == g.min
        assert got[k].max == g.max


def test_groupby_folding(backend):
    # Repeated keys fold into one group per key.
    table = [[0, 5, 100, 0], [1, 5, 200, 0], [0, 5, 300, 0], [2, 5, 50, 0]]
    backend.load_table(table)
    ref = reference.groupby_reference(table, 4, 0, 2)
    groups = (Query(backend, SCHEMA)
              .group_by("id")
              .sum("salary")
              .limit(4)
              .execute())
    got = {g.key: g for g in groups}
    assert len(got) == len(ref)
    for k, g in ref.items():
        assert got[k].count == g.count
        assert got[k].sum == g.sum


def test_random_queries(backend):
    import random

    rng = random.Random(0xDB9A)
    table = [[rng.randrange(0, 100), rng.randrange(0, 100),
              rng.randrange(0, 1000), 0] for _ in range(32)]
    backend.load_table(table)
    ops = [
        (regs.OP_COUNT, 1), (regs.OP_SUM, 2), (regs.OP_MIN, 2),
        (regs.OP_MAX, 2), (regs.OP_AVG, 2),
    ]
    for run in range(12):
        op, col = rng.choice(ops)
        num_rows = 1 + rng.randrange(1, len(table) + 1)
        ref = reference.reference(table, num_rows, col)
        q = Query(backend, SCHEMA).limit(num_rows)
        if op == regs.OP_COUNT:
            q = q.count()
        elif op == regs.OP_SUM:
            q = q.sum("salary")
        elif op == regs.OP_MIN:
            q = q.min("salary")
        elif op == regs.OP_MAX:
            q = q.max("salary")
        else:
            q = q.avg("salary")
        r = q.execute()
        assert r.count == ref.count
        if op in (regs.OP_SUM, regs.OP_AVG):
            assert r.result == ref.sum
        elif op == regs.OP_MIN:
            assert r.result == ref.min
        elif op == regs.OP_MAX:
            assert r.result == ref.max
