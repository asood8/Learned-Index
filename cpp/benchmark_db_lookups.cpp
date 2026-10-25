// Point-lookup speed of the structure the database actually uses.
//
// The main lookup benchmarks time the Phase 2 segmented model over a
// dense, read-only sorted array. The database doesn't use that.
// DurableStore sits on the Phase 3 gapped array, which has gaps to skip,
// walks from each prediction with locate(), and has a model that drifts
// between refits as inserts land. This times that structure directly,
// both freshly built and after 10% more keys have been inserted, next to
// the same baselines.
//
// The baselines include tlx's B+-tree (see tlx_baseline.h), not just the
// one in bplus_tree.h, at two node sizes.
//
// Every structure's answers are checked before anything is timed. Each
// pass then times every structure once, in turn, and the reported number
// is the median over passes. An earlier version timed the structures one
// after another instead, and that made results depend on when each one
// ran: the same array measured 126 ns early in a run and 142 ns later in
// the same run, a bigger swing than some of the differences being
// measured.
//
// It also prints memory per key, two ways: the whole structure, and just
// the part that does the routing (a B+-tree's inner nodes, a learned
// index's segments). Only the second is comparable to a model's size on
// its own; the first is what the structure really costs.
//
// usage: benchmark_db_lookups <keys.bin> [passes]
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "bplus_tree.h"
#include "gapped_array.h"
#include "segmented_model.h"
#include "tlx_baseline.h"

std::vector<int64_t> load_keys(const std::string& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) throw std::runtime_error("could not open " + path);
  const std::streamsize size = f.tellg();
  f.seekg(0, std::ios::beg);
  std::vector<int64_t> keys(size / sizeof(int64_t));
  if (!f.read(reinterpret_cast<char*>(keys.data()), size)) throw std::runtime_error("failed to read " + path);
  return keys;
}

std::vector<int64_t> sample(const std::vector<int64_t>& from, size_t count, uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<size_t> pick(0, from.size() - 1);
  std::vector<int64_t> out(count);
  for (auto& q : out) q = from[pick(rng)];
  return out;
}

volatile int64_t g_sink = 0;  // written after every timed loop so the loop can't be optimized away

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

