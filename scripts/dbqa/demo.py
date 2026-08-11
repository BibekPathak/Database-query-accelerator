"""DBQA demo: a few queries against the Verilator model.

Shows the full control-plane flow -- configure (load the table, build a
query) -> execute (compile to AXI-Lite register writes, start, poll) ->
result -- using the fluent Query API. The backend co-simulates the actual
RTL through the axil_server stdio harness.

Run with `make python-demo` (after `make axil-server`), or:
    cd scripts && PYTHONPATH=. python3 -m dbqa.demo
"""

import os
import sys

# Make the `dbqa` package importable when this file is run directly.
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def fmt(num):
    return f"{num:,.0f}"


def main():
    from dbqa import Query, VerilatorBackend

    schema = {"id": 0, "age": 1, "salary": 2, "dept": 3}
    # id, age, salary, dept
    table = [
        [0, 25, 50000, 0],
        [1, 31, 72000, 1],
        [2, 28, 61000, 0],
        [3, 35, 98000, 1],
        [4, 42, 115000, 0],
        [5, 30, 55000, 1],
    ]

    print("DBQA -- SQL analytics on FPGA fabric (Verilator co-simulation)")
    print(f"table: {len(table)} rows, {len(schema)} columns: "
          f"{', '.join(schema)}")
    print()

    with VerilatorBackend() as backend:
        backend.load_table(table)

        print("1. SELECT COUNT(*) FROM emp")
        r = Query(backend, schema).limit(len(table)).count().execute()
        print(f"   count = {r.count}\n")

        print("2. SELECT SUM(salary), AVG(salary) FROM emp")
        r = Query(backend, schema).limit(len(table)).avg("salary").execute()
        print(f"   sum  = {fmt(r.result)}")
        print(f"   avg  = {fmt(r.avg)}\n")

        print("3. SELECT MIN(salary), MAX(salary) FROM emp")
        rmin = Query(backend, schema).limit(len(table)).min("salary").execute()
        rmax = Query(backend, schema).limit(len(table)).max("salary").execute()
        print(f"   min  = {fmt(rmin.result)}")
        print(f"   max  = {fmt(rmax.result)}\n")

        print("4. SELECT SUM(salary) FROM emp WHERE age >= 30")
        r = (Query(backend, schema)
             .where("age", ">=", 30)
             .limit(len(table))
             .sum("salary")
             .execute())
        print(f"   rows = {r.count}, sum = {fmt(r.result)}\n")

        print("5. SELECT dept, SUM(salary) FROM emp GROUP BY dept")
        groups = (Query(backend, schema)
                  .group_by("dept")
                  .sum("salary")
                  .limit(len(table))
                  .execute())
        for g in sorted(groups, key=lambda g: g.key):
            print(f"   dept {g.key}: rows = {g.count}, sum = {fmt(g.sum)}")
        print()

        print("Done. Results were computed by the RTL and read back over "
              "AXI-Lite.")

    return 0


if __name__ == "__main__":
    sys.exit(main())
