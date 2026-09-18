# Learned Index DB

[![tests](https://github.com/asood8/Learned-Index/actions/workflows/tests.yml/badge.svg)](https://github.com/asood8/Learned-Index/actions/workflows/tests.yml)

This is an embedded SQL database written in C++17. The storage engine is
a learned index instead of a B-tree. To find a key, it uses a small
model, made of a few straight lines, to guess where the key is in a
sorted array, and then checks the nearby slots until it finds the right
one.

The design is based on Kraska et al., "The Case for Learned Index
Structures" (2018), with ideas from two later projects: PGM-index (how
to split the data into pieces) and ALEX (how to handle inserts). I also
included the published PGM-index in the repo so I could benchmark
against a real implementation, not just my own baselines.

"Embedded" means the same thing it does for SQLite: a single process, a
write-ahead log on disk, and a SQL layer on top of the storage engine.
It doesn't support joins, multi-statement transactions, or concurrent
access. It does support `CREATE TABLE`, `INSERT`, `SELECT` (with
`WHERE`, `ORDER BY`, `LIMIT`, `COUNT` and `SUM`), `UPDATE`, `DELETE`,
secondary indexes, `EXPLAIN`, crash recovery, checkpointing, and a REPL.

## Results

All of these are from real runs on 1M keys, done in one session on a
machine with nothing else running. Lookup times are the median of five
runs, and everything else is the median of three.

| | result |
|---|---|
| Point lookup, uniform keys | 113.6 ns (B+-tree: 301.7 ns, binary search: 178.7 ns) |
| Point lookup, skewed keys | 117.7 ns (B+-tree: 319.1 ns, binary search: 185.2 ns) |
| The same lookup inside the database | 142.0 ns uniform, 147.3 ns skewed, using the gapped array the database actually runs on |
| Index memory | 0.001–0.010 bytes per key, vs about 37.3 for the B+-tree |
| Inserts | 20,211 ns, about 12x faster than inserting into a plain sorted vector |
| vs. the published PGM-index | slower on uniform data (124.0 vs 89.5 ns), about even on skewed (116.7 vs 114.0 ns) |
| Secondary index lookup | 18–22x faster than the full scan it replaces |
| Checkpointing | startup time went from 9.12 ms to 0.48 ms |
| Learned Bloom filter | lost badly: 50.4% false positives vs 1.021% for a normal one |

A few notes on these.

The index is about 2.7x faster than my B+-tree and uses thousands of
times less memory, which matches the main claim of the original paper.
The memory gap comes from how each structure grows. A B+-tree costs
roughly 37 bytes per key no matter how large it gets, while the model's
size depends only on how many segments it needs. A million uniform keys
need 84 segments, so the whole model is a couple of kilobytes.

Lookups inside the database are slower than lookups on the bare index
(142.0 ns vs 113.6). The database has to leave empty slots in the array
for inserts, and its model gets a little less accurate with every insert
until it's rebuilt. I think 142.0 ns is the more honest number to quote,
and it's one that most projects like this don't measure.

The comparison with PGM-index didn't go my way. PGM is clearly faster on
uniform data and about even on skewed data, and it uses fewer segments
and less memory per segment. There's more on this below.

## Building and running

Each program is a single `.cpp` file in `cpp/`, compiled with g++
(C++17). The Makefile covers the common tasks and puts the binaries in
`build/`.

```bash
make test        # the three test suites
make asan        # the same suites under AddressSanitizer and UBSan
make difftest    # differential test against SQLite
make demos       # durability and catalog restart demos
make repl        # build and start the SQL shell
make data        # generate the benchmark datasets (needs numpy)
make all         # build everything, including benchmarks
```

The write-ahead log uses POSIX calls like `fsync` and `ftruncate`, so it
builds on Linux, or on Windows through WSL.

Here's a short REPL session. The REPL uses a real write-ahead log, so
closing and reopening it is an actual restart.

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

The tag after each result shows which access path the executor chose.
`EXPLAIN` shows what the model actually did:

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

"Predicted position 2, corrected in 3 probes" is the model's real guess
and the number of slots it really had to check, so it reports what
actually happened, not just a plan.

## How it works

### Predicting where a key is

If you plot each key in a sorted array against its position, you get a
curve that always goes up. For fairly regular data, that curve is close
to a straight line, and working out a point on a line takes one
multiplication and one addition. A B-tree lookup has to go down several
levels of nodes instead, and each step usually means waiting on memory
(a cache miss).

A prediction isn't an exact answer, though. The model might say "about
slot 1,400" when the key is really at 1,396, so every lookup has two
steps: predict, then correct. Most of the bugs in this project ended up
being in the correction step.

The correction is a short walk. Starting at the predicted slot, it moves
left as long as the slot to its left is empty or holds a key greater
than or equal to the target, then moves right past empty slots and
smaller keys. For example, take the array
`[10, _, 20, 30, _, 40, 50, _]`, where `_` is an empty slot, and search
for the first key that's at least 35, starting from a bad prediction of
slot 7. Walking left passes 50, 40 and the empty slot 4, then stops
because slot 3 holds 30. Walking right from there skips the empty slot
and stops at 40 in slot 5. Starting from slot 0 instead gets to the same
place by walking right the whole way.

Since the array is sorted, the walk finds the right answer no matter
what the model predicted. The model only changes how far it has to go.
That ended up mattering a lot, and the bugs section explains what
happened back when correctness depended on the model.

### Fitting segments

A single line only works if the data is close to uniform. On the skewed
dataset, the best single line is off by up to 15.7M positions in an
array of 1M keys, so the "local" search ends up covering the whole
array. In practice that's 207.8 ns per lookup, which is slower than
plain binary search at 185.2 ns.

So the model is split into pieces. `build_segmented_model` goes through
the sorted keys once, extending the current segment as long as one line
can keep every key within eps slots of its real position (eps is the
allowed error, 64 by default). When no line fits anymore, it closes that
segment and starts a new one. Because each segment's line has to start
at the segment's first point, each new key just narrows down which
slopes are still allowed, so building the whole model takes one pass
over the data. On 1M keys this gives 84 segments for uniform data and
151 for skewed, with lookups at 113.6 and 117.7 ns.

Pinning each line to its first point keeps things simple, but it doesn't
give the fewest segments. `build_optimal_segmented_model` removes that
restriction. It keeps track of every line that still fits the keys seen
so far, by tracking the flattest and steepest lines that still work
(with a convex hull), and updates them as each new key comes in. This is
O'Rourke's algorithm from 1981, which PGM-index also uses, and it gives
the minimum possible number of segments. It needs 20–29% fewer segments
than the pinned version, and its counts match PGM-index exactly:

| data (1M keys) | eps | pinned | optimal | PGM-index |
|---|---|---|---|---|
| uniform | 64 | 79 | 57 | 57 |
| skewed | 64 | 157 | 126 | 126 |
| uniform | 16 | 1,253 | 916 | 916 |
| skewed | 16 | 1,453 | 1,025 | 1,025 |

The database still uses the pinned version, because the optimal one
costs too much here. Fitting 1M keys optimally takes 21.8 ms, compared
to 3.5 ms pinned. The database refits its model on every full rebalance,
so switching made inserts 2.7x slower (56,623 ns vs 20,252) and only
saved about 1 KB of segments. PGM-index's own fitting code takes 17.4
ms, so most of that cost comes from the algorithm itself, not my
implementation. Optimal segmentation makes sense for an index that gets
built once and then mostly read, and that isn't what this database does.

### Supporting inserts

Inserting into the middle of a sorted array means shifting everything
after it. To avoid that, the storage engine uses a gapped array that's
about 70% full, with the empty slots spread throughout. An insert
usually lands in a nearby gap after moving a few neighbors, and the
model doesn't need to change.

Each insert moves some keys slightly away from where the model expects
them. To stop that from adding up, every segment counts the inserts that
land in it, and once the count reaches eps, that part of the array gets
spread out again and its model is rebuilt. A much less frequent
rebalance of the whole array runs as a fallback. With 100k inserts into
1M keys, that came to 1,189 local rebalances and 195 full ones. Inserts
took 20,211 ns each, compared to 245,941 ns for a sorted vector that
shifts everything over.

### Crash recovery

Every write is saved to a log file first, and forced all the way to disk
with `fsync`, before the data in memory changes, so if the process
crashes in the middle of a write, that write is either fully there after
a restart or not there at all. Each log record has a type tag, the key,
the value, and a CRC-32 checksum. When the log is opened, it's checked
and cut back to the last complete record. That second part fixed a real
bug: an earlier version skipped over a half-written record but left its
bytes in the file, and new writes got appended after them.

The log only grows, so there's also checkpointing. `checkpoint()` writes
the whole store to a snapshot file and then empties the log. The order
is what makes it safe to crash at any point. The snapshot is written to
a temporary file, fsync'd, and renamed into place, and only then is the
log cleared. If a crash happens between the rename and clearing the log,
you're left with the new snapshot and a log of writes it already
includes, and replaying those doesn't change anything. On startup, the
snapshot is loaded in one pass and any newer log entries are replayed on
top. For a store with 20,000 logged updates, startup went from 9.12 ms
to 0.48 ms.

### The SQL layer

The SQL side is a hand-written tokenizer and recursive-descent parser,
an executor, and a catalog. None of it is generated.

The storage engine only maps `int64` keys to bytes, so each row's key
combines a table id (the top 16 bits) with the row's primary key (the
bottom 48). That keeps each table's rows together and in order, which
suits the segmented model well. The catalog is just another table (table
id 0) with one row per table and per index, similar to SQLite's
`sqlite_master`. On startup it rebuilds itself by reading those rows
back.

