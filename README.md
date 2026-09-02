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

## Next: Phase 3

Handle inserts without shifting the whole array — a gapped array,
leaving small unused slots scattered through the sorted data on
purpose.
