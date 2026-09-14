#!/usr/bin/env python3
"""
Differential test: this database against SQLite.

Generates random sequences of SQL, runs every statement against both
this database (through cpp/sql_harness) and an in-memory SQLite
database, and stops at the first place they disagree. After every
statement that changes data, whole tables are compared too. A failing
sequence is shrunk to a short reproduction before it's printed.

Some things are translated or normalized on purpose, because they're
documented differences rather than bugs:
  - The first column is the primary key here, so SQLite gets INTEGER
    PRIMARY KEY on it.
  - INSERT on an existing primary key overwrites the row here, so SQLite
    gets INSERT OR REPLACE. UPDATE gets UPDATE OR REPLACE to match.
  - This database has no NULL, so SUM over zero rows is 0 here and NULL
    in SQLite. Those count as equal.
  - Without ORDER BY, rows are compared as a multiset. With ORDER BY,
    ties can come back in either order, so the sort column's sequence
    is compared exactly and the rows as a multiset.
  - Values stay inside the documented limits: non-negative primary keys,
    indexed values under 2^20.

Anything else is expected to match exactly.

Some behaviors can be switched off with --skip once they're known, so a
run can look past them for other problems:
  dup_insert   INSERTs that reuse an existing primary key
  agg_limit    COUNT(*) / SUM(col) combined with LIMIT
  pk_update    UPDATEs that change a row's primary key

Usage:
  g++ -O2 -std=c++17 -o cpp/sql_harness cpp/sql_harness.cpp
  python3 python/sqlite_diff_test.py --runs 200 --steps 300
"""
import argparse
import json
import os
import random
import shutil
import sqlite3
import subprocess
import sys
import tempfile
from collections import Counter

TABLES = {
    "users": [("id", "INT"), ("name", "TEXT"), ("age", "INT"), ("score", "INT")],
    "items": [("id", "INT"), ("label", "TEXT"), ("qty", "INT")],
}
# Small ranges on purpose, so equality predicates actually match rows
# and repeated primary keys actually happen.
RANGES = {"id": (0, 150), "age": (0, 40), "score": (0, 300), "qty": (0, 25)}
TEXT_POOL = ["n%d" % i for i in range(12)]
FEATURES = ("dup_insert", "agg_limit", "pk_update")


class HarnessCrashed(Exception):
    pass


class Harness:
    """One running cpp/sql_harness process with a fresh log file."""

    def __init__(self, exe, wal_path):
        self.proc = subprocess.Popen([exe, wal_path], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, text=True, bufsize=1)

    def run(self, line):
        try:
            self.proc.stdin.write(line + "\n")
            self.proc.stdin.flush()
        except BrokenPipeError:
            raise HarnessCrashed(self._death())
        head = self.proc.stdout.readline()
        if not head:
            raise HarnessCrashed(self._death())
        head = head.rstrip("\n")
        if head.startswith("ROWS "):
            count = int(head[5:])
            return ("rows", [tuple(json.loads(self.proc.stdout.readline())) for _ in range(count)])
        if head == "OK":
            return ("ok", None)
        if head.startswith("OK "):
            return ("ok", int(head[3:]))
        if head.startswith("ERR "):
            return ("err", head[4:])
        raise ValueError("unexpected harness output: %r" % head)

    def _death(self):
        self.proc.wait(timeout=5)
        err = self.proc.stderr.read().strip()
        return "harness exited with code %s%s" % (self.proc.returncode, (": " + err[-300:]) if err else "")

    def close(self):
        try:
            self.proc.stdin.close()
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()


def show(rows, limit=6):
    rows = list(rows)
    text = ", ".join(repr(r) for r in rows[:limit])
    return "[" + text + (", ... %d more" % (len(rows) - limit) if len(rows) > limit else "") + "]"