The executor picks the cheapest path it can for each query. An equality
check on the primary key becomes a point lookup, `BETWEEN` on the
primary key becomes a range scan, an equality check on an indexed column
uses the secondary index, and anything else is a full scan with
filtering.

Secondary indexes didn't need a new data structure. Indexed values can
repeat, but the learned index needs unique keys, so each index entry
combines the indexed value (top 20 bits) with the row's primary key
(bottom 28 bits). That makes every entry unique and puts all rows with
the same value next to each other, so a lookup is just a range scan on
the same gapped array. On a 1,000-row table this was 18–22x faster than
the full scan it replaced (6,376–7,745 ns vs 134,959–155,267 ns).

Before anything gets written, values are checked against their columns.
Types have to match, primary keys have to fit in 48 bits (28 once the
table has an index), and indexed values have to fit in 20 bits. Anything
out of range is rejected instead of being silently turned into a
different key.

## Benchmarks

### Against a B+-tree and binary search

The B+-tree is one I wrote to use as the baseline (order 64, with node
splitting and linked leaves).

| 1M keys | binary search | B+-tree | one line | segmented |
|---|---|---|---|---|
| uniform | 178.7 ns | 301.7 ns | 118.3 ns | 113.6 ns |
| skewed | 185.2 ns | 319.1 ns | 207.8 ns | 117.7 ns |

