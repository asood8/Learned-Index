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
against real implementations and not only against my own baselines.

"Embedded" means the same thing it does for SQLite: a single process, a
write-ahead log on disk, and a SQL layer on top of the storage engine.
It doesn't support joins, multi-statement transactions, or concurrent
access. It does support `CREATE TABLE`, `INSERT`, `SELECT` (with
`WHERE`, `ORDER BY`, `LIMIT`, `COUNT` and `SUM`), `UPDATE`, `DELETE`,
secondary indexes, `EXPLAIN`, crash recovery, checkpointing, and a REPL.

## Results

All of these come from one run of `scripts/run_benchmarks.sh` on my
laptop (an Intel i7-13620H, under WSL2) on October 24, 2026. The raw
output, along with the machine, compiler and commit it came from, is in
`results/runs/2026-10-24_2324/`. Lookup times are medians over
interleaved passes. On this machine the same benchmark can move 10–20%
from one run to the next, so the ratios mean more than the exact
nanoseconds. Everything is on 1M keys unless it says otherwise.

| | result |
|---|---|
| Point lookup in the database | 124.5 ns on uniform keys, 139.3 ns on skewed. tlx's B+-tree: 199.2 and 205.1 ns. Binary search: 176.0 and 172.9 ns |
| The same at 10M keys | 305.6 and 392.8 ns. tlx: 395.6 and 445.7 ns |
| The read-only index (no room for inserts) | 93.7 ns uniform, 103.1 ns skewed |
| Memory | the model takes 0.0065–0.0074 bytes per key, against 0.26–0.53 for tlx's inner nodes. The whole gapped array takes 11.4 bytes per key, against 8.6–9.3 for tlx |
| Inserts in random order | 395 ns uniform, 520 ns skewed. tlx: 423 and 451 ns |
| Inserts in increasing order | about 300 ns, roughly 2x slower than tlx. Appends past the end: 475–614 ns, 12–17x slower than tlx |
| vs. the published PGM-index | even on uniform data (97.8 vs 97.5 ns), faster on skewed (100.5 vs 121.3 ns) |
| Secondary index lookup | 17.9x faster than the full scan it replaces, on a 1,000-row table |
| Checkpointing | opening a store went from 7.60 ms to 0.48 ms |
| Learned Bloom filter | lost badly: 50.4% false positives vs 1.021% for a normal one |

A few notes on these.

The comparison that matters is the one against tlx. I wrote my own
B+-tree as the first baseline, and the database beats it by 2.1–2.8x,
but mine allocates every node separately and was never tuned, so that
mostly says my B+-tree is slow. tlx is a maintained library (it
replaced the stx B+-tree that the SOSD learned-index benchmark uses). I
tried it with node sizes from 128 bytes to 2 KB, and against its fastest
setting, 512-byte nodes, the database's lookups are 1.5–1.6x faster at
1M keys and 1.1–1.3x faster at 10M. That's the same direction the
learned-index papers report, and a smaller gap than the numbers usually
quoted.

The memory result is real, but smaller than it sounds. The model is
35–80 times smaller than tlx's inner nodes, and over 200 times smaller
than my B+-tree's. But the gapped array keeps about 30% of its slots
empty for inserts, and those empty slots cost more than the model saves,
so in total it takes more memory than a packed B+-tree. An earlier
version of this README compared the model with my B+-tree's whole size,
keys and values included, and called that thousands of times less
memory. That wasn't a fair comparison.

Inserts are the weak spot. Random-order inserts cost about the same as
tlx's, within about 15% either way. Keys that arrive in increasing order
are about 2x slower, and appends past the end (an auto-increment id) are
12–17x slower, because a B+-tree appends by touching only its rightmost
leaf. Until recently inserts were far worse than this, which is one of
the bug stories below.

Lookups inside the database are about a third slower than on the bare
read-only index (124.5 vs 94.7 ns on uniform data, measured side by
side). The database has to leave empty slots in the array for inserts,
and its model drifts a little between refits. The database number is
the one to quote.

## Building and running

