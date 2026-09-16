# Learned Index DB

[![tests](https://github.com/asood8/Learned-Index/actions/workflows/tests.yml/badge.svg)](https://github.com/asood8/Learned-Index/actions/workflows/tests.yml)

An embedded SQL database in C++17 whose storage engine is a learned
index rather than a B-tree. To find a key, it evaluates a small
piecewise-linear model that predicts where the key sits in a sorted
array, then walks a short distance from that guess to the exact slot.
There is no tree to descend and no pointers to chase.

The design follows Kraska et al., "The Case for Learned Index
Structures" (2018), and borrows from PGM-index for error-bounded
segmentation and from ALEX for handling inserts. The published
PGM-index is vendored here as a benchmark opponent, so the comparisons
are against a real implementation rather than a strawman.

It is embedded in the same sense SQLite is: one process, one
write-ahead log on disk, and a SQL front end over a storage engine.
There are no joins, no multi-statement transactions, and no concurrent
access. Within those limits it is a working database — `CREATE TABLE`,
`INSERT`, `SELECT` with `WHERE`/`ORDER BY`/`LIMIT`/`COUNT`/`SUM`,
`UPDATE`, `DELETE`, secondary indexes, `EXPLAIN`, crash recovery,
checkpointing, and a REPL.

## Results

Every number comes from an actual run on 1M keys, measured in one
sitting on an otherwise idle machine. Lookup figures are medians of
five runs, the rest medians of three.

| | result |
|---|---|
| Point lookup, uniform keys | 113.6 ns, against 301.7 ns for a B+-tree and 178.7 ns for binary search |
| Point lookup, skewed keys | 117.7 ns, against 319.1 ns and 185.2 ns |
| The same lookup inside the database | 142.0 ns (uniform), 147.3 ns (skewed), through the gapped array the database really uses |
| Index memory | 0.001–0.010 bytes per key, against about 37.3 for the B+-tree |
| Inserts | 20,211 ns, about 12x faster than shifting a sorted vector |
| Against the published PGM-index | slower on uniform data (124.0 vs 89.5 ns), a tie on skewed (116.7 vs 114.0 ns) |
| Secondary index lookup | 18–22x faster than the full scan it replaces |
| Checkpointing | opening a store dropped from 9.12 ms to 0.48 ms |
| Learned Bloom filter | lost badly: 50.4% false positives against 1.021% for a classic one |

Three things in that table are worth pulling out.

The index is roughly 2.7x faster than the B+-tree and needs three to
four orders of magnitude less memory, which is the headline claim of
the original paper reproduced on my own data. The memory difference is
structural: a B+-tree pays about 37 bytes per key no matter how large
it gets, while a piecewise-linear model pays per *segment*. A million
uniform keys need 84 segments, so the whole model is a couple of
kilobytes.

The database's own lookup path is slower than the index it is built
on, 142.0 ns against 113.6, because live data needs gaps for inserts
and a model that drifts between refits. That is the number I would
quote for "how fast is a lookup in this database", and it is the
number most write-ups of this kind never measure.

The comparison against the real PGM-index is a mixed result rather
than a win. PGM is clearly faster on uniform data and level with this
index on skewed data, using fewer segments and less memory per
segment. Details are below.

## Try it

Everything is a single `.cpp` file under `cpp/`, built with g++
(C++17). The Makefile wraps the usual tasks and puts binaries in
`build/`.

```bash
make test        # the three test suites
make asan        # the same suites under AddressSanitizer and UBSan
make difftest    # differential test against SQLite
make demos       # durability and catalog restart demos
make repl        # build and start the SQL shell
make data        # generate the benchmark datasets (needs numpy)
make all         # everything, benchmarks included
```

A session in the REPL, which is backed by a real write-ahead log, so
closing and reopening it is a genuine restart:

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
id  name   age
--  -----  ---
1   Alice  30
2   Bob    25
(2 rows) [range scan]
db> SELECT * FRUM users;
Parse error: expected keyword FROM, got 'FRUM'
db> .tables
users
db> .exit
bye.
```

The tag after each result is the executor's actual routing decision.
`EXPLAIN` goes further and reports what the model did:

```
db> EXPLAIN SELECT * FROM users WHERE id = 2;
point lookup via learned index -- predicted position 2, corrected in 3 probes, key found
db> EXPLAIN SELECT * FROM users WHERE id = 999;
point lookup via learned index -- predicted position 2, corrected in 4 probes, key not found
db> EXPLAIN SELECT * FROM users WHERE id BETWEEN 1 AND 3;
range scan over the gapped array -- returned 3 row(s)
db> EXPLAIN SELECT * FROM users WHERE age = 30;
full table scan, no index used -- filtered down to 1 matching row(s)
db> EXPLAIN INSERT INTO users VALUES (4, 'x', 1);
Parse error: EXPLAIN is only supported for SELECT statements
```

"Predicted position 2, corrected in 3 probes" is the real prediction
and the real number of slots examined, closer to `EXPLAIN ANALYZE`
than to a plan sketch.

The write-ahead log uses POSIX calls such as `fsync` and `ftruncate`,
so the project builds on Linux, or on Windows through WSL.

## How it works

### Predicting where a key lives

A sorted array of keys defines a function: given a key, return its
position. Plot key against position and that function is the data's
cumulative distribution, scaled. If the data is anywhere near regular,
a straight line approximates it well, and a line costs one multiply
and one add to evaluate — no pointer chasing, no cache misses walking
down levels.

The catch is that a prediction is not an answer. The model says
"around slot 1,400"; the key might be at 1,396. Every lookup therefore
has two parts: predict, then correct. Correction is what makes the
structure honest, and it is where the interesting failure modes live.

Correction here is a walk. Starting at the predicted slot, step left
while the slot to the left holds a key greater than or equal to the
target (or is a gap), then step right past gaps and smaller keys. Take
an array holding `[10, _, 20, 30, _, 40, 50, _]`, where `_` is a gap,
and look for the first key at least 35 with a bad prediction of slot
7. Walking left passes 50, then 40, then the gap, and stops at slot 3,
which holds 30. Walking right from there skips the gap and lands on 40
at slot 5. A prediction of slot 0 reaches the same answer from the
other side.

The walk is correct no matter what the model says, because the array
is sorted; the model only decides how far it has to walk. That
property is worth more than it sounds, and the section on bugs below
explains what happened when correctness depended on the model instead.

### Fitting the segments

One line over a whole dataset only works if the data is close to
uniform. On the skewed set, a single least-squares line has a maximum
error of 15.7M positions in an array of 1M keys, which means the
"local" search covers the whole array and the model has bought
nothing. Measured: 207.8 ns per lookup with one line, slower than
plain binary search at 185.2 ns.

So the model is piecewise. `build_segmented_model` walks the sorted
keys once and extends a segment for as long as a single line can keep
every point within ±eps of its true position (eps = 64 by default).
When no valid slope remains, it closes the segment and starts another.
Each new point turns into one interval constraint on the slope, so the
whole fit is an intersection of intervals in a single O(n) pass. The
result on 1M keys: 84 segments for uniform data, 151 for skewed, and
lookups at 113.6 and 117.7 ns.

That version pins each segment's line to the segment's first point,
which keeps the math to one variable. It is not optimal. The file also
contains `build_optimal_segmented_model`, which drops the pin and
tracks the full set of lines that still fit — bounded by the
shallowest and steepest line that satisfies every point so far, each
pivoting on a convex hull of the error bounds as points arrive. That
is O'Rourke's 1981 algorithm, the one PGM-index uses, and it finds the
fewest segments possible. It needs 20–29% fewer segments than the
pinned version and matches PGM-index's segment counts exactly:

| data (1M keys) | eps | pinned | optimal | PGM-index |
|---|---|---|---|---|
| uniform | 64 | 79 | 57 | 57 |
| skewed | 64 | 157 | 126 | 126 |
| uniform | 16 | 1,253 | 916 | 916 |
| skewed | 16 | 1,453 | 1,025 | 1,025 |

The database still uses the pinned version, for a measured reason:
fitting optimally takes 21.8 ms per million keys against 3.5 ms, and
the structure refits on every full rebalance, which made inserts 2.7x
slower (56,623 ns against 20,252) in exchange for about 1 KB of saved
segments. PGM's own fit takes 17.4 ms, so most of that cost belongs to
the algorithm rather than to my implementation. Optimal segmentation
is the right choice for an index built once and read many times, which
is not this one.

### Making it updatable

A sorted array is hostile to inserts: putting a key in the middle
shifts everything after it. The storage engine is therefore a gapped
array, about 70% full, with the free slots spread throughout. An
insert usually lands in a nearby gap after shifting a handful of
neighbours, and the model is left alone.

Each insert nudges keys slightly away from where the model expects
them. That drift is bounded by rebalancing: every segment counts the
inserts that land in it, and once the count reaches eps, that
segment's slice of the array is redistributed and refit. A much rarer
whole-array rebalance runs as a safety net. On 1M keys with 100k
inserts, that works out to 1,189 local rebalances and 195 full ones,
and inserts cost 20,211 ns each against 245,941 for the naive
shift-everything version.

### Surviving a crash

Every write is appended to a log and fsync'd before the in-memory
structure is touched, so a process that dies mid-write comes back with
the write either fully applied or not at all. Each record carries a
type tag, the key, the value, and a CRC-32. Opening the log verifies
it and truncates it back to the last intact record, which matters more
than it sounds: an earlier version skipped a torn record but left its
bytes in place, and the next writes landed behind them.

Logs only grow, so there is also checkpointing. `checkpoint()` writes
the whole store to a snapshot file and then empties the log. The
ordering is what makes it safe: the snapshot goes to a temporary file,
is fsync'd, is renamed into place, and only then is the log emptied. A
crash between the rename and the truncation leaves a snapshot plus a
log of writes it already contains, and replaying those changes
nothing. Startup loads the snapshot in a single bulk layout and
replays whatever the log has gathered since. For a store with 20,000
logged updates, opening went from 9.12 ms to 0.48 ms.

### The SQL layer

The front end is a hand-written tokenizer and recursive-descent
parser, an executor, and a catalog. Nothing here is generated.

The storage engine only knows `int64 -> bytes`, so every row's key
packs a table id into the high 16 bits and the row's primary key into
the low 48. Rows of one table stay contiguous and sorted, which is
exactly what the segmented model likes. The catalog is a table like
any other: table id 0 holds a row per table and per index, the same
pattern as `sqlite_master`, and the catalog rebuilds itself on startup
by range-scanning its own rows.

The executor routes each query to the cheapest path the storage engine
supports: equality on the primary key becomes a point lookup,
`BETWEEN` on the primary key becomes a range scan, equality on an
indexed column becomes a secondary index lookup, and anything else
falls back to a full scan with in-memory filtering.

Secondary indexes needed no new data structure. Indexed values repeat,
while the learned index needs unique keys, so each index entry is a
composite: the indexed value in the top 20 bits and the row's primary
key in the low 28. Every entry is unique, all rows sharing a value sit
in one contiguous range, and a lookup is a range scan over the same
gapped array. On a 1,000-row table that is 18–22x faster than the full
scan it replaces (6,376–7,745 ns against 134,959–155,267).

Values are checked against their column before anything is written:
types must match, primary keys must fit 48 bits (28 once a table has
an index), and indexed values must fit 20 bits. Anything outside those
ranges is refused rather than silently masked into a different key.

## What the numbers say

### Against a B+-tree and binary search

The B+-tree here is a real one — order 64, node splitting, linked
leaves — written specifically to be the baseline to beat.

| 1M keys | binary search | B+-tree | one line | segmented |
|---|---|---|---|---|
| uniform | 178.7 ns | 301.7 ns | 118.3 ns | 113.6 ns |
| skewed | 185.2 ns | 319.1 ns | 207.8 ns | 117.7 ns |

Two details are more interesting than the headline. First, plain
binary search beats the B+-tree, which surprises people and is the
actual motivating fact behind learned indexes. This B+-tree is correct
but not cache-optimized: each node is heap-allocated, so descending
three or four levels means three or four jumps to scattered memory,
each a likely cache miss, while binary search stays inside one
contiguous block. The original paper reports the same thing. Second,
the single-line model wins on uniform data and loses badly on skewed
data, which is the entire argument for segmentation, visible in one
row of a table.

Scaling holds up. Across 100K, 1M and 5M keys, the segmented index
stays ahead of both baselines, and the margin over the B+-tree grows:
at 5M skewed keys it is 187.8 ns against 548.2.

### Memory

| n | B+-tree | segmented index |
|---|---|---|
| 100K | 3,732,368 bytes (37.3/key) | 248–992 bytes (0.002–0.010/key) |
| 1M | 37,347,776 bytes (37.3/key) | 1,928–3,800 bytes (0.002–0.004/key) |
| 5M | 186,754,792 bytes (37.4/key) | 10,160–12,584 bytes (0.002–0.003/key) |

The B+-tree's cost per key is flat because it is pointer and node
overhead. The model's cost scales with segment count, not with the
number of keys, which is why it stays in the kilobytes while the tree
reaches 178 MB. Both exclude the sorted data itself, which every
approach needs.

### Against the published PGM-index

`cpp/third_party/pgm/` holds the real PGM-index, vendored unmodified,
benchmarked on the same data with the same eps.

| 1M keys | ours (pinned) | ours (optimal) | PGM-index |
|---|---|---|---|
| uniform | 124.0 ns, 79 segments, 1,896 bytes | 111.9 ns, 57 segments, 1,368 bytes | 89.5 ns, 57 segments, 984 bytes |
| skewed | 116.7 ns, 157 segments, 3,768 bytes | 117.5 ns, 126 segments, 3,024 bytes | 114.0 ns, 126 segments, 2,192 bytes |

PGM wins on uniform data and ties on skewed. Its remaining memory
advantage at equal segment counts is representation rather than
segmentation: 16 bytes per segment against 24 here, since it keeps a
32-bit float slope where this project keeps a 64-bit double. Turning
PGM's recursive routing layer off (a template parameter)
moves its uniform time from 89.5 to 109.8 ns, which confirms the
hierarchy is doing real work rather than adding overhead.

An earlier version of this README claimed a 2x win over PGM on skewed
data. That was an artifact of a broken dataset generator, described
below.

### Inside the database

The figures above time the read-only structure. The database uses a
gapped array with live inserts, so it pays for gaps and for model
drift. `cpp/benchmark_db_lookups.cpp` times that path against the same
baselines, before and after inserting 100k keys.

| 1M keys, uniform | fresh | after 100k inserts |
|---|---|---|
| binary search | 168.0 ns | 178.2 ns |
| B+-tree | 285.5 ns | 291.6 ns |
| read-only segmented model | 110.1 ns | 122.0 ns |
| gapped array (the database) | 162.5 ns | 142.0 ns |

So a real lookup runs about 1.9–2x faster than the B+-tree, not the
2.7x the read-only structure manages. Missing keys cost about the same
as hits, which is a consequence of the walk rather than an accident.
One oddity I cannot yet explain: on uniform data the freshly built
structure is slower than the same structure after 100,000 inserts
(162.5 against 142.0 ns). Inserts trigger rebalances that refit the
model, so the later layout may simply predict better, but I have not
confirmed it.

### Durability

| operation | ns/op |
|---|---|
| plain insert, no log | 3,251 |
| durable put: log, fsync, insert | 118,125 |

That is a 36x overhead, essentially all of it `fsync`, which is why
real databases batch writes into one sync. The same demo on a
different machine reported 1,926,085 ns against 2,702, a 713x
overhead, because one fsync there costs about 1.9 ms instead of 113
µs. Not a line of code differs between those runs. A durability
multiplier describes the disk underneath at least as much as the
database on top, which is worth remembering whenever one is quoted.

### Under attack

Concentrating inserts into a narrow key range is the lever an attacker
with write access actually has: it forces one region's segments to
exhaust their gaps and rebalance far more often than scattered inserts
would. Published poisoning attacks on ALEX and PGM-index report
slowdowns of up to about 20%, which sets the scale.

Each scenario inserts 100,000 keys into the same 900,000-key
structure, nine times, interleaved, comparing against the benign
scattered case from the same round.

| scenario | local rebalances | full rebalances | slowdown, two runs |
|---|---|---|---|
| benign, scattered | 0 | 195 | — |
| 5.0% window | 231 | 195 | +1.6%, −1.3% |
| 2.0% window | 792 | 195 | +5.0%, +2.2% |
| 1.2% window | 1,096 | 189 | +11.7%, +9.9% |

The effect is real and grows as the window narrows, topping out around
10%. The rebalance counts explain why it is not worse: local
rebalances climb from 0 to 1,096 while full rebalances stay flat and
even dip, so per-segment rebalancing absorbs nearly all the pressure
and the expensive whole-array rebuilds the attack is aiming for never
arrive. That resilience was a side effect. Per-segment rebalancing was
added to make ordinary inserts faster.

### Two experiments that went opposite ways

The same "learn the structure" idea was applied to two other places,
with different outcomes worth reporting together.

A learned Bloom filter replaces the bit array with logistic regression
over hashed features, trained with gradient descent, keeping a backup
set so there are still no false negatives. It lost decisively.

| filter | memory | false positive rate |
|---|---|---|
| classic, built for 1% | 119,814 bytes | 1.021% |
| learned, same byte budget | 462,288 bytes | 50.380% |
| learned, 64x the memory | 7,668,048 bytes | 50.225% |

Giving it 64 times the memory changed nothing, which rules out the
obvious explanation. The real one is that membership in a set of
arbitrary integers has no structure to learn. A classic Bloom filter
hashes and flips bits, which is memorization, and memorization is
about as space-efficient as information theory allows for this
question. Sorted position, by contrast, is a smooth function worth
approximating. The technique is not broken; it was asked a question
its data has no answer to.

A learned cache went the other way. Logistic regression over recency
and frequency, trained on one Zipfian trace and evaluated on another,
approximates Belady's optimal eviction decisions using only
information available at the time.

| policy | hit rate |
|---|---|
| LRU | 58.67% |
| learned, Hawkeye-style | 63.36% |
| Belady's optimal (needs the future) | 74.63% |

That closes 29.4% of the gap between LRU and a ceiling no online
policy can reach, which is in the range the Hawkeye and LRB papers
report. Same tool, two problems, two honest answers.

## Bugs worth reading about

Nearly every stage of this project surfaced a real bug, and in almost
every case a correctness check caught it rather than a crash. A few
are worth writing down.

### The dataset that was not skewed

The best number this project ever produced was 26.9 ns per lookup on
"skewed" data, roughly 12x faster than the B+-tree. It was an
artifact.

The generator drew from a lognormal distribution and scaled the draws
so the largest landed at n × 10. With sigma = 2 the largest draw is
thousands of times bigger than a typical one, so nearly every draw
floored into the same few hundred integers, and the loop that nudges
collisions up by one turned them into a single run of consecutive
integers: 999,819 of 1,000,000 keys, with 99.98% of gaps exactly 1. A
straight line fits consecutive integers perfectly, which is why
"skewed" data needed 2 or 3 segments where uniform data needed 84.

Scaling to 1e15 instead fixed it, and everything was re-measured:

| | flawed data | genuinely skewed |
|---|---|---|
| segments (eps = 64) | 2–3 | 151 |
| segmented lookup | 26.9 ns | 117.7 ns |
| against the B+-tree | 12.5x | 2.7x |
| against PGM-index | 2x faster | a tie |

What survived: beating the B+-tree, the memory result, and the
argument for segmentation over a single line. What did not: the
headline. I had been checking results constantly and never checked the
input. One look at the distribution of gaps would have caught it on
day one.

### A guarantee that covered less than I assumed

Segments guarantee that every key is predicted within ±eps, so every
lookup scanned a ±eps window. That guarantee only covers keys the
model was *trained* on. Any key inserted since the last refit, and any
query for a key that isn't there, has no such bound, and nothing in
the code said so.

It surfaced through a secondary index. One row's age had been updated
to an outlier, which stretched a segment's key range far beyond its
data, and a query for a value inside that stretch predicted slot 1644
in a 1430-slot array. Four separate functions scanned that window, and
each failed differently when it missed. The lower-bound function
returned a later key instead of the first match. `insert` placed keys
out of sorted order, which a randomized test later found in 284 of 300
trials on committed code, and no existing test had ever caught. A
first attempt at a fix added progressively larger fallback scans to
`search`, which made every lookup for an absent key O(n) — 630,566 ns
— and since every insert checks for duplicates first, insert
throughput collapsed from 21,691 to 540,180 ns, slower than the naive
vector the structure exists to beat. That attempt was measured and
thrown away rather than committed.

The fix was to stop depending on the model for correctness. All four
paths now share the walk described earlier, which is right regardless
of prediction quality, plus a clamp that keeps a segment's line from
predicting past the next segment's start. The model decides speed; the
sorted array decides correctness. Worst case is still an O(n) walk if
the model is badly wrong, but slow is a much better failure than
wrong.

### A write-ahead log that lost writes after a crash

Replay skipped a record torn by a crash, which is correct, but left
its bytes in the file. The log reopened in append mode, so the next
write landed behind the garbage, and the restart after that read the
torn bytes together with the next record. Writes made after a crash
could be lost or misread — in the one component whose entire purpose
is not losing writes.

There were no checksums either, so a tail of zero bytes (which some
filesystems leave after a crash) parsed as valid records, and a
damaged byte mid-log was accepted as data. The tests came first: chop
the last bytes off a log, append zeros, flip a byte in the middle,
hand it a file in the wrong format. Four of five checks failed on the
old code. The format now has a header and a CRC-32 per record, and
opening a log truncates it to the last intact record before anything
new is appended.

### What differential testing found

Unit tests only cover the cases I thought of, so
`python/sqlite_diff_test.py` generates random SQL, runs each statement
against both this database and SQLite, and compares. When they
disagree it shrinks the sequence to a minimal reproduction. It found
two real bugs within its first few runs.

`LIMIT` was applied before `COUNT(*)` and `SUM` rather than to their
result, so `SELECT COUNT(*) FROM items LIMIT 3` returned 3 on a
four-row table. And an `INSERT` reusing an existing primary key
overwrote the row but left its old secondary index entry in place, so
a query on the old value still found the row through the index. The
same flaw applied to an `UPDATE` that moved a row onto another row's
key.

A later pass found that values outside the packing limits were not
rejected but masked, which produced wrong answers rather than bad data
alone: with an index on age, `WHERE age = 1048621` (that is 2^20 + 45)
returned the age-45 rows, and `WHERE id BETWEEN -5 AND 3` returned
nothing at all, because the negative bound wrapped into a huge key. Of
the 13 checks written for this, 12 fail on the previous version.

### The pattern

Bug #3 was an unclamped bound in `insert`. Bug #5 was the identical
mistake in `search`, unfixed for months because nobody grepped for
siblings. Bug #6 was the same class again, spread across four
functions at once. The lesson that finally stuck was not "grep for
siblings" but "delete the siblings": all four lookups now share one
code path, so there is nowhere for a partial fix to hide.

## How it is tested

- 41 storage-engine tests, 20 parser tests, and 79 end-to-end SQL
  tests, all passing.
- Randomized trials against reference data structures: gapped-array
  contents against a `std::map`, outlier-heavy insert and delete
  sequences against a `std::set`, and the optimal segmenter against a
  brute-force search over every candidate line.
- Crash-recovery tests that damage a real log file: truncated,
  zero-padded, bit-flipped, and wrong-format.
- Differential testing against SQLite, with automatic shrinking of
  failures. The current pass is 200 runs of 300 statements plus 50
  runs of 1,000, including deliberately invalid statements,
  checkpoints and restarts, with no disagreements.
- AddressSanitizer and UBSan on every suite. This caught two real
  memory bugs that ordinary runs missed.
- GitHub Actions runs all of the above on every push.

Benchmarks are deliberately excluded from CI, since timings from
shared machines mean nothing. Every number in this README was measured
on a quiet machine, and the runs behind each are recorded in
`results/`.

## Limits

Stated plainly, because most of them are deliberate:

- No joins, no multi-statement transactions, no concurrent access.
  Each is its own project.
- The first column of a table is the primary key. There is no
  `PRIMARY KEY` syntax.
- Keys are packed: 16 bits of table id and 48 bits of primary key, or
  20 bits of value and 28 bits of primary key inside a secondary
  index. Values outside those ranges are rejected.
- A lookup is O(n) in the worst case, if the model is badly wrong.
  Correctness never depends on the model, only speed does.
- A checkpoint rewrites the whole store, so the write that triggers
  one is slow. A production database would do that work in the
  background.
- `EXPLAIN` covers `SELECT` only.

## References

- Kraska, Beutel, Chi, Dean, Polyzotis. "The Case for Learned Index
  Structures" (2018).
- Ferragina, Vinciguerra. "The PGM-index" (2020). Vendored under
  `cpp/third_party/pgm/`, Apache 2.0.
- Ding et al. "ALEX: An Updatable Adaptive Learned Index" (2020).
- O'Rourke. "An on-line algorithm for fitting straight lines between
  data ranges" (1981).
- Jain, Lin. "Back to the Future: Leveraging Belady's Algorithm for
  Improved Cache Replacement" (2016).
