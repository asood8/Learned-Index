// Phase 0 benchmark: loads a dataset, builds a sorted-vector binary
// search baseline and a real B+-tree baseline, verifies both are
// correct, then times real lookups against each. These numbers are
// the score every later phase has to beat.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "bplus_tree.h"
#include "linear_model.h"
#include "segmented_model.h"

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
    std::fprintf(stderr, "usage: %s <keys.bin> [num_queries]\n", argv[0]);
    return 1;
  }
  std::string path = argv[1];
  int num_queries = argc > 2 ? std::atoi(argv[2]) : 100000;

  std::vector<int64_t> keys = load_keys(path);
  size_t n = keys.size();
  std::printf("== %s (%zu keys) ==\n", path.c_str(), n);

  // baseline 1: sorted vector + binary search. keys.bin is already
  // sorted ascending by construction in data_gen.py.

  // baseline 2: real B+-tree, built by inserting every key in order
  BPlusTree tree;
  for (size_t i = 0; i < n; i++) tree.insert(keys[i], static_cast<int64_t>(i));

  // Phase 1: fit the linear model over the same sorted keys
  LinearModel model = fit_linear_model(keys);
  std::printf("learned model: position ~= %.8f * key + %.4f  (max_error = %lld)\n",
              model.slope, model.intercept, static_cast<long long>(model.max_error));

  // Phase 2: build the segmented model with a chosen error bound
  const int64_t eps = 64;
  SegmentedModel seg_model = build_segmented_model(keys, eps);
  std::printf("segmented model: %zu segments (eps = %lld) vs 1 segment for Phase 1\n",
              seg_model.segments.size(), static_cast<long long>(eps));

  // correctness check before trusting any timing number -- now checks
  // all structures against the same sample
  size_t check_n = std::min(n, static_cast<size_t>(2000));
  for (size_t i = 0; i < check_n; i++) {
    int64_t val;
    bool found = tree.search(keys[i], val);
    if (!found || val != static_cast<int64_t>(i)) {
      std::fprintf(stderr, "B+-tree correctness check FAILED at i=%zu\n", i);
      return 1;
    }
    bool li_found = learned_search(keys, model, keys[i], val);
    if (!li_found || val != static_cast<int64_t>(i)) {
      std::fprintf(stderr, "learned index correctness check FAILED at i=%zu\n", i);
      return 1;
    }
    bool seg_found = segmented_search(keys, seg_model, keys[i], val);
    if (!seg_found || val != static_cast<int64_t>(i)) {
      std::fprintf(stderr, "segmented index correctness check FAILED at i=%zu\n", i);
      return 1;
    }
  }
  std::printf("correctness check passed (%zu keys), tree holds %zu entries\n",
              check_n, tree.size());

  // random query keys, sampled from real existing keys (lookup-hit benchmark)
  std::mt19937_64 rng(42);
  std::uniform_int_distribution<size_t> dist(0, n - 1);
  std::vector<int64_t> queries(num_queries);
  for (int i = 0; i < num_queries; i++) queries[i] = keys[dist(rng)];

  volatile int64_t sink = 0;  // keeps the optimizer from deleting the loops

  auto t0 = std::chrono::steady_clock::now();
  for (int64_t q : queries) {
    auto it = std::lower_bound(keys.begin(), keys.end(), q);
    sink += (it - keys.begin());
  }
  auto t1 = std::chrono::steady_clock::now();
  double bs_ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / num_queries;

  auto t2 = std::chrono::steady_clock::now();
  for (int64_t q : queries) {
    int64_t val;
    tree.search(q, val);
    sink += val;
  }
  auto t3 = std::chrono::steady_clock::now();
  double bt_ns = std::chrono::duration<double, std::nano>(t3 - t2).count() / num_queries;

  auto t4 = std::chrono::steady_clock::now();
  for (int64_t q : queries) {
    int64_t val;
    learned_search(keys, model, q, val);
    sink += val;
  }
  auto t5 = std::chrono::steady_clock::now();
  double li_ns = std::chrono::duration<double, std::nano>(t5 - t4).count() / num_queries;

  auto t6 = std::chrono::steady_clock::now();
  for (int64_t q : queries) {
    int64_t val;
    segmented_search(keys, seg_model, q, val);
    sink += val;
  }
  auto t7 = std::chrono::steady_clock::now();
  double seg_ns = std::chrono::duration<double, std::nano>(t7 - t6).count() / num_queries;

  std::printf("binary search:    %8.1f ns/lookup\n", bs_ns);
  std::printf("B+-tree:          %8.1f ns/lookup\n", bt_ns);
  std::printf("learned index:    %8.1f ns/lookup\n", li_ns);
  std::printf("segmented index:  %8.1f ns/lookup\n", seg_ns);
  std::printf("(sink=%lld, ignore)\n\n", static_cast<long long>(sink));

  std::ofstream csv("results/phase0_baseline.csv", std::ios::app);
  csv << path << "," << n << "," << num_queries << ",binary_search," << bs_ns << "\n";
  csv << path << "," << n << "," << num_queries << ",bplus_tree," << bt_ns << "\n";
  csv << path << "," << n << "," << num_queries << ",learned_index," << li_ns << "\n";
  csv << path << "," << n << "," << num_queries << ",segmented_index," << seg_ns << "\n";

  return 0;
}
