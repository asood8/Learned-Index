# Learned Index DB

[![tests](https://github.com/asood8/Learned-Index/actions/workflows/tests.yml/badge.svg)](https://github.com/asood8/Learned-Index/actions/workflows/tests.yml)

A small embedded SQL database in C++17 whose storage engine is a
learned index instead of a B-tree. To find a key, it evaluates a
piecewise-linear model that predicts where the key sits in a sorted
array, then walks from that guess to the exact slot. The design
follows Kraska et al., "The Case for Learned Index Structures" (2018),
and borrows from PGM-index (error-bounded segmentation) and ALEX
(gapped arrays for inserts).

It's embedded in the same sense SQLite is: one process, a write-ahead
log on disk, and a SQL front end over a storage engine. There are no
joins, no multi-statement transactions, and no concurrent access.

## Results at a glance

Every number here came from an actual run, on 1M keys unless noted.
The section in parentheses has the details.

| | result |
|---|---|
| Point lookup, skewed keys | 26.9 ns, vs 336.1 ns for a B+-tree and 219.1 ns for binary search (7a) |
| Point lookup, uniform keys | 147.9 ns, vs 330.1 ns for a B+-tree and 220.7 ns for binary search (7a) |
| Index memory | 0.000–0.002 bytes/key, vs about 37.3 for the B+-tree (7a) |
| Inserts | 19,638 ns/insert, about 15.5x faster than shifting a sorted vector (3) |
| Against the published PGM-index | slower on uniform data (154.7 vs 125.6 ns), about 2x faster on skewed (47.6 vs 104.0 ns) (7b) |
| Learned Bloom filter | lost badly: 50.4% false positives vs 1.021% for a classic one (6a) |
| Secondary index | 18–22x faster than a full scan for the same query (stretch goals) |

## Building and running

Every program is a single `.cpp` file under `cpp/`, built with g++
(C++17). The Makefile wraps the common tasks and puts binaries in
`build/`:

```bash
make test        # build and run the three test suites
make asan        # the same suites under AddressSanitizer and UBSan
make difftest    # compare against SQLite (Python standard library only)
make demos       # the durability and catalog restart demos
make repl        # build and start the SQL shell
make data        # generate the benchmark datasets (needs numpy)
make all         # build everything, benchmarks included
```

The benchmarks are run by hand, since most of them take a dataset
path, and each section below names the one it used. The write-ahead
log uses POSIX calls like `fsync` and `ftruncate`, so this builds on
Linux, or on Windows through WSL.

The rest of this file is a build log, written phase by phase as the
project grew. Each section records what was built, what broke, and
what the numbers were at the time, so some earlier sections describe
limitations that later ones fix.

## Phase 0 — baselines

The baselines every later phase has to beat.

### What's here

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

### How to run it

```bash
python3 python/data_gen.py --n 1000000 --outdir data
g++ -O2 -std=c++17 -o cpp/benchmark cpp/benchmark.cpp
./cpp/benchmark data/uniform_1000000.bin 200000
./cpp/benchmark data/skewed_1000000.bin 200000
```

### Results (1M keys, 200k random lookups, all hits)

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

## Phase 7c — adversarial insert pattern

`cpp/adversarial_test.cpp` asks what happens when someone with insert
access concentrates their writes instead of spreading them out. Every
scenario inserts 100,000 new keys into the same 900,000-key starting
structure. The benign case scatters them across the whole key range,
like the Phase 3 benchmark. The adversarial cases cram them into one
window covering 5%, 2%, or 1.2% of the range, which makes the
segments in that region run out of gaps and rebalance much more
often. Published poisoning attacks on ALEX and PGM-index report
slowdowns of up to about 20%, which gives a rough yardstick.

The first version of this test ran each scenario once. It reported
+13.6% for the 1.2% window, with the slowdown growing steadily as the
window shrank. When I reran it later on a different machine, single
runs of the 1.2% window came back anywhere between −7% and +38%, so
one run per scenario couldn't separate the effect from ordinary
timing noise. The test now runs every scenario 9 times, interleaved
so that drift in machine state hits all of them about equally, and
compares each adversarial run against the benign run from the same
round. Two separate 9-round runs:

| scenario | local rebalances | full rebalances | median slowdown, run 1 | median slowdown, run 2 |
|---|---|---|---|---|
| benign (scattered) | 0 | 195 | — | — |
| 5.0% window | 231 | 195 | +4.1% | −0.9% |
| 2.0% window | 792 | 195 | +8.8% | −4.2% |
| 1.2% window | 1,096 | 189 | +15.6% | +15.0% |

The 1.2% window is the only result here I'd trust. Its median came
out around 15% both times, close to the original 13.6%. Individual
rounds still varied a lot (from −17% to +51% across all 18), so 15%
is a typical cost, not a fixed one. The 5% and 2% windows showed no
consistent effect: slower in one run, faster in the other. The
"steady escalation" in the original single-run table was noise.

The rebalance counts don't depend on timing, and they tell a clearer
story. As the window narrows, local rebalances go from 0 to 1,096
while full rebalances stay flat and even drop slightly, to 189, at
the tightest window. Phase 3's per-segment rebalancing soaks up
nearly all of the concentrated pressure, so the attack never gets
the expensive whole-array rebuilds it's aiming for. Local rebalances
still cost something, and at the tightest window there are enough of
them to add up to roughly 15%. That resilience was a side effect,
since per-segment rebalancing was only added to make ordinary inserts
faster.

These numbers were measured after the secondary-index fix described
at the end of this file. That fix changed the 2% window's local
rebalance count from 795 to 792 and none of the other counts.

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
column and backfills it from the rows already in the table. After
that, every `INSERT`, `UPDATE`, and `DELETE` keeps it in sync, and a
query like `WHERE age = 45` goes through the index instead of
scanning the whole table. The index definition is stored in the
catalog like a table schema, so it survives a restart.

No new data structure was needed. Ages repeat, but the learned index
needs unique keys, so each index entry is a composite key: the
indexed value in the top 20 bits and the row's primary key in the low
28 (`pack_secondary_composite` in `table_key.h`). That makes every
entry unique and puts all the `age = 45` rows in one contiguous key
range, so the lookup is a range scan over the same gapped array that
holds the tables. The limits are real: `INT` columns only, values up
to about 1M, and primary keys up to about 268M.

On the executor test's 1,000-row table, `WHERE age = 33` through the
index took 6,376–7,745 ns across four runs. `WHERE name =
'user_500'`, which has no index and falls back to a full scan, took
134,959–155,267 ns. That's 18–22x faster.

### The bug: an index entry that was written but couldn't be found

One test kept failing. It inserts a row with age 45, updates it to
77, and then queries `WHERE age = 77`. The query came back empty even
though the index entry was there.

The table had ages 20 through 69, plus one row that an earlier test
had updated to 99. With eps = 64, the index split into two segments:
one for ages 20–69 and one starting at the lone 99. A key for age 77
falls inside the first segment's key range, but that segment's line
had only ever been fit up to 69. For 77 it kept extrapolating past
its own data and predicted slot 1644. The array only had 1430 slots,
and the key actually belonged at slot 1407.

The underlying problem is that the eps guarantee only covers keys the
model was trained on. A key inserted since the last refit is
untrained, and so is any lookup for a key that isn't there. Nothing
limited how far off those predictions could be. Four functions
searched the same ±eps window around the prediction, and each one
failed differently when the window missed.

`lower_bound_index()`, where range scans start, returned a key later
than the true first match. In the failing test that was the age-99
key at the very end of the array, so the scan saw a key above its
upper bound and stopped immediately.

`insert()` fell back to "just past the window" when nothing in the
window qualified, which put keys out of sorted order. No existing
test caught this. While debugging I ran a fuzz test: 300 randomized
trials of outlier-shaped data with inserts and removes, checked
against a `std::set` after every operation. On the committed code the
array ended up out of order in 284 of the 300 trials.

`search()` and `search_explain()` had picked up extra fallback scans
during my first attempt at fixing this, first over a bounded range
and then over the whole array. Point lookups passed after that, but
only because they searched everywhere. The array was still out of
order, and range scans were still wrong in 299 of 300 fuzz trials.
Worse, every lookup for an absent key became O(n), about 630,000 ns.
`insert()` does one of those lookups for every new key as its
duplicate check, so the Phase 3 insert benchmark went from 21,691 to
540,180 ns per insert. That's slower than the naive vector that
shifts every element (263,398 ns), which the gapped array exists to
beat. That version was never committed.

### The fix

Both changes are in `gapped_array.h`.

The first replaces the four windows with one walk. A new `locate()`
function starts at the predicted slot and moves left as long as the
slot to its left is a gap or holds a key ≥ the target. Then it moves
right past gaps and keys smaller than the target. Once the left walk
stops, every real key to its left is smaller than the target, because
the array is sorted. So the first key ≥ the target that the right
walk reaches is the correct answer, no matter what the model
predicted.

A small example: the slots hold `[10, _, 20, 30, _, 40, 50, _]`,
where `_` is a gap. We want the first key ≥ 35, and the model badly
predicts slot 7. Walking left, it passes the 50 in slot 6, the 40 in
slot 5, and the gap in slot 4, then stops because slot 3 holds 30,
which is less than 35. Walking right, it skips the gap and stops at
40 in slot 5. If the model had predicted slot 0 instead, the right
walk would have covered the whole distance and reached the same slot.

`search`, `search_explain`, `lower_bound_index`, and `insert` all go
through `locate()` now. Having one code path matters here. The Phase
5 clamping bug sat in `search()` for ten phases because it was fixed
in one function and never copied into its siblings (see UPDATE/DELETE
above), and this time the same kind of flaw was spread across four
functions at once.

The second change is at the model level. Each segment's line passes
exactly through its own first key, so the position where every
segment starts is known precisely. A segment's line was only fit to
the keys between its own start and the next segment's start, so its
predictions are now clamped to that range. For the age-77 key the
prediction now lands on the age-99 segment's start, which is right
where values in that gap belong, instead of past the end of the
array.

### What this guarantees and what it doesn't

Correctness no longer depends on the model at all. It only depends on
the array staying sorted, and `insert()` keeps it sorted now because
it finds its slot with the same walk. Speed still depends on the
model. The walk takes time proportional to how far off the prediction
is, so a bad model makes lookups slow instead of wrong, and the worst
case is still O(n).

The clamp limits how far off an untrained key's prediction can be
right after a refit. Its prediction falls between the predictions for
its trained neighbors, or at the next segment's start, so it's off by
at most about eps plus the distance between those neighbors. Between
refits keys drift, and the per-segment rebalance thresholds keep that
drift in check. Neither of these is a hard bound the way eps is for
trained keys.

I also considered fixing this purely at the model level, by forcing
extra segment splits around outliers. I didn't go that way because
it would still leave every untrained lookup without a guarantee. The
walk makes every lookup correct, and the clamp handles the outlier
case that made it slow.

### Results after the fix

All tests pass: 22/22 storage tests, 20/20 parser tests, and 60/60
executor tests. They also run clean under AddressSanitizer and UBSan,
apart from the B+-tree's known leak. The three new storage tests
recreate the failing executor case without any SQL, and run 100
randomized outlier trials with inserts and removes, checked against a
`std::set` after every operation.

The 300-trial fuzz test now finds nothing wrong: no out-of-order
arrays, no wrong range scans, and no wrong point lookups, down from
284, 299, and 295 failing trials.

Insert speed didn't change. On the Phase 3 benchmark (1M uniform
keys, 100,265 inserts, run back to back on the same machine) it was
21,608 ns per insert before the fix and 21,537 after. The skewed
dataset went from 32,189 to 28,633, but that's one run each, so I'm
not calling it an improvement.

The Phase 7c adversarial test didn't show a change either, though
single runs of it were too noisy to compare before and after. The
multi-round medians in that section were measured after this fix.

## Crash recovery in the write-ahead log

Going back over the durability code turned up a bug in the one job a
write-ahead log exists to do. Phase 5's replay already skipped a
record that a crash had cut off partway through. What it didn't do was
remove that record's bytes from the file. The log reopened in append
mode, so the first write after the restart landed right behind the
half-written record. On the restart after that, replay read the torn
record's leftover bytes plus the start of the next record as if they
were one complete record, and everything after that point was either
lost or misread.

Two smaller problems came with it. Records had no checksums, so a run
of zero bytes at the end of the file, which some filesystems can leave
behind after a crash, parsed as valid PUTs of key 0, and a damaged
byte in the middle of the log was accepted as data. The value length
was also trusted before being checked against the file, so a damaged
length could make replay try to allocate gigabytes.

I wrote the tests before touching the code. Each one writes a real log
through `DurableStore`, damages the file, and restarts from it: cut
the last 3 bytes off, restart, write, and restart again; append 64
zero bytes; flip one byte in the middle; and point it at a file that
isn't a log at all. That's five checks across four scenarios, and on
the old code four of them failed. The only one that passed was
dropping the torn record itself.

The fix is in `write_ahead_log.h`. The file now starts with an 8-byte
magic string, every record ends with a CRC-32 of its own bytes, and
opening the log finds the last record that checks out and cuts the
file back to it before anything new is written. Replay stops at the
first bad record even if later bytes look fine, because nothing after
a damaged record can be trusted to line up; that's the usual rule for
write-ahead logs. A file that doesn't start with the magic string is
refused and left alone, instead of being treated as one long torn
record and truncated to nothing. Each record also goes out in a single
`write()`, and if that fails partway the file is cut back, so a failed
append can't leave half a record behind either.

All five checks pass now, along with the other suites, and the
durability demos still recover all of their data. Logs written by the
old code can't be read by the new one; there were no real databases
around to migrate, so I didn't write a converter. This version also
left one gap: after a new log file was created, its parent directory
wasn't fsync'd, which some filesystems need before the file itself is
guaranteed to survive a crash. That got fixed along with
checkpointing, which needed the same thing for renaming a snapshot
into place.

## Differential testing against SQLite

Unit tests only cover the cases I thought of. To check the SQL layer
more broadly, `python/sqlite_diff_test.py` generates random sequences
of `CREATE TABLE`, `CREATE INDEX`, `INSERT`, `UPDATE`, `DELETE`, and
`SELECT` (every `WHERE` form, `ORDER BY`, `LIMIT`, `COUNT(*)`, and
`SUM`), plus simulated restarts. It runs each statement against this
database, through a small line-based front end in
`cpp/sql_harness.cpp`, and against an in-memory SQLite database, and
compares the results. After every statement that changes data it also
compares the full tables. When the two disagree, it shrinks the
failing sequence by deleting statements for as long as the
disagreement still shows up, and prints what's left.

A few known differences are translated rather than reported. SQLite
gets `INTEGER PRIMARY KEY` on the first column, and `INSERT OR
REPLACE` / `UPDATE OR REPLACE` to match this database's overwrite
behavior. `SUM` over zero rows counts as 0, since there's no NULL
here. Values stay inside the documented key limits.

It found two bugs in its first few runs. The first was that `LIMIT`
got applied before `COUNT(*)` and `SUM` instead of to their result.
On a 4-row table:

```
SELECT COUNT(*) FROM items LIMIT 3;
  this db: [(3,)]
  sqlite:  [(4,)]
```

`SUM(col) ... LIMIT 3` likewise added up only the first three rows.
The fix moves `LIMIT` after the aggregate, so it applies to the one
summary row.

The second was a stale secondary index entry. An `INSERT` that reuses
an existing primary key overwrites the row, but it only added the new
index entry and never removed the old one. The shrunk reproduction:

```
CREATE TABLE items (id INT, label TEXT, qty INT);
INSERT INTO items VALUES (30, 'n8', 1);
CREATE INDEX idx_items_qty ON items(qty);
UPDATE items SET qty = 14, label = 'n1';
INSERT INTO items VALUES (30, 'n2', 20);
SELECT SUM(qty) FROM items WHERE qty = 14;
  this db: [(20,)]
  sqlite:  [(0,)]
```

The row's qty is 20 by the end, but its old index entry for 14 was
still there, so the index lookup found it anyway. `UPDATE ... SET id =
X` had the same flaw when another row already had X: that row got
overwritten, but its index entries stayed. The tester hadn't generated
that case yet, because it only moved rows to unused ids. I fixed both
paths and changed the generator to sometimes aim at an id that's
already taken, so the second path gets exercised too.

Both bugs have regression tests in `executor_test.cpp` now. The four
new checks fail on the old executor and pass on the new one. After the
fixes, 200 runs of 300 statements each, with nothing skipped, found no
disagreements, and neither did 50 longer runs of 1,000 statements.

At first the tester stayed inside the documented key limits, because
the database didn't enforce them: values outside them were masked by
the key packing instead of rejected. That's fixed now (see "Input
validation" below), and the tester generates those statements too.

## Rows stored in the learned index

Until this change, the learned index wasn't actually where the data
lived. `DurableStore` kept every row in a `std::unordered_map` and used
the gapped array only to know which keys existed and in what order. A
point lookup searched the learned index and then looked the key up in
the hash map, which could have answered the question by itself. The
index was only doing real work for range scans and ordered full scans.

Now the gapped array stores a value next to each key.
`BasicGappedArray<V>` keeps the values in a second array, parallel to
the keys, and every slot move goes through one of two small helpers,
so a value can't get left behind when its key shifts, is removed, or is
redistributed by a local or full rebalance. `DurableStore` no longer
has a hash map at all: `get`, `put`, `remove`, and `range_scan` all go
through the learned index. The key-only `GappedArray` used by the
Phase 3 and 7c benchmarks is the same template with an empty value
type, and its value handling compiles out, so those benchmarks run the
same code as before. The insert benchmark still triggers exactly the
same rebalances (1,189 local and 195 full on the uniform set).

A new storage test runs 30 randomized trials of inserts, overwrites,
and removes against a `std::map`, half of them with eps = 4 so
rebalances happen constantly, and checks every key's value at the end.
Everything else still passes: 29/29 storage tests, 20/20 parser, 64/64
executor, clean under AddressSanitizer and UBSan, and the SQLite
differential test agrees across 200 runs of 300 statements and 50 runs
of 1,000.

I haven't measured what this does to lookup speed or memory yet. The
numbers I have so far came from a laptop with too much else running to
trust, so they'll go in with the other benchmark reruns.

## Optimal segmentation

The segmenter from Phase 2 pins each segment's line to the segment's
first point. That keeps the math simple, but the best line for a run
of points rarely passes exactly through the first one. On 1M lognormal
keys spread over a wide key range, it needed 157 segments where
PGM-index needed 126. (That isn't the "skewed" dataset from the
earlier phases, which turned out to be almost entirely consecutive
integers. A section on that will come with the benchmark reruns.)

`build_optimal_segmented_model()` in `segmented_model.h` drops the
pin. Each point (x, y) requires the line to pass through the vertical
gap from y − eps to y + eps at x, and the lines that do that for every
point so far form a convex region. Deciding whether the next point
still fits only takes two lines from that region: the shallowest,
which runs from the top of one gap down to the bottom of a later one,
and the steepest, which runs from the bottom of one gap up to the top
of a later one. A new point whose whole gap lies below the shallowest
line or above the steepest one can't fit, so it starts a new segment.
If the point only cuts into one of those lines, that line pivots onto
a new endpoint, found by walking a convex hull of the earlier gap
endpoints. This is O'Rourke's algorithm from 1981, the same one
PGM-index is built on, and it runs in O(n) overall. Since any part of
a run that fits also fits, starting a new segment only when forced
gives the fewest segments possible. The fitting uses exact 128-bit
integer arithmetic, so "fits" has no floating-point slack in it.

To check it, a test compares it with a brute-force search on 400 small
random inputs. The brute force tries every line through two gap
endpoints, which is slow but obviously correct, and the two produce
exactly the same segments every time. On the benchmark data the
segment counts match PGM-index exactly:

| data (1M keys) | eps | pinned | optimal | PGM-index |
|---|---|---|---|---|
| uniform | 64 | 79 | 57 | 57 |
| wide-range lognormal | 64 | 157 | 126 | 126 |
| uniform | 16 | 1,253 | 916 | 916 |
| wide-range lognormal | 16 | 1,453 | 1,025 | 1,025 |

That's 20–29% fewer segments, with every key still predicted within
eps. The memory gap to PGM-index doesn't fully close (3,024 vs 2,192
bytes on the lognormal keys at eps = 64), but what's left is
representation rather than segmentation: my segments are 24 bytes, an
int64 key and two doubles, and PGM-index stores its slope as a float.