Besides the segmented column, two things in this table stand out. First,
plain binary search beats the B+-tree. My B+-tree allocates each node
separately, so going down three or four levels means three or four jumps
to different places in memory, each one probably a cache miss. Binary
search stays inside one block of memory. The original learned index
paper makes the same observation, and it's a big part of why learned
indexes are interesting in the first place. Second, the single-line
model wins on uniform data and loses badly on skewed data, which is the
whole reason for splitting it into segments.

The results hold up as the data grows. From 100K to 5M keys, the
segmented index stays ahead of both baselines, and the gap with the
B+-tree gets wider: at 5M skewed keys it's 187.8 ns vs 548.2.

### Memory

| n | B+-tree | segmented index |
|---|---|---|
| 100K | 3,732,368 bytes (37.3/key) | 248–992 bytes (0.002–0.010/key) |
| 1M | 37,347,776 bytes (37.3/key) | 1,928–3,800 bytes (0.002–0.004/key) |
| 5M | 186,754,792 bytes (37.4/key) | 10,160–12,584 bytes (0.002–0.003/key) |

The B+-tree's cost per key stays flat because it's mostly node and
pointer overhead. The model's size depends on the number of segments,
not the number of keys, so it stays in the kilobytes while the B+-tree
grows to 178 MB. Neither number includes the sorted data itself, since
every approach needs that anyway.

### Against PGM-index

`cpp/third_party/pgm/` has the real PGM-index, unmodified. I ran it on
the same data with the same eps.

