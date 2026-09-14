#!/usr/bin/env python3
"""
Differential test: this database against SQLite.

Generates random sequences of SQL, runs every statement against both
this database (through cpp/sql_harness) and an in-memory SQLite
database, and stops at the first place they disagree. After every
statement that changes data, whole tables are compared too. Along the
way the database is restarted from disk and checkpointed, both on
request and automatically. A failing sequence is shrunk to a short
reproduction before it's printed.

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
  - SQLite accepts some statements this database refuses on purpose: a
    value of the wrong type for its column, a primary key outside
    0..2^48-1 (0..2^28-1 on a table with a secondary index), an indexed
    value outside 0..2^20-1, a WHERE on a column that doesn't exist, and
    CREATE INDEX on a table whose rows don't fit those limits. The
    tester knows these rules. For a statement that breaks one, it
    expects this database to return an error and change nothing, and
    doesn't run it on SQLite.

Anything else is expected to match exactly.

Some kinds of statement can be switched off with --skip, so a run can
look past a known problem for other ones:
  dup_insert   INSERTs that reuse an existing primary key
  agg_limit    COUNT(*) / SUM(col) combined with LIMIT
  pk_update    UPDATEs that change a row's primary key
  limits       values past the documented limits, and type mismatches

Usage:
  make difftest
or by hand:
  g++ -O2 -std=c++17 -o build/sql_harness cpp/sql_harness.cpp
  python3 python/sqlite_diff_test.py --harness build/sql_harness --runs 200
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
FEATURES = ("dup_insert", "agg_limit", "pk_update", "limits")

# The limits that come from how keys are packed (cpp/table_key.h).
PK_MAX = (1 << 48) - 1
INDEXED_PK_MAX = (1 << 28) - 1
INDEXED_VALUE_MAX = (1 << 20) - 1
EDGE_INTS = [-1, -1000, INDEXED_VALUE_MAX, INDEXED_VALUE_MAX + 1, INDEXED_PK_MAX, INDEXED_PK_MAX + 1,
             PK_MAX, PK_MAX + 1, 10 ** 15]


def sql_literal(v):
    return "'%s'" % v if isinstance(v, str) else str(v)


class HarnessCrashed(Exception):
    pass


class Harness:
    """One running sql_harness process with a fresh store."""

    def __init__(self, exe, wal_path, checkpoint_every):
        self.proc = subprocess.Popen([exe, wal_path, str(checkpoint_every)], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)

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


class Rules:
    """This database's validation rules, mirrored from cpp/executor.h,
    plus the one piece of state they depend on: which columns have an
    index. Whether a statement should be refused is decided when it
    runs, not when it's generated, so it stays right when shrinking
    deletes an earlier CREATE INDEX."""

    def __init__(self, conn):
        self.conn = conn
        self.indexed = set()  # (table, column)

    @staticmethod
    def type_ok(ctype, v):
        return (ctype == "INT") == isinstance(v, int)

    def value_ok(self, table, col, v):
        ctype = dict(TABLES[table])[col]
        if not self.type_ok(ctype, v):
            return False
        if col == "id":
            has_index = any(t == table for t, _ in self.indexed)
            return 0 <= v <= (INDEXED_PK_MAX if has_index else PK_MAX)
        if (table, col) in self.indexed:
            return 0 <= v <= INDEXED_VALUE_MAX
        return True

    def index_ok(self, table, col):
        rows = self.conn.execute("SELECT id, %s FROM %s" % (col, table)).fetchall()
        return all(pk <= INDEXED_PK_MAX and 0 <= v <= INDEXED_VALUE_MAX for pk, v in rows)


def show(rows, limit=6):
    rows = list(rows)
    text = ", ".join(repr(r) for r in rows[:limit])
    return "[" + text + (", ... %d more" % (len(rows) - limit) if len(rows) > limit else "") + "]"


def execute(harness, conn, rules, op):
    """Runs one op on both sides. Returns a description of the first
    disagreement, or None if they agree."""
    if op["kind"] in ("restart", "checkpoint"):
        ours = harness.run(op["ours"])
        if ours[0] != "ok":
            return "%s failed: %r" % (op["ours"], ours)
        return compare_tables(harness, conn)

    should_refuse = "valid" in op and not op["valid"](rules)
    ours = harness.run(op["ours"])
    if should_refuse:
        if ours[0] != "err":
            return "this db accepted a statement it should refuse: %r" % (ours,)
        return compare_tables(harness, conn)  # and refusing it can't have changed anything

    try:
        cur = conn.execute(op["sqlite"])
        theirs = ("rows", cur.fetchall()) if op["kind"] == "select" else ("ok", cur.rowcount)
    except sqlite3.Error as e:
        theirs = ("err", str(e))

    if ours[0] == "err" or theirs[0] == "err":
        if ours[0] == theirs[0]:
            return None
        return "this db: %r\n  sqlite:  %r" % (ours, theirs)
    if "index" in op:
        rules.indexed.add(op["index"])
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

    def __init__(self, rng, conn, rules, skip):
        self.rng, self.conn, self.rules, self.skip = rng, conn, rules, skip
        self.limits = "limits" not in skip

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
        if r < 0.88:
            return self.delete(table)
        if r < 0.92:
            op = self.create_index()
            if op:
                return op
        if r < 0.95:
            return {"kind": "checkpoint", "ours": ".checkpoint"}
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

    def _odd_value(self, ctype):
        """A value at or past a documented limit, or one of the wrong type."""
        if self.rng.random() < 0.3:
            return self.rng.randint(0, 99) if ctype == "TEXT" else "x%d" % self.rng.randint(0, 9)
        return self.rng.choice(EDGE_INTS)

    def value(self, col, ctype):
        if self.limits and self.rng.random() < 0.08:
            return self._odd_value(ctype)
        if ctype == "TEXT":
            return self.rng.choice(TEXT_POOL)
        return self.rng.randint(*RANGES[col])

    def literal(self, table, col, ctype):
        """A value for a predicate: half the time one that's in the table."""
        if self.rng.random() < 0.5:
            existing = self._existing(table, col)
            if existing:
                return self.rng.choice(existing)
        return self.value(col, ctype)

    def predicate(self, table):
        """Returns (sql, ok), where ok is False if the database should refuse it."""
        if self.limits and self.rng.random() < 0.02:
            return "nope = 1", False
        col, ctype = self.rng.choice(TABLES[table])
        r = self.rng.random()
        if r < 0.45:
            v = self.literal(table, col, ctype)
            return "%s = %s" % (col, sql_literal(v)), Rules.type_ok(ctype, v)
        if r < 0.65:
            a, b = self.literal(table, col, ctype), self.literal(table, col, ctype)
            if isinstance(a, int) and isinstance(b, int) and a > b and self.rng.random() < 0.9:
                a, b = b, a
            return ("%s BETWEEN %s AND %s" % (col, sql_literal(a), sql_literal(b)),
                    Rules.type_ok(ctype, a) and Rules.type_ok(ctype, b))
        v = self.literal(table, col, ctype)
        op = self.rng.choice(["<", ">", "<=", ">="])
        return "%s %s %s" % (col, op, sql_literal(v)), Rules.type_ok(ctype, v)

    def insert(self, table):
        cols = TABLES[table]
        if "dup_insert" in self.skip:
            pk = self._fresh_id(table)
            if pk is None:
                return self.select(table)
        else:
            pk = self.rng.randint(*RANGES["id"])
        if self.limits and self.rng.random() < 0.08:
            pk = self._odd_value("INT")
        values = [pk] + [self.value(c, t) for c, t in cols[1:]]
        pairs = list(zip([c for c, _ in cols], values))
        text = ", ".join(sql_literal(v) for v in values)
        return self._op("insert", "INSERT INTO %s VALUES (%s);" % (table, text),
                        "INSERT OR REPLACE INTO %s VALUES (%s)" % (table, text),
                        valid=lambda rules: all(rules.value_ok(table, c, v) for c, v in pairs))

    def update(self, table):
        cols = TABLES[table]
        if "pk_update" not in self.skip and self.rng.random() < 0.15:
            ids = self._existing(table, "id")
            # usually an unused id; sometimes one another row already has,
            # which overwrites that row (UPDATE OR REPLACE in SQLite)
            new = self.rng.choice(ids) if ids and self.rng.random() < 0.3 else self._fresh_id(table)
            if self.limits and self.rng.random() < 0.1:
                new = self._odd_value("INT")
            if ids and new is not None:
                body = "%s SET id = %s WHERE id = %d" % (table, sql_literal(new), self.rng.choice(ids))
                return self._op("update", "UPDATE %s;" % body, "UPDATE OR REPLACE %s" % body,
                                valid=lambda rules: rules.value_ok(table, "id", new))
        chosen = self.rng.sample(cols[1:], self.rng.randint(1, 2))
        assignments = [(c, self.value(c, t)) for c, t in chosen]
        body = "%s SET %s" % (table, ", ".join("%s = %s" % (c, sql_literal(v)) for c, v in assignments))
        where_ok = True
        if self.rng.random() < 0.9:
            where, where_ok = self.predicate(table)
            body += " WHERE " + where
        return self._op("update", "UPDATE %s;" % body, "UPDATE OR REPLACE %s" % body,
                        valid=lambda rules: where_ok and all(rules.value_ok(table, c, v) for c, v in assignments))

    def delete(self, table):
        body, where_ok = "FROM %s" % table, True
        if self.rng.random() < 0.97:
            where, where_ok = self.predicate(table)
            body += " WHERE " + where
        return self._op("delete", "DELETE %s;" % body, "DELETE %s" % body, valid=lambda rules: where_ok)

    def create_index(self):
        options = [(t, c) for t, cols in TABLES.items() for c, ty in cols[1:]
                   if ty == "INT" and (t, c) not in self.rules.indexed]
        if not options:
            return None
        table, col = self.rng.choice(options)
        sql = "CREATE INDEX idx_%s_%s ON %s(%s)" % (table, col, table, col)
        return self._op("ddl", sql + ";", sql, index=(table, col),
                        valid=lambda rules: rules.index_ok(table, col))

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
        where_ok = True
        if self.rng.random() < 0.75:
            where, where_ok = self.predicate(table)
            sql += " WHERE " + where
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
                        limit=limit, valid=lambda rules: where_ok)


