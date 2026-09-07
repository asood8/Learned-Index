// A learned Bloom filter (Kraska et al.): replace the bit array with
// a small classifier trained to distinguish real members from
// non-members. Since the classifier can be wrong in one direction
// (calling a real member "absent" -- a false negative), every real
// member the trained model still scores below threshold gets added
// to a small backup set, so the overall structure keeps the same
// zero-false-negative guarantee a classic Bloom filter gives.
//
// The classifier itself is logistic regression over hashed features
// (the same "hashing trick" used in large-scale industrial models):
// each key hashes to a handful of weight buckets, and the model's
// score is the sigmoid of those weights summed. Unlike every model
// earlier in this project, this one has no closed-form solution --
// the sigmoid makes the loss nonlinear in the weights, so it's
// trained with real gradient descent instead of solved in one shot.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <unordered_set>
#include <utility>
#include <vector>

class LearnedBloomFilter {
 public:
  static LearnedBloomFilter train(const std::vector<int64_t>& members, int64_t key_min, int64_t key_max,
                                   size_t num_weights, int num_hashes, int epochs, double lr,
                                   uint64_t seed) {
    LearnedBloomFilter lbf;
    lbf.num_weights_ = num_weights;
    lbf.num_hashes_ = num_hashes;
    lbf.weights_.assign(num_weights, 0.0);

    std::unordered_set<int64_t> member_set(members.begin(), members.end());
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int64_t> key_dist(key_min, key_max);

    // training set: every real member (label 1) plus an equal number
    // of confirmed non-members sampled from the same key range
    // (label 0) -- a classifier can only learn to separate the two
    // if it sees examples of both
    std::vector<std::pair<int64_t, int>> training;
    training.reserve(members.size() * 2);
    for (int64_t k : members) training.push_back({k, 1});
    size_t neg_needed = members.size();
    while (neg_needed > 0) {
      int64_t k = key_dist(rng);
      if (member_set.count(k) == 0) {
        training.push_back({k, 0});
        neg_needed--;
      }
    }

    for (int epoch = 0; epoch < epochs; epoch++) {
      std::shuffle(training.begin(), training.end(), rng);
      for (auto& [key, label] : training) {
        const double p = lbf.score(key);
        const double grad = p - static_cast<double>(label);  // standard logistic regression gradient
        for (int h = 0; h < num_hashes; h++) {
          lbf.weights_[lbf.bucket(key, h)] -= lr * grad;
        }
      }
    }

    // backup set: catch every real member the trained model still
    // scores below threshold, so false negatives never happen
    lbf.threshold_ = 0.5;
    for (int64_t k : members) {
      if (lbf.score(k) < lbf.threshold_) lbf.backup_.insert(k);
    }

    return lbf;
  }

  bool contains(int64_t key) const {
    if (score(key) >= threshold_) return true;
    return backup_.count(key) > 0;
  }

  size_t size_bytes() const { return num_weights_ * sizeof(double) + backup_.size() * sizeof(int64_t); }
  size_t backup_size() const { return backup_.size(); }

 private:
  std::vector<double> weights_;
  size_t num_weights_ = 0;
  int num_hashes_ = 4;
  double threshold_ = 0.5;
  std::unordered_set<int64_t> backup_;

  size_t bucket(int64_t key, int h) const {
    uint64_t x = static_cast<uint64_t>(key) + 0x9E3779B97F4A7C15ULL * static_cast<uint64_t>(h + 1);
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return static_cast<size_t>(x % num_weights_);
  }

  double score(int64_t key) const {
    double s = 0.0;
    for (int h = 0; h < num_hashes_; h++) s += weights_[bucket(key, h)];
    return 1.0 / (1.0 + std::exp(-s));  // sigmoid
  }
};