| 1M keys | mine (pinned) | mine (optimal) | PGM-index |
|---|---|---|---|
| uniform | 124.0 ns, 79 segments, 1,896 bytes | 111.9 ns, 57 segments, 1,368 bytes | 89.5 ns, 57 segments, 984 bytes |
| skewed | 116.7 ns, 157 segments, 3,768 bytes | 117.5 ns, 126 segments, 3,024 bytes | 114.0 ns, 126 segments, 2,192 bytes |

PGM is faster on uniform data and about the same on skewed. At equal
segment counts, the memory difference comes down to how segments are
stored: PGM uses 16 bytes per segment and I use 24, because it stores
the slope as a 32-bit float and I use a 64-bit double. Turning off PGM's
recursive routing layer (a template parameter) slows its uniform lookups
from 89.5 to 109.8 ns, so that layer clearly helps.

An earlier version of this README said my index was 2x faster than PGM
on skewed data. That came from a broken dataset generator, which I cover
in the bugs section.

### Inside the database

Everything above times the index on its own. The database uses the
gapped array with inserts, so it has to deal with empty slots and a
model that drifts. `cpp/benchmark_db_lookups.cpp` measures that path
against the same baselines, before and after inserting 100k keys.

| 1M keys, uniform | fresh | after 100k inserts |
|---|---|---|
| binary search | 168.0 ns | 178.2 ns |
| B+-tree | 285.5 ns | 291.6 ns |
| read-only segmented model | 110.1 ns | 122.0 ns |
| gapped array (the database) | 162.5 ns | 142.0 ns |

So a real lookup in the database is about 1.9–2x faster than the
B+-tree, not the 2.7x the read-only index gets. Lookups for keys that
don't exist cost about the same as ones that find something, since the
walk does the same amount of work either way. One thing I haven't
figured out yet: on uniform data, the freshly built array is slower than
the same array after 100,000 inserts (162.5 vs 142.0 ns). The inserts
trigger rebalances that refit the model, so the layout afterward might
just be easier to predict, but I haven't confirmed that.

### Durability

| operation | ns/op |
|---|---|
| plain insert, no log | 3,251 |
| durable put: log, fsync, insert | 118,125 |

That's about 36x slower per write, and nearly all of it is `fsync`,
which is why real databases group several writes into one sync. When I
ran the same test on a different machine, it came out to 1,926,085 ns vs
2,702, or about 713x, because a single fsync there takes around 1.9 ms
instead of 113 µs. The code was exactly the same. So a number like "36x"
tells you about as much about the disk as it does about the database.

### Concentrated inserts

If someone controls what gets inserted, the obvious attack is to put all
the inserts into a narrow range of keys. That uses up the gaps in one
area and forces much more rebalancing than the same number of spread-out
inserts. Published attacks on ALEX and PGM-index report slowdowns of up
to about 20%, which gives a rough point of comparison.

Each scenario inserts 100,000 keys into the same 900,000-key array. I
ran each one nine times, alternating between scenarios, and compared
each run to the normal spread-out case from the same round.

| scenario | local rebalances | full rebalances | slowdown, two runs |
|---|---|---|---|
| spread out (normal) | 0 | 195 | — |
| 5.0% window | 231 | 195 | +1.6%, −1.3% |
| 2.0% window | 792 | 195 | +5.0%, +2.2% |
| 1.2% window | 1,096 | 189 | +11.7%, +9.9% |

The slowdown is real and grows as the range gets narrower, reaching
about 10%. The rebalance counts show why it isn't worse. Local
rebalances go from 0 to 1,096, but full rebalances stay flat (and even
drop slightly), so the per-segment rebalancing handles almost all of it
and the expensive full rebuilds don't happen any more often. I didn't
design it with this in mind; per-segment rebalancing was only added to
make normal inserts faster.

### Learned Bloom filter and learned cache

I also tried the same idea of learning the data's structure in two other
places, and they went in opposite directions.

The first was a learned Bloom filter: a small machine learning model
(logistic regression) that guesses whether a key is in the set, plus a
backup list so it never wrongly says a key is missing. It lost badly.

| filter | memory | false positive rate |
|---|---|---|
| normal Bloom filter, built for 1% | 119,814 bytes | 1.021% |
| learned, matched byte budget | 462,288 bytes | 50.380% |
| learned, 64x the memory | 7,668,048 bytes | 50.225% |

