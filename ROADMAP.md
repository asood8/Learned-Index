# Learned Index Database — Roadmap

An embedded SQL database whose storage engine is a learned index
(a small model predicting a key's position) instead of a B-tree —
architected the same way SQLite is: a SQL front end sitting on top
of a swappable storage engine.

**Status:** Storage engine (Phases 0-7) and Phase 8 (rows/keys/catalog)
complete; Phase 9 (SQL tokenizer + parser) done (see `README.md`,
`python/`, `cpp/`, `results/`). CREATE TABLE, INSERT, and SELECT (with
=, <, >, <=, >=, BETWEEN) all parse into a clean AST; 20/20 tests
passing, including that malformed input actually throws.

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

### Phase 4 — Correctness test suite ✅ done
Dedicated edge-case testing beyond earlier phases' sample checks:
empty structures, single elements, duplicate inserts, fresh-seed
stress runs. Found and fixed two real bugs: a crash on empty
structures (`segmented_search` was missing the empty-model guard
`GappedArray::search` already had, found via AddressSanitizer), and
inconsistent duplicate-key handling (the gapped array created a
second copy instead of a no-op, unlike the B+-tree). Result: 19/19
tests passing; all earlier phases' benchmarks re-verified afterward
to confirm neither fix changed performance. *(~140 lines, done)*

### Phase 5 — Durability (write-ahead log) ✅ done
Every insert logged and `fsync`'d to disk before touching the
in-memory structure; replayed on startup. Proven with an actual
simulated restart (destroy the object, rebuild from the same file on
disk) — all 5,000 test keys recovered and verified. Found and fixed a
real out-of-bounds bug along the way: a decreasing-key insert pattern
(never exercised before) could extrapolate a wildly negative
predicted position, leaving an unvalidated negative index reaching
`find_nearest_gap`. Fixed by clamping the prediction window into
valid bounds at the source. Measured cost of real durability: ~36x
per-operation overhead vs. no WAL, almost entirely `fsync` (~113μs/call
measured independently) — the reason real systems batch commits
instead of syncing every write. *(~120 lines, done)*

### Phase 6 — Cooperating learned layers *(optional, parallel — can be built before or after Part 2)*
**6a: learned Bloom filter ✅ done — a genuine negative result.**
Logistic regression over hashed features, trained via real gradient
descent (no closed-form solution here, unlike every earlier model),
with a backup set preserving the zero-false-negative guarantee.
Compared honestly against a classic Bloom filter built with the
optimal-bits formula: classic won decisively (1.0% vs 50.4% FPR), and
giving the learned filter 64x more memory (matched slot count instead
of matched bytes) didn't meaningfully help — ruling out "too few
buckets" as the explanation. Root cause: arbitrary numeric key
membership has no learnable structure the way sorted position does;
the paper's real use case (malicious URLs) has genuine lexical
patterns to generalize from, and this synthetic data doesn't have an
equivalent. *(~150 lines, done)*

**6b: Hawkeye/LRB-style cache classifier ✅ done — a genuine positive
result.** Logistic regression over recency and frequency (log1p-scaled),
trained via gradient descent on Belady-labeled data from a training
trace, evaluated on a completely separate trace. Result: 63.36% hit
rate vs. LRU's 58.67%, closing 29.4% of the gap to Belady's optimal
ceiling (74.63%, unreachable online since it needs the future) — a
real, meaningful win, in the realistic range actual Hawkeye/LRB papers
report. Direct contrast with 6a: recency/frequency genuinely predict
reuse in a skewed workload, unlike arbitrary key membership.
*(~180 lines, done)*

### Phase 7 — Full benchmark suite
**7a: dataset/distribution sweep + real memory footprint ✅ done.**
Swept 100K/1M/5M keys × uniform/skewed, measuring index-only memory
(excluding the shared raw array) alongside latency. Confirmed both of
the original paper's headline claims hold at every scale tested, not
just one: B+-tree costs a consistent ~37.3 bytes/key; segmented index
costs 0.000–0.002 bytes/key (3–4 orders of magnitude smaller, scales
with segment count, not `n`). Segmented index also wins on speed at
every scale, most dramatically on skewed data (up to ~7x over binary
search). One honest nuance found: segment routing has a small real
cost that isn't paid back on uniform data at small `n`, where a
single line was already sufficient. Real SOSD datasets weren't
fetchable in this environment (hosted outside available network
domains) — used our own generators instead. *(~200 lines, done)*

**7b: real PGM-index comparison + adversarial testing ✅ done.**
Vendored the actual published PGM-index (Apache 2.0) into
`cpp/third_party/pgm/` for an honest side-by-side. Mixed, genuinely
investigated result: PGM-index wins on uniform data (fewer, better
segments — direct confirmation of the "pinned anchor" tradeoff flagged
back in Phase 2), ours wins 2x on skewed data. Dug into *why*: PGM's
default recursive routing layer helps when there are many segments to
route among (uniform) and is pure overhead when there are only 2
(skewed) — confirmed directly by testing with recursion disabled,
though it didn't fully close the gap. Adversarial test targeted the
actual Phase 3 mechanism: concentrating the same insert count into a
narrowing window produced a real, escalating degradation (−0.2% → 
+3.5% → **+13.6%**), comparable to the ~20% figure from real published
poisoning attacks on ALEX/PGM-index. Local-rebalance count climbed
sharply while full-rebalance count stayed flat — Phase 3's per-segment
refinement absorbs most of the attack, a genuine unplanned benefit of
work originally done purely for speed. *(~350 lines, done)*

---

## Part 2 — SQL layer

### Phase 8 — Rows, keys, and a catalog ✅ done
Had to extend the WAL/`DurableStore` first: they only tracked key
existence, not real values, so `int64 → bytes` storage didn't
actually exist yet. Row format (type tag + bytes per column), key
packing (16-bit table ID + 48-bit primary key, non-negative keys
only), and a catalog storing schemas as rows under table ID 0 — the
same pattern SQLite/Postgres use. Catalog rebuilds itself via its own
WAL replay pass rather than a range query, since `DurableStore` only
supports point lookups until Phase 10. Proven with a real restart:
2 tables, 150 rows, all correctly recovered. *(~280 lines, done)*

### Phase 9 — SQL tokenizer + parser ✅ done
Tokenizer (keywords/identifiers/literals/punctuation) plus a
recursive-descent parser producing a `std::variant` AST across three
statement kinds. `CREATE TABLE`, `INSERT INTO ... VALUES (...)`, and
`SELECT * FROM ... WHERE col =/</>/<=/>=  or BETWEEN ... AND ...` all
supported. Tested against every statement shape plus case-
insensitivity, whitespace, negative literals, and that malformed
input actually throws: 20/20 passing. *(~330 lines, done)*

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
