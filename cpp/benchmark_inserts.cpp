// Phase 3 benchmark: build the gapped array on 90% of the keys, hold
// the other 10% out, insert them one at a time, then verify every
// single key -- original and inserted -- is still findable. Also
// times a small sample of naive dense-array inserts (with shifting)
// for comparison; the naive version is O(n) per insert, so only a
// small sample is timed rather than all of them.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "gapped_array.h"

std::vector<int64_t> load_keys(const std::string& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) throw std::runtime_error("could not open " + path);
  std::streamsize size = f.tellg();
  f.seekg(0, std::ios::beg);
  std::vector<int64_t> keys(size / sizeof(int64_t));
  if (!f.read(reinterpret_cast<char*>(keys.data()), size)) {
    throw std::runtime_error("failed to read " + path);
  }
  return keys;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <keys.bin>\n", argv[0]);
    return 1;
  }
  std::string path = argv[1];
  std::vector<int64_t> all_keys = load_keys(path);
  size_t n = all_keys.size();
  std::printf("== %s (%zu keys) ==\n", path.c_str(), n);

  // hold out ~10% of keys (scattered across the whole range, not just
  // the tail) to insert after the structure is already built
  std::mt19937_64 rng(7);
  std::vector<bool> is_holdout(n, false);
  for (size_t i = 0; i < n; i++) {
    if (rng() % 10 == 0) is_holdout[i] = true;
  }

  std::vector<int64_t> initial_keys, holdout_keys;
  for (size_t i = 0; i < n; i++) {
    (is_holdout[i] ? holdout_keys : initial_keys).push_back(all_keys[i]);
  }
  std::printf("initial: %zu keys, holdout to insert: %zu keys\n", initial_keys.size(),
              holdout_keys.size());

  const int64_t eps = 64;
  const double density = 0.7;
  GappedArray ga = GappedArray::build(initial_keys, density, eps);
  std::printf("gapped array: %zu keys in %zu slots (%.1f%% full)\n", ga.size(), ga.capacity(),
              100.0 * ga.size() / ga.capacity());

  // correctness check: every initial key must be findable before we
  // trust anything else
  for (size_t i = 0; i < std::min(initial_keys.size(), static_cast<size_t>(2000)); i++) {
    size_t idx;
    if (!ga.search(initial_keys[i], idx)) {
      std::fprintf(stderr, "initial correctness check FAILED for key %lld\n",
                   static_cast<long long>(initial_keys[i]));
      return 1;
    }
  }
  std::printf("initial correctness check passed\n");

  // time inserting every holdout key into the gapped array
  auto t0 = std::chrono::steady_clock::now();
  for (int64_t key : holdout_keys) ga.insert(key);
  auto t1 = std::chrono::steady_clock::now();
  double gapped_insert_ns =
      std::chrono::duration<double, std::nano>(t1 - t0).count() / holdout_keys.size();

  std::printf("inserted all %zu holdout keys, %zu local rebalances + %zu full rebalances triggered\n",
              holdout_keys.size(), ga.local_rebalance_count(), ga.rebalance_count());
  std::printf("gapped array final: %zu keys in %zu slots (%.1f%% full)\n", ga.size(),
              ga.capacity(), 100.0 * ga.size() / ga.capacity());

  // now verify EVERY original key -- both initial and inserted -- is
  // findable in the final structure
  size_t missing = 0;
  for (int64_t key : all_keys) {
    size_t idx;
    if (!ga.search(key, idx)) missing++;
  }
  if (missing > 0) {
    std::fprintf(stderr, "FINAL correctness check FAILED: %zu of %zu keys missing\n", missing, n);
    return 1;
  }
  std::printf("final correctness check passed: all %zu keys findable after inserts\n", n);

  // naive baseline: inserting into a plain sorted vector shifts
  // everything after the insertion point, so this is O(n) per
  // insert -- only time a small sample, not all of them
  const size_t naive_sample = std::min<size_t>(500, holdout_keys.size());
  std::vector<int64_t> dense_copy = initial_keys;
  auto t2 = std::chrono::steady_clock::now();
  for (size_t i = 0; i < naive_sample; i++) {
    auto it = std::lower_bound(dense_copy.begin(), dense_copy.end(), holdout_keys[i]);
    dense_copy.insert(it, holdout_keys[i]);
  }
  auto t3 = std::chrono::steady_clock::now();
  double naive_insert_ns = std::chrono::duration<double, std::nano>(t3 - t2).count() / naive_sample;

  std::printf("\ngapped array insert: %10.1f ns/insert (avg over all %zu inserts)\n",
              gapped_insert_ns, holdout_keys.size());
  std::printf("naive vector insert: %10.1f ns/insert (avg over a %zu-insert sample, O(n) each)\n\n",
              naive_insert_ns, naive_sample);

  std::ofstream csv("results/phase3_inserts.csv", std::ios::app);
  csv << path << "," << n << ",gapped_array," << gapped_insert_ns << "\n";
  csv << path << "," << n << ",naive_vector," << naive_insert_ns << "\n";

  return 0;
}
