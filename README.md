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

## Next: Phase 4

Full benchmark suite — sweep dataset sizes/distributions, measure
memory footprint, and compare against the real published PGM-index.