def execute(harness, conn, op):
    """Runs one op on both sides. Returns a description of the first
    disagreement, or None if they agree."""
    if op["kind"] == "restart":
        ours = harness.run(".restart")
        if ours[0] != "ok":
            return "restart failed: %r" % (ours,)
        return compare_tables(harness, conn)

    ours = harness.run(op["ours"])
    try:
        cur = conn.execute(op["sqlite"])
        theirs = ("rows", cur.fetchall()) if op["kind"] == "select" else ("ok", cur.rowcount)
    except sqlite3.Error as e:
        theirs = ("err", str(e))

    if ours[0] == "err" or theirs[0] == "err":
        if ours[0] == theirs[0]:
            return None
        return "this db: %r\n  sqlite:  %r" % (ours, theirs)
    if op["kind"] == "select":
        return compare_select(conn, op, ours[1], theirs[1])
    if op["kind"] in ("update", "delete") and ours[1] != theirs[1]:
        return "rows affected: this db %s, sqlite %s" % (ours[1], theirs[1])
    return compare_tables(harness, conn)


def compare_select(conn, op, ours, theirs):
    ours = [tuple(r) for r in ours]
    if op["target"] != "star":
        theirs = [tuple(0 if v is None else v for v in r) for r in theirs]
        same = ours == theirs
    else:
        full = Counter(conn.execute(op["sqlite_nolimit"]).fetchall())
        extra = Counter(ours) - full  # rows this db returned that shouldn't match at all
        if op["order_col"] is not None:
            col = op["order_col"]
            same = [r[col] for r in ours] == [r[col] for r in theirs] and not extra
            if not op["limit"]:
                same = same and Counter(ours) == Counter(theirs)
        elif op["limit"]:
            same = len(ours) == len(theirs) and not extra
        else:
            same = Counter(ours) == Counter(theirs)
    if same:
        return None
    return "this db: %s\n  sqlite:  %s" % (show(ours), show(theirs))


def compare_tables(harness, conn):
    tables = [r[0] for r in conn.execute("SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY name")]
    for table in tables:
        ours = harness.run("SELECT * FROM %s;" % table)
        theirs = conn.execute("SELECT * FROM %s" % table).fetchall()
        if ours[0] != "rows":
            return "reading table %s back failed: %r" % (table, ours)
        ours_rows, theirs_rows = Counter(ours[1]), Counter(theirs)
        if ours_rows != theirs_rows:
            return ("table %s no longer matches\n  only in this db: %s\n  only in sqlite:  %s"
                    % (table, show((ours_rows - theirs_rows).elements()), show((theirs_rows - ours_rows).elements())))
    return None