def replay(ops, exe, workdir, checkpoint_every):
    """Runs a fixed list of ops from scratch. Returns (index, message) for
    the first disagreement, or None."""
    conn = sqlite3.connect(":memory:")
    rules = Rules(conn)
    harness = Harness(exe, os.path.join(workdir, "replay.wal"), checkpoint_every)
    try:
        for i, op in enumerate(ops):
            try:
                problem = execute(harness, conn, rules, op)
            except HarnessCrashed as e:
                problem = str(e)
            if problem:
                return i, problem
        return None
    finally:
        harness.close()
        conn.close()


def shrink(ops, exe, workdir, checkpoint_every):
    """Deletes chunks of statements, then single statements, for as long
    as some disagreement still reproduces."""
    chunk = max(1, len(ops) // 2)
    while True:
        i, removed = 0, False
        while i < len(ops):
            candidate = ops[:i] + ops[i + chunk:]
            found = replay(candidate, exe, workdir, checkpoint_every) if candidate else None
            if found:
                ops = candidate[:found[0] + 1]
                removed = True
            else:
                i += chunk
        if chunk == 1 and not removed:
            return ops
        if not removed:
            chunk = max(1, chunk // 2)


def run_one(seed, steps, exe, skip, workdir, checkpoint_every):
    rng = random.Random(seed)
    conn = sqlite3.connect(":memory:")
    rules = Rules(conn)
    harness = Harness(exe, os.path.join(workdir, "run.wal"), checkpoint_every)
    gen = Generator(rng, conn, rules, skip)
    history = []
    try:
        pending = gen.opening()
        for _ in range(steps):
            op = pending.pop(0) if pending else gen.next()
            history.append(op)
            try:
                problem = execute(harness, conn, rules, op)
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
    ap.add_argument("--harness", default="build/sql_harness")
    ap.add_argument("--runs", type=int, default=100)
    ap.add_argument("--steps", type=int, default=300)
    ap.add_argument("--first-seed", type=int, default=1)
    ap.add_argument("--skip", default="", help="comma-separated: " + ", ".join(FEATURES))
    ap.add_argument("--checkpoint-every", type=int, default=25,
                    help="writes between automatic checkpoints in the harness (0 turns them off)")
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
            history = run_one(seed, args.steps, args.harness, skip, workdir, args.checkpoint_every)
            if history is None:
                continue
            failures += 1
            small = shrink(history, args.harness, workdir, args.checkpoint_every)
            index, problem = replay(small, args.harness, workdir, args.checkpoint_every)
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
