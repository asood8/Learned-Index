#!/usr/bin/env python3
"""
Phase 0: dataset generator for the learned index project.

Generates sorted, unique int64 key sets in two distributions:
  - uniform:  keys spread evenly across the key space (the "easy" case)
  - skewed:   keys drawn from a lognormal distribution, so they cluster
              heavily in some ranges and thin out in others (the case
              that breaks a single straight-line model)

Each dataset is written as a raw binary file of int64 values (sorted
ascending) so the C++ side can read it with one fread-style call and
no parsing overhead.
"""
import argparse
import os

import numpy as np


def gen_uniform(n: int, seed: int = 0) -> np.ndarray:
    rng = np.random.default_rng(seed)
    # sample without replacement from a much larger space so keys are
    # unique but still roughly evenly spread out
    keys = rng.choice(n * 10, size=n, replace=False)
    keys.sort()
    return keys.astype(np.int64)


def gen_skewed(n: int, seed: int = 0) -> np.ndarray:
    rng = np.random.default_rng(seed)
    # Sort n lognormal draws, then scale them into integer key space.
    #
    # This used to scale so the largest key landed at n * 10, like the
    # uniform set. That was a bug. With sigma = 2 the largest draw is
    # thousands of times bigger than a typical one, so almost every
    # draw floored into the same few hundred integers, and the
    # collision nudge below turned them into one long run of
    # consecutive integers (999,819 of the 1M keys). A straight line
    # fits that exactly, which made the learned index look far better
    # on "skewed" data than it really is. Scaling to a max of 1e15
    # leaves room for nearly every draw to get its own integer, so the
    # keys keep their lognormal shape. 1e15 is still well inside the
    # range where doubles represent integers exactly.
    raw = np.sort(rng.lognormal(mean=0.0, sigma=2.0, size=n))
    keys = np.floor(raw * (1e15 / raw.max())).astype(np.int64)
    # any leftover ties get nudged up by 1 to keep keys strictly
    # increasing; at this scale that almost never happens
    for i in range(1, n):
        if keys[i] <= keys[i - 1]:
            keys[i] = keys[i - 1] + 1
    return keys


def write_bin(path: str, keys: np.ndarray) -> None:
    keys.tofile(path)
    print(f"wrote {len(keys):,} keys -> {path} ({os.path.getsize(path):,} bytes)")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=1_000_000)
    ap.add_argument("--outdir", default="data")
    args = ap.parse_args()

    os.makedirs(args.outdir, exist_ok=True)
    write_bin(os.path.join(args.outdir, f"uniform_{args.n}.bin"), gen_uniform(args.n))
    write_bin(os.path.join(args.outdir, f"skewed_{args.n}.bin"), gen_skewed(args.n))
