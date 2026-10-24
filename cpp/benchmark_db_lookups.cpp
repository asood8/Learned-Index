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
// Every structure's answers are checked before anything is timed. Each
// pass then times every structure once, in turn, and the reported number
// is the median over passes. An earlier version timed the structures one
// after another instead, and that made results depend on when each one
// ran: the same array measured 126 ns early in a run and 142 ns later in
// the same run, a bigger swing than some of the differences being
// measured.
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

  const bool fresh_ok = answers_ok(hits, true, bs_fresh) && answers_ok(misses, false, bs_fresh) &&
                        answers_ok(hits, true, btree_lookup) && answers_ok(misses, false, btree_lookup) &&
                        answers_ok(hits, true, dense_fresh) && answers_ok(misses, false, dense_fresh) &&
                        answers_ok(hits, true, gapped_lookup) && answers_ok(misses, false, gapped_lookup);
  if (!fresh_ok) {
    std::fprintf(stderr, "correctness check FAILED on the freshly built structures\n");
    return 1;
  }

  // t[structure][0] = hits, t[structure][1] = misses, one entry per pass
  std::vector<double> t[4][2];
  for (int p = 0; p < passes; p++) {
    t[0][0].push_back(time_pass(hits, bs_fresh));
    t[1][0].push_back(time_pass(hits, btree_lookup));
    t[2][0].push_back(time_pass(hits, dense_fresh));
    t[3][0].push_back(time_pass(hits, gapped_lookup));
    t[0][1].push_back(time_pass(misses, bs_fresh));
    t[1][1].push_back(time_pass(misses, btree_lookup));
    t[2][1].push_back(time_pass(misses, dense_fresh));
    t[3][1].push_back(time_pass(misses, gapped_lookup));
  }

  const char* names[4] = {"binary search", "B+-tree", "Phase 2 model, dense array", "gapped array (what the db uses)"};
  std::ofstream csv("results/db_lookups.csv", std::ios::app);
  std::printf("== %s: %zu keys built, %zu held out ==\n", path.c_str(), initial.size(), holdout.size());
  std::printf("%zu random hits and %zu misses, median of %d interleaved passes, ns/lookup\n\n", query_count,
              query_count, passes);
  std::printf("%-34s %10s %10s\n", "freshly built", "hits", "misses");
  for (int s = 0; s < 4; s++) {
    const double hit_ns = median(t[s][0]), miss_ns = median(t[s][1]);
    std::printf("%-34s %10.1f %10.1f\n", names[s], hit_ns, miss_ns);
    csv << path << ",fresh," << names[s] << "," << hit_ns << "," << miss_ns << "\n";
  }

  // Insert the held-out keys. The gapped array absorbs them with local
  // rebalances; the dense structures have to be rebuilt from scratch,
  // which is the cost the gapped array exists to avoid.
  for (size_t i = 0; i < holdout.size(); i++) {
    ga.insert(holdout[i]);
    tree.insert(holdout[i], static_cast<int64_t>(initial.size() + i));  // any non-negative value; -1 means "missing"
  }
  const SegmentedModel rebuilt_model = build_segmented_model(all_keys, eps);
  const std::vector<int64_t> hits_after = sample(all_keys, query_count, 44);
  const auto bs_after = binary_search_in(all_keys);
  const auto dense_after = dense_lookup_in(all_keys, rebuilt_model);

  const bool after_ok = answers_ok(hits_after, true, bs_after) && answers_ok(hits_after, true, btree_lookup) &&
                        answers_ok(hits_after, true, dense_after) && answers_ok(hits_after, true, gapped_lookup);
  if (!after_ok) {
    std::fprintf(stderr, "correctness check FAILED after inserts\n");
    return 1;
  }

  std::vector<double> u[4];
  for (int p = 0; p < passes; p++) {
    u[0].push_back(time_pass(hits_after, bs_after));
    u[1].push_back(time_pass(hits_after, btree_lookup));
    u[2].push_back(time_pass(hits_after, dense_after));
    u[3].push_back(time_pass(hits_after, gapped_lookup));
  }
  const char* after_names[4] = {"binary search (rebuilt array)", "B+-tree", "Phase 2 model (rebuilt)",
                                "gapped array (what the db uses)"};
  std::printf("\nafter inserting the %zu held-out keys (%zu local + %zu full rebalances)\n", holdout.size(),
              ga.local_rebalance_count(), ga.rebalance_count());
  for (int s = 0; s < 4; s++) {
    const double hit_ns = median(u[s]);
    std::printf("%-34s %10.1f\n", after_names[s], hit_ns);
    csv << path << ",after_inserts," << after_names[s] << "," << hit_ns << ",-1\n";
  }
  return 0;
}
