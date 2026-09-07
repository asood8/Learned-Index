// A standard Bloom filter, sized using the textbook optimal formulas
// for a target false-positive rate. This is the baseline the learned
// version (learned_bloom_filter.h) needs to beat -- same job (fast
// "definitely not here" answers), same false-negative-free guarantee,
// different mechanism.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

class ClassicBloomFilter {
 public:
  static ClassicBloomFilter build(const std::vector<int64_t>& members, double target_fpr) {
    ClassicBloomFilter bf;
    const size_t n = members.size();

    // optimal bit count: m = -n*ln(p) / (ln 2)^2
    const double m = -(static_cast<double>(n) * std::log(target_fpr)) / (std::log(2.0) * std::log(2.0));
    bf.num_bits_ = std::max<size_t>(8, static_cast<size_t>(std::ceil(m)));

    // optimal hash count: k = (m/n) * ln 2
    const double k = (static_cast<double>(bf.num_bits_) / static_cast<double>(n)) * std::log(2.0);
    bf.num_hashes_ = std::max(1, static_cast<int>(std::round(k)));

    bf.bits_.assign(bf.num_bits_, false);
    for (int64_t key : members) {
      for (int h = 0; h < bf.num_hashes_; h++) bf.bits_[bf.hash(key, h)] = true;
    }
    return bf;
  }

  bool contains(int64_t key) const {
    for (int h = 0; h < num_hashes_; h++) {
      if (!bits_[hash(key, h)]) return false;
    }
    return true;
  }

  size_t size_bytes() const { return (num_bits_ + 7) / 8; }
  size_t num_bits() const { return num_bits_; }
  int num_hashes() const { return num_hashes_; }

 private:
  std::vector<bool> bits_;
  size_t num_bits_ = 0;
  int num_hashes_ = 1;

  size_t hash(int64_t key, int seed) const {
    uint64_t x = static_cast<uint64_t>(key) + 0x9E3779B97F4A7C15ULL * static_cast<uint64_t>(seed + 1);
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return static_cast<size_t>(x % num_bits_);
  }
};
