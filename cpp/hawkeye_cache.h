// A Hawkeye/LRB-style learned cache. The idea: train a classifier to
// approximate Belady's optimal eviction decisions using only
// information available online (recency, frequency) -- Belady itself
// needs the future, which isn't available at runtime, but it's fully
// available on a *training* trace, so its decisions there become the
// labels a classifier learns to predict from.
//
// Like the Bloom filter's classifier, this is logistic regression
// with no closed-form solution, trained with real gradient descent.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <random>
#include <unordered_map>
#include <vector>

#include "belady.h"

struct CacheTrainingExample {
  double recency;
  double frequency;
  int label;  // 1 = cache-friendly (Belady would want this kept), 0 = cache-averse
};

// Labels each access: cache-friendly if this key's next use is closer
// than `capacity` accesses away (a cache that size could plausibly
// still be holding it then), cache-averse otherwise. This is a
// simplified stand-in for Hawkeye's own "OPTgen" labeling -- not
// simulating the full cache occupancy history, just using the
// capacity itself as the reuse-distance cutoff -- chosen because it's
// easy to verify correct while still being directly motivated by why
// Belady's rule works at all: reuse distance relative to how much
// competing traffic will flow through before that reuse.
std::vector<CacheTrainingExample> build_training_data(const std::vector<int64_t>& trace, size_t capacity) {
  const std::vector<int64_t> next_idx = next_occurrence_index(trace);
  std::vector<CacheTrainingExample> examples;
  examples.reserve(trace.size());

  std::unordered_map<int64_t, int64_t> last_seen;
  std::unordered_map<int64_t, int64_t> freq_count;

  for (size_t i = 0; i < trace.size(); i++) {
    const int64_t key = trace[i];
    auto seen_it = last_seen.find(key);
    const double recency =
        (seen_it == last_seen.end()) ? static_cast<double>(trace.size()) : static_cast<double>(i - seen_it->second);
    const double frequency = static_cast<double>(freq_count[key]);

    const int64_t next = next_idx[i];
    const int64_t dist = (next == BELADY_NEVER_AGAIN) ? BELADY_NEVER_AGAIN : (next - static_cast<int64_t>(i));
    const int label = (dist <= static_cast<int64_t>(capacity)) ? 1 : 0;

    examples.push_back({recency, frequency, label});
    last_seen[key] = static_cast<int64_t>(i);
    freq_count[key]++;
  }
  return examples;
}

class HawkeyeClassifier {
 public:
  static HawkeyeClassifier train(const std::vector<CacheTrainingExample>& examples, int epochs, double lr,
                                  uint64_t seed) {
    HawkeyeClassifier model;
    std::mt19937_64 rng(seed);
    std::vector<size_t> order(examples.size());
    std::iota(order.begin(), order.end(), 0);

    for (int epoch = 0; epoch < epochs; epoch++) {
      std::shuffle(order.begin(), order.end(), rng);
      for (size_t idx : order) {
        const auto& ex = examples[idx];
        // log1p compresses the heavy-tailed recency/frequency counts
        // onto a scale gradient descent can actually converge on
        const double x1 = std::log1p(ex.recency);
        const double x2 = std::log1p(ex.frequency);
        const double p = model.predict_raw(x1, x2);
        const double grad = p - static_cast<double>(ex.label);
        model.w0_ -= lr * grad;
        model.w1_ -= lr * grad * x1;
        model.w2_ -= lr * grad * x2;
      }
    }
    return model;
  }

  // Higher = more likely cache-friendly (worth keeping).
  double predict(double recency, double frequency) const {
    return predict_raw(std::log1p(recency), std::log1p(frequency));
  }

 private:
  double w0_ = 0.0, w1_ = 0.0, w2_ = 0.0;

  double predict_raw(double x1, double x2) const {
    const double z = w0_ + w1_ * x1 + w2_ * x2;
    return 1.0 / (1.0 + std::exp(-z));
  }
};

class HawkeyeCache {
 public:
  HawkeyeCache(size_t capacity, const HawkeyeClassifier& model) : capacity_(capacity), model_(model) {}

  bool access(int64_t key, size_t current_time) {
    auto it = entries_.find(key);
    if (it != entries_.end()) {
      it->second.frequency++;
      it->second.last_access = current_time;
      return true;
    }
    if (entries_.size() >= capacity_) {
      int64_t worst_key = -1;
      double worst_score = 2.0;  // above any real sigmoid output
      for (const auto& [k, e] : entries_) {
        const double recency = static_cast<double>(current_time - e.last_access);
        const double score = model_.predict(recency, static_cast<double>(e.frequency));
        if (score < worst_score) {
          worst_score = score;
          worst_key = k;
        }
      }
      entries_.erase(worst_key);
    }
    entries_[key] = {current_time, 1};
    return false;
  }

 private:
  struct Entry {
    size_t last_access;
    size_t frequency;
  };
  size_t capacity_;
  const HawkeyeClassifier& model_;
  std::unordered_map<int64_t, Entry> entries_;
};
