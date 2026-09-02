// Phase 1: a single learned model that predicts a key's position
// directly, replacing the tree walk entirely with one arithmetic
// calculation. Fitting is a few passes of plain arithmetic -- there
// is no iterative training loop, because ordinary least squares for
// one input variable has a direct, closed-form solution.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

struct LinearModel {
  double slope = 0.0;
  double intercept = 0.0;
  int64_t max_error = 0;  // worst |actual_position - predicted_position| seen while fitting

  // Predicts an approximate array index for this key. Not clamped to
  // [0, n-1] here -- the caller does that, since this function alone
  // doesn't know how big the array is.
  int64_t predict(int64_t key) const {
    double p = slope * static_cast<double>(key) + intercept;
    return static_cast<int64_t>(std::llround(p));
  }
};

// Fits position ~= slope*key + intercept over a sorted array, where
// the position label for keys[i] is simply i (sorted order gives us
// that for free -- no separate label data needed).
LinearModel fit_linear_model(const std::vector<int64_t>& keys) {
  const size_t n = keys.size();
  LinearModel model;
  if (n < 2) return model;

  // Positions are exactly 0, 1, 2, ..., n-1, so their mean has a
  // closed form -- (n-1)/2 -- with no loop required.
  const double mean_y = (static_cast<double>(n) - 1.0) / 2.0;

  // Pass 1: mean of the keys. This one does need a loop, since keys
  // can be any values at all.
  double sum_x = 0.0;
  for (size_t i = 0; i < n; i++) sum_x += static_cast<double>(keys[i]);
  const double mean_x = sum_x / static_cast<double>(n);

  // Pass 2: slope = Cov(key, position) / Var(key) -- the same
  // "covariance over variance" formula as a stock's CAPM beta.
  // Centering each value around its mean first (rather than
  // expanding into raw sums of squares) avoids subtracting two huge,
  // nearly-equal numbers, which is where floating point precision
  // quietly falls apart at this scale.
  double numerator = 0.0;    // sum of (key_i - mean_x)(position_i - mean_y)
  double denominator = 0.0;  // sum of (key_i - mean_x)^2
  for (size_t i = 0; i < n; i++) {
    double dx = static_cast<double>(keys[i]) - mean_x;
    double dy = static_cast<double>(i) - mean_y;
    numerator += dx * dy;
    denominator += dx * dx;
  }
  model.slope = numerator / denominator;
  model.intercept = mean_y - model.slope * mean_x;

  // Pass 3: now that we have a model, measure exactly how wrong it
  // is at its worst. That becomes the local search radius every
  // future lookup uses to correct the model's guess.
  int64_t max_error = 0;
  for (size_t i = 0; i < n; i++) {
    int64_t predicted = model.predict(keys[i]);
    int64_t actual = static_cast<int64_t>(i);
    int64_t err = std::llabs(actual - predicted);
    if (err > max_error) max_error = err;
  }
  model.max_error = max_error;

  return model;
}

// Looks up `key` using the model's prediction plus a bounded local
// search: the model gets us close, this finds the exact answer.
bool learned_search(const std::vector<int64_t>& keys, const LinearModel& model,
                     int64_t key, int64_t& out_value) {
  const int64_t n = static_cast<int64_t>(keys.size());
  const int64_t predicted = model.predict(key);

  const int64_t lo = std::max<int64_t>(0, predicted - model.max_error);
  const int64_t hi = std::min<int64_t>(n - 1, predicted + model.max_error);

  auto begin_it = keys.begin() + lo;
  auto end_it = keys.begin() + hi + 1;  // +1: end iterator is exclusive
  auto it = std::lower_bound(begin_it, end_it, key);
  if (it != end_it && *it == key) {
    out_value = it - keys.begin();
    return true;
  }
  return false;
}