### Why the database doesn't use it

I switched the gapped array to the optimal fit and then measured
inserts. They got about 2.5x slower: roughly 60,000 ns per insert,
against 22,000–25,000 with the pinned fit, across four back-to-back
pairs. The cost is in fitting. Building a model for 1M keys took 21–34
ms with the optimal version and 3–5 ms with the pinned one, and
PGM-index's own builder took 17–19 ms, so most of that cost comes from
the algorithm and some from my implementation. It matters because the
gapped array refits its whole model on every full rebalance, and the
Phase 3 benchmark triggers 195 of those per 100,000 inserts.

In return, the database would have saved about 1 KB of segments (180
instead of 235 on the lognormal keys) and maybe one comparison when
finding a key's segment, since both fits predict within the same eps.
That's not worth 2.5x on every insert, so the gapped array stays on
the pinned fit, and the optimal version is for indexes that get built
once and read many times. These timings came from a laptop with a lot
running in the background, so treat them as rough, although the ratios
held up across repeated runs. Lookup times for the optimal model are
part of the benchmark reruns.

## Input validation

The differential tester was originally kept inside the documented key
limits because the database didn't enforce them. Only the primary
key's type was checked. An `INSERT` or `UPDATE` could put a string
into an `INT` column. A negative primary key, or an indexed value of
2^20 or more, was masked by the key packing into some other key
instead of being refused. A `WHERE` on a column that doesn't exist
quietly matched nothing.

