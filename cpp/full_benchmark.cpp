// Phase 7, part 1: sweeps dataset size and distribution, measuring
// both lookup latency (as in every earlier phase) and index-only
// memory footprint -- the original paper's *other* headline claim,
// and the one most reimplementations skip. "Index-only" deliberately
// excludes the raw sorted array every approach needs regardless; the
// point is measuring what each *index structure* costs on top of that
// shared data, not the data itself.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "bplus_tree.h"
#include "key_generators.h"
#include "linear_model.h"
#include "segmented_model.h"

struct Result {
  std::string dataset;
  size_t n;
  std::string structure;
  double ns_per_lookup;
  size_t index_bytes;
};

void bench_one(const std::vector<int64_t>& keys, const std::string& dataset_name,
               std::vector<Result>& results) {
  const size_t n = keys.size();
  const int num_queries = 100000;
  std::mt19937_64 rng(2024);
  std::uniform_int_distribution<size_t> pick(0, n - 1);
  std::vector<int64_t> queries(num_queries);
  for (int i = 0; i < num_queries; i++) queries[i] = keys[pick(rng)];

  volatile int64_t sink = 0;

  // binary search: no separate index structure at all -- zero bytes
  // on top of the shared array
  {
    auto t0 = std::chrono::steady_clock::now();
    for (int64_t q : queries) {
      auto it = std::lower_bound(keys.begin(), keys.end(), q);
      sink += (it - keys.begin());
    }
    auto t1 = std::chrono::steady_clock::now();
    double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / num_queries;
    results.push_back({dataset_name, n, "binary_search", ns, 0});
  }

  // B+-tree
  {
    BPlusTree tree;
    for (size_t i = 0; i < n; i++) tree.insert(keys[i], static_cast<int64_t>(i));
    auto t0 = std::chrono::steady_clock::now();
    for (int64_t q : queries) {
      int64_t val;
      tree.search(q, val);
      sink += val;
    }
    auto t1 = std::chrono::steady_clock::now();
    double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / num_queries;
    results.push_back({dataset_name, n, "bplus_tree", ns, tree.memory_bytes()});
  }

  // Phase 1: single learned index
  {
    LinearModel model = fit_linear_model(keys);
    auto t0 = std::chrono::steady_clock::now();
    for (int64_t q : queries) {
      int64_t val;
      learned_search(keys, model, q, val);
      sink += val;
    }
    auto t1 = std::chrono::steady_clock::now();
    double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / num_queries;
    results.push_back({dataset_name, n, "learned_index", ns, sizeof(LinearModel)});
  }

  // Phase 2: segmented learned index
  {
    const int64_t eps = 64;
    SegmentedModel model = build_segmented_model(keys, eps);
    auto t0 = std::chrono::steady_clock::now();
    for (int64_t q : queries) {
      int64_t val;
      segmented_search(keys, model, q, val);
      sink += val;
    }
    auto t1 = std::chrono::steady_clock::now();
    double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / num_queries;
    size_t bytes = model.segments.size() * sizeof(Segment) + sizeof(SegmentedModel);
    results.push_back({dataset_name, n, "segmented_index", ns, bytes});
  }

  // the same lookup, with optimal segmentation instead of pinned anchors
  {
    const int64_t eps = 64;
    SegmentedModel model = build_optimal_segmented_model(keys, eps);
    auto t0 = std::chrono::steady_clock::now();
    for (int64_t q : queries) {
      int64_t val;
      segmented_search(keys, model, q, val);
      sink += val;
    }
    auto t1 = std::chrono::steady_clock::now();
    double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / num_queries;
    size_t bytes = model.segments.size() * sizeof(Segment) + sizeof(SegmentedModel);
    results.push_back({dataset_name, n, "segmented_optimal", ns, bytes});
  }

  if (sink == 123456789) std::printf("%lld", static_cast<long long>(sink));  // keep sink genuinely used
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
      std::printf("  %-9s %-15s %10.1f ns/lookup   index: %8zu bytes (%.3f bytes/key)\n",
                  r.dataset.c_str(), r.structure.c_str(), r.ns_per_lookup, r.index_bytes,
                  static_cast<double>(r.index_bytes) / n);
    }
    std::printf("\n");
  }

  std::ofstream csv("results/phase7_sweep.csv");
  csv << "dataset,n,structure,ns_per_lookup,index_bytes,bytes_per_key\n";
  for (const auto& r : results) {
    csv << r.dataset << "," << r.n << "," << r.structure << "," << r.ns_per_lookup << "," << r.index_bytes
        << "," << (static_cast<double>(r.index_bytes) / r.n) << "\n";
  }
  std::printf("wrote results/phase7_sweep.csv\n");

  return 0;
}
