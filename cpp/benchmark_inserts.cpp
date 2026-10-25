// Phase 3 benchmark: build on 90% of the keys, insert the other 10% one
// at a time, then check that every key -- original and inserted -- is
// still findable. The gapped array is timed next to my B+-tree and tlx's
// (tlx_baseline.h), in four insert orders:
//
//   sorted    the held-out keys, scattered across the whole range, in
//             increasing order (this benchmark's only order until v4)
//   shuffled  the same keys in random order
//   append    the largest 10% of the keys, in increasing order, the way
//             an auto-increment id arrives
//   spread    as many new keys, drawn evenly from the whole key range
//             instead of from the data, in random order. On uniform data
//             that's no different from shuffled; on skewed data most of
//             them land where the array has few keys
//
// For the gapped array it also reports how much work its refreshes did:
// slots re-laid per insert, and refreshes that had to borrow room from
// neighboring segments.
//
// Until v4 this only compared the gapped array with inserting into a
// plain sorted vector, which shifts everything after the insertion point.
// That baseline is O(n) per insert, so beating it says very little, and
// it hid that the gapped array's inserts had become O(n) as well (a full
// rebuild every 512 inserts). The vector is still timed on a small
// sample, for context.
//
// usage: benchmark_inserts <keys.bin>
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
#include "gapped_array.h"
#include "tlx_baseline.h"

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

template <typename F>
double ns_per(size_t count, F f) {
  const auto t0 = std::chrono::steady_clock::now();
  f();
  const auto t1 = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::nano>(t1 - t0).count() / count;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <keys.bin>\n", argv[0]);
    return 1;
  }
  const std::string path = argv[1];
  const std::vector<int64_t> all_keys = load_keys(path);
  const size_t n = all_keys.size();
  const int64_t eps = 64;
  const double density = 0.7;
  std::printf("== %s (%zu keys) ==\n", path.c_str(), n);
  std::printf("%-9s %14s %12s %12s %12s   %s\n", "order", "gapped array", "B+-tree", "tlx 256 B", "tlx 512 B",
              "gapped array refreshes");

  // results/phase3_inserts.csv has the pre-v4 runs, without an order
  // column; these go in their own file
  const bool new_file = !std::ifstream("results/inserts.csv").good();
  std::ofstream csv("results/inserts.csv", std::ios::app);
  if (new_file) csv << "path,n,structure,order,ns_per_insert\n";
  const char* orders[4] = {"sorted", "shuffled", "append", "spread"};
  for (int order = 0; order < 4; order++) {
    // ~10% held out, scattered across the whole range (the same split as
    // benchmark_db_lookups), or for appends the largest 10%
    std::vector<int64_t> initial, holdout;
    if (order == 2) {
      initial.assign(all_keys.begin(), all_keys.begin() + n * 9 / 10);
      holdout.assign(all_keys.begin() + n * 9 / 10, all_keys.end());
    } else {
      std::mt19937_64 rng(7);
      for (int64_t k : all_keys) (rng() % 10 == 0 ? holdout : initial).push_back(k);
      if (order == 1) std::shuffle(holdout.begin(), holdout.end(), std::mt19937_64(99));
    }
    if (order == 3) {
      const size_t count = holdout.size();
      std::mt19937_64 rng(123);
      std::uniform_int_distribution<int64_t> anywhere(all_keys.front(), all_keys.back() - 1);
      std::vector<int64_t> picked;
      while (picked.size() < count) {
        const int64_t k = anywhere(rng);
        if (!std::binary_search(initial.begin(), initial.end(), k)) picked.push_back(k);
      }
      std::sort(picked.begin(), picked.end());
      picked.erase(std::unique(picked.begin(), picked.end()), picked.end());
      holdout = picked;
      std::shuffle(holdout.begin(), holdout.end(), std::mt19937_64(99));
    }

    GappedArray ga = GappedArray::build(initial, density, eps);
    BPlusTree tree;
    for (size_t i = 0; i < initial.size(); i++) tree.insert(initial[i], static_cast<int64_t>(i));
    TlxDefault tlx_default;
    TlxWide tlx_wide;
    tlx_default.bulk_load(initial.begin(), initial.end());
    tlx_wide.bulk_load(initial.begin(), initial.end());

    const double ga_ns = ns_per(holdout.size(), [&] {
      for (int64_t k : holdout) ga.insert(k);
    });
    const double tree_ns = ns_per(holdout.size(), [&] {
      for (int64_t k : holdout) tree.insert(k, 1);
    });
    const double tlx_default_ns = ns_per(holdout.size(), [&] {
      for (int64_t k : holdout) tlx_default.insert(k);
    });
    const double tlx_wide_ns = ns_per(holdout.size(), [&] {
      for (int64_t k : holdout) tlx_wide.insert(k);
    });

    // every key, original and inserted, has to be findable in all four
    size_t missing = 0;
    for (const std::vector<int64_t>* keys : {&initial, &holdout}) {
      for (int64_t k : *keys) {
        size_t idx;
        int64_t v;
        if (!ga.search(k, idx) || !tree.search(k, v) || tlx_default.find(k) == tlx_default.end() ||
            tlx_wide.find(k) == tlx_wide.end()) {
          missing++;
        }
      }
    }
    if (missing > 0 || !ga.consistent() || ga.size() != initial.size() + holdout.size()) {
      std::fprintf(stderr, "correctness check FAILED after %s inserts: %zu keys missing\n", orders[order], missing);
      return 1;
    }

    std::printf("%-9s %14.1f %12.1f %12.1f %12.1f   %zu (%zu widened), %zu full rebuilds; %.0f slots re-laid per "
                "insert; %.1f%% full afterwards\n",
                orders[order], ga_ns, tree_ns, tlx_default_ns, tlx_wide_ns, ga.local_rebalance_count(),
                ga.widened_refresh_count(), ga.rebalance_count(),
                static_cast<double>(ga.relaid_slot_count()) / holdout.size(), 100.0 * ga.size() / ga.capacity());
    csv << path << "," << n << ",gapped_array," << orders[order] << "," << ga_ns << "\n";
    csv << path << "," << n << ",bplus_tree," << orders[order] << "," << tree_ns << "\n";
    csv << path << "," << n << ",tlx_256," << orders[order] << "," << tlx_default_ns << "\n";
    csv << path << "," << n << ",tlx_512," << orders[order] << "," << tlx_wide_ns << "\n";
  }

  // the old baseline: a plain sorted vector, which shifts everything
  // after the insertion point, so only a small sample is timed
  std::mt19937_64 rng(7);
  std::vector<int64_t> dense, sample;
  for (int64_t k : all_keys) (rng() % 10 == 0 ? sample : dense).push_back(k);
  sample.resize(std::min<size_t>(500, sample.size()));
  const double vector_ns = ns_per(sample.size(), [&] {
    for (int64_t k : sample) dense.insert(std::lower_bound(dense.begin(), dense.end(), k), k);
  });
  std::printf("\nplain sorted vector, for context: %.1f ns/insert (a %zu-insert sample, O(n) each)\n", vector_ns,
              sample.size());
  csv << path << "," << n << ",naive_vector,sorted," << vector_ns << "\n";
  return 0;
}
