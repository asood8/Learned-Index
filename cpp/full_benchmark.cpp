// Phase 7, part 1: sweeps dataset size and distribution, measuring
// both lookup latency (as in every earlier phase) and memory -- the
// original paper's *other* headline claim. Memory is reported two ways.
// "Index" is the part that does the routing: a model's segments, or a
// B+-tree's inner nodes. "Total" adds the keys themselves (8 bytes each
// in a sorted array; in the B+-tree's case its leaves, which also hold an
// 8-byte value per key). An earlier version used the B+-tree's whole
// size as its "index" figure, leaves included, while leaving the sorted
// array out of the models' figures, which made the gap look about 20x
// bigger than it is.
//
// Each pass times every structure once, in turn, and the reported
// number is the median over passes. This also used to time each
// structure once, one after another, so the order they ran in could move
// the results (at 5M keys, one such run had the pinned and optimal
// models at 331.2 and 189.9 ns).
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "bplus_tree.h"
#include "csv_out.h"
#include "key_generators.h"
#include "linear_model.h"
#include "segmented_model.h"

struct Result {
  std::string dataset;
  size_t n;
  std::string structure;
  double ns_per_lookup;
  size_t index_bytes;
  size_t total_bytes;
};

volatile int64_t g_sink = 0;  // keeps the optimizer from deleting the loops

template <typename Lookup>
double time_pass(const std::vector<int64_t>& queries, Lookup lookup) {
  int64_t sink = 0;
  const auto t0 = std::chrono::steady_clock::now();
  for (int64_t q : queries) sink += lookup(q);
  const auto t1 = std::chrono::steady_clock::now();
  g_sink = sink;
  return std::chrono::duration<double, std::nano>(t1 - t0).count() / queries.size();
}

double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  const size_t n = v.size();
  return (n % 2 == 1) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

void bench_one(const std::vector<int64_t>& keys, const std::string& dataset_name,
               std::vector<Result>& results) {
  const size_t n = keys.size();
  const int num_queries = 100000;
  const int passes = 7;
  const int64_t eps = 64;
  std::mt19937_64 rng(2024);
  std::uniform_int_distribution<size_t> pick(0, n - 1);
  std::vector<int64_t> queries(num_queries);
  for (int i = 0; i < num_queries; i++) queries[i] = keys[pick(rng)];

  BPlusTree tree;
  for (size_t i = 0; i < n; i++) tree.insert(keys[i], static_cast<int64_t>(i));
  const LinearModel line = fit_linear_model(keys);                      // Phase 1: one line
  const SegmentedModel pinned = build_segmented_model(keys, eps);        // Phase 2
  const SegmentedModel optimal = build_optimal_segmented_model(keys, eps);

  // binary search has no structure of its own on top of the sorted array
  auto binary_search = [&](int64_t q) -> int64_t {
    return std::lower_bound(keys.begin(), keys.end(), q) - keys.begin();
  };
  auto bplus = [&](int64_t q) -> int64_t {
    int64_t val = 0;
    tree.search(q, val);
    return val;
  };
  auto one_line = [&](int64_t q) -> int64_t {
    int64_t val = 0;
    learned_search(keys, line, q, val);
    return val;
  };
  auto segmented = [&](int64_t q) -> int64_t {
    int64_t val = 0;
    segmented_search(keys, pinned, q, val);
    return val;
  };
  auto segmented_optimal = [&](int64_t q) -> int64_t {
    int64_t val = 0;
    segmented_search(keys, optimal, q, val);
    return val;
  };
  std::vector<double> t[5];
  for (int p = 0; p < passes; p++) {
    t[0].push_back(time_pass(queries, binary_search));
    t[1].push_back(time_pass(queries, bplus));
    t[2].push_back(time_pass(queries, one_line));
    t[3].push_back(time_pass(queries, segmented));
    t[4].push_back(time_pass(queries, segmented_optimal));
  }

  const size_t array_bytes = n * sizeof(int64_t);
  const size_t pinned_bytes = pinned.segments.size() * sizeof(Segment) + sizeof(SegmentedModel);
  const size_t optimal_bytes = optimal.segments.size() * sizeof(Segment) + sizeof(SegmentedModel);
  results.push_back({dataset_name, n, "binary_search", median(t[0]), 0, array_bytes});
  results.push_back({dataset_name, n, "bplus_tree", median(t[1]), tree.inner_memory_bytes(), tree.memory_bytes()});
  results.push_back(
      {dataset_name, n, "learned_index", median(t[2]), sizeof(LinearModel), array_bytes + sizeof(LinearModel)});
  results.push_back({dataset_name, n, "segmented_index", median(t[3]), pinned_bytes, array_bytes + pinned_bytes});
  results.push_back(
      {dataset_name, n, "segmented_optimal", median(t[4]), optimal_bytes, array_bytes + optimal_bytes});
}

int main() {
  std::vector<size_t> sizes = {100000, 1000000, 5000000};
  std::vector<Result> results;

  for (size_t n : sizes) {
    std::printf("=== n = %zu ===\n", n);
    std::vector<int64_t> uniform = gen_uniform(n, 100);
    bench_one(uniform, "uniform", results);
    std::vector<int64_t> skewed = gen_skewed(n, 200);
    bench_one(skewed, "skewed", results);

    for (const auto& r : results) {
      if (r.n != n) continue;
      std::printf("  %-9s %-17s %10.1f ns/lookup   index: %9zu bytes (%.4f bytes/key)   total: %.2f bytes/key\n",
                  r.dataset.c_str(), r.structure.c_str(), r.ns_per_lookup, r.index_bytes,
                  static_cast<double>(r.index_bytes) / n, static_cast<double>(r.total_bytes) / n);
    }
    std::printf("\n");
  }

  const std::string csv_path = results_path("phase7_sweep.csv");
  std::ofstream csv(csv_path);
  csv << "dataset,n,structure,ns_per_lookup,index_bytes,index_bytes_per_key,total_bytes_per_key\n";
  for (const auto& r : results) {
    csv << r.dataset << "," << r.n << "," << r.structure << "," << r.ns_per_lookup << "," << r.index_bytes << ","
        << (static_cast<double>(r.index_bytes) / r.n) << "," << (static_cast<double>(r.total_bytes) / r.n) << "\n";
  }
  std::printf("medians of 7 interleaved passes; wrote %s\n", csv_path.c_str());

  return 0;
}