class Generator:
    """Produces random statements. Reads SQLite's current state to pick
    literals that actually hit rows, so it has to be driven in step with
    execution."""

    def __init__(self, rng, conn, skip):
        self.rng, self.conn, self.skip = rng, conn, skip
        self.indexed = set()

    def opening(self):
        ops = []
        for table, cols in TABLES.items():
            ours_cols = ", ".join("%s %s" % c for c in cols)
            sqlite_cols = ", ".join(("%s INTEGER PRIMARY KEY" % n) if i == 0 else ("%s %s" % (n, t))
                                    for i, (n, t) in enumerate(cols))
            ops.append(self._op("ddl", "CREATE TABLE %s (%s);" % (table, ours_cols),
                                "CREATE TABLE %s (%s)" % (table, sqlite_cols)))
        return ops

    def next(self):
        r = self.rng.random()
        table = self.rng.choice(list(TABLES))
        if r < 0.33:
            return self.insert(table)
        if r < 0.65:
            return self.select(table)
        if r < 0.79:
            return self.update(table)
        if r < 0.89:
            return self.delete(table)
        if r < 0.93:
            op = self.create_index()
            if op:
                return op
        return {"kind": "restart", "ours": ".restart"}

    @staticmethod
    def _op(kind, ours, sqlite, **extra):
        op = {"kind": kind, "ours": ours, "sqlite": sqlite}
        op.update(extra)
        return op

    def _existing(self, table, col):
        return [r[0] for r in self.conn.execute("SELECT %s FROM %s" % (col, table))]

    def _fresh_id(self, table):
        taken = set(self._existing(table, "id"))
        free = [i for i in range(RANGES["id"][0], RANGES["id"][1] + 1) if i not in taken]
        return self.rng.choice(free) if free else None

    def value(self, col, ctype):
        if ctype == "TEXT":
            return "'%s'" % self.rng.choice(TEXT_POOL)
        return str(self.rng.randint(*RANGES[col]))

    def literal(self, table, col, ctype):
        """A value for a predicate: half the time one that's in the table."""
        if self.rng.random() < 0.5:
            existing = self._existing(table, col)
            if existing:
                v = self.rng.choice(existing)
                return "'%s'" % v if ctype == "TEXT" else str(v)
        return self.value(col, ctype)

    def predicate(self, table):
        col, ctype = self.rng.choice(TABLES[table])
        r = self.rng.random()
        if r < 0.45:
            return "%s = %s" % (col, self.literal(table, col, ctype))
        if r < 0.65:
            a, b = self.literal(table, col, ctype), self.literal(table, col, ctype)
            if ctype == "INT" and int(a) > int(b) and self.rng.random() < 0.9:
                a, b = b, a
            return "%s BETWEEN %s AND %s" % (col, a, b)
        op = self.rng.choice(["<", ">", "<=", ">="])
        return "%s %s %s" % (col, op, self.literal(table, col, ctype))

    def insert(self, table):
        cols = TABLES[table]
        pk = self.rng.randint(*RANGES["id"]) if "dup_insert" not in self.skip else self._fresh_id(table)
        if pk is None:
            return self.select(table)
        values = ", ".join([str(pk)] + [self.value(c, t) for c, t in cols[1:]])
        return self._op("insert", "INSERT INTO %s VALUES (%s);" % (table, values),
                        "INSERT OR REPLACE INTO %s VALUES (%s)" % (table, values))

    def update(self, table):
        cols = TABLES[table]
        if "pk_update" not in self.skip and self.rng.random() < 0.15:
            ids = self._existing(table, "id")
            # usually an unused id; sometimes one another row already has,
            # which overwrites that row (UPDATE OR REPLACE in SQLite)
            new = self.rng.choice(ids) if ids and self.rng.random() < 0.3 else self._fresh_id(table)
            if ids and new is not None:
                body = "%s SET id = %d WHERE id = %d" % (table, new, self.rng.choice(ids))
                return self._op("update", "UPDATE %s;" % body, "UPDATE OR REPLACE %s" % body)
        chosen = self.rng.sample(cols[1:], self.rng.randint(1, 2))
        body = "%s SET %s" % (table, ", ".join("%s = %s" % (c, self.value(c, t)) for c, t in chosen))
        if self.rng.random() < 0.9:
            body += " WHERE " + self.predicate(table)
        return self._op("update", "UPDATE %s;" % body, "UPDATE OR REPLACE %s" % body)

    def delete(self, table):
        body = "FROM %s" % table
        if self.rng.random() < 0.97:
            body += " WHERE " + self.predicate(table)
        return self._op("delete", "DELETE %s;" % body, "DELETE %s" % body)

    def create_index(self):
        options = [(t, c) for t, cols in TABLES.items() for c, ty in cols[1:]
                   if ty == "INT" and (t, c) not in self.indexed]
        if not options:
            return None
        table, col = self.rng.choice(options)
        self.indexed.add((table, col))
        sql = "CREATE INDEX idx_%s_%s ON %s(%s)" % (table, col, table, col)
        return self._op("ddl", sql + ";", sql)

    def select(self, table):
        cols = TABLES[table]
        r = self.rng.random()
        if r < 0.7:
            target, head = "star", "*"
        elif r < 0.85:
            target, head = "count", "COUNT(*)"
        else:
            target = "sum"
            head = "SUM(%s)" % self.rng.choice([c for c, t in cols if t == "INT"])
        sql = "SELECT %s FROM %s" % (head, table)
        if self.rng.random() < 0.75:
            sql += " WHERE " + self.predicate(table)
        order_col = None
        if target == "star" and self.rng.random() < 0.35:
            order_col = self.rng.randrange(len(cols))
            sql += " ORDER BY %s%s" % (cols[order_col][0], self.rng.choice(["", " ASC", " DESC"]))
        base = sql
        limit = target == "star" or "agg_limit" not in self.skip
        limit = limit and self.rng.random() < 0.25
        if limit:
            sql += " LIMIT %d" % self.rng.randint(0, 8)
        return self._op("select", sql + ";", sql, sqlite_nolimit=base, target=target, order_col=order_col,
                        limit=limit)


