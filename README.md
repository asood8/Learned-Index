# Learned Index — Phase 0

Replacing a B-tree lookup with a small model that predicts a key's
position directly. This is Phase 0: the baselines every later phase
has to beat.

## What's here

- `python/data_gen.py` — generates two sorted, unique `int64` key
  sets as raw binary files: `uniform` (evenly spread) and `skewed`
  (lognormal — dense near zero, long sparse tail). Skewed data is
  the harder case; it's what breaks a single straight-line model in
  Phase 2.
- `cpp/bplus_tree.h` — a real in-memory B+-tree (insert + search,
  with node splitting). Order 64, leaves linked left-to-right.
- `cpp/benchmark.cpp` — loads a dataset, builds both baselines, runs
  a correctness check, then times real lookups with `<chrono>`.
  Appends results to `results/phase0_baseline.csv`.

## How to run it

```bash
python3 python/data_gen.py --n 1000000 --outdir data
g++ -O2 -std=c++17 -o cpp/benchmark cpp/benchmark.cpp
./cpp/benchmark data/uniform_1000000.bin 200000
./cpp/benchmark data/skewed_1000000.bin 200000
```

## Results (1M keys, 200k random lookups, all hits)

| dataset | binary search | B+-tree |
|---|---|---|
| uniform | 218.6 ns/lookup | 309.4 ns/lookup |
| skewed  | 215.7 ns/lookup | 296.4 ns/lookup |

**The interesting part: plain binary search beat the B+-tree here.**
This isn't a bug — it's the actual motivating fact behind learned
indexes. This B+-tree implementation is correct but not
cache-optimized: each node is heap-allocated with `std::vector`
members, so walking down 3-4 levels means 3-4 separate jumps to
scattered memory locations, each one a likely cache miss. Binary
search over one contiguous sorted array, by contrast, stays in a
single memory block that the CPU can keep mostly cached across
repeated queries.

This is close to the same result the original learned-index paper
(Kraska et al., 2018) reports: naive in-memory B-trees often don't
clearly beat binary search either, once you're working entirely in
RAM instead of on disk. A model that predicts the position in one
arithmetic step — no pointer chasing at all — is trying to beat
*both* of these, not just the tree.

## Phase 1 — single learned index

Fit one linear regression (key → position) directly in C++ using the
closed-form least-squares solution (`cpp/linear_model.h`) — no
training loop, since a single-variable regression has an exact
formula. Track the worst prediction error while fitting; that becomes
the radius of a bounded local search done after every prediction.

**Results (1M keys, 200k lookups):**

| dataset | binary search | B+-tree | learned index |
|---|---|---|---|
| uniform | 219.5 ns | 332.5 ns | **150.8 ns** |
| skewed  | 222.7 ns | 333.9 ns | 248.9 ns |

On uniform data the learned index wins outright — about 31% faster
than binary search. On skewed data it loses slightly, and the fitted
model shows exactly why: `max_error = 8,941,571` on an array of only
1,000,000 keys. One straight line can't bend to fit data that's dense
near zero and sparse everywhere else, so the "local search" window
has to be clamped down to nearly the whole array, degenerating into
roughly the same cost as binary search plus wasted prediction
overhead. This is the concrete, measured motivation for Phase 2.

## Phase 2 — segmented (multi-line) learned index

