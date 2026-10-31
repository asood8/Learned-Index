# Learned Index DB

[![tests](https://github.com/asood8/Learned-Index/actions/workflows/tests.yml/badge.svg)](https://github.com/asood8/Learned-Index/actions/workflows/tests.yml)

This is an embedded SQL database written in C++17. The storage engine is
a learned index instead of a B-tree. To find a key, it uses a small
model, made of a few straight lines, to guess where the key is in a
sorted array, and then checks the nearby slots until it finds the right
one.

The design is based on Kraska et al., "The Case for Learned Index
Structures" (2018), with ideas from later work: the way it splits the
data into pieces is the same greedy method FITing-tree uses, PGM-index
showed how to do that split optimally, and ALEX showed how to handle
inserts with a gapped array. I also included the published PGM-index and
a real B+-tree library (tlx) in the repo, so the benchmarks compare
against real implementations and not only against my own baselines. The
whole database can also be built on tlx's B+-tree instead of the learned
index, with nothing else changed, which is how the "whole database"
numbers below are measured.

"Embedded" means the same thing it does for SQLite: a single process, a
write-ahead log on disk, and a SQL layer on top of the storage engine.
It doesn't support joins, multi-statement transactions, or concurrent
access. It does support `CREATE TABLE`, `INSERT`, `SELECT` (with
`WHERE`, `ORDER BY`, `LIMIT`, `COUNT` and `SUM`), `UPDATE`, `DELETE`,
secondary indexes, `EXPLAIN`, crash recovery, checkpointing, and a REPL.

## Results

All of these come from two runs of `scripts/run_benchmarks.sh` on October
31, 2026, at the same commit, with the same compiler (g++ 15.2) on Ubuntu
26.04. One was on my laptop (an Intel i7-13620H under WSL2, with nothing
else running). The other was on a rented AWS `c7a.xlarge` (an AMD EPYC 9R14
virtual machine with 4 vCPUs and network storage). The raw output, along
with the machine, compiler and commit, is in
`results/runs/2026-10-31_0201/` (laptop) and
`results/runs/2026-10-31_0514/` (AWS). Lookup times are medians over
interleaved passes, and the whole-database comparisons are medians of three
runs of each build. On the laptop the same benchmark can move 10–20% from
one run to the next, and more at 10M keys, so the ratios mean more than the
exact nanoseconds. The AWS machine was much steadier. Everything is on 1M
keys unless it says otherwise.

| | laptop | AWS |
|---|---|---|
| Point lookup in the storage engine (uniform, skewed keys) | 115.3, 120.8 ns | 78.4, 84.7 ns |
| tlx's B+-tree | 178.3, 185.7 ns | 145.6, 147.3 ns |
| Binary search | 167.7, 167.6 ns | 132.3, 135.6 ns |
| Speedup over tlx at 10M keys | 1.33–1.35x (moves a lot, see below) | 1.45–1.47x |
| Speedup over tlx on four real datasets (10M keys) | 1.10–1.39x | 1.11–1.54x |
| The same lookup through SQL, statement parsed ahead of time, against the same database built on tlx | 1.08–1.20x | 1.06–1.15x |
| The same, statement parsed each time | 1.02–1.06x | 0.99–1.02x |
| Inserts in random order (tlx in brackets) | 375, 408 ns (277, 322) | 336, 347 ns (271, 263) |
| Inserts in increasing order | 2.0–2.3x slower than tlx | 2.2–2.3x slower than tlx |
| Appends past the end | 12–15x slower than tlx | 11–15x slower than tlx |
| Against the published PGM-index (uniform, skewed; PGM in brackets) | 89.4, 92.8 ns (92.9, 112.1) | 72.1, 75.4 ns (70.9, 87.7) |
| Secondary index lookup, against the full scan it replaces (1,000 rows) | 22.0x faster | 28.6x faster |
| Opening a store of 2,000 rows, before and after a checkpoint | 7.96 ms, 0.47 ms | 7.81 ms, 0.42 ms |

Two results don't depend on the machine. The model takes 0.0065–0.0074 bytes
per key, against 0.26–0.53 for tlx's inner nodes, while the whole gapped array
takes 11.4 bytes per key, against 8.6–9.3 for tlx. And the learned Bloom filter
lost badly: 50.4% false positives against 1.021% for a normal one.

A few notes on these.

The comparison that matters is the one against tlx. I wrote my own B+-tree as
the first baseline, and the learned index beats it by 1.6–3.0x, but mine
allocates every node separately and was never tuned, so that mostly says my
B+-tree is slow. tlx is a maintained library (it replaced the stx B+-tree that
the SOSD learned-index benchmark uses). I tried it with node sizes from 128
bytes to 2 KB on the laptop and use its fastest setting there, 512-byte nodes.
I didn't repeat that tuning on AWS, where plain binary search actually beats
tlx at that setting, so tlx may not be at its best there. A sorted array can't
take inserts, though, and both the gapped array and tlx can. Against tlx, the
learned index's lookups are 1.5–1.9x faster at 1M keys and 1.1–1.5x faster on
four real datasets. That's the same direction the learned-index papers report,
and a smaller gap than the numbers usually quoted.

Inside the whole database, most of that disappears. Running the same SQL on
the same engine built on tlx, a point lookup is 1.06–1.20x faster on the
learned index when the statement is already parsed, and about even when it
isn't (0.99–1.06x), mostly because parsing, planning and turning bytes back
into a row cost more than the lookup itself. Appends are 1.5–3.4x slower, and
inserts that don't follow the existing key distribution can be up to 3.3x
slower. "The whole database on each index" below has the details.

The memory result is real, but smaller than it sounds. The model is 35–80 times
smaller than tlx's inner nodes, and over 200 times smaller than my B+-tree's.
But the gapped array keeps about 30% of its slots empty for inserts, and those
empty slots cost more than the model saves, so in total it takes more memory
than a packed B+-tree. An earlier version of this README compared the model
with my B+-tree's whole size, keys and values included, and called that
thousands of times less memory. That wasn't a fair comparison.

## Building and running

Each program is a single `.cpp` file in `cpp/`, compiled with g++
(C++17). The Makefile covers the common tasks and puts the binaries in
`build/`.

```bash
make test        # the three test suites
make asan        # the same suites under AddressSanitizer and UBSan
make difftest    # differential test against SQLite (EPS=4 stresses rebalancing)
make test-btree  # the test suites on the database built on tlx's B+-tree
make demos       # durability and catalog restart demos
make repl        # build and start the SQL shell
make data        # generate the synthetic benchmark datasets (needs numpy)
make all         # build everything, including benchmarks

python3 python/sosd_data.py   # download four real datasets from SOSD (needs numpy; zstd too before Python 3.14)
scripts/run_benchmarks.sh     # rerun every benchmark in this README
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

Walking one slot at a time is fast when the guess is close, but a very
bad guess used to mean a very long walk, O(n) in the worst case. So
the walk now goes at most eps slots in each direction. That's the
model's own error bound, so if the answer still isn't there, the model
is clearly off. At that point the search switches to galloping: it
jumps 1, 2, 4, 8, ... slots until it passes the answer, then does a
binary search inside the last jump, which takes O(log n) steps.
Starting a lookup at the wrong end of a 1.3M-slot array went from
about 607,000 ns to about 180 ns, and normal lookups cost the same as
before. I also tried galloping sooner, since the average walk is about
25 slots, but that made normal lookups slower (157 vs 134 ns when
galloping from the first step). Stepping through neighboring slots
stays in the same cache lines, while a binary search jumps around.

(Numbers in this section and in the bugs section come from experiments
while the code was being written, so they're from different runs than
the tables.)

### Fitting segments

A single line only works if the data is close to uniform. On the skewed
dataset, the best single line is off by up to 15.7M positions in an
array of 1M keys, so the "local" search ends up covering the whole
array. In practice that's 194.0 ns per lookup on my laptop, which is slower
than plain binary search at 177.3 ns.

So the model is split into pieces. `build_segmented_model` goes through
the sorted keys once, extending the current segment as long as one line
can keep every key within eps slots of its real position (eps is the
allowed error, 64 by default). When no line fits anymore, it closes that
segment and starts a new one. Because each segment's line has to start
at the segment's first point, each new key just narrows down which
slopes are still allowed, so building the whole model takes one pass
over the data. This is the same greedy method as FITing-tree's
ShrinkingCone algorithm (Galakatos et al., 2019). On 1M keys it gives 84
segments for uniform data and 151 for skewed, with lookups at 88.4 and
93.0 ns on my laptop.

To use a segment, a lookup first has to find it, with a binary search
over the segments' starting keys. That turned out to be about a third of
every lookup. It was `std::upper_bound`, and since a random key is as
likely to go left as right at each step, the processor guessed the
branch wrong about half the time. Rewriting it to pick each half with a
conditional move instead of a branch took it from about 48 ns to about
9 ns, and lookups in the database went from about 148 ns to about 125.

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
to 3.5 ms pinned, and the database refits part of its model every few
dozen inserts. Switching made inserts 3–4x slower (1,233–1,717 ns vs
303–620) and left lookups about the same. PGM-index's own fitting code
takes 17.4 ms, so most of that cost comes from the algorithm itself, not
my implementation. Optimal segmentation makes sense for an index that
gets built once and then mostly read, and that isn't what this database
does.

### Supporting inserts

Inserting into the middle of a sorted array means shifting everything
after it. To avoid that, the storage engine uses a gapped array that's
about 70% full, with the empty slots spread throughout. An insert
usually lands in a nearby gap after moving a few neighbors, and the
model doesn't need to change.

Each of those moves pushes a key one slot away from where the model
expects it. To stop that from adding up, every segment counts the
inserts that changed it: the one that gained a key, and any neighbor
whose keys got pushed. When the count reaches eps, that segment's part
of the array is spread out evenly again and refit. Since each counted
insert moves a key by at most one slot, a key the model was fit on is
never more than 2 × eps from its prediction.

Sometimes a segment's part of the array is too full to spread out (90%
full). Then it borrows room from its neighbors: the refresh widens to the
pair of segments around it, then four, then eight, until the window has
enough empty slots, with a lower limit for each wider window. This is how
a packed-memory array rebalances (Itai, Konheim and Rodeh 1981; Bender,
Demaine and Farach-Colton 2000). The whole array is only rebuilt, bigger,
once it's more than 80% full. The last segment can skip all that and
just grow the array, which is what happens when keys keep arriving past
the end. Segments are also capped at 4,096 keys, because on perfectly
regular keys one line fits everything, and refreshing that one segment
would mean rebuilding the whole array.

Inserting 100,000 keys into 900,000 takes 1,200–1,600 of those
refreshes, depending on the order the keys arrive in, and no full
rebuilds. The bugs section explains why this part was rewritten.

The borrowing has a cost when inserts don't follow the data. Skewed data
leaves a few segments that cover most of the key range but hold few keys.
Inserts spread evenly over the key range pour into those small regions,
which fill almost at once and keep borrowing from their neighbors. On 1M
skewed keys, that re-lays 161 slots per insert instead of the usual 49,
and the inserts take 1,066 ns instead of 408 on my laptop (the "spread" rows under
Inserts below). A B+-tree only splits the leaf that's full, so it
doesn't care where inserts land.

### Crash recovery

Every write is saved to a log file first, and forced all the way to disk
with `fsync`, before the data in memory changes, so if the process
crashes in the middle of a write, that write is either fully there after
a restart or not there at all. Each log record has a type tag, the key,
the value, and a CRC-32 checksum. When the log is opened, it's checked
and cut back to the last complete record. That second part fixed a real
bug: an earlier version skipped over a half-written record but left its
bytes in the file, and new writes got appended after them.

An fsync takes about 2 ms on this laptop, far more than anything else
a write does, so the log can also fsync only every k-th write. Every
record still reaches the operating system before the write returns, so
a crash of the process loses nothing, but up to k − 1 writes can be lost
if the machine loses power. That's the same trade SQLite makes with
`synchronous=NORMAL`. It isn't group commit, where several clients share
one fsync and each still waits for it, because this database has one
client and no transactions to group.

The log only grows, so there's also checkpointing. `checkpoint()` writes
the whole store to a snapshot file and then empties the log. The order
is what makes it safe to crash at any point. The snapshot is written to
a temporary file, fsync'd, and renamed into place, and only then is the
log cleared. If a crash happens between the rename and clearing the log,
you're left with the new snapshot and a log of writes it already
includes, and applying those again doesn't change anything.

On startup, the snapshot is loaded, the log records written since are
applied to it (for each key, the last record logged for it wins), and
the array is built once from the result. It used to replay the log one
insert at a time, which took 59.8 s for a log of 1M writes. For a store
of 2,000 rows that had each been written ten times, so 20,000 log
records, opening it took 7.96 ms from the log and 0.47 ms after a
checkpoint on my laptop.

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
the same gapped array. On a 1,000-row table this was 22.0x faster than
the full scan it replaced on my laptop (6,631 vs 146,170 ns), and 28.6x on
AWS (3,193 vs 91,463 ns). That number moves a lot
between runs (I've seen 18x to 29x), since each query only takes a few
microseconds, and on a table this small any index would win.

Before anything gets written, values are checked against their columns.
Types have to match, primary keys have to fit in 48 bits (28 once the
table has an index), and indexed values have to fit in 20 bits. Anything
out of range is rejected instead of being silently turned into a
different key.

The storage layer refers to its index through one type alias, so
building with `-DLIDB_BTREE_INDEX` puts tlx's B+-tree there instead
(`cpp/btree_row_index.h`), with the parser, executor, catalog, log and
snapshots unchanged. Both builds pass the same test suites and the same
differential test against SQLite.

## Benchmarks

### Against a B+-tree and binary search

These time the read-only index on a plain sorted array, against binary
search and the B+-tree I wrote as the first baseline (order 64, with
node splitting and linked leaves). The comparison with tlx is in "Inside
the storage engine" below.

| 1M keys | binary search | my B+-tree | one line | segmented |
|---|---|---|---|---|
| uniform, laptop | 175.8 ns | 300.5 ns | 119.2 ns | 88.4 ns |
| skewed, laptop | 177.3 ns | 302.5 ns | 194.0 ns | 93.0 ns |
| uniform, AWS | 130.6 ns | 249.5 ns | 94.4 ns | 72.5 ns |
| skewed, AWS | 131.1 ns | 262.3 ns | 148.6 ns | 76.1 ns |

Besides the segmented column, two things in this table stand out. First,
plain binary search beats my B+-tree. My B+-tree allocates each node
separately, so going down three or four levels means three or four
jumps to different places in memory, each one probably a cache miss.
Binary search stays inside one block of memory. Second, the single-line
model wins on uniform data and loses on skewed data, which is the
whole reason for splitting it into segments.

The results hold up as the data grows. From 100K to 5M keys, the
segmented index stays ahead of both baselines, and the gap with the
B+-tree gets wider: at 5M skewed keys it's 158.6 ns on the laptop, against
514.7 for the B+-tree and 343.2 for binary search (152.1, 435.0 and 259.5
on AWS).

### Memory

There are two fair ways to compare memory: just the part that does the
routing (a B+-tree's inner nodes, or a learned index's segments), or the
whole structure, keys included.

| | routing only | whole structure |
|---|---|---|
| sorted array (binary search) | none | 8.00 bytes/key |
| my B+-tree | 1.58–1.60 bytes/key | 37.3 bytes/key (it also stores an 8-byte value per key) |
| tlx, 256-byte nodes | 0.53 bytes/key | 9.28 bytes/key |
| tlx, 512-byte nodes | 0.26 bytes/key | 8.63 bytes/key |
| read-only segmented model | 0.0019–0.0099 bytes/key | about 8.00 bytes/key |
| gapped array (the database) | 0.0065–0.0074 bytes/key | 11.44 bytes/key |

The tlx rows are for trees bulk-loaded on 900K keys, so their leaves are
completely full, which is the best case for tlx. The read-only model's
range covers 100K to 5M keys; the gapped array's is for 900K, and its
model is bigger than the read-only one because of the 4,096-key cap on
segments. On the real datasets below, the gapped array's model takes
0.006–0.015 bytes per key on books and wiki, and 0.07 on fb and osm,
which are harder to fit.

A model's size depends on how many segments it needs, not on how many
keys there are, so it stays in the kilobytes: 1,928–3,800 bytes for 1M
keys, and 10,160–12,584 bytes for 5M. A B+-tree's routing part grows
with the number of keys (8,004,944 bytes for my B+-tree at 5M). So the
model really is far smaller than the routing it replaces: 35–80 times
smaller than tlx's inner nodes, and 200–800 times smaller than my
B+-tree's. But the whole gapped array is bigger than a packed tlx tree,
because of its empty slots.

An earlier version of this table put the B+-tree's whole size (37.3
bytes per key) next to the model's and said neither included the sorted
data. That was wrong: the B+-tree's figure included its leaves, which
hold every key and value.

### Against PGM-index

`cpp/third_party/pgm/` has the real PGM-index, unmodified. I ran it on
the same data with the same eps.

| 1M keys | mine (pinned) | mine (optimal) | PGM-index | PGM, no recursion |
|---|---|---|---|---|
| uniform, laptop | 89.4 ns | 85.9 ns | 92.9 ns | 105.5 ns |
| skewed, laptop | 92.8 ns | 90.1 ns | 112.1 ns | 114.6 ns |
| uniform, AWS | 72.1 ns | 70.3 ns | 70.9 ns | 78.7 ns |
| skewed, AWS | 75.4 ns | 74.7 ns | 87.7 ns | 85.2 ns |

| 1M keys | mine (pinned) | mine (optimal) | PGM-index |
|---|---|---|---|
| uniform | 79 segments, 1,896 bytes | 57 segments, 1,368 bytes | 57 segments, 984 bytes |
| skewed | 157 segments, 3,768 bytes | 126 segments, 3,024 bytes | 126 segments, 2,192 bytes |

Mine is faster on skewed data on both machines, and about even on uniform
data. That's recent: before the branchless segment search described above,
PGM was clearly faster on uniform data (89.5 vs 124.0 ns, in an earlier
session). I left PGM's code as it is. At equal segment counts, the memory
difference comes down to how segments are stored: PGM uses 16 bytes per
segment and I use 24, because it stores the slope as a 32-bit float and
I use a 64-bit double. Turning off PGM's recursive routing layer (a
template parameter) slows its uniform lookups, so that layer helps there, and
it makes little difference on skewed data.

An earlier version of this README said my index was 2x faster than PGM
on skewed data. That came from a broken dataset generator, which I cover
in the bugs section.

### Inside the storage engine

Everything above times the index on its own. The database uses the
gapped array with inserts, so it has to deal with empty slots and a
model that drifts. `cpp/benchmark_db_lookups.cpp` measures that path
against the same baselines plus tlx, before and after inserting 10% more
keys.

Laptop:

| 1M keys | uniform, fresh | uniform, after inserts | skewed, fresh | skewed, after inserts |
|---|---|---|---|---|
| binary search | 167.7 ns | 184.0 ns | 167.6 ns | 177.8 ns |
| my B+-tree | 279.0 ns | 300.3 ns | 289.9 ns | 289.4 ns |
| tlx, 256-byte nodes | 189.9 ns | 279.3 ns | 199.7 ns | 266.6 ns |
| tlx, 512-byte nodes | 178.3 ns | 214.1 ns | 185.7 ns | 220.9 ns |
| read-only segmented model | 90.4 ns | 94.9 ns | 93.9 ns | 96.6 ns |
| gapped array (the database) | 115.3 ns | 125.9 ns | 120.8 ns | 126.1 ns |

| 10M keys | uniform, fresh | uniform, after inserts | skewed, fresh | skewed, after inserts |
|---|---|---|---|---|
| binary search | 406.1 ns | 451.5 ns | 403.9 ns | 441.1 ns |
| my B+-tree | 663.2 ns | 715.6 ns | 660.8 ns | 673.7 ns |
| tlx, 256-byte nodes | 467.1 ns | 651.4 ns | 416.9 ns | 616.5 ns |
| tlx, 512-byte nodes | 390.2 ns | 492.5 ns | 390.4 ns | 523.4 ns |
| read-only segmented model | 193.1 ns | 212.9 ns | 195.6 ns | 213.9 ns |
| gapped array (the database) | 293.0 ns | 295.1 ns | 288.5 ns | 298.0 ns |

AWS:

| 1M keys | uniform, fresh | uniform, after inserts | skewed, fresh | skewed, after inserts |
|---|---|---|---|---|
| binary search | 132.3 ns | 135.6 ns | 135.6 ns | 135.2 ns |
| my B+-tree | 232.1 ns | 237.0 ns | 228.8 ns | 242.0 ns |
| tlx, 256-byte nodes | 162.5 ns | 226.8 ns | 158.8 ns | 227.4 ns |
| tlx, 512-byte nodes | 145.6 ns | 178.0 ns | 147.3 ns | 177.8 ns |
| read-only segmented model | 76.4 ns | 81.6 ns | 82.3 ns | 85.4 ns |
| gapped array (the database) | 78.4 ns | 88.7 ns | 84.7 ns | 92.1 ns |

| 10M keys | uniform, fresh | uniform, after inserts | skewed, fresh | skewed, after inserts |
|---|---|---|---|---|
| binary search | 312.6 ns | 319.0 ns | 311.7 ns | 327.4 ns |
| my B+-tree | 477.3 ns | 480.9 ns | 485.2 ns | 486.4 ns |
| tlx, 256-byte nodes | 383.1 ns | 468.6 ns | 377.9 ns | 470.7 ns |
| tlx, 512-byte nodes | 343.6 ns | 375.4 ns | 338.2 ns | 374.1 ns |
| read-only segmented model | 177.2 ns | 178.3 ns | 177.3 ns | 180.5 ns |
| gapped array (the database) | 234.1 ns | 252.0 ns | 233.0 ns | 252.3 ns |

So the database's lookups are 1.5–1.9x faster than tlx at 1M keys and 1.3–1.5x
faster at 10M, where nothing fits in the processor's cache anymore. After
inserts the gap widens to 1.5–2.0x, because tlx's full leaves split and end up
half empty, while the gapped array had room set aside. Lookups for keys that
don't exist cost about the same as ones that find something (116.0 vs 115.3 ns
on uniform data on the laptop), since the walk does the same amount of work
either way.

The laptop's 10M number needs a warning. Five days earlier, the same benchmark
on the same laptop measured 1.02–1.08x, and I wanted to know why. So I ran the
10M uniform benchmark as 14 separate processes in one sitting, and got anything
from 1.11x to 1.47x. The speedups fall into two clusters, 1.11–1.20x and
1.34–1.47x, mostly because the gapped array takes either about 290 or about
350 ns. It isn't transparent huge pages, because both clusters show up with
them on and off, and I haven't worked out what it is. Interleaving passes
doesn't help here, because all of a run's passes share one process, so each
run draws one sample. The raw numbers are in `results/experiments/`. I didn't
repeat this on AWS, though the whole-database runs there, which do use separate
processes, agree with each other closely. So at 10M keys on the laptop, treat
the speedup as a range.

An earlier version of this benchmark said the array got *faster* after
100,000 inserts (162.5 vs 142.0 ns), and I couldn't explain it. It
turned out not to be real. The inserts don't change how well the
model predicts: the average walk was 25.1 slots before and after, and
the average guess was off by 22.3 slots before and 22.4 after. What
changed was *when* each one was measured. The old benchmark timed one
structure after another, and that alone moves results a lot here: the
exact same array, with the same queries, took 126.1 ns early in a run
and 142.1 ns later in the same run. Timed alternately, the array after
inserts was about 4% slower (142.8 vs 137.1 ns), which fits it being
11% bigger. Every lookup benchmark now takes turns between the
structures on every pass.

### Real datasets

`python/sosd_data.py` downloads four datasets from SOSD (Marcus et al.,
2020): book popularity (books), Facebook user ids (fb), Wikipedia edit
timestamps (wiki) and OpenStreetMap cell ids (osm), checks them against
SOSD's published checksums, and keeps every 20th or 80th key to get
10M. wiki has repeated timestamps, so 2,417 duplicates were dropped, and
osm's keys were shifted right one bit to fit in a signed 64-bit integer.

| 10M keys, fresh | books | fb | osm | wiki |
|---|---|---|---|---|
| tlx, 512-byte nodes, laptop | 398.8 ns | 433.9 ns | 418.7 ns | 412.4 ns |
| gapped array, laptop | 286.2 ns | 395.1 ns | 370.8 ns | 335.9 ns |
| speedup, laptop | 1.39x | 1.10x | 1.13x | 1.23x |
| tlx, 512-byte nodes, AWS | 337.7 ns | 335.7 ns | 336.0 ns | 337.8 ns |
| gapped array, AWS | 220.0 ns | 298.4 ns | 303.0 ns | 266.0 ns |
| speedup, AWS | 1.54x | 1.13x | 1.11x | 1.27x |
| model size | 0.0060 bytes/key | 0.0695 | 0.0723 | 0.0147 |

The two machines agree on the order: books gains the most, then wiki, and fb
and osm the least. fb and osm are the two datasets SOSD also finds hard for
learned indexes, and they need 5–12 times as many segments as the other two.
After inserting 10% more keys, the gapped array was 1.2–1.7x faster than tlx on
all four, again mostly because tlx's leaves split.

### Inserts

`cpp/benchmark_inserts.cpp` builds each structure on 90% of the keys and
inserts the other 10%, in four orders: the held-out keys scattered
across the whole range, smallest first ("increasing"); the same keys
shuffled ("random"); the largest 10% in order, the way auto-increment
ids arrive ("appended"); and as many new keys drawn evenly from the
whole key range instead of from the data, shuffled ("spread"). Each
order is a single timed pass, since inserting changes the structure, so
these numbers are noisier than the lookup ones.

| 1M keys, ns per insert, laptop | gapped array | my B+-tree | tlx, 256-byte nodes | tlx, 512-byte nodes |
|---|---|---|---|---|
| uniform, increasing | 298.7 | 124.5 | 144.7 | 129.3 |
| uniform, random | 375.0 | 400.5 | 321.2 | 277.1 |
| uniform, appended | 553.4 | 66.4 | 45.9 | 37.4 |
| uniform, spread | 375.1 | 393.6 | 316.8 | 272.5 |
| skewed, increasing | 294.0 | 130.0 | 145.5 | 150.1 |
| skewed, random | 408.0 | 436.0 | 379.2 | 321.8 |
| skewed, appended | 437.6 | 70.6 | 46.2 | 36.9 |
| skewed, spread | 1,065.7 | 215.1 | 155.1 | 145.1 |

| 1M keys, ns per insert, AWS | gapped array | my B+-tree | tlx, 256-byte nodes | tlx, 512-byte nodes |
|---|---|---|---|---|
| uniform, increasing | 262.3 | 119.2 | 132.2 | 116.5 |
| uniform, random | 336.0 | 392.5 | 320.6 | 270.8 |
| uniform, appended | 537.9 | 62.3 | 43.3 | 36.4 |
| uniform, spread | 333.1 | 391.6 | 315.5 | 264.8 |
| skewed, increasing | 253.3 | 116.2 | 130.6 | 114.5 |
| skewed, random | 347.0 | 385.5 | 313.5 | 262.7 |
| skewed, appended | 415.2 | 61.8 | 43.1 | 36.2 |
| skewed, spread | 897.6 | 163.1 | 122.8 | 116.4 |

Random inserts are where the gapped array comes closest: 1.2–1.4x slower than
tlx's fastest setting at 1M keys, and on the 10M real datasets anywhere from
1.3x faster to 1.1x slower. Appends are its worst case: a B+-tree only touches
its last leaf, while the gapped array keeps shifting keys at its end and
refreshing its last segment. That matters for a database, since auto-increment
primary keys are common.

The spread order shows the cost described in "Supporting inserts":

| ns per insert (gapped array / tlx, 512-byte nodes) | laptop, keys from the data | laptop, spread evenly | AWS, keys from the data | AWS, spread evenly | slots re-laid per insert |
|---|---|---|---|---|---|
| uniform, 1M | 375 / 277 | 375 / 273 | 336 / 271 | 333 / 265 | 49 → 48 |
| skewed, 1M | 408 / 322 | 1,066 / 145 | 347 / 263 | 898 / 116 | 49 → 161 |
| books, 10M | 545 / 518 | 934 / 461 | 487 / 459 | 820 / 450 | 47 → 77 |
| fb, 10M | 480 / 519 | 1,318 / 201 | 360 / 463 | 1,238 / 151 | 3 → 129 |
| osm, 10M | 498 / 536 | 1,043 / 283 | 376 / 458 | 930 / 248 | 5 → 85 |
| wiki, 10M | 569 / 513 | 898 / 402 | 504 / 456 | 830 / 384 | 35 → 71 |

The slots re-laid don't depend on the machine. On uniform keys the two orders
draw from the same distribution, and the times match on both machines.
Everywhere else, spreading the inserts makes the gapped array 1.6–3.4x slower,
while tlx stays the same or gets up to 3.1x faster, presumably because the new
keys land in a few leaves that stay in cache.

### The whole database on each index

The benchmarks above time the index on its own. `cpp/engine_benchmark.cpp`
runs the whole database through SQL instead, on a table of
`(id INT, val INT)` rows, and is built twice: once on the learned index
and once on tlx's B+-tree, with everything else the same. The two builds
can't take turns inside one run, so each one ran three times,
alternating, and these are the medians. The log was on a tmpfs, so fsync
costs nothing except in the durable-insert rows.

| ns per operation (learned index / tlx), laptop | 1M uniform | 1M skewed | 10M uniform |
|---|---|---|---|
| the store's own lookup | 306 / 398 | 254 / 406 | 472 / 723 |
| point lookup, parsed ahead of time | 559 / 602 | 556 / 608 | 876 / 1,055 |
| point lookup, parsed each time | 1,366 / 1,391 | 1,320 / 1,401 | 1,585 / 1,639 |
| range scan of 100 rows | 10,405 / 10,475 | 10,323 / 10,115 | 10,617 / 10,471 |
| insert, random order | 2,581 / 2,484 | 2,591 / 2,512 | 2,911 / 2,917 |
| insert, appended | 3,187 / 1,709 | 3,192 / 1,701 | 5,652 / 1,685 |
| 95% reads, 5% inserts following the data | 651 / 761 | 650 / 760 | 965 / 1,397 |
| 50% reads, 50% inserts spread evenly | 1,592 / 1,236 | 2,041 / 1,055 | 1,443 / 1,797 |
| durable insert, fsync every write | 2,006,000 / 2,858,975 | 2,539,828 / 2,380,500 | 2,106,833 / 2,321,222 |
| durable insert, fsync every 64 writes | 33,368 / 47,661 | 35,297 / 44,260 | 34,603 / 42,342 |
| opening the database from a snapshot | 129 ms / 201 ms | 124 ms / 205 ms | 1,604 ms / 2,280 ms |

| ns per operation (learned index / tlx), AWS | 1M uniform | 1M skewed | 10M uniform |
|---|---|---|---|
| the store's own lookup | 277 / 327 | 279 / 314 | 398 / 571 |
| point lookup, parsed ahead of time | 546 / 589 | 549 / 584 | 748 / 859 |
| point lookup, parsed each time | 1,294 / 1,285 | 1,273 / 1,295 | 1,526 / 1,564 |
| range scan of 100 rows | 9,339 / 9,567 | 9,464 / 9,392 | 9,779 / 9,996 |
| insert, random order | 3,574 / 3,576 | 3,622 / 3,593 | 3,852 / 3,977 |
| insert, appended | 4,031 / 2,698 | 4,065 / 2,740 | 6,305 / 2,736 |
| 95% reads, 5% inserts following the data | 680 / 794 | 680 / 786 | 911 / 1,176 |
| 50% reads, 50% inserts spread evenly | 2,068 / 1,776 | 2,449 / 1,562 | 1,903 / 2,105 |
| durable insert, fsync every write | 2,696,860 / 2,693,157 | 2,687,446 / 2,689,301 | 2,694,454 / 2,692,404 |
| durable insert, fsync every 64 writes | 47,414 / 47,029 | 47,193 / 46,943 | 47,305 / 46,970 |
| opening the database from a snapshot | 109 ms / 189 ms | 109 ms / 189 ms | 1,465 ms / 2,122 ms |

Memory per row after opening from a snapshot is 89.2 bytes on the learned index
and 94.3 on tlx, everywhere, since it doesn't depend on the machine.

The store's own lookup, timed in a loop of its own, is 1.1–1.6x faster on the
learned index. Once the executor has planned the query and turned the stored
bytes back into a row, the difference is 1.06–1.20x, and once the statement has
to be parsed it's about even (0.99–1.06x). Part of that is simple arithmetic: a
parsed query takes about 1,300 ns at 1M keys, and the lookup is only about 280
of it. That's not the whole story, though. On AWS at 1M keys, where the runs
agree closely, the store's lookups differ by 50 and 35 ns and the pre-parsed
queries by 43 and 35 ns, so there the saving just carries through. But at 10M
keys it shrinks from 173 to 112 ns, and on the real datasets by more (130 to 18
ns on fb). Parsing shrinks it further, and sometimes reverses it, when a fixed
extra cost shouldn't change it at all. I haven't worked out why. Range scans are
even at every length, since decoding the rows costs far more than finding the
first one. Random inserts are even too, and appends are 1.5–3.4x slower on the
learned index, as in the storage-level benchmark.

The mixed workloads show both sides of the learned index. With inserts
that follow the existing data, a read-heavy mix is 1.16–1.45x faster,
probably because tlx's lookups slow down as its leaves split (the
storage-level benchmark shows that happening). With inserts spread
evenly over the key range, which for skewed data means landing mostly
where the table has few keys, the learned index is 1.6–1.9x slower on
skewed data and up to 3.3x slower on the real datasets (fb), for the
reason described in "Supporting inserts".

For durable writes, the index doesn't matter. On AWS, where nearly every run's
durable insert took 2.7 ms, the medians on the two indexes match to within 1% on
all seven datasets. Syncing every 64 writes brings that down to about 47 µs,
and the two still match to within about 1%. The laptop's SSD is much noisier
(separate runs ranged from 1.6 to 4.0 ms), so its differences in the table don't
mean much. Opening a database from a snapshot is 1.4–1.7x faster on the
learned index, probably because laying out a sorted array is cheaper than
building a tree (my B+-tree wrapper also makes a second pass to move the rows
in), and memory per row is about the same, since most of a row is the row
itself.

On the four real datasets (one run of each build, so noisier), a pre-parsed
point lookup ranged from even to 1.20x faster on the learned index, a parsed
one from 10% slower to 7% faster, appends were 2.3–3.3x slower, and inserts
spread evenly over the key range were up to 3.3x slower. The full tables, with
the lowest and highest of the three runs for the synthetic data, are in
`engine_summary.md` in each run's directory.

### Durability

A durable write in the storage-engine demo takes about 1.9 ms on my laptop,
against about 0.83 µs for the same insert with no log, and nearly all of that
is `fsync`. On AWS, with network storage, it's 2.7 ms against 0.79 µs. The ratio
(about 2,300x and 3,400x) says more about the disk than the database: on the
machine I started this project on, an fsync took about 113 µs, and the same code
came out at 36x.

### Concentrated inserts

If someone controls what gets inserted, the obvious attack is to put all
the inserts into a narrow range of keys. That uses up the gaps in one
area and forces much more rebalancing than the same number of spread-out
inserts.

Each scenario inserts 100,000 keys into the same 900,000-key array. The
test runs each one nine times, alternating between scenarios, and
compares each run to the normal spread-out case from the same round.

| scenario | refreshes | full rebuilds | ns per insert | slowdown |
|---|---|---|---|---|
| spread out (normal), laptop | 1,368 | 0 | 374.3 | — |
| 5.0% window, laptop | 1,336 | 0 | 593.4 | +57% |
| 2.0% window, laptop | 1,314 | 0 | 612.1 | +64% |
| 1.2% window, laptop | 1,265 | 0 | 642.5 | +71% |
| spread out (normal), AWS | 1,368 | 0 | 329.8 | — |
| 5.0% window, AWS | 1,336 | 0 | 535.6 | +62% |
| 2.0% window, AWS | 1,314 | 0 | 563.2 | +71% |
| 1.2% window, AWS | 1,265 | 0 | 593.7 | +80% |

The slowdown column is the median of the nine per-round comparisons, which is
why it doesn't quite match the ratio of the medians. Concentrating the inserts
makes them 57–80% slower, more so as the window narrows, but there are still
no full rebuilds: the crowded region borrows room from its neighbors
instead. Before the insert changes described in the bugs section, this
same test showed slowdowns of at most 12%. That wasn't resistance. Every
insert was paying for a full rebuild of the array every 512 inserts,
which swamped everything else. A concentrated insert now costs about
540–640 ns, against more than 20,000 ns for any insert back then.

This is the simplest version of the attack. One study that poisoned the
data ALEX and PGM-index were built on measured slowdowns of up to 20%
(Bachfischer et al., 2022). Attacks designed around ALEX's own insert
mechanism did far more: up to 1,641x slower, and an out-of-memory crash
after a few hundred insertions (Yang et al., VLDB 2024). I haven't tried
anything that targeted against this database.

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
| learned, 64x the memory (an earlier run) | 7,668,048 bytes | 50.225% |

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

Scaling to 1e15 instead fixed it, and I re-ran every benchmark (these
are the numbers from that rerun):

| | broken generator | fixed generator |
|---|---|---|
| segments (eps = 64) | 2–3 | 151 |
| segmented lookup | 26.9 ns | 117.7 ns |
| vs. my B+-tree | 12.5x faster | 2.7x faster |
| vs. PGM-index | 2x faster | about even |

Beating the B+-tree, the memory results, and the case for segments over
a single line all held up. The headline number didn't. I'd been checking
results the whole time but never actually looked at the input data, and
a quick look at the gaps between keys would have caught this right at
the start.

### Inserts were O(n) the whole time

For most of this project, the insert benchmark said an insert took about
20,000 ns, 12 times faster than inserting into a plain sorted vector.
A sorted vector shifts everything after the insertion point, though, so
beating it doesn't mean much. When I finally compared inserts against
tlx, the gapped array was 45 to 1,200 times slower, and the gap grew
with the size of the array.

Most of the cost was a safety net: a full rebuild of the whole array
every 512 inserts, however big the array was. One rebuild took 10.8 ms
at 1M keys, which works out to about 21 µs per insert, nearly all of
the insert time. At 10M keys it was about 262 µs per insert. The safety
net was there because of a gap in the per-segment counting. An insert
only counted against the segment the new key belonged to, but the shift
that makes room can push keys belonging to the next segment, and those
pushes were never counted anywhere.

Fixing it took four changes. Every insert now counts against each
segment whose keys it moved, which makes the 2 × eps bound described
above actually hold, so the safety net could go. Then two other things
turned out to force full rebuilds too: a crowded region had no way to
borrow room from its neighbors, and keys appended past the end filled the
last region every few hundred inserts. Those got the windowed refresh and
the growing last segment. And on perfectly regular keys, a single segment
could cover the whole array, so refreshing it was a full rebuild in
disguise; that's what the 4,096-key cap is for. After all four, inserts
that follow the data take 300–600 ns at 1M keys and 320–670 ns at 10M, with no full rebuilds
in any of the benchmarks.

The tests had a story of their own. I wrote a test that checks the 2 ×
eps bound, and then checked that it fails when the neighbor counting is
taken out. It didn't fail. Without the counting, the drift still mostly
stays under about 1.6 × eps, because the gap search takes whichever side
has the nearest empty slot, and once the gaps at the start of a
neighbor are used up, it stops pushing into it. Drift only runs away
when the neighbor has a long run of empty slots right at its edge, which
deleting keys creates. The test now deletes 300 keys next to a segment
boundary first, and without the counting the worst error reaches 2.47 ×
eps. So in practice the safety net was mostly covering for a problem
that rarely happened, and the counting is what turns "rarely" into a
guarantee.

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
predicting past the start of the next one. At that point the worst
case was still an O(n) walk if the model was very wrong, which is a lot
better than a wrong answer. Later I capped the walk and added the
galloping search described above, which brought the worst case down to
O(log n).

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
example. The idea isn't new (RAGS and SQLancer test real database
engines this way, on a much bigger scale), but it found two real bugs in
its first few runs.

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

The slow inserts point at a second pattern: a baseline that's too easy
to beat hides problems. Beating a sorted vector looked like a result,
and it covered up inserts that were getting slower as the array grew.

## Testing

- 52 storage engine tests, 20 parser tests, and 79 end-to-end SQL tests,
  all passing, and the same suites on the build that uses tlx's B+-tree
  (78 SQL tests there, since one checks the learned index's `EXPLAIN`
  output).
- A test that starts lookups from 200,000 random slots, including the
  worst possible ones, and checks that every one gives the same answer
  as a normal lookup and that even the worst start stays cheap.
- Randomized tests against reference data structures: the gapped array's
  contents against a `std::map`, insert and delete sequences with
  outliers against a `std::set`, and the optimal segmenter against a
  brute-force search over every possible line. These also check the
  gapped array's own bookkeeping after every batch of changes.
- Tests for the insert changes: counts of full rebuilds for random
  inserts and appends (both fail on the old code), the segment cap, the
  2 × eps drift bound (which fails without the neighbor counting), the
  segment search against `std::upper_bound`, and randomized restarts
  that check the log is applied correctly on top of a snapshot.
- Crash recovery tests that damage a real log file by truncating it,
  padding it with zeros, flipping a bit, or swapping in a file with the
  wrong format, and one where a child process writes without fsyncing
  and then dies, to check that batched fsync loses nothing when only the
  process crashes.
- Differential testing against SQLite, with automatic shrinking of
  failures. The latest pass was 200 runs of 300 statements and 50 runs
  of 1,000, including invalid statements, checkpoints and restarts, with
  no mismatches: with the normal error bound, with eps = 4, which makes
  the gapped array rebalance constantly even on small tables, and on the
  B+-tree build.
- AddressSanitizer and UBSan on every test suite. These caught two real
  memory bugs that normal runs didn't.
- GitHub Actions runs all of this on every push.

The benchmarks aren't part of CI, because timings on shared machines are
too noisy to mean much. `scripts/run_benchmarks.sh` reruns all of them
and saves the output and CSVs with a note of the machine, compiler and
commit they came from, in `results/runs/`. Everything here was measured on two
machines, so the absolute numbers are specific to them.

## Limits

Most of these are on purpose:

- No joins, multi-statement transactions, or concurrent access. Each of
  those would be a big project on its own.
- The whole store lives in memory, and opening it reads all of it. The
  log and the snapshot are the only things on disk. SQLite, by
  comparison, reads pages from disk only when a query needs them.
- Inserts in increasing order, and especially appends past the end, are
  much slower than a B+-tree's, and so are inserts that land where the
  data is sparse (see Benchmarks).
- The first column of a table is always the primary key. There's no
  `PRIMARY KEY` syntax.
- Keys are packed into 64 bits: 16 bits for the table id and 48 for the
  primary key, or 20 bits for the value and 28 for the primary key
  inside a secondary index. Values outside those ranges are rejected.
- A lookup is O(log n) in the worst case, plus any run of empty slots
  it has to skip. Deletes don't trigger any rebalancing, so the empty
  slots they leave stay until inserts next refresh that part of the
  array, or the whole array is rebuilt.
- A checkpoint rewrites the whole store, so the write that triggers one
  is slow. A production database would do that in the background.
- `EXPLAIN` only works with `SELECT`.
- Every benchmark ran on two machines, neither of them bare metal: my laptop
  under WSL2, and an AWS virtual machine with network storage. The real
  datasets are downsampled to 10M keys.

## References

- Kraska, Beutel, Chi, Dean, Polyzotis. "The Case for Learned Index
  Structures" (SIGMOD 2018).
- Galakatos, Markovitch, Binnig, Fonseca, Kraska. "FITing-Tree: A
  Data-aware Index Structure" (SIGMOD 2019).
- Ferragina, Vinciguerra. "The PGM-index" (VLDB 2020). Included under
  `cpp/third_party/pgm/`, Apache 2.0.
- Ding et al. "ALEX: An Updatable Adaptive Learned Index" (SIGMOD 2020).
- Marcus et al. "Benchmarking Learned Indexes" (PVLDB 2020), the SOSD
  benchmark, and its datasets.
- Bingmann. tlx, a collection of C++ data structures, including the
  B+-tree used as a baseline here. Included under
  `cpp/third_party/tlx/`, Boost Software License 1.0.
- O'Rourke. "An on-line algorithm for fitting straight lines between
  data ranges" (1981).
- Itai, Konheim, Rodeh. "A Sparse Table Implementation of Priority
  Queues" (ICALP 1981), and Bender, Demaine, Farach-Colton.
  "Cache-Oblivious B-Trees" (FOCS 2000), for packed-memory arrays.
- Fuad, Vashisth, Balmau, Kemme. "PostLearn: Towards A Learned Index For
  PostgreSQL" (CHEOPS 2026), a learned index inside PostgreSQL.
- Bachfischer, Borovica-Gajic, Rubinstein. "Testing the Robustness of
  Learned Index Structures" (2022).
- Yang, Kornaropoulos, Cheng. "Algorithmic Complexity Attacks on
  Dynamic Learned Indexes" (VLDB 2024).
- Slutz. "Massive Stochastic Testing of SQL" (VLDB 1998), and Rigger,
  Su. "Testing Database Engines via Pivoted Query Synthesis" (OSDI
  2020), on differential testing of databases.
- Jain, Lin. "Back to the Future: Leveraging Belady's Algorithm for
  Improved Cache Replacement" (ISCA 2016).