Each program is a single `.cpp` file in `cpp/`, compiled with g++
(C++17). The Makefile covers the common tasks and puts the binaries in
`build/`.

```bash
make test        # the three test suites
make asan        # the same suites under AddressSanitizer and UBSan
make difftest    # differential test against SQLite (EPS=4 stresses rebalancing)
make demos       # durability and catalog restart demos
make repl        # build and start the SQL shell
make data        # generate the benchmark datasets (needs numpy)
make all         # build everything, including benchmarks

scripts/run_benchmarks.sh   # rerun every benchmark in this README
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
array. In practice that's 204.8 ns per lookup, which is slower than
plain binary search at 183.9 ns.

So the model is split into pieces. `build_segmented_model` goes through
the sorted keys once, extending the current segment as long as one line
can keep every key within eps slots of its real position (eps is the
allowed error, 64 by default). When no line fits anymore, it closes that
segment and starts a new one. Because each segment's line has to start
at the segment's first point, each new key just narrows down which
slopes are still allowed, so building the whole model takes one pass
over the data. This is the same greedy method as FITing-tree's
ShrinkingCone algorithm (Galakatos et al., 2019). On 1M keys it gives 84
segments for uniform data and 151 for skewed, with lookups at 93.7 and
103.1 ns.

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
rebuilds. The bugs section explains why this part
was rewritten.

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
includes, and applying those again doesn't change anything.

On startup, the snapshot is loaded, the log records written since are
applied to it (for each key, the last record logged for it wins), and
the array is built once from the result. It used to replay the log one
insert at a time, which took 59.8 s for a log of 1M writes. For a store
of 2,000 rows that had each been written ten times, so 20,000 log
records, opening it took 7.60 ms from the log and 0.48 ms after a
checkpoint.

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
the same gapped array. On a 1,000-row table this was 17.9x faster than
the full scan it replaced (6,572 vs 117,746 ns). That number moves a lot
between runs (I've seen 18x to 29x), since each query only takes a few
microseconds, and on a table this small any index would win.

Before anything gets written, values are checked against their columns.
Types have to match, primary keys have to fit in 48 bits (28 once the
table has an index), and indexed values have to fit in 20 bits. Anything
out of range is rejected instead of being silently turned into a
different key.

## Benchmarks

### Against a B+-tree and binary search

These time the read-only index on a plain sorted array, against binary
search and the B+-tree I wrote as the first baseline (order 64, with
node splitting and linked leaves). The comparison with tlx is in "Inside
the database" below.

| 1M keys | binary search | my B+-tree | one line | segmented |
|---|---|---|---|---|
| uniform | 204.9 ns | 331.6 ns | 127.3 ns | 93.7 ns |
| skewed | 183.9 ns | 302.7 ns | 204.8 ns | 103.1 ns |

Besides the segmented column, two things in this table stand out. First,
plain binary search beats my B+-tree. My B+-tree allocates each node
separately, so going down three or four levels means three or four
jumps to different places in memory, each one probably a cache miss.
Binary search stays inside one block of memory. Second, the single-line
model wins on uniform data and loses badly on skewed data, which is the
whole reason for splitting it into segments.

The results hold up as the data grows. From 100K to 5M keys, the
segmented index stays ahead of both baselines, and the gap with the
B+-tree gets wider: at 5M skewed keys it's 166.1 ns, against 784.0 for
the B+-tree and 390.7 for binary search.

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
segments.

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

| 1M keys | mine (pinned) | mine (optimal) | PGM-index |
|---|---|---|---|
| uniform | 97.8 ns, 79 segments, 1,896 bytes | 91.4 ns, 57 segments, 1,368 bytes | 97.5 ns, 57 segments, 984 bytes |
| skewed | 100.5 ns, 157 segments, 3,768 bytes | 101.1 ns, 126 segments, 3,024 bytes | 121.3 ns, 126 segments, 2,192 bytes |

They're even on uniform data, and mine is faster on skewed data. That's
recent: it changed with the branchless segment search described above.
Before that, PGM was clearly faster on uniform data (89.5 vs 124.0 ns,
in an earlier session). I left PGM's code as it is. At equal segment
counts, the memory difference comes down to how segments are stored:
PGM uses 16 bytes per segment and I use 24, because it stores the slope
as a 32-bit float and I use a 64-bit double. Turning off PGM's recursive
routing layer (a template parameter) slows its uniform lookups from 97.5
to 108.9 ns, so that layer helps.

An earlier version of this README said my index was 2x faster than PGM
on skewed data. That came from a broken dataset generator, which I cover
in the bugs section.

### Inside the database

Everything above times the index on its own. The database uses the
gapped array with inserts, so it has to deal with empty slots and a
model that drifts. `cpp/benchmark_db_lookups.cpp` measures that path
against the same baselines plus tlx, before and after inserting 100k
keys.

| 1M keys | uniform, fresh | uniform, after inserts | skewed, fresh | skewed, after inserts |
|---|---|---|---|---|
| binary search | 176.0 ns | 183.2 ns | 172.9 ns | 183.9 ns |
| my B+-tree | 349.4 ns | 316.0 ns | 296.1 ns | 304.3 ns |
| tlx, 256-byte nodes | 243.5 ns | 327.6 ns | 217.5 ns | 325.5 ns |
| tlx, 512-byte nodes | 199.2 ns | 258.5 ns | 205.1 ns | 237.6 ns |
| read-only segmented model | 94.7 ns | 105.6 ns | 103.9 ns | 104.1 ns |
| gapped array (the database) | 124.5 ns | 137.3 ns | 139.3 ns | 130.4 ns |

| 10M keys | uniform, fresh | uniform, after inserts | skewed, fresh | skewed, after inserts |
|---|---|---|---|---|
| binary search | 466.4 ns | 483.9 ns | 684.1 ns | 602.4 ns |
| my B+-tree | 832.6 ns | 637.0 ns | 875.7 ns | 766.6 ns |
| tlx, 256-byte nodes | 458.1 ns | 625.4 ns | 579.8 ns | 659.1 ns |
| tlx, 512-byte nodes | 395.6 ns | 486.0 ns | 445.7 ns | 510.9 ns |
| read-only segmented model | 194.7 ns | 215.5 ns | 226.6 ns | 230.9 ns |
| gapped array (the database) | 305.6 ns | 323.3 ns | 392.8 ns | 341.9 ns |

So a real lookup in the database is 1.5–1.6x faster than tlx at its
best at 1M keys, and 1.1–1.3x faster at 10M, where nothing fits in the
processor's cache anymore. After inserts the gap widens, because tlx's
full leaves split and end up half empty, while the gapped array had room
set aside. Lookups for keys that don't exist cost about the same as ones
that find something (129.0 vs 124.5 ns on uniform data), since the walk
does the same amount of work either way.

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

### Inserts

`cpp/benchmark_inserts.cpp` builds each structure on 90% of the keys and
inserts the other 10%, in three orders: the held-out keys scattered
across the whole range, smallest first ("increasing"); the same keys
shuffled ("random"); and the largest 10% in order, the way
auto-increment ids arrive ("appended").

| 1M keys, ns per insert | gapped array | my B+-tree | tlx, 256-byte nodes | tlx, 512-byte nodes |
|---|---|---|---|---|
| uniform, increasing | 303.1 | 130.9 | 146.4 | 136.3 |
| uniform, random | 395.2 | 443.6 | 609.8 | 422.8 |
| uniform, appended | 614.4 | 78.0 | 46.6 | 37.2 |
| skewed, increasing | 298.1 | 121.8 | 143.5 | 159.4 |
| skewed, random | 520.0 | 662.7 | 739.5 | 450.5 |
| skewed, appended | 475.2 | 81.2 | 45.6 | 38.4 |

Random inserts are where the gapped array holds its own. Appends are its
worst case: a B+-tree only touches its last leaf, while the gapped array
keeps shifting keys at its end and refreshing its last segment. That
matters for a database, since auto-increment primary keys are common.

### Durability

A durable write here takes about 1.48 ms, against about 0.89 µs for the
same insert with no log, and nearly all of that is `fsync`. That's why
real databases group several writes into one sync. The ratio (about
1,660x) says more about the disk than the database: on the machine I
started this project on, an fsync took about 113 µs, and the same code
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
| spread out (normal) | 1,368 | 0 | 391.2 | — |
| 5.0% window | 1,336 | 0 | 618.6 | +59% |
| 2.0% window | 1,314 | 0 | 697.5 | +73% |
| 1.2% window | 1,265 | 0 | 692.8 | +77% |

Concentrating the inserts makes them 60–80% slower, but there are still
no full rebuilds: the crowded region borrows room from its neighbors
instead. Before the insert changes described in the bugs section, this
same test showed slowdowns of at most 12%. That wasn't resistance. Every
insert was paying for a full rebuild of the array every 512 inserts,
which swamped everything else. A concentrated insert now costs about
650 ns, against more than 20,000 ns for any insert back then.

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
take 300–600 ns at 1M keys and 320–670 ns at 10M, with no full rebuilds
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

- 50 storage engine tests, 20 parser tests, and 79 end-to-end SQL tests,
  all passing.
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
  wrong format.
- Differential testing against SQLite, with automatic shrinking of
  failures. The latest pass was 200 runs of 300 statements and 50 runs
  of 1,000, including invalid statements, checkpoints and restarts, with
  no mismatches, both with the normal error bound and with eps = 4, which
  makes the gapped array rebalance constantly even on small tables.
- AddressSanitizer and UBSan on every test suite. These caught two real
  memory bugs that normal runs didn't.
- GitHub Actions runs all of this on every push.

The benchmarks aren't part of CI, because timings on shared machines are
too noisy to mean much. `scripts/run_benchmarks.sh` reruns all of them
and saves the output with a note of the machine, compiler and commit it
came from, in `results/runs/`. Everything here was measured on one
laptop, so the absolute numbers are specific to it.

## Limits

Most of these are on purpose:

- No joins, multi-statement transactions, or concurrent access. Each of
  those would be a big project on its own.
- The whole store lives in memory, and opening it reads all of it. The
  log and the snapshot are the only things on disk. SQLite, by
  comparison, reads pages from disk only when a query needs them.
- Inserts in increasing order, and especially appends past the end, are
  much slower than a B+-tree's (see Benchmarks).
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
- Every benchmark ran on one laptop, on synthetic data.

## References

- Kraska, Beutel, Chi, Dean, Polyzotis. "The Case for Learned Index
  Structures" (SIGMOD 2018).
- Galakatos, Markovitch, Binnig, Fonseca, Kraska. "FITing-Tree: A
  Data-aware Index Structure" (SIGMOD 2019).
- Ferragina, Vinciguerra. "The PGM-index" (VLDB 2020). Included under
  `cpp/third_party/pgm/`, Apache 2.0.
- Ding et al. "ALEX: An Updatable Adaptive Learned Index" (SIGMOD 2020).
- Marcus et al. "Benchmarking Learned Indexes" (PVLDB 2020), the SOSD
  benchmark.
- Bingmann. tlx, a collection of C++ data structures, including the
  B+-tree used as a baseline here. Included under
  `cpp/third_party/tlx/`, Boost Software License 1.0.
- O'Rourke. "An on-line algorithm for fitting straight lines between
  data ranges" (1981).
- Itai, Konheim, Rodeh. "A Sparse Table Implementation of Priority
  Queues" (ICALP 1981), and Bender, Demaine, Farach-Colton.
  "Cache-Oblivious B-Trees" (FOCS 2000), for packed-memory arrays.
- Bachfischer, Borovica-Gajic, Rubinstein. "Testing the Robustness of
  Learned Index Structures" (2022).
- Yang, Kornaropoulos, Cheng. "Algorithmic Complexity Attacks on
  Dynamic Learned Indexes" (VLDB 2024).
- Slutz. "Massive Stochastic Testing of SQL" (VLDB 1998), and Rigger,
  Su. "Testing Database Engines via Pivoted Query Synthesis" (OSDI
  2020), on differential testing of databases.
- Jain, Lin. "Back to the Future: Leveraging Belady's Algorithm for
  Improved Cache Replacement" (ISCA 2016).