Giving it 64 times more memory didn't help at all, which rules out the
obvious explanation. The real reason is that whether some arbitrary
integer is in a set just isn't something with a pattern to learn. A
normal Bloom filter hashes and sets bits, which is basically
memorization, and that's already about as efficient as you can get for
this problem. A key's position in a sorted array is different, because
it's a smooth function that a model can actually approximate.

The second was a learned cache, and that one worked. It uses the same
kind of model to decide which item to remove from the cache, based on
how recently and how often each item was used. I trained it on one
access pattern and tested it on a different one, both with a few items
used far more than the rest. The goal is to get close to Belady's
policy, which makes the best possible choice but needs to know future
requests, so it can't be used for real.

| policy | hit rate |
|---|---|
| LRU | 58.67% |
| learned, Hawkeye-style | 63.36% |
| Belady's optimal (needs to know the future) | 74.63% |

That closes 29.4% of the gap between LRU and Belady's policy. That's in
line with what the Hawkeye and LRB papers report.

## Bugs I found along the way

Almost every part of this project had at least one real bug, and nearly
all of them were caught by a correctness check rather than a crash.
These are the ones I think are most interesting.

### The skewed dataset wasn't skewed

The best number this project ever produced was 26.9 ns per lookup on
"skewed" data, about 12x faster than the B+-tree. It turned out to be
wrong.

The generator drew numbers from a lognormal distribution (one with a
long tail of very large values) and scaled them so the largest one
became n × 10. With sigma = 2, the largest draw is thousands of times
bigger than a typical one, so almost all the draws rounded down to the
same few hundred integers. Then the step that bumps duplicates up by one
turned them into one long run of consecutive integers: 999,819 of the
1,000,000 keys, with 99.98% of the gaps equal to 1. A straight line fits
consecutive integers perfectly, which is why the "skewed" data needed
only 2 or 3 segments when the uniform data needed 84.

Scaling to 1e15 instead fixed it, and I re-ran every benchmark:

| | broken generator | fixed generator |
|---|---|---|
| segments (eps = 64) | 2–3 | 151 |
| segmented lookup | 26.9 ns | 117.7 ns |
| vs. the B+-tree | 12.5x faster | 2.7x faster |
| vs. PGM-index | 2x faster | about even |

Beating the B+-tree, the memory results, and the case for segments over
a single line all held up. The headline number didn't. I'd been checking
results the whole time but never actually looked at the input data, and
a quick look at the gaps between keys would have caught this right at
the start.

### The error bound didn't cover new keys

Each segment guarantees that every key is predicted within ±eps, so
every lookup used to scan a ±eps window around the prediction. But that
guarantee only applies to keys the model was trained on. A key inserted
after the last refit, or a search for a key that doesn't exist, has no
such bound, and nothing in the code accounted for that.

It showed up through a secondary index. One row's age had been updated
to an outlier, which stretched a segment across a much wider range of
keys than its data actually covered. A search for a value inside that
range predicted slot 1644 in an array with 1430 slots. Four different
functions scanned that window, and each one broke in a different way.
The lower-bound function returned a later key instead of the first
match. `insert` put keys out of sorted order, which a randomized test
later found in 284 of 300 trials on the committed code. No existing test
had caught it. My first attempt at a fix added bigger and bigger
fallback scans to `search`, which made every search for a missing key
O(n), at 630,566 ns. Every insert checks for duplicates first, so
inserts went from 21,691 to 540,180 ns, slower than the plain sorted
vector this structure is supposed to beat. I measured that, threw it
out, and never committed it.

The real fix was to stop relying on the model for correctness. All four
functions now use the walk described above, which is correct no matter
how bad the prediction is, and there's a clamp that keeps a segment from
predicting past the start of the next one. The worst case is still an
O(n) walk if the model is very wrong, but a slow lookup is a lot better
than a wrong one.

### The write-ahead log could lose writes after a crash

When replaying the log, a record that was only half written because of a
crash got skipped, which is correct. But its bytes were left in the
file, and because the log was reopened in append mode, the next write
went right after them. On the following restart, replay read the
leftover bytes together with the next record, so writes made after a
crash could be lost or read incorrectly. That's a bad bug to have in the
one component whose whole job is to not lose writes.