def replay(ops, exe, workdir):
    """Runs a fixed list of ops from scratch. Returns (index, message) for
    the first disagreement, or None."""
    conn = sqlite3.connect(":memory:")
    harness = Harness(exe, os.path.join(workdir, "replay.wal"))
    try:
        for i, op in enumerate(ops):
            try:
                problem = execute(harness, conn, op)
            except HarnessCrashed as e:
                problem = str(e)
            if problem:
                return i, problem
        return None
    finally:
        harness.close()
        conn.close()


def shrink(ops, exe, workdir):
    """Deletes chunks of statements, then single statements, for as long
    as some disagreement still reproduces."""
    chunk = max(1, len(ops) // 2)
    while True:
        i, removed = 0, False
        while i < len(ops):
            candidate = ops[:i] + ops[i + chunk:]
            found = replay(candidate, exe, workdir) if candidate else None
            if found:
                ops = candidate[:found[0] + 1]
                removed = True
            else:
                i += chunk
        if chunk == 1 and not removed:
            return ops
        if not removed:
            chunk = max(1, chunk // 2)


def run_one(seed, steps, exe, skip, workdir):
    rng = random.Random(seed)
    conn = sqlite3.connect(":memory:")
    harness = Harness(exe, os.path.join(workdir, "run.wal"))
    gen = Generator(rng, conn, skip)
    history = []
    try:
        pending = gen.opening()
        for _ in range(steps):
            op = pending.pop(0) if pending else gen.next()
            history.append(op)
            try:
                problem = execute(harness, conn, op)
            except HarnessCrashed as e:
                problem = str(e)
            if problem:
                return history
        return None
    finally:
        harness.close()
        conn.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--harness", default="cpp/sql_harness")
    ap.add_argument("--runs", type=int, default=100)
    ap.add_argument("--steps", type=int, default=300)
    ap.add_argument("--first-seed", type=int, default=1)
    ap.add_argument("--skip", default="", help="comma-separated: " + ", ".join(FEATURES))
    ap.add_argument("--max-failures", type=int, default=3)
    args = ap.parse_args()

    skip = {s for s in args.skip.split(",") if s}
    unknown = skip - set(FEATURES)
    if unknown:
        sys.exit("unknown --skip value(s): %s" % ", ".join(sorted(unknown)))

    workdir = tempfile.mkdtemp(prefix="sqlite_diff_")
    failures = 0
    try:
        for seed in range(args.first_seed, args.first_seed + args.runs):
            history = run_one(seed, args.steps, args.harness, skip, workdir)
            if history is None:
                continue
            failures += 1
            small = shrink(history, args.harness, workdir)
            index, problem = replay(small, args.harness, workdir)
            print("seed %d: disagreement after %d statements, shrunk to %d:" % (seed, len(history), len(small)))
            for op in small[:index]:
                print("    " + op["ours"])
            print("  > " + small[index]["ours"])
            print("  " + problem + "\n")
            if failures >= args.max_failures:
                break
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    done = seed - args.first_seed + 1
    print("%d run(s) of up to %d statements, %d with a disagreement%s"
          % (done, args.steps, failures, " (skipping: %s)" % ", ".join(sorted(skip)) if skip else ""))
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