Greedy epsilon-bounded segmentation (`cpp/segmented_model.h`): walk the
sorted data left to right, extending each segment as far as a single
line (anchored exactly at the segment's first point) can stay within
±eps of every other point in it. The moment no valid slope remains,
close the segment and start a new one. One O(n) pass builds the whole
model — no iteration, same as Phase 1.

**Results (1M keys, 200k lookups, eps = 64):**

| dataset | binary search | B+-tree | Phase 1 (1 line) | Phase 2 (segmented) |
|---|---|---|---|---|
| uniform | 218.1 ns | 323.0 ns | 154.7 ns | 155.4 ns (84 segments) |
| skewed  | 212.8 ns | 299.1 ns | 228.5 ns | **25.7 ns (3 segments)** |

Uniform data needed 84 segments and performs about the same as Phase
1 — it barely needed help. Skewed data needed only **3 segments** and
went from *losing* to binary search (Phase 1) to being roughly **8x
faster** than it. Three cheap-to-search lines did what one line
structurally couldn't — this is Phase 2's whole thesis, borne out in
a real, verified number instead of a theoretical claim.

## Phase 3 — inserts via a gapped array

`cpp/gapped_array.h` spreads real keys across an array larger than
necessary (density 0.7 → 30% left as gaps), so most inserts slide
into a nearby empty slot instead of shifting everything after them.
The Phase 2 segmentation algorithm is reused unchanged, just refit
against each key's *gapped* position instead of its dense rank.

**A real bug, found the same way as every other phase — by checking:**
the first working version passed every "did the gap-fill logic work"
check, but the *final* correctness check (are all 1M keys still
findable after 100k inserts) failed for 11 keys. Zero rebalances had
been triggered — every insert found a nearby gap and "succeeded" —
but each small shift nudges nearby keys slightly further from where
the model originally predicted them, and that drift compounds. In a
few unlucky, heavily-inserted-into neighborhoods it exceeded the
±64 search window, even though the keys were still physically
present. Fixed by forcing a periodic rebalance tied to `eps` itself
(not to array size) — since in the worst case, every insert since the
last rebalance could have landed near the same key.

**Results (1M keys, 90% initial / 10% held out and inserted):**

| approach | ns/insert |
|---|---|
| naive vector (shift on every insert) | 302,445 |
| gapped array (with periodic rebalance) | 105,749 |

~2.9x faster than the naive baseline, and — the number that actually
matters — **100% correct**: every one of the 1,000,000 keys is
findable after all inserts, verified directly rather than assumed.
The rebalance count (1,566 full rebuilds) shows the honest cost of
the current fix: the threshold is global, so any 64 inserts anywhere
in the array force a full rebuild of the *entire* structure, not just
the drifting region.

**Known limitation, since fixed:** the first working version rebalanced
the *entire* array whenever drift exceeded eps anywhere, which was
correct but expensive (1,566 full rebalances). Rebalancing is now
tracked **per segment** — each segment refreshes only its own small
physical region once its own insert count crosses the threshold, with
a much less frequent whole-array rebalance kept as a safety net for
drift that spills across a segment boundary.

**Results after the fix, same 1M-key / 100k-insert test:**

| version | ns/insert | rebalances |
|---|---|---|
| naive vector (shift on every insert) | 304,954 | — |
| gapped array, global rebalance only | 105,749 | 1,566 full |
| gapped array, per-segment rebalance | **19,638** | 1,186 local + 196 full |

~15.5x faster than the naive baseline, and ~5.4x faster than the
global-only version — from a change that didn't touch correctness at
all, only *how much* gets rebuilt when drift is detected. Verified
correct for all 1,000,000 keys both before and after the change.

## Phase 4 — correctness test suite

`cpp/test_suite.cpp` goes beyond the sample-based "check the first
2,000 keys" pattern used in earlier phases and specifically targets
edge cases: empty structures, single elements, duplicate inserts, and
fresh-seed stress runs. Every previous phase found a real bug this
way, and this one was no exception — two more turned up:

1. **A real crash** (found via AddressSanitizer): when we generalized
   `build_segmented_model` for Phase 3, `segmented_search` never got
   the same "is this model empty" guard that `GappedArray::search`
   already had. Searching or inserting into a genuinely empty
   structure walked off the end of an empty vector. Fixed by adding
   the missing guard, plus a matching bootstrap path in
   `GappedArray::insert` for inserting into an empty array.
2. **Inconsistent duplicate-key behavior**: the B+-tree already
   treats inserting an existing key as an overwrite (no duplicate
   entry). The gapped array didn't — it silently created a second
   physical copy of the same key. Fixed by checking for an existing
   key first and treating a duplicate insert as a no-op, matching the
   B+-tree's behavior.

**Final result: 19/19 tests passing**, covering the B+-tree, Phase 1's
linear model, Phase 2's segmented model, and Phase 3's gapped array.
All of Phase 0–3's benchmark numbers were re-verified afterward to
confirm neither fix changed performance.

## Phase 5 — durability (write-ahead log)

`cpp/write_ahead_log.h` appends every insert to a plain file and
forces it to disk with `fsync` *before* the in-memory structure is
touched. `cpp/durable_store.h` wraps the Phase 3 gapped array with
this log, replaying it on construction so state survives a restart.

**Proven with an actual simulated restart**, not just asserted:
`cpp/durability_demo.cpp` builds a `DurableStore`, inserts 5,000 keys,
then lets it go out of scope entirely (closing the file, discarding
all in-memory state) before constructing a **brand new** `DurableStore`
from the same file on disk — the same sequence of events as a real
process crash and restart. Result: **all 5,000 keys recovered and
verified**, exactly as if nothing had happened.

**Another real bug, found by this new access pattern specifically:**
the demo's overhead comparison inserts keys in *decreasing* order —
nothing before this had exercised that direction. A key routed to a
segment whose line doesn't represent it well (extrapolating the wrong
way) could produce a wildly negative predicted position, leaving
`hi` negative and an unvalidated negative index flowing straight into
`find_nearest_gap`, which checked `right < n` but never `right >= 0`.
AddressSanitizer caught the resulting out-of-bounds read immediately.
Fixed at the root — `lo`/`hi` are now clamped into `[0, n-1]` with
`std::clamp` so a bad prediction can never produce an invalid index —
plus the missing `right >= 0` check added defensively. All 19 Phase 4
tests and the durability demo both still pass after the fix.

**The honest cost of durability, measured, not assumed:**

