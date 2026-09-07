// Compares the classic and learned Bloom filters honestly: same
// member set, a matched memory budget (not just a matched formula
// parameter), and an empirical false-positive rate measured the same
// way for both -- sampling confirmed true negatives from the same
// key range, not just trusting the target FPR each was built for.
#include <cstdio>
#include <random>
#include <unordered_set>
#include <vector>

#include "classic_bloom_filter.h"
#include "learned_bloom_filter.h"

int main() {
  std::mt19937_64 rng(42);
  std::vector<int64_t> members;
  int64_t k = 0;
  for (int i = 0; i < 100000; i++) {
    k += 1 + static_cast<int64_t>(rng() % 50);
    members.push_back(k);
  }
  const int64_t key_min = members.front();
  const int64_t key_max = members.back();

  const double target_fpr = 0.01;
  ClassicBloomFilter classic = ClassicBloomFilter::build(members, target_fpr);
  std::printf("classic bloom filter: %zu bits (%zu bytes), %d hash functions, built for %.1f%% FPR\n",
              classic.num_bits(), classic.size_bytes(), classic.num_hashes(), target_fpr * 100);

  // give the learned filter the same BYTE budget as the classic
  // filter's bit array, not the same bit count -- each weight is an
  // 8-byte double, so it gets far fewer slots for the same memory
  const size_t num_weights = std::max<size_t>(64, classic.size_bytes() / sizeof(double));
  LearnedBloomFilter learned =
      LearnedBloomFilter::train(members, key_min, key_max, num_weights, /*num_hashes=*/4,
                                 /*epochs=*/5, /*lr=*/0.5, /*seed=*/7);
  std::printf("learned bloom filter: %zu weights + %zu backup entries = %zu bytes total\n",
              num_weights, learned.backup_size(), learned.size_bytes());

  bool classic_no_fn = true, learned_no_fn = true;
  for (int64_t key : members) {
    if (!classic.contains(key)) classic_no_fn = false;
    if (!learned.contains(key)) learned_no_fn = false;
  }
  std::printf("\nzero false negatives -- classic: %s, learned: %s\n",
              classic_no_fn ? "yes" : "NO (bug)", learned_no_fn ? "yes" : "NO (bug)");

  std::unordered_set<int64_t> member_set(members.begin(), members.end());
  std::uniform_int_distribution<int64_t> key_dist(key_min, key_max);
  const int trials = 200000;
  int classic_fp = 0, learned_fp = 0, tested = 0;
  while (tested < trials) {
    int64_t q = key_dist(rng);
    if (member_set.count(q)) continue;  // only test confirmed true negatives
    tested++;
    if (classic.contains(q)) classic_fp++;
    if (learned.contains(q)) learned_fp++;
  }
  std::printf("\nempirical false-positive rate over %d confirmed true-negative queries:\n", tested);
  std::printf("  classic bloom filter: %.3f%%\n", 100.0 * classic_fp / tested);
  std::printf("  learned bloom filter: %.3f%%\n", 100.0 * learned_fp / tested);

  return 0;
}
