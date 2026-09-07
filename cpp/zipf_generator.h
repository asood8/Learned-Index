// Generates a Zipf-distributed access trace: a small number of keys
// get accessed disproportionately often, the classic "hot key"
// pattern real caching systems actually see. Uses inverse-transform
// sampling over a precomputed CDF.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

class ZipfGenerator {
 public:
  ZipfGenerator(size_t universe_size, double skew, uint64_t seed) : rng_(seed) {
    cdf_.resize(universe_size);
    double sum = 0.0;
    for (size_t i = 1; i <= universe_size; i++) sum += 1.0 / std::pow(static_cast<double>(i), skew);
    double cumulative = 0.0;
    for (size_t i = 1; i <= universe_size; i++) {
      cumulative += (1.0 / std::pow(static_cast<double>(i), skew)) / sum;
      cdf_[i - 1] = cumulative;
    }
  }

  // Returns a key in [0, universe_size), with rank 0 the most popular.
  int64_t next() {
    const double u = dist_(rng_);
    auto it = std::lower_bound(cdf_.begin(), cdf_.end(), u);
    return static_cast<int64_t>(it - cdf_.begin());
  }

  std::vector<int64_t> generate_trace(size_t length) {
    std::vector<int64_t> trace(length);
    for (size_t i = 0; i < length; i++) trace[i] = next();
    return trace;
  }

 private:
  std::mt19937_64 rng_;
  std::uniform_real_distribution<double> dist_{0.0, 1.0};
  std::vector<double> cdf_;
};