Some of that produced wrong answers, not just bad data. With an index
on `age`, `WHERE age = 1048621` (that's 2^20 + 45) came back with all
the age-45 rows, because the index lookup masked the value down to 45.
`WHERE id BETWEEN -5 AND 3` came back empty, because the negative
bound got packed into a huge key.

`executor.h` now checks every value against its column before anything
is written:

- a value's type has to match its column's type;
- a primary key has to be between 0 and 2^48 − 1, or 2^28 − 1 on a
  table with a secondary index, since index entries pack the primary
  key into 28 bits;
- a value in an indexed column has to be between 0 and 2^20 − 1;
- a `WHERE` has to name a real column and compare it with a literal of
  the same type;
- `CREATE INDEX` refuses a table whose existing rows don't fit those
  limits, and checks before building anything;
- `CREATE TABLE` requires the first column, the primary key, to be
  `INT`, and column names to be unique.

Queries whose bounds fall outside the representable range are still
fine, since there's nothing wrong with asking. Range scans clamp their
bounds, and an out-of-range value simply matches nothing.

There are 13 new checks for this in `executor_test.cpp`, and 12 of
them fail on the previous executor. The thirteenth checks that a large
primary key is still accepted where it's legal. The differential
tester now generates these statements as well. It carries a small copy
of the rules, and for a statement that breaks one, it expects an error
and an unchanged database, and doesn't send it to SQLite. With those
statements and checkpoints mixed in, 200 runs of 300 statements and 50
runs of 1,000 found no disagreements.

## Checkpointing

Before this, the write-ahead log only grew. Every startup replayed
every write ever made, so a store whose rows kept getting updated got
slower to open even though it wasn't getting any bigger.

`DurableStore::checkpoint()` now writes the whole store to a snapshot
file, in key order with a CRC-32 over it, and then empties the log. On
startup the snapshot is loaded in one pass, which lays the gapped
array out directly instead of inserting keys one at a time, and then
anything logged since is replayed on top. A checkpoint also happens
automatically every 100,000 writes by default, and the REPL has a
`.checkpoint` command. The automatic one makes that one write slow,
since it rewrites the whole store. A production database would do the
work in the background.

The order of the steps is what makes it safe to crash at any point.
The snapshot is written to a temporary file, fsync'd, and only then
renamed over the old one, followed by an fsync of the directory. The
log is emptied only after that. A crash while the snapshot is being
written leaves the old snapshot and the full log, which together are
still complete. A crash after the rename but before the log is emptied
leaves the new snapshot plus a log of writes it already contains.
Replaying those on top of it lands in the same state, because each key
ends up with whatever its last logged write said. A snapshot that
fails its checksum is refused rather than skipped, because skipping it
would silently lose everything the emptied log no longer has.

This forced one change elsewhere. The catalog used to rebuild itself on
startup by reading the log directly, a workaround from before range
scans existed. Once checkpoints started emptying the log, a restart
after a checkpoint would have come back with no tables at all. It now
range-scans its own rows out of the store instead.

The tests go through each of those crash points: a checkpoint followed
by more writes, a crash between the rename and emptying the log, a
leftover half-written temporary snapshot, a damaged snapshot,
automatic checkpoints, and the catalog and its indexes coming back
after a checkpoint. The differential tester also checkpoints
automatically every 25 writes and asks for extra checkpoints at
random.

`cpp/checkpoint_demo.cpp` measures what it does for startup. It writes
2,000 rows 10 times each, 20,000 fsync'd writes in all, and then opens
the store from the full log, and again after a checkpoint:

| | on disk | opening the store (median of 5) |
|---|---|---|
| before the checkpoint | 1,508,908-byte log | 9.78 ms, replaying 20,000 records |
| after the checkpoint | 140,910-byte snapshot, empty log | 0.49 ms, loading 2,000 rows |

Opening got about 20x faster, and every row came back with its latest
value both times. A second run gave 10.74 ms against 0.46 ms. The gap
depends on how many updates have piled up, since the log grows with
every write while the snapshot stays the size of the data. One thing I
learned running it: WSL's `/tmp` is a tmpfs, where fsync does nothing,
so building the store there took almost no time. On the ext4 home
directory it took 39.8 s, about 2 ms per fsync.

## Building with make, and CI

The project has a `Makefile` now, so building and testing is a few
short commands (listed near the top of this file), and binaries go in
`build/` instead of next to the source. A GitHub Actions workflow in
`.github/workflows/tests.yml` runs on every push. It builds every
program, runs the three test suites normally and again under
AddressSanitizer and UBSan, runs the durability demos, and does 30
runs of the SQLite differential test. The badge at the top of this
file shows how the latest run went. CI doesn't run the benchmarks,
since timings from shared machines would be too noisy to mean
anything.
