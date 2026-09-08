// Phase 7b, part 1: benchmarks our own from-scratch segmented index
// against the real, published PGM-index implementation (vendored in
// cpp/third_party/pgm/) -- a fairer bar than only ever comparing
// against our own B-tree, since it answers "how does this compare to
// what people actually ship," not just "did I beat my own baseline."
#include <chrono>
#include <cstdio>
#include <fstream>
#include <random>
#include <vector>

#include "key_generators.h"
#include "segmented_model.h"
#include "third_party/pgm/pgm_index.hpp"

template <typename SearchFn>
double time_lookups(const std::vector<int64_t>& queries, SearchFn search_fn) {
  volatile int64_t sink = 0;
  auto t0 = std::chrono::steady_clock::now();
  for (int64_t q : queries) sink += search_fn(q);
  auto t1 = std::chrono::steady_clock::now();
  if (sink == 0xdeadbeef) std::printf("%lld", static_cast<long long>(sink));
  return std::chrono::duration<double, std::nano>(t1 - t0).count() / queries.size();
}

void run_comparison(const std::vector<int64_t>& keys, const std::string& name) {
  const size_t n = keys.size();
  const int num_queries = 100000;
  std::mt19937_64 rng(777);
  std::uniform_int_distribution<size_t> pick(0, n - 1);
  std::vector<int64_t> queries(num_queries);
  for (int i = 0; i < num_queries; i++) queries[i] = keys[pick(rng)];

  const int64_t eps = 64;

  // ours
  SegmentedModel ours = build_segmented_model(keys, eps);
  bool ours_correct = true;
  double ours_ns = time_lookups(queries, [&](int64_t q) -> int64_t {
    int64_t val;
    if (!segmented_search(keys, ours, q, val)) ours_correct = false;
    return val;
  });
  size_t ours_bytes = ours.segments.size() * sizeof(Segment);
  size_t ours_segments = ours.segments.size();

  // real PGM-index, same epsilon
  pgm::PGMIndex<int64_t, 64> pgm_index(keys);
  bool pgm_correct = true;
  double pgm_ns = time_lookups(queries, [&](int64_t q) -> int64_t {
    auto range = pgm_index.search(q);
    auto it = std::lower_bound(keys.begin() + range.lo, keys.begin() + range.hi, q);
    if (it == keys.begin() + range.hi || *it != q) pgm_correct = false;
    return it - keys.begin();
  });
  size_t pgm_bytes = pgm_index.size_in_bytes();
  size_t pgm_segments = pgm_index.segments_count();

  // also test with EpsilonRecursive=0 -- disables PGM's recursive
  // routing layer entirely, to directly confirm whether that layer's
  // overhead explains a surprising result rather than just guessing
  pgm::PGMIndex<int64_t, 64, 0> pgm_norecursive(keys);
  bool pgm_nr_correct = true;
  double pgm_nr_ns = time_lookups(queries, [&](int64_t q) -> int64_t {
    auto range = pgm_norecursive.search(q);
    auto it = std::lower_bound(keys.begin() + range.lo, keys.begin() + range.hi, q);
    if (it == keys.begin() + range.hi || *it != q) pgm_nr_correct = false;
    return it - keys.begin();
  });

  std::printf("== %s (%zu keys, eps=%lld) ==\n", name.c_str(), n, static_cast<long long>(eps));
  std::printf("  correctness  -- ours: %s, PGM-index: %s, PGM (no recursion): %s\n", ours_correct ? "pass" : "FAIL",
              pgm_correct ? "pass" : "FAIL", pgm_nr_correct ? "pass" : "FAIL");
  std::printf("  latency      -- ours: %8.1f ns/lookup   PGM-index: %8.1f ns/lookup   PGM (no recursion): %8.1f ns/lookup\n",
              ours_ns, pgm_ns, pgm_nr_ns);
  std::printf("  segments     -- ours: %8zu             PGM-index: %8zu\n", ours_segments, pgm_segments);
  std::printf("  memory       -- ours: %8zu bytes       PGM-index: %8zu bytes\n\n", ours_bytes, pgm_bytes);

  std::ofstream csv("results/phase7b_pgm_comparison.csv", std::ios::app);
  csv << name << "," << n << ",ours," << ours_ns << "," << ours_bytes << "," << ours_segments << "\n";
  csv << name << "," << n << ",pgm_index," << pgm_ns << "," << pgm_bytes << "," << pgm_segments << "\n";
  csv << name << "," << n << ",pgm_index_norecursive," << pgm_nr_ns << ",," << "\n";
}

int main() {
  std::ofstream csv("results/phase7b_pgm_comparison.csv");
  csv << "dataset,n,structure,ns_per_lookup,memory_bytes,segments\n";

  std::vector<int64_t> uniform = gen_uniform(1000000, 100);
  run_comparison(uniform, "uniform");

  std::vector<int64_t> skewed = gen_skewed(1000000, 200);
  run_comparison(skewed, "skewed");

  return 0;
}
