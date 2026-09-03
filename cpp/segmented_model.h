// Phase 2: instead of one line for the whole dataset, build as many
// small line segments as necessary to guarantee every prediction
// lands within a chosen error bound (epsilon) -- chosen up front,
// not measured after the fact like Phase 1's max_error.
//
// Each segment's anchor point is hit *exactly* by that segment's
// line (zero slack at the anchor). Every other point in the segment
// gets the full +/-eps tolerance. This is a deliberate simplification
// of the general bounded-error segmentation problem: pinning the
// anchor turns each new point into one clean interval constraint on
// a single number (the slope), so correctness is just "intersecting
// real intervals," with no interaction effects between points to
// reason about. A fully general version could occasionally pack
// points into slightly longer segments, but this version is easy to
// verify is correct, which matters more here.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

struct Segment {
  int64_t start_key;
  double slope;
  double intercept;
};

struct SegmentedModel {
  std::vector<Segment> segments;
  int64_t eps = 0;  // the error bound every segment was built to guarantee
};

// Builds segments left to right in one O(n) pass, fitting against
// caller-supplied position labels rather than assuming position ==
// array index. Phase 2 used this with positions = {0,1,2,...,n-1}
// (dense array indices); Phase 3's gapped array reuses this exact
// same fitting logic, just handing it gapped-array slot numbers
// instead -- the segmentation algorithm itself doesn't care what the
// positions mean, only that they're numbers to predict.
SegmentedModel build_segmented_model(const std::vector<int64_t>& keys,
                                      const std::vector<int64_t>& positions, int64_t eps) {
  SegmentedModel model;
  model.eps = eps;
  const size_t n = keys.size();
  if (n == 0) return model;

  size_t i = 0;
  while (i < n) {
    const int64_t anchor_x = keys[i];
    const int64_t anchor_y = positions[i];

    double slope_min = -std::numeric_limits<double>::infinity();
    double slope_max = std::numeric_limits<double>::infinity();
    size_t j = i;  // last index successfully included in this segment

    for (size_t k = i + 1; k < n; k++) {
      const double dx = static_cast<double>(keys[k] - anchor_x);
      const double dy = static_cast<double>(positions[k] - anchor_y);

      // With the anchor fixed exactly, point k is within eps iff the
      // slope falls in this interval -- derived directly from
      // |dy - slope*dx| <= eps, solved for slope.
      const double new_min = (dy - static_cast<double>(eps)) / dx;
      const double new_max = (dy + static_cast<double>(eps)) / dx;

      const double candidate_min = std::max(slope_min, new_min);
      const double candidate_max = std::min(slope_max, new_max);
      if (candidate_min > candidate_max) break;  // no slope works for every point anymore

      slope_min = candidate_min;
      slope_max = candidate_max;
      j = k;
    }

    Segment seg;
    seg.start_key = anchor_x;
    seg.slope = (j == i) ? 0.0 : (slope_min + slope_max) / 2.0;  // any slope in range is valid; midpoint is the safe default
    seg.intercept = static_cast<double>(anchor_y) - seg.slope * static_cast<double>(anchor_x);
    model.segments.push_back(seg);

    i = j + 1;
  }

  return model;
}

// Convenience overload for the dense-array case (Phase 2): positions
// are just the array indices themselves.
SegmentedModel build_segmented_model(const std::vector<int64_t>& keys, int64_t eps) {
  std::vector<int64_t> positions(keys.size());
  for (size_t i = 0; i < keys.size(); i++) positions[i] = static_cast<int64_t>(i);
  return build_segmented_model(keys, positions, eps);
}

// Finds the index of the last segment whose start_key <= key -- the
// segment responsible for this key. Same upper_bound + step-back
// technique as routing through an internal B+-tree node, just over a
// flat list instead of tree levels.
size_t find_segment_index(const std::vector<Segment>& segments, int64_t key) {
  auto it = std::upper_bound(segments.begin(), segments.end(), key,
                              [](int64_t k, const Segment& s) { return k < s.start_key; });
  if (it == segments.begin()) return 0;  // defensive: key smaller than the first segment
  return static_cast<size_t>((it - segments.begin()) - 1);
}

bool segmented_search(const std::vector<int64_t>& keys, const SegmentedModel& model,
                       int64_t key, int64_t& out_value) {
  if (model.segments.empty()) return false;
  const size_t seg_idx = find_segment_index(model.segments, key);
  const Segment& seg = model.segments[seg_idx];

  const double predicted_d = seg.slope * static_cast<double>(key) + seg.intercept;
  const int64_t predicted = static_cast<int64_t>(std::llround(predicted_d));

  const int64_t n = static_cast<int64_t>(keys.size());
  const int64_t lo = std::max<int64_t>(0, predicted - model.eps);
  const int64_t hi = std::min<int64_t>(n - 1, predicted + model.eps);

  auto begin_it = keys.begin() + lo;
  auto end_it = keys.begin() + hi + 1;
  auto it = std::lower_bound(begin_it, end_it, key);
  if (it != end_it && *it == key) {
    out_value = it - keys.begin();
    return true;
  }
  return false;
}