| operation | ns/op |
|---|---|
| plain insert (no WAL) | 3,251 |
| durable `put()` (log + fsync + insert) | 118,125 |

Roughly a **36x** overhead — durability isn't free, and `fsync` is
almost the entirety of that cost (measured independently at ~113μs
per call earlier). This is exactly why real databases batch multiple
writes into one fsync ("group commit") instead of syncing after every
single one — not implemented here, but worth knowing why it exists.

## Phase 6a — learned Bloom filter (a genuine negative result)

`cpp/learned_bloom_filter.h` replaces a Bloom filter's bit array with
logistic regression over hashed features (the "hashing trick"),
trained with real gradient descent — the first model in this project
with no closed-form solution, since the sigmoid makes the loss
nonlinear. Every real member the trained model still scores below
threshold goes into a small backup set, preserving the same
zero-false-negative guarantee a classic Bloom filter gives.

**Compared honestly against `cpp/classic_bloom_filter.h`** (built with
the textbook optimal-bits formula), on the same 100,000-key member
set, with empirical false-positive rate measured identically for both
against confirmed true negatives:

| filter | memory | empirical FPR |
|---|---|---|
| classic (built for 1.0% FPR) | 119,814 bytes | 1.021% |
| learned (matched byte budget) | 462,288 bytes | 50.380% |
| learned (matched *slot count*, 64x more memory) | 7,668,048 bytes | 50.225% |

**The classic filter won decisively, and giving the learned filter
64x more memory didn't meaningfully help** — ruling out "too few
buckets causing collisions" as the explanation. The real reason: our
synthetic keys are arbitrary sorted integers with no learnable
structure separating members from non-members. The paper's own
motivating use case (classifying malicious URLs) works because
malicious URLs share real lexical patterns a classifier can
generalize from. Whether one specific integer happens to be among
100,000 essentially-arbitrary values has no such pattern — it's exact
memorization or nothing, and a classic Bloom filter's hash-and-flip-a-bit
*is* memorization, about as space-efficient as information theory
allows. This is worth contrasting directly with why the learned
*index* (Phases 1–3) worked so well: sorted order gives a real, smooth,
learnable relationship between a key and its position (the CDF).
Set membership in an arbitrary collection has no equivalent structure
to learn — the technique isn't broken, it's just being asked a
question this specific data has no learnable answer to.

## Phase 6b — Hawkeye-style learned cache (a genuine positive result)

`cpp/hawkeye_cache.h` trains a second logistic regression — this time
over **recency and frequency**, the two classic predictors of reuse —
to approximate Belady's optimal eviction decisions using only
information available online. Belady's algorithm itself (`cpp/belady.h`)
needs the future to decide what to evict, so it's only usable directly
in two ways: generating training labels on a trace where the "future"
is just data already in hand, and computing an unreachable ceiling
hit rate for comparison.

**Trained on one Zipfian trace, evaluated on a completely separate
one** (different seed) against `cpp/lru_cache.h`, on a workload where
10,000 keys compete for a 500-slot cache (5%) under realistic skew:

| policy | hit rate |
|---|---|
| LRU | 58.67% |
| learned (Hawkeye-style) | **63.36%** |
| Belady's optimal (ceiling, needs the future) | 74.63% |

The learned cache closed **29.4% of the gap** between LRU and the
unreachable theoretical ceiling — a real, meaningful improvement, not
a rounding-error win, and in the same realistic range real Hawkeye/LRB
papers report (nothing online reaches OPT; the point is closing part
of that gap). This is the direct contrast with Phase 6a: **recency and
frequency genuinely predict reuse** in a skewed access pattern — real,
learnable structure — which is exactly what arbitrary key membership
lacked. Same tool (logistic regression, real gradient descent, no
closed-form solution), applied to two different problems, with two
honestly different outcomes.

## Phase 7a — dataset/distribution sweep and real memory footprint

`cpp/full_benchmark.cpp` sweeps three dataset sizes (100K, 1M, 5M —
a real 50x range) across both distributions, measuring not just
latency but **index-only memory** — deliberately excluding the raw
sorted array every approach needs regardless, since the point is what
each *index structure* costs on top of shared data, not the data
itself.

**Memory (the result that matters most here):** the B+-tree costs a
consistent **~37.3 bytes/key** at every single scale tested — that's
real pointer/node overhead, not something that shrinks as data grows.
The segmented index costs **0.000–0.002 bytes/key** — three to four
orders of magnitude smaller — because its memory scales with segment
*count* (in the hundreds), not with `n` at all. This is the original
paper's other headline claim, now demonstrated with our own numbers
across a real size range instead of asserted.

**Speed**, consistent across all three sizes:

