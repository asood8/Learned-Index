# Learned Index Database — Roadmap

An embedded SQL database whose storage engine is a learned index
(a small model predicting a key's position) instead of a B-tree —
architected the same way SQLite is: a SQL front end sitting on top
of a swappable storage engine.

**Status:** Phases 0–3 complete (see `README.md`, `python/`, `cpp/`,
`results/`). Phase 3's per-segment rebalancing refinement is done:
19,638ns/insert, ~15.5x faster than naive full-shift inserts, verified
correct for all 1M keys after 100k inserts.

---

## Part 1 — Storage engine

### Phase 0 — Baselines ✅ done
Sorted uniform + skewed synthetic datasets, a real B+-tree (insert +
search + node splitting), a binary-search baseline, and a benchmark
harness with a correctness check before any timing. Result: binary
search beat the hand-rolled B+-tree (~218ns vs ~309ns/lookup on 1M
uniform keys) — the actual motivating fact for everything below.
*(~300 lines, done)*

### Phase 1 — Single learned index ✅ done
Fit one linear regression (key → position) directly in C++ via the
closed-form least-squares solution — no training loop needed. Track
the worst prediction error while fitting; that becomes the bounded
local-search window for every lookup. Result: beat binary search by
~31% on uniform data (150.8ns vs 219.5ns); lost slightly on skewed
data (248.9ns vs 222.7ns) because one line can't fit the skew —
`max_error` came out to 8.9M on a 1M-key array. *(~130 lines, done)*

### Phase 2 — Multi-segment learned index ✅ done
Greedy epsilon-bounded segmentation, anchor pinned exactly per
segment, one O(n) pass. Result: 3 segments on skewed data took
lookups from 228.5ns (Phase 1, losing to binary search) down to
25.7ns — an ~8x speedup over binary search on the exact dataset that
broke a single line. Uniform data needed 84 segments and performed
about the same as Phase 1, as expected. *(~110 lines, done)*

### Phase 3 — Updates (inserts) ✅ done, including the per-segment refinement
Gapped array (density 0.7) reusing Phase 2's exact segmentation
algorithm, refit against gapped positions instead of dense rank. A
real drift bug was found via the final correctness check (11 of 1M
keys unfindable after inserts, despite zero rebalances — compounding
shift drift from many nearby inserts) and fixed by forcing periodic
rebalances tied to `eps`. That first fix was correct but expensive
(1,566 full-array rebalances); rebalancing was then made per-segment,
so each segment refreshes only its own small physical region, with a
much rarer full-array rebalance kept as a safety net. Final result:
19,638ns/insert (~15.5x faster than naive full-shift inserts, ~5.4x
faster than the global-only fix), verified correct for all 1M keys
throughout. *(~230 lines, done)*

### Phase 4 — Correctness test suite
Before trusting any speed number: empty array, duplicate keys,
boundary keys (first/last), and correctness after a batch of random
inserts. Proves it's right, not just fast. *(~100–200 lines)*

### Phase 5 — Durability (write-ahead log)
The one non-negotiable addition for calling this a database rather
than a data structure. Every `put` gets appended to a log file
*before* touching the in-memory structure; on startup, replay the log
to rebuild state. This is the literal "D" in ACID. *(~80–120 lines)*

### Phase 6 — Cooperating learned layers *(optional, parallel — can be built before or after Part 2)*
Give the classifier idea from earlier in this project an actual job:
- A **learned Bloom filter** in front of `get` — a small classifier
  answers "definitely not here" and skips everything else.
- A **Hawkeye/LRB-style cache classifier** — decides which recently
  accessed keys are worth keeping hot, instead of plain LRU, and
  learns from each access the learned index serves.

Both sit *inside* `get`/`put` without changing what those functions
look like from the outside — the SQL layer in Part 2 won't know or
care whether this phase happened first or last. *(~200–300 lines)*

### Phase 7 — Full benchmark suite
The "prove it" phase:
- Sweep dataset size (1M/10M/100M) and distribution (uniform, skewed,
  and the real SOSD benchmark dataset the literature uses).
- Measure **memory footprint**, not just latency — the original
  paper's other headline claim, and the one most reimplementations
  skip.
- Compare against your own B+-tree/binary-search baselines **and**
  the real published [PGM-index](https://github.com/gvinciguerra/PGM-index)
  implementation — a fairer bar than grading your own homework.
- Test under a deliberately adversarial/poisoned key-insertion
  pattern and report the degradation — this is a real, published
  research angle (poisoning attacks on ALEX/PGM-index have been shown
  to degrade performance by up to 20%), not a stretch of the concept.

*(~250–350 lines)*

---

## Part 2 — SQL layer

### Phase 8 — Rows, keys, and a catalog
The storage engine only knows `int64 → bytes`. To get tables:
- **Key**: pack a table ID and primary key into one `int64`
  (`(table_id << 48) | primary_key`) — keeps each table's rows
  contiguous, which your Phase 2 segments already specialize on.
- **Row**: a small binary format — per column, a type tag plus bytes
  (8 bytes for an int, length-prefixed for text).
- **Catalog**: reserve table ID 0 for a table describing all other
  tables' schemas — the same pattern SQLite's `sqlite_master` and
  Postgres's `pg_catalog` use.

*(~150–200 lines)*

### Phase 9 — SQL tokenizer + parser
A tokenizer (keywords/identifiers/literals/punctuation), then a
recursive-descent parser producing a small AST. Keep the grammar
small and real: `CREATE TABLE`, `INSERT INTO ... VALUES (...)`,
`SELECT * FROM ... WHERE col = / < / > / BETWEEN ...`.
*(~250–350 lines)*

### Phase 10 — Query executor
Where everything above gets used. Route each query to the cheapest
path the storage engine supports:
- `WHERE id = 42` → one learned-index point lookup (Phase 1/2).
- `WHERE id BETWEEN x AND y` → range scan via linked leaves.
- Anything else, or no `WHERE` → full sequential scan (the
  intentional slow path, same as any database without a secondary
  index on that column).

*(~150–250 lines)*

### Phase 11 — REPL
Read a line, parse it, execute it, print the result, loop — a tiny
`sqlite3`-style command line. The single highest payoff-per-line item
in the project: the difference between a benchmark script and someone
typing `SELECT * FROM users WHERE id = 42;` and watching it return.
*(~80–120 lines)*

### Phase 12 — `EXPLAIN`
Since the executor already knows which path it took, print it:
`EXPLAIN SELECT ...` → "point lookup via learned index, predicted
position 4,812, corrected in 2 probes." Makes the AI part visible
instead of buried three layers down. *(~40–60 lines)*

---

## Stretch goals (roughly in order of payoff)
- `UPDATE` / `DELETE` — mechanically similar to `INSERT`, reuses the
  executor's `WHERE`-matching.
- `ORDER BY`, `LIMIT`, `COUNT`/`SUM`.
- A second index on a non-key column — forces you to confront *why*
  only one column gets the fast path, deepening the index story.

## Explicitly out of scope (say so in the README)
Multi-table joins, transactions spanning multiple statements, and
concurrent multi-client access are each their own substantial
project. With Parts 1 and 2 done, the accurate label is an
**embedded SQL database with a learned-index storage engine** — the
same category as SQLite, not a client-server system like Postgres.

## Last step — a public writeup
Not code. A short technical post: what you built, where it beat a
real B-tree, where it didn't, what happened when you tried to poison
it. This is what turns a repo someone glances at for ten seconds into
something an interviewer can actually ask you about.

---

## Rough total size
Phase 0 (done, ~300) + Phases 1–7 (~1,130–1,570) + Phases 8–12
(~670–980) + stretch (~200–300) ≈ **2,300–3,150 lines**, not counting
the writeup.