// Checks that `lookup` finds every query (or, for misses, none of
// them) before its timing is trusted.
template <typename Lookup>
bool answers_ok(const std::vector<int64_t>& queries, bool expect_found, Lookup lookup) {
  for (int64_t q : queries) {
    if ((lookup(q) >= 0) != expect_found) return false;
  }
  return true;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <keys.bin> [passes]\n", argv[0]);
    return 1;
  }
  const std::string path = argv[1];
  const int passes = (argc > 2) ? std::max(1, std::atoi(argv[2])) : 5;
  const int64_t eps = 64;
  const double density = 0.7;  // same settings the database uses
  const size_t query_count = 200000;

  const std::vector<int64_t> all_keys = load_keys(path);

  // Same 90/10 split as benchmark_inserts, so the held-out keys double
  // as guaranteed misses before they're inserted.
  std::mt19937_64 split_rng(7);
  std::vector<int64_t> initial, holdout;
  for (int64_t k : all_keys) (split_rng() % 10 == 0 ? holdout : initial).push_back(k);

  GappedArray ga = GappedArray::build(initial, density, eps);
  BPlusTree tree;
  for (size_t i = 0; i < initial.size(); i++) tree.insert(initial[i], static_cast<int64_t>(i));
  TlxDefault tlx_default;
  TlxWide tlx_wide;
  tlx_default.bulk_load(initial.begin(), initial.end());
  tlx_wide.bulk_load(initial.begin(), initial.end());
  const SegmentedModel dense_model = build_segmented_model(initial, eps);

  // Each lookup returns a position/value on a hit and -1 on a miss.
  auto binary_search_in = [](const std::vector<int64_t>& keys) {
    return [&keys](int64_t q) -> int64_t {
      const auto it = std::lower_bound(keys.begin(), keys.end(), q);
      return (it != keys.end() && *it == q) ? static_cast<int64_t>(it - keys.begin()) : -1;
    };
  };
  auto btree_lookup = [&tree](int64_t q) -> int64_t {
    int64_t v;
    return tree.search(q, v) ? v : -1;
  };
  auto tlx_default_lookup = [&tlx_default](int64_t q) -> int64_t {
    return tlx_default.find(q) != tlx_default.end() ? q : -1;  // keys are non-negative
  };
  auto tlx_wide_lookup = [&tlx_wide](int64_t q) -> int64_t { return tlx_wide.find(q) != tlx_wide.end() ? q : -1; };
  auto dense_lookup_in = [](const std::vector<int64_t>& keys, const SegmentedModel& model) {
    return [&keys, &model](int64_t q) -> int64_t {
      int64_t v;
      return segmented_search(keys, model, q, v) ? v : -1;
    };
  };
  auto gapped_lookup = [&ga](int64_t q) -> int64_t {
    size_t idx;
    return ga.search(q, idx) ? static_cast<int64_t>(idx) : -1;
  };

  const std::vector<int64_t> hits = sample(initial, query_count, 42);
  const std::vector<int64_t> misses = sample(holdout, query_count, 43);
  const auto bs_fresh = binary_search_in(initial);
  const auto dense_fresh = dense_lookup_in(initial, dense_model);

  auto both_ok = [&](auto lookup) { return answers_ok(hits, true, lookup) && answers_ok(misses, false, lookup); };
  if (!(both_ok(bs_fresh) && both_ok(btree_lookup) && both_ok(tlx_default_lookup) && both_ok(tlx_wide_lookup) &&
        both_ok(dense_fresh) && both_ok(gapped_lookup))) {
    std::fprintf(stderr, "correctness check FAILED on the freshly built structures\n");
    return 1;
  }

  // t[structure][0] = hits, t[structure][1] = misses, one entry per pass
  constexpr int kStructures = 6;
  std::vector<double> t[kStructures][2];
  for (int p = 0; p < passes; p++) {
    for (int m = 0; m < 2; m++) {
      const std::vector<int64_t>& qs = (m == 0) ? hits : misses;
      t[0][m].push_back(time_pass(qs, bs_fresh));
      t[1][m].push_back(time_pass(qs, btree_lookup));
      t[2][m].push_back(time_pass(qs, tlx_default_lookup));
      t[3][m].push_back(time_pass(qs, tlx_wide_lookup));
      t[4][m].push_back(time_pass(qs, dense_fresh));
      t[5][m].push_back(time_pass(qs, gapped_lookup));
    }
  }

  const char* names[kStructures] = {"binary search",
                                    "B+-tree (bplus_tree.h)",
                                    "tlx B+-tree (256-byte nodes)",
                                    "tlx B+-tree (512-byte nodes)",
                                    "Phase 2 model, dense array",
                                    "gapped array (what the db uses)"};
  std::ofstream csv("results/db_lookups.csv", std::ios::app);
  std::printf("== %s: %zu keys built, %zu held out ==\n", path.c_str(), initial.size(), holdout.size());
  std::printf("%zu random hits and %zu misses, median of %d interleaved passes, ns/lookup\n\n", query_count,
              query_count, passes);
  std::printf("%-34s %10s %10s\n", "freshly built", "hits", "misses");
  for (int s = 0; s < kStructures; s++) {
    const double hit_ns = median(t[s][0]), miss_ns = median(t[s][1]);
    std::printf("%-34s %10.1f %10.1f\n", names[s], hit_ns, miss_ns);
    csv << path << ",fresh," << names[s] << "," << hit_ns << "," << miss_ns << "\n";
  }

  // Memory per key, freshly built.
  const double n = static_cast<double>(initial.size());
  const auto [tlx_default_total, tlx_default_inner] = tlx_bytes<TlxDefaultNodes>(tlx_default);
  const auto [tlx_wide_total, tlx_wide_inner] = tlx_bytes<TlxWideNodes>(tlx_wide);
  const double dense_model_bytes = static_cast<double>(dense_model.segments.size() * sizeof(Segment));
  const double gapped_model_bytes = static_cast<double>(ga.segment_count() * sizeof(Segment));
  struct MemoryRow {
    const char* name;
    double total, routing;
    const char* note;
  };
  const MemoryRow memory[kStructures] = {
      {names[0], 8.0, 0.0, "just the keys"},
      {names[1], tree.memory_bytes() / n, tree.inner_memory_bytes() / n, "also stores an 8-byte value per key"},
      {names[2], tlx_default_total / n, tlx_default_inner / n, "leaves full (bulk-loaded)"},
      {names[3], tlx_wide_total / n, tlx_wide_inner / n, "leaves full (bulk-loaded)"},
      {names[4], 8.0 + dense_model_bytes / n, dense_model_bytes / n, "read-only; no room for inserts"},
      {names[5], (ga.capacity() * sizeof(int64_t) + gapped_model_bytes) / n, gapped_model_bytes / n,
       "keys + empty slots + segments"},
  };
  std::ofstream mem_csv("results/db_memory.csv", std::ios::app);
  std::printf("\n%-34s %12s %14s\n", "memory, bytes per key", "whole thing", "routing only");
  for (const MemoryRow& row : memory) {
    std::printf("%-34s %12.2f %14.4f   %s\n", row.name, row.total, row.routing, row.note);
    mem_csv << path << "," << row.name << "," << row.total << "," << row.routing << "\n";
  }

  // Insert the held-out keys. The gapped array absorbs them with local
  // rebalances; the dense structures have to be rebuilt from scratch,
  // which is the cost the gapped array exists to avoid.
  for (size_t i = 0; i < holdout.size(); i++) {
    ga.insert(holdout[i]);
    tree.insert(holdout[i], static_cast<int64_t>(initial.size() + i));  // any non-negative value; -1 means "missing"
    tlx_default.insert(holdout[i]);
    tlx_wide.insert(holdout[i]);
  }
  const SegmentedModel rebuilt_model = build_segmented_model(all_keys, eps);
  const std::vector<int64_t> hits_after = sample(all_keys, query_count, 44);
  const auto bs_after = binary_search_in(all_keys);
  const auto dense_after = dense_lookup_in(all_keys, rebuilt_model);

  const bool after_ok = answers_ok(hits_after, true, bs_after) && answers_ok(hits_after, true, btree_lookup) &&
                        answers_ok(hits_after, true, tlx_default_lookup) &&
                        answers_ok(hits_after, true, tlx_wide_lookup) && answers_ok(hits_after, true, dense_after) &&
                        answers_ok(hits_after, true, gapped_lookup) && ga.consistent();
  if (!after_ok) {
    std::fprintf(stderr, "correctness check FAILED after inserts\n");
    return 1;
  }

  std::vector<double> u[kStructures];
  for (int p = 0; p < passes; p++) {
    u[0].push_back(time_pass(hits_after, bs_after));
    u[1].push_back(time_pass(hits_after, btree_lookup));
    u[2].push_back(time_pass(hits_after, tlx_default_lookup));
    u[3].push_back(time_pass(hits_after, tlx_wide_lookup));
    u[4].push_back(time_pass(hits_after, dense_after));
    u[5].push_back(time_pass(hits_after, gapped_lookup));
  }
  const char* after_names[kStructures] = {"binary search (rebuilt array)",
                                          names[1],
                                          names[2],
                                          names[3],
                                          "Phase 2 model (rebuilt)",
                                          names[5]};
  std::printf("\nafter inserting the %zu held-out keys (%zu local + %zu full rebalances)\n", holdout.size(),
              ga.local_rebalance_count(), ga.rebalance_count());
  for (int s = 0; s < kStructures; s++) {
    const double hit_ns = median(u[s]);
    std::printf("%-34s %10.1f\n", after_names[s], hit_ns);
    csv << path << ",after_inserts," << after_names[s] << "," << hit_ns << ",-1\n";
  }
  return 0;
}
