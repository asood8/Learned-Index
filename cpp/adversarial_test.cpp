// Phase 7c: a real poisoning-style attack, targeting the actual
// mechanism Phase 3 built rather than a disconnected toy example.
// Both scenarios insert the exact same *number* of keys into an
// identical starting structure -- the only difference is whether
// those keys are scattered across the whole key range (benign,
// matching Phase 3's own original test) or crammed into one narrow
// window (adversarial). Concentrating inserts this way is exactly
// the lever a real attacker with insert access has: it forces one
// small region's segment(s) to exhaust their local gaps and trigger
// far more rebalances than the same insert *count* would otherwise
// need. This mirrors real published research (poisoning attacks on
// ALEX and PGM-index have been shown to degrade performance by up to
// ~20%) -- not a stretch of the concept, the same actual attack shape.
#include <chrono>
#include <cstdio>
#include <random>
#include <unordered_set>
#include <vector>

#include "gapped_array.h"
#include "key_generators.h"

std::vector<int64_t> scattered_holdout(const std::vector<int64_t>& base, size_t count, uint64_t seed) {
  std::unordered_set<int64_t> existing(base.begin(), base.end());
  int64_t min_key = base.front(), max_key = base.back();
  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<int64_t> dist(min_key, max_key);
  std::vector<int64_t> result;
  while (result.size() < count) {
    int64_t k = dist(rng);
    if (!existing.count(k)) {
      existing.insert(k);
      result.push_back(k);
    }
  }
  return result;
}

std::vector<int64_t> adversarial_holdout(const std::vector<int64_t>& base, size_t count,
                                          int64_t window_start, int64_t window_width, uint64_t seed) {
  std::unordered_set<int64_t> existing(base.begin(), base.end());
  // build the list of free values in the window directly rather than
  // rejection-sampling -- as the window fills up (a tight window
  // needing nearly all its capacity), rejection sampling degrades
  // badly (classic coupon-collector slowdown), while this stays O(window_width)
  std::vector<int64_t> free_values;
  free_values.reserve(static_cast<size_t>(window_width));
  for (int64_t k = window_start; k < window_start + window_width; k++) {
    if (!existing.count(k)) free_values.push_back(k);
  }
  std::mt19937_64 rng(seed);
  std::shuffle(free_values.begin(), free_values.end(), rng);
  free_values.resize(std::min(count, free_values.size()));
  return free_values;
}

double run_scenario(const std::vector<int64_t>& base, const std::vector<int64_t>& to_insert,
                     size_t& out_rebalances, size_t& out_local_rebalances) {
  GappedArray ga = GappedArray::build(base, 0.7, 64);
  auto t0 = std::chrono::steady_clock::now();
  for (int64_t key : to_insert) ga.insert(key);
  auto t1 = std::chrono::steady_clock::now();
  out_rebalances = ga.rebalance_count();
  out_local_rebalances = ga.local_rebalance_count();

  size_t missing = 0;
  for (int64_t key : base) {
    size_t idx;
    if (!ga.search(key, idx)) missing++;
  }
  for (int64_t key : to_insert) {
    size_t idx;
    if (!ga.search(key, idx)) missing++;
  }
  if (missing > 0) {
    std::fprintf(stderr, "CORRECTNESS FAILED: %zu keys missing\n", missing);
  }

  return std::chrono::duration<double, std::nano>(t1 - t0).count() / to_insert.size();
}

int main() {
  const size_t base_n = 900000;
  const size_t insert_n = 100000;

  std::vector<int64_t> base = gen_uniform(base_n, 500);
  const int64_t range_span = base.back() - base.front();

  std::vector<int64_t> benign = scattered_holdout(base, insert_n, 42);
  size_t benign_reb, benign_local;
  double benign_ns = run_scenario(base, benign, benign_reb, benign_local);
  std::printf("base: %zu keys, inserting %zu more in each scenario\n", base_n, insert_n);
  std::printf("benign (scattered):     %10.1f ns/insert   %zu local + %zu full rebalances\n\n", benign_ns,
              benign_local, benign_reb);

  // try several window widths, from wide down to the narrowest that
  // can still physically fit insert_n unique new keys
  for (double fraction : {0.05, 0.02, 0.012}) {
    const int64_t window_width = static_cast<int64_t>(range_span * fraction);
    const int64_t window_start = base.front() + range_span / 2;
    std::vector<int64_t> adversarial = adversarial_holdout(base, insert_n, window_start, window_width, 42);

    size_t adv_reb, adv_local;
    double adv_ns = run_scenario(base, adversarial, adv_reb, adv_local);
    const double degradation = (adv_ns - benign_ns) / benign_ns * 100.0;
    std::printf("window=%.1f%% of range (%lld wide): %10.1f ns/insert   %zu local + %zu full rebalances   degradation: %+.1f%%\n",
                fraction * 100.0, static_cast<long long>(window_width), adv_ns, adv_local, adv_reb, degradation);
  }

  return 0;
}
