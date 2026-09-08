// Shared dataset generators, matching python/data_gen.py's approach
// (including the nudge-on-collision technique for skewed data), so
// every C++ benchmark in this project uses the exact same generation
// logic instead of subtly different copies.
#pragma once

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

std::vector<int64_t> gen_uniform(size_t n, uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::vector<int64_t> pool(n * 10);
  for (size_t i = 0; i < pool.size(); i++) pool[i] = static_cast<int64_t>(i);
  std::shuffle(pool.begin(), pool.end(), rng);
  std::vector<int64_t> keys(pool.begin(), pool.begin() + n);
  std::sort(keys.begin(), keys.end());
  return keys;
}

std::vector<int64_t> gen_skewed(size_t n, uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::lognormal_distribution<double> dist(0.0, 2.0);
  std::vector<double> raw(n);
  for (size_t i = 0; i < n; i++) raw[i] = dist(rng);
  std::sort(raw.begin(), raw.end());
  const double max_raw = raw.back();

  std::vector<int64_t> keys(n);
  int64_t last = -1;
  for (size_t i = 0; i < n; i++) {
    int64_t v = static_cast<int64_t>(raw[i] * (static_cast<double>(n) * 10.0 / max_raw));
    if (v <= last) v = last + 1;
    keys[i] = v;
    last = v;
  }
  return keys;
}