| n | binary search | B+-tree | learned (Phase 1) | segmented (Phase 2) |
|---|---|---|---|---|
| 100K uniform | 127.9 ns | 186.4 ns | 74.0 ns | 87.1 ns |
| 1M uniform | 220.7 ns | 330.1 ns | 139.2 ns | 147.9 ns |
| 5M uniform | 357.7 ns | 557.0 ns | 190.6 ns | 200.8 ns |
| 100K skewed | 128.7 ns | 180.0 ns | 149.4 ns | **18.2 ns** |
| 1M skewed | 219.1 ns | 336.1 ns | 227.7 ns | **26.9 ns** |
| 5M skewed | 319.8 ns | 630.1 ns | 343.2 ns | **85.8 ns** |

The Phase 1 vs. Phase 2 story from much earlier holds at every scale:
one line wins on uniform data, loses badly to a single line's own
failure mode on skewed data, and segmentation fixes exactly that,
consistently, not as a one-off result.

**One small honest nuance worth keeping**: on 100K uniform keys,
segmented (87.1ns) is slightly *slower* than the single-line model
(74.0ns) — segment routing has a small, real cost, and when the data
doesn't actually need multiple segments, that cost isn't paid back by
anything. It only pays for itself when segmentation is actually
solving a real problem (skewed data, or larger uniform datasets where
memory-hierarchy effects start to matter).

**Honestly out of scope for this sweep**: the real SOSD benchmark
datasets aren't fetchable in this environment (hosted outside the
network domains available here) — reported with our own uniform and
skewed generators instead, which is what every phase so far has used
anyway.

## Phase 7b — comparison against the real, published PGM-index

Rather than only ever comparing against our own B-tree, `cpp/pgm_comparison.cpp`
benchmarks our from-scratch segmented index against the actual
[PGM-index](https://github.com/gvinciguerra/PGM-index) implementation —
vendored unmodified into `cpp/third_party/pgm/` (Apache 2.0) — on the
same 1M-key datasets, same `eps=64`.

| dataset | ours | PGM-index (default) | PGM-index (no recursion) |
|---|---|---|---|
| uniform | 154.7 ns, 79 segments, 1,896 bytes | **125.6 ns**, 57 segments, 984 bytes | 155.7 ns |
| skewed | **47.6 ns**, 2 segments, 48 bytes | 104.0 ns, 2 segments, 104 bytes | 93.0 ns |

**A genuinely mixed, honest result — not a clean win either way.**
On uniform data, the real implementation wins on every axis: fewer,
better-optimized segments (57 vs. our 79 — direct confirmation of the
tradeoff flagged back in Phase 2, where pinning each segment's anchor
exactly was chosen for easy-to-verify correctness over full
optimality), less memory, and faster lookups.

On skewed data, **ours is over 2x faster** — and this was worth
actually investigating rather than just reporting. PGM-index builds a
recursive routing layer by default (`EpsilonRecursive=4`) that
evaluates a model at *each* level before ever reaching the base
segments. Testing with recursion explicitly disabled
(`EpsilonRecursive=0`) confirms it both ways: uniform data got
*slower* without it (125.6→155.7ns — the hierarchy pays for itself
across 57+ segments), while skewed data got *faster* without it
(104.0→93.0ns — pure overhead when there are only 2 segments to route
between, nothing to gain from hierarchy). That's a real, confirmed,
mechanistic explanation, not a guess. It doesn't fully close the gap,
though — even with recursion off, PGM-index (93.0ns) is still roughly
2x slower than ours (47.6ns) on this dataset, and pinning down the
remaining difference would need real profiling this project didn't do.

The honest takeaway: **a mature, general-purpose implementation
optimizes for the case that's actually hard** (many segments needing
efficient routing) at a small, real cost on the case that's trivial
(few segments) — exactly the kind of engineering tradeoff a
general-purpose library has to make that a narrower, single-purpose
implementation doesn't.

## Phase 7c — adversarial insert pattern (a real, escalating vulnerability)

