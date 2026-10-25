// Phase 7b, part 1: benchmarks our own from-scratch segmented index
// against the real, published PGM-index implementation (vendored in
// cpp/third_party/pgm/) -- a fairer bar than only ever comparing
// against our own B-tree, since it answers "how does this compare to
// what people actually ship," not just "did I beat my own baseline."
#include <algorithm>
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

double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  const size_t n = v.size();
  return (n % 2 == 1) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

void run_comparison(const std::vector<int64_t>& keys, const std::string& name) {
  const size_t n = keys.size();
  const int num_queries = 100000;
  std::mt19937_64 rng(777);
  std::uniform_int_distribution<size_t> pick(0, n - 1);
  std::vector<int64_t> queries(num_queries);
  for (int i = 0; i < num_queries; i++) queries[i] = keys[pick(rng)];

  const int64_t eps = 64;
  const int passes = 7;

  SegmentedModel ours = build_segmented_model(keys, eps);
  SegmentedModel optimal = build_optimal_segmented_model(keys, eps);  // ours, without pinned anchors
  pgm::PGMIndex<int64_t, 64> pgm_index(keys);
  // EpsilonRecursive = 0 turns off PGM's recursive routing layer, to
  // check whether that layer explains the difference rather than guess
  pgm::PGMIndex<int64_t, 64, 0> pgm_norecursive(keys);

  // each returns the key's position, or -1 if it wasn't found
  auto ours_fn = [&](int64_t q) -> int64_t {
    int64_t val;
    return segmented_search(keys, ours, q, val) ? val : -1;
  };
  auto optimal_fn = [&](int64_t q) -> int64_t {
    int64_t val;
    return segmented_search(keys, optimal, q, val) ? val : -1;
  };
  auto pgm_fn = [&](int64_t q) -> int64_t {
    auto range = pgm_index.search(q);
    auto it = std::lower_bound(keys.begin() + range.lo, keys.begin() + range.hi, q);
    return (it == keys.begin() + range.hi || *it != q) ? -1 : it - keys.begin();
  };
  auto pgm_nr_fn = [&](int64_t q) -> int64_t {
    auto range = pgm_norecursive.search(q);
    auto it = std::lower_bound(keys.begin() + range.lo, keys.begin() + range.hi, q);
    return (it == keys.begin() + range.hi || *it != q) ? -1 : it - keys.begin();
  };
  auto finds_all = [&](auto fn) {
    for (int64_t q : queries) {
      if (fn(q) < 0) return false;
    }
    return true;
  };
  const bool ours_correct = finds_all(ours_fn), optimal_correct = finds_all(optimal_fn),
             pgm_correct = finds_all(pgm_fn), pgm_nr_correct = finds_all(pgm_nr_fn);

  // Each pass times all four in turn; the numbers are medians over
  // passes. This used to time each one once, one after another, so the
  // order they ran in could move the results.
  std::vector<double> t[4];
  for (int p = 0; p < passes; p++) {
    t[0].push_back(time_lookups(queries, ours_fn));
    t[1].push_back(time_lookups(queries, optimal_fn));
    t[2].push_back(time_lookups(queries, pgm_fn));
    t[3].push_back(time_lookups(queries, pgm_nr_fn));
  }
  const double ours_ns = median(t[0]), optimal_ns = median(t[1]), pgm_ns = median(t[2]), pgm_nr_ns = median(t[3]);
  const size_t ours_bytes = ours.segments.size() * sizeof(Segment), ours_segments = ours.segments.size();
  const size_t optimal_bytes = optimal.segments.size() * sizeof(Segment), optimal_segments = optimal.segments.size();
  const size_t pgm_bytes = pgm_index.size_in_bytes(), pgm_segments = pgm_index.segments_count();

  std::printf("== %s (%zu keys, eps=%lld) ==\n", name.c_str(), n, static_cast<long long>(eps));
  std::printf("  correctness  -- ours (pinned): %s   ours (optimal): %s   PGM-index: %s   PGM (no recursion): %s\n",
              ours_correct ? "pass" : "FAIL", optimal_correct ? "pass" : "FAIL", pgm_correct ? "pass" : "FAIL",
              pgm_nr_correct ? "pass" : "FAIL");
  std::printf("  latency      -- ours (pinned): %7.1f   ours (optimal): %7.1f   PGM-index: %7.1f   PGM (no recursion): %7.1f  ns/lookup\n",
              ours_ns, optimal_ns, pgm_ns, pgm_nr_ns);
  std::printf("  segments     -- ours (pinned): %7zu   ours (optimal): %7zu   PGM-index: %7zu\n", ours_segments,
              optimal_segments, pgm_segments);
  std::printf("  memory       -- ours (pinned): %7zu   ours (optimal): %7zu   PGM-index: %7zu  bytes\n\n", ours_bytes,
              optimal_bytes, pgm_bytes);

  std::ofstream csv("results/phase7b_pgm_comparison.csv", std::ios::app);
  csv << name << "," << n << ",ours," << ours_ns << "," << ours_bytes << "," << ours_segments << "\n";
  csv << name << "," << n << ",ours_optimal," << optimal_ns << "," << optimal_bytes << "," << optimal_segments << "\n";
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