There were also no checksums, so a run of zero bytes at the end of the
file (which some filesystems can leave behind after a crash) was read as
valid records, and a corrupted byte in the middle was accepted as data.
I wrote the tests first: cut the last few bytes off a log, add zeros to
the end, flip a byte in the middle, and pass in a file that isn't a log
at all. Four of the five checks failed on the old code. The log format
now has a header and a CRC-32 on every record, and opening a log cuts it
back to the last complete record before anything new gets written.

### What differential testing found

Unit tests only check the cases I thought of, so
`python/sqlite_diff_test.py` generates random SQL, runs every statement
on both this database and SQLite, and compares the results. When they
don't match, it shrinks the sequence of statements down to a minimal
example. It found two real bugs in its first few runs.

The first was that `LIMIT` was applied before `COUNT(*)` and `SUM`
instead of after, so `SELECT COUNT(*) FROM items LIMIT 3` returned 3 on
a table with four rows. The second was that an `INSERT` with an existing
primary key replaced the row but left its old secondary index entry
behind, so searching for the old value still found the row through the
index. An `UPDATE` that moved a row onto another row's key had the same
problem.

Later, while adding input validation, I found that values outside the
key limits weren't being rejected. Instead, their extra bits were
quietly cut off, which gave wrong results. With an index on age,
`WHERE age = 1048621` (2^20 + 45) returned the rows with age 45, and
`WHERE id BETWEEN -5 AND 3` returned nothing at all because the negative
bound turned into a huge key. I wrote 13 checks for this, and 12 of them
fail on the previous version.

### A pattern in these bugs

One of the earliest bugs was a missing bounds check in `insert`. Much
later, I found the exact same mistake in `search`, where it had been
sitting for most of the project because I never checked for the same
pattern anywhere else. The error-bound bug above was the same kind of
problem again, in four functions at once. What finally fixed this for
good was merging those functions: all four lookups now go through one
piece of code, so there's only one place for that logic to be wrong.

## Testing

- 41 storage engine tests, 20 parser tests, and 79 end-to-end SQL tests,
  all passing.
- Randomized tests against reference data structures: the gapped array's
  contents against a `std::map`, insert and delete sequences with
  outliers against a `std::set`, and the optimal segmenter against a
  brute-force search over every possible line.
- Crash recovery tests that damage a real log file by truncating it,
  padding it with zeros, flipping a bit, or swapping in a file with the
  wrong format.
- Differential testing against SQLite, with automatic shrinking of
  failures. The latest pass was 200 runs of 300 statements and 50 runs
  of 1,000, including invalid statements, checkpoints and restarts, with
  no mismatches.
- AddressSanitizer and UBSan on every test suite. These caught two real
  memory bugs that normal runs didn't.
- GitHub Actions runs all of this on every push.

The benchmarks aren't part of CI, because timings on shared machines are
too noisy to mean much. Every number in this README was measured on a
quiet machine, and the raw results are in `results/`.

## Limits

Most of these are on purpose:

- No joins, multi-statement transactions, or concurrent access. Each of
  those would be a big project on its own.
- The first column of a table is always the primary key. There's no
  `PRIMARY KEY` syntax.
- Keys are packed into 64 bits: 16 bits for the table id and 48 for the
  primary key, or 20 bits for the value and 28 for the primary key
  inside a secondary index. Values outside those ranges are rejected.
- A lookup can be O(n) in the worst case if the model is very wrong.
  That only affects speed, not correctness.
- A checkpoint rewrites the whole store, so the write that triggers one
  is slow. A production database would do that in the background.
- `EXPLAIN` only works with `SELECT`.

## References

- Kraska, Beutel, Chi, Dean, Polyzotis. "The Case for Learned Index
  Structures" (2018).
- Ferragina, Vinciguerra. "The PGM-index" (2020). Included under
  `cpp/third_party/pgm/`, Apache 2.0.
- Ding et al. "ALEX: An Updatable Adaptive Learned Index" (2020).
- O'Rourke. "An on-line algorithm for fitting straight lines between
  data ranges" (1981).
- Jain, Lin. "Back to the Future: Leveraging Belady's Algorithm for
  Improved Cache Replacement" (2016).