`cpp/adversarial_test.cpp` targets the actual mechanism Phase 3
built, not a disconnected toy example: both scenarios insert the same
100,000 keys into an identical 900,000-key starting structure — the
only difference is whether those keys scatter across the whole key
range (benign, matching Phase 3's original test) or concentrate into
one narrow window (adversarial). This is exactly the lever a real
attacker with insert access has, and it mirrors real published
research: poisoning attacks on ALEX and PGM-index have been shown to
degrade performance by up to ~20%.

**A real, escalating effect as the window narrows:**

| attack window | ns/insert | local rebalances | full rebalances | degradation |
|---|---|---|---|---|
| benign (scattered) | 14,805.1 | 0 | 195 | — |
| 5.0% of key range | 14,773.9 | 231 | 195 | −0.2% (noise) |
| 2.0% of key range | 15,320.1 | 795 | 195 | +3.5% |
| 1.2% of key range | 16,818.2 | 1,096 | **189** | **+13.6%** |

A mild concentration has no real effect. As it tightens, degradation
climbs clearly and monotonically, reaching **+13.6%** — comparable to,
though a bit under, the ~20% figure from real published attacks on
ALEX/PGM-index. The mechanism is visible directly in the data: full
rebalance count stays roughly flat (even *dips slightly* at the
tightest window) while local rebalances climb sharply (231→1,096) —
Phase 3's per-segment rebalancing is absorbing most of the pressure,
exactly as it was designed to, avoiding the far more expensive
full-array rebuilds an attacker might hope to trigger. But absorbing
isn't free: each local rebalance still costs something, and enough of
them under concentrated pressure adds up to a real, measurable
slowdown. This is a genuinely good outcome to report honestly — the
Phase 3 refinement built purely for speed turned out to also provide
real (if partial) resilience against exactly this kind of attack,
which wasn't the goal when it was built.

## Phase 8 — rows, keys, and a catalog

The storage engine only ever knew `int64 -> key existence`, not real
values — Phase 8 had to actually build `int64 -> bytes` first
(extending the WAL and `DurableStore` from Phase 5), then layer
tables on top of that.

- **`cpp/write_ahead_log.h` / `cpp/durable_store.h`**: each log record
  now carries a length-prefixed value alongside its key, and a
  partial record from a mid-write crash (torn key, length, or value)
  is dropped rather than misread — real WAL recovery always discards
  a torn trailing record. `DurableStore` reuses `GappedArray`'s
  existing no-op-on-duplicate-key behavior (the Phase 4 bugfix) to
  correctly handle both fresh inserts *and* updates during replay
  with no special-casing.
- **`cpp/row_format.h`**: a row is a list of typed values, each
  encoded as a type tag plus bytes (8 raw bytes for an int,
  length-prefixed for text) — a simplified version of what SQLite
  calls its record format.
- **`cpp/table_key.h`**: packs a 16-bit table ID and a 48-bit primary
  key into the one `int64` the storage engine understands, keeping
  every table's rows contiguous and sorted together. Stated plainly:
  primary keys must be non-negative and fit in 48 bits — not a claim
  of supporting arbitrary 64-bit keys.
- **`cpp/catalog.h`**: table ID 0 is reserved for a table describing
  every other table's schema, stored as ordinary rows in the same
  engine — the same pattern SQLite's `sqlite_master` and Postgres's
  `pg_catalog` use. One honest architectural note: `DurableStore` only
  supports point lookups right now, not "list every key in a range"
  (that's Phase 10's job), so the catalog rebuilds itself on startup
  with its own independent pass over the WAL rather than querying the
  index — a little redundant, but correct, and it doesn't fake a
  capability that legitimately belongs to a later phase.

**Proven with the same kind of real restart as Phase 5**:
`cpp/phase8_demo.cpp` creates two tables with different schemas
(`users`, `products`), inserts 150 rows total, then destroys
everything and rebuilds both the catalog and the store from the same
WAL file. Result: both schemas and all 150 rows decode back correctly
— verified directly, not assumed.

## Phase 9 — SQL tokenizer and parser

`cpp/sql_tokenizer.h` splits raw text into keywords, identifiers,
literals, and punctuation. `cpp/sql_ast.h` defines three small AST
types (`CreateTableStmt`, `InsertStmt`, `SelectStmt`), held in a
`std::variant`. `cpp/sql_parser.h` is a straightforward recursive-descent
parser over that token stream — the same technique as writing a
calculator/expression parser, just with a few more statement shapes.

**Grammar supported**: `CREATE TABLE t (col TYPE, ...)`,
`INSERT INTO t VALUES (...)`, and `SELECT * FROM t [WHERE col op val]`
where `op` is `=`, `<`, `>`, `<=`, `>=`, or `BETWEEN ... AND ...`.
Keywords are case-insensitive; whitespace is irrelevant, matching
normal SQL conventions.

**Tested against the whole grammar, not just the happy path**:
`cpp/sql_parser_test.cpp` checks every statement shape's AST fields
directly, plus case-insensitivity, irregular whitespace, negative
integer literals, and — just as important — that genuinely malformed
input (a misspelled keyword, a missing value) actually throws instead
of silently producing a wrong parse. **20/20 tests passing.**

## Phase 10 — the query executor

`cpp/executor.h` walks the AST from Phase 9 and routes each query to
the cheapest path the storage engine actually supports:

- `WHERE <primary key> = v` → one point lookup (`store_.get`) — the
  exact mechanism from Phase 1/2/3, now driven by a query someone typed.
- `WHERE <primary key> BETWEEN a AND b` → a real range scan.
- Anything else, or no `WHERE` at all → a full table scan, filtering
  each row in memory when there's a `WHERE` on a non-key column.

The first column in a table's schema is always treated as the primary
key — this project's `CREATE TABLE` grammar has no explicit
`PRIMARY KEY` syntax, so that's a stated simplification, not an
accident. A full table scan and a `BETWEEN` range scan turned out to
be the exact same underlying operation with different bounds, so both
go through one shared `range_scan` primitive on `DurableStore`.

**A real bug, found the moment everything got wired together for
real**: the first test run returned 1 row for `WHERE id BETWEEN 100
AND 110` instead of 11, and a full table scan returned 2 rows instead
of 1000. The cause: `range_scan_keys`'s loop condition checked
`data_[idx] <= hi` on *every* slot, including gaps — and since a gap
holds `EMPTY_SLOT` (`INT64_MAX`), which is always greater than any
real bound, the scan stopped dead at the very first gap it hit. Given
gaps make up ~30% of the array by design (the entire point of Phase
3), this bug was guaranteed to trigger constantly, not as an edge
case. Fixed by separating "skip this slot" (a gap) from "stop the
scan" (a real key past the bound) — gaps get skipped and the scan
continues; only an actual key beyond the range stops it.

**Tested against real data end-to-end**: a 1,000-row table, checking
not just that each access method gets chosen correctly but that the
*actual returned rows* are correct — point lookup returns the exact
row, the range scan returns exactly the 11 rows it should in sorted
order, the filtered scan returns exactly the rows matching the
predicate, and the full scan returns all 1,000. **14/14 tests
passing.** Confirmed the fix didn't regress Phase 4's 19 tests or
Phase 9's 20.

## Phase 11 — the REPL

`cpp/repl.cpp` is the payoff for every phase before it: read a line,
parse it, execute it, print the result, loop — the same shape as
`sqlite3`'s own command line. The database is backed by a real WAL
file on disk, so closing and reopening the REPL is a genuine restart
using the exact durability machinery proven back in Phase 5/8, not a
simulation of one.

A real session, piped in as a demonstration:

```
db> CREATE TABLE users (id INT, name TEXT, age INT);
OK
db> INSERT INTO users VALUES (1, 'Alice', 30);
OK
db> SELECT * FROM users WHERE id = 2;
id  name  age
--  ----  ---
2   Bob   25
(1 row) [point lookup]
db> SELECT * FROM users WHERE id BETWEEN 1 AND 2;
(2 rows) [range scan]
db> SELECT * FRUM users;
Parse error: expected keyword FROM, got 'FRUM'
db> .tables
users
db> .exit
bye.
```

That `[point lookup]` / `[range scan]` tag after each result isn't
decoration — it's the executor's real routing decision from Phase 10,
made visible. A malformed query prints a clean error and the session
keeps going, rather than crashing. **Verified the persistence claim
directly, not just assumed it**: closing the REPL entirely and
starting a completely separate process against the same WAL file
recovers all 3 rows and the `users` table itself, correctly.

No new bugs surfaced this phase — by this point nearly every piece
being wired together (parser, executor, catalog, durability) had
already been individually tested through nine earlier phases, so a
thin CLI layer on top had little new surface area left to hide a bug in.

## Phase 12 — EXPLAIN

The last phase on the roadmap, and the one that makes the AI part
visible instead of buried three layers down. `GappedArray::search_explain`
is a diagnostic-only twin of the real `search()` — same logic, but it
also tracks the model's raw predicted position and how many slots the
local search actually examined, kept entirely separate so this
instrumentation never costs anything on the real query path. `EXPLAIN`
is scoped to `SELECT` only in the grammar — a deliberate simplification
that avoids a genuinely awkward C++ problem (a `Statement` containing
itself inside a `std::variant` needs pointer indirection to even
compile), and real access-method routing only happens for `SELECT`
anyway.

A real session:

```
db> EXPLAIN SELECT * FROM users WHERE id = 2;
point lookup via learned index -- predicted position 2, corrected in 4 probes, key found
db> EXPLAIN SELECT * FROM users WHERE id = 999;
point lookup via learned index -- predicted position 2, corrected in 5 probes, key not found
db> EXPLAIN SELECT * FROM users WHERE id BETWEEN 1 AND 3;
range scan over the gapped array -- returned 3 row(s)
db> EXPLAIN SELECT * FROM users WHERE age = 30;
full table scan, no index used -- filtered down to 1 matching row(s)
db> EXPLAIN INSERT INTO users VALUES (4, 'x', 1);
Parse error: EXPLAIN is only supported for SELECT statements
```

That "predicted position 2, corrected in 4 probes" line is the actual
model prediction and actual local-search cost from a real query, not
a canned string — this is closer to a real database's `EXPLAIN
ANALYZE` (which executes and reports real statistics) than bare
`EXPLAIN` (which only shows a hypothetical plan), and that fits this
project's whole identity better: real measurements over theoretical
claims, the same standard every phase before this was held to.
**22/22 tests passing**, including that a missing key correctly
reports "not found" with real diagnostics rather than just succeeding
silently, and that `EXPLAIN` on a non-`SELECT` statement is rejected
rather than silently ignored.

# The roadmap is complete

Phases 0 through 12 are all done: a learned-index storage engine
(segmentation, gaps, durability, real adversarial testing) underneath
a real SQL front end (parser, executor, REPL, EXPLAIN). Everything
below is optional stretch work, done on top of that finished base.

# Stretch goals

## UPDATE / DELETE (and a real bug from Phase 2/3, finally caught)

`UPDATE table SET col = val [, ...] [WHERE ...]` and
`DELETE FROM table [WHERE ...]` are both implemented, sharing routing
logic with `SELECT` via a new `find_matches` helper (same point-lookup
/ range-scan / full-scan dispatch, just returning storage keys
alongside rows so they can be re-put or removed).

- **`DELETE` needed a real WAL format change.** A plain "key, no
  value" record is indistinguishable from a genuinely empty value, so
  every record now carries an explicit one-byte type tag (`PUT` or
  `DELETE`) ahead of the key. `GappedArray::remove()` just turns a
  slot back into a gap — reusing the exact concept that made inserts
  cheap in the first place, not a new "tombstone" mechanism.
- **`UPDATE` handles primary-key reassignment properly**, not just
  ordinary column changes: if the first column (the primary key) is
  itself being set, the row's storage key has to move — the old key
  is removed and the new one is put, rather than silently corrupting
  the index.

**Testing `UPDATE ... SET id = 5000` surfaced a real,
previously-undiscovered bug that had been sitting in `search()` since
Phase 2/3.** The original `search()` clamped its search window with
separate `std::max`/`std::min` calls — the exact same bug independently
found and fixed in `insert()` back in Phase 5, and in
`lower_bound_index()` in Phase 10, but never backported to `search()`
itself. When a prediction extrapolates far enough beyond the array
(reassigning a primary key to a much larger, previously-unused value
is exactly this case), clamping each bound against a different limit
independently can produce `lo > hi`, silently emptying the search
window instead of narrowing it — the row was genuinely present
(confirmed via `range_scan`), but `search()` looked in an empty
window and reported "not found." It had gone undetected for ten
phases because ordinary queries never extrapolated far enough to
trigger it — even the earlier "search for a nonexistent key" `EXPLAIN`
test happened to get the right answer for the wrong reason, since an
empty window's default "not found" coincidentally matched what a
genuinely-absent key should report anyway. Fixed by applying the same
`std::clamp` treatment already proven correct elsewhere. **All 48
executor tests now pass**, including that a deletion and a
primary-key reassignment both survive a real restart, not just the
current session.

## ORDER BY, LIMIT, and aggregates

`SELECT` now supports `ORDER BY col [ASC|DESC]`, `LIMIT n`, and two
aggregates, `COUNT(*)` and `SUM(col)`. All three are post-processing
steps applied uniformly after rows are collected — regardless of
which access method found them — so a point lookup, a range scan, and
a full scan all support the same clauses without duplicated logic:
sort first, then truncate to the limit, then (if the query was an
aggregate) collapse the remaining rows into a single summary row.
`SUM` correctly rejects a TEXT column rather than silently returning
garbage. **All 48 executor tests pass**, including that `ORDER BY`
and `LIMIT` compose correctly together (limiting applies *after*
sorting, not before — a `LIMIT 3` on a descending sort returns the 3
largest values, not an arbitrary 3 followed by a sort that never
mattered).

## Secondary index on a non-key column (and two bugs it exposed)

`CREATE INDEX idx_age ON users(age);` builds an index on an `INT`
column, backfilling it from the rows already in the table, and every
later `INSERT`, `UPDATE`, and `DELETE` keeps it in sync. A `WHERE age
= 45` on an indexed column then routes to a **secondary index lookup**
instead of a full scan. The index's metadata lives in the catalog like
any schema, so it survives a restart.

It needed no new data structure. Ages repeat, but the learned index
needs unique keys, so each entry is a composite key: the indexed
value in the top 20 bits, the row's primary key in the low 28
(`pack_secondary_composite` in `table_key.h`). Every entry is unique,
and all rows with `age = 45` sit in one contiguous key range, so the
lookup is just a range scan over the same gapped array that holds the
tables. (Simplification, stated plainly: `INT` columns only, values up
to ~1M, primary keys up to ~268M.)

On the executor test's 1000-row table, `WHERE age = 33` through the
index vs. `WHERE name = 'user_500'` (no index on `name`, so a full
scan): **18.1x–22.2x faster across four runs** (6,376–7,745 ns vs
134,959–155,267 ns).

### The bug: an index entry that was written but couldn't be found

One test failed: insert a row with age 45, `UPDATE` it to 77, and
`WHERE age = 77` found nothing, even though the index entry was
there. The table had ages 20–69, plus one row an earlier test had
updated to 99. With eps = 64 the index segmented into two pieces: one
covering ages 20–69 and one starting at the lone 99. A key for age 77
falls in the first segment's key range, but that segment's line was
only ever fit up to 69, so for 77 it extrapolated straight past its
own data: **it predicted slot 1644 in a 1430-slot array, for a key
whose true position was slot 1407.**

That's the general problem: **the eps guarantee only covers keys the
model was trained on.** Every key inserted since the last refit, and
every lookup for a key that isn't there, is untrained, and nothing
bounded how far those predictions could miss. Four functions each
scanned the same ±eps window around the prediction, and each went
wrong in its own way when the window missed:

- `lower_bound_index()` (range scans) returned a key *later* than the
  true first match: here the age-99 key at the very end, so the scan
  saw a key above its bound and stopped immediately. This was the
  failing test.
- `insert()` defaulted to "just past the window" when nothing in it
  qualified, **placing keys out of sorted order**. No existing test
  caught this. A fuzz run (300 randomized trials of outlier-shaped
  data with inserts and removes, checked against a `std::set` after
  every operation) found the array out of order in **284 of 300
  trials** on the previously committed code.
- `search()` / `search_explain()` had grown bounded, then full-array,
  fallback scans during a first attempt at this bug. Point lookups
  then passed, but only by scanning everywhere. That hid the
  out-of-order array rather than fixing it (range scans were still
  wrong in 299 of 300 fuzz trials), and it made **every lookup for an
  absent key O(n)**: 630,566 ns instead of tens of ns. `insert()` does
  one of those per new key (its duplicate check), so the Phase 3
  insert benchmark went from 21,691 to **540,180 ns/insert**, slower
  than the naive shift-everything vector (263,398 ns) it's supposed to
  beat. That attempt never shipped.

### The fix: correctness from sortedness, speed from the model

Two changes, both in `gapped_array.h`:

1. **One walk instead of four windows.** A single `locate()` walks
   from the predicted slot toward the answer: first left while the
   slot to the left is a gap or a key ≥ target, then right past gaps
   and keys < target. After the left walk every real key to the left
   is < target (the array is sorted), so the first key ≥ target the
   right walk reaches is the true lower bound, *whatever the model
   predicted*. For example, slots `[10, _, 20, 30, _, 40, 50, _]`
   (`_` = gap), first key ≥ 35, bad prediction slot 7: the left walk
   passes 50, 40, and the gap at slot 4, then stops because slot 3
   holds 30 < 35; the right walk skips that gap and lands on 40 at
   slot 5. A prediction of slot 0 reaches the same slot by walking
   right the whole way. `search`, `search_explain`,
   `lower_bound_index`, and `insert` all use `locate()` now, so a fix
   can't land in one of them and be missed in another. That's the
   pattern that let the Phase 5 clamping bug sit in `search()` for ten
   phases (see UPDATE/DELETE above), and here it happened again,
   across four functions at once.
2. **A segment can't predict past the next segment's anchor.** This
   is the model-level part: a segment's line was only fit between its
   own anchor and the next one's (both hit exactly, since anchors are
   pinned), so its predictions are clamped to that stretch. For the
   age-77 key the prediction now lands on the age-99 anchor, right
   where untrained values in that gap belong, instead of off the end
   of the array.

**What this does and doesn't guarantee.** Correctness no longer
depends on the model at all, only on the array being sorted, which
`insert()` now maintains because it finds its slot with the same
walk. Speed still depends on the model: the walk costs O(prediction
error), so a badly wrong model makes lookups slow, not wrong, and the
worst case is O(n). The clamp limits the error for untrained keys at
refit time. The argument: an untrained key's prediction lands between
its trained neighbors' predictions, or at the next anchor, so it's
off by at most about eps plus the spacing between those neighbors.
Drift between refits is limited by the per-segment rebalance
thresholds. Neither is a hard bound the way eps is for trained keys.
The other model-level option, forcing extra segment splits around
outliers, was set aside because it still leaves every untrained
lookup outside any guarantee. The walk makes every lookup correct,
and the clamp handles the outlier case that made it slow.

**Measured after the fix:**
- **All tests pass: 22/22 storage, 20/20 parser, 60/60 executor**,
  also clean under AddressSanitizer + UBSan (the only report is the
  B+-tree's known, deliberate leak). The three new storage tests are
  the failing executor test's exact shape reproduced with no SQL
  involved, plus 100 randomized outlier trials with inserts and
  removes, checked against a `std::set` after every operation.
- The 300-trial fuzz: **0 out-of-order, 0 wrong range scans, 0 wrong
  point lookups** (was 284 / 299 / 295 on the committed code).
- Phase 3 insert benchmark (1M uniform keys, 100,265 inserts, same
  machine, back to back): **21,608 ns/insert before vs 21,537 after,
  no change.** Skewed: 32,189 vs 28,633, one run each, so not a claim
  that it's faster.
- Phase 7c adversarial test: no measurable change, though "measurable"
  is doing real work there. On this machine its degradation number
  varies a lot from run to run: the 1.2%-window scenario ranged from
  −3.7% to +22.8% across five runs before the fix, and −7.0% to
  +37.9% across four runs after.
