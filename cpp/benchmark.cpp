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
#include "csv_out.h"
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

volatile int64_t g_sink = 0;  // keeps the optimizer from deleting the loops

// Nanoseconds per query for one pass of `lookup` over every query.
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

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <keys.bin> [num_queries] [passes]\n", argv[0]);
    return 1;
  }
  std::string path = argv[1];
  int num_queries = argc > 2 ? std::atoi(argv[2]) : 100000;
  const int passes = argc > 3 ? std::max(1, std::atoi(argv[3])) : 7;

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

  // Each pass times every structure once, in turn, and the reported
  // number is the median over passes. This used to time each structure
  // once, one after another, which lets the order skew the results: a
  // single run measured the skewed B+-tree at 869.6 ns, against 317.0 ns
  // in another program minutes later.
  auto bs = [&](int64_t q) -> int64_t { return std::lower_bound(keys.begin(), keys.end(), q) - keys.begin(); };
  auto bt = [&](int64_t q) -> int64_t {
    int64_t val = 0;
    tree.search(q, val);
    return val;
  };
  auto li = [&](int64_t q) -> int64_t {
    int64_t val = 0;
    learned_search(keys, model, q, val);
    return val;
  };
  auto seg = [&](int64_t q) -> int64_t {
    int64_t val = 0;
    segmented_search(keys, seg_model, q, val);
    return val;
  };
  std::vector<double> times[4];
  for (int p = 0; p < passes; p++) {
    times[0].push_back(time_pass(queries, bs));
    times[1].push_back(time_pass(queries, bt));
    times[2].push_back(time_pass(queries, li));
    times[3].push_back(time_pass(queries, seg));
  }
  const double bs_ns = median(times[0]), bt_ns = median(times[1]), li_ns = median(times[2]),
               seg_ns = median(times[3]);

  std::printf("binary search:    %8.1f ns/lookup\n", bs_ns);
  std::printf("B+-tree:          %8.1f ns/lookup\n", bt_ns);
  std::printf("learned index:    %8.1f ns/lookup\n", li_ns);
  std::printf("segmented index:  %8.1f ns/lookup\n", seg_ns);
  std::printf("(median of %d interleaved passes)\n\n", passes);

  std::ofstream csv = open_csv("phase0_baseline.csv", "path,n,num_queries,structure,ns_per_lookup");
  csv << csv_field(path) << "," << n << "," << num_queries << ",binary_search," << bs_ns << "\n";
  csv << csv_field(path) << "," << n << "," << num_queries << ",bplus_tree," << bt_ns << "\n";
  csv << csv_field(path) << "," << n << "," << num_queries << ",learned_index," << li_ns << "\n";
  csv << csv_field(path) << "," << n << "," << num_queries << ",segmented_index," << seg_ns << "\n";

  return 0;
}
