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
//
// That "slightly" turned out to be about 25% (157 segments vs the
// published PGM-index's 126 on 1M wide-range lognormal keys), so
// build_optimal_segmented_model() at the bottom of this file drops the
// pinned anchor and finds the fewest segments possible.
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

// Optimal segmentation.
//
// Pinning each segment's line to its first point (above) turns every
// new point into one interval constraint on the slope, but it throws
// away freedom: the best line for a run of points rarely passes exactly
// through the first one. This version keeps track of every line that
// still fits.
//
// Each point (x, y) requires the line to pass through the vertical gap
// from (x, y - eps) to (x, y + eps). The lines that do this for every
// point so far form a convex region, and deciding whether the next point
// still fits only takes two of them: the shallowest and the steepest
// fitting lines. The shallowest runs from an upper endpoint on the left
// down to a lower endpoint on the right; the steepest runs from a lower
// endpoint on the left up to an upper endpoint on the right. A new
// point whose gap lies entirely below the shallowest line or entirely
// above the steepest one can't fit, and closes the segment. Otherwise,
// if the point cuts into either line, that line pivots to a new
// endpoint, found by walking a convex hull of the earlier endpoints:
// the lower hull of the upper endpoints, and the upper hull of the lower
// endpoints. This is O'Rourke's 1981 algorithm, the one PGM-index is
// built on, and it's O(n) overall because each hull point is passed at
// most once.
//
// Any part of a run that fits also fits, so closing a segment only when
// forced gives the fewest segments possible. When a segment closes, its
// line is the average of the two extreme lines. That average fits too:
// at every point, its value is the midpoint of two values that are both
// inside the point's gap.
//
// All of the fitting uses exact integer arithmetic (128-bit cross
// products), so "fits" has no floating-point tolerance in it. Doubles
// only appear at the end, when a segment's line is written out in the
// same Segment format everything else reads. eps must be at least 1;
// with eps = 0 the two endpoints of a gap coincide and the geometry
// degenerates, so that case falls back to the pinned version.
namespace optimal_segmentation {

struct Point {
  int64_t x;
  int64_t y;
};

// (a - o) x (b - o): positive if o -> a -> b turns left.
inline __int128 cross(const Point& o, const Point& a, const Point& b) {
  return (static_cast<__int128>(a.x) - o.x) * (static_cast<__int128>(b.y) - o.y) -
         (static_cast<__int128>(a.y) - o.y) * (static_cast<__int128>(b.x) - o.x);
}

// Is the slope from a to b less than the slope from c to d? Both must
// point rightward. a and b may share an x (a vertical drop from an
// upper endpoint to the lower endpoint below it), which compares as
// less than every rightward slope.
inline bool slope_less(const Point& a, const Point& b, const Point& c, const Point& d) {
  const __int128 dx1 = static_cast<__int128>(b.x) - a.x, dy1 = static_cast<__int128>(b.y) - a.y;
  const __int128 dx2 = static_cast<__int128>(d.x) - c.x, dy2 = static_cast<__int128>(d.y) - c.y;
  return dy1 * dx2 < dy2 * dx1;
}

class Segmenter {
 public:
  explicit Segmenter(int64_t eps) : eps_(eps) {}

  // Adds (x, y) to the current segment if some line can still fit every
  // point in it. Returns false, changing nothing, if not. x must be
  // larger than every x already added.
  bool add(int64_t x, int64_t y) {
    const Point hi{x, y + eps_}, lo{x, y - eps_};
    if (count_ == 0) {
      upper_.assign(1, hi);
      lower_.assign(1, lo);
      upper_start_ = lower_start_ = 0;
      shallow_left_ = hi;
      steep_left_ = lo;
      count_ = 1;
      return true;
    }
    if (count_ == 1) {
      shallow_right_ = lo;
      steep_right_ = hi;
      upper_.push_back(hi);
      lower_.push_back(lo);
      count_ = 2;
      return true;
    }

    if (slope_less(shallow_right_, hi, shallow_left_, shallow_right_)) return false;  // gap below the shallowest line
    if (slope_less(steep_left_, steep_right_, steep_right_, lo)) return false;        // gap above the steepest line

    // The top of the gap is below the steepest line, so that line has to
    // come down: it now ends at hi and pivots on whichever earlier lower
    // endpoint makes it steepest while staying above all of them.
    if (slope_less(steep_left_, hi, steep_left_, steep_right_)) {
      size_t best = lower_start_;
      for (size_t i = lower_start_ + 1; i < lower_.size(); i++) {
        if (slope_less(lower_[best], hi, lower_[i], hi)) break;
        best = i;
      }
      steep_left_ = lower_[best];
      steep_right_ = hi;
      lower_start_ = best;
      size_t end = upper_.size();
      while (end >= upper_start_ + 2 && cross(upper_[end - 2], upper_[end - 1], hi) <= 0) end--;
      upper_.resize(end);
      upper_.push_back(hi);
    }

    // Mirror image: the bottom of the gap is above the shallowest line,
    // so that line has to come up, ending at lo.
    if (slope_less(shallow_left_, shallow_right_, shallow_left_, lo)) {
      size_t best = upper_start_;
      for (size_t i = upper_start_ + 1; i < upper_.size(); i++) {
        if (slope_less(upper_[i], lo, upper_[best], lo)) break;
        best = i;
      }
      shallow_left_ = upper_[best];
      shallow_right_ = lo;
      upper_start_ = best;
      size_t end = lower_.size();
      while (end >= lower_start_ + 2 && cross(lower_[end - 2], lower_[end - 1], lo) >= 0) end--;
      lower_.resize(end);
      lower_.push_back(lo);
    }

    count_++;
    return true;
  }

  // The line for the points added so far, as a Segment starting at
  // start_key (the first key added).
  Segment line(int64_t start_key) const {
    Segment seg;
    seg.start_key = start_key;
    if (count_ == 1) {
      seg.slope = 0.0;
      seg.intercept = static_cast<double>((shallow_left_.y + steep_left_.y) / 2);  // the point's own y
      return seg;
    }
    // Work relative to start_key so the long doubles stay exact.
    auto rel = [start_key](const Point& p) {
      return static_cast<long double>(static_cast<__int128>(p.x) - start_key);
    };
    const long double shallow = static_cast<long double>(shallow_right_.y - shallow_left_.y) /
                                (rel(shallow_right_) - rel(shallow_left_));
    const long double steep = static_cast<long double>(steep_right_.y - steep_left_.y) /
                              (rel(steep_right_) - rel(steep_left_));
    const long double shallow_at_start = shallow_left_.y - shallow * rel(shallow_left_);
    const long double steep_at_start = steep_left_.y - steep * rel(steep_left_);
    const long double slope = (shallow + steep) / 2;
    const long double at_start = (shallow_at_start + steep_at_start) / 2;
    seg.slope = static_cast<double>(slope);
    seg.intercept = static_cast<double>(at_start - slope * static_cast<long double>(start_key));
    return seg;
  }

  void reset() { count_ = 0; }

 private:
  int64_t eps_;
  size_t count_ = 0;
  std::vector<Point> upper_;  // lower hull of the upper endpoints
  std::vector<Point> lower_;  // upper hull of the lower endpoints
  size_t upper_start_ = 0, lower_start_ = 0;  // hull points before these can't matter anymore
  Point shallow_left_{}, shallow_right_{}, steep_left_{}, steep_right_{};
};

}  // namespace optimal_segmentation

inline SegmentedModel build_optimal_segmented_model(const std::vector<int64_t>& keys,
                                                    const std::vector<int64_t>& positions, int64_t eps) {
  if (eps < 1) return build_segmented_model(keys, positions, eps);
  SegmentedModel model;
  model.eps = eps;
  if (keys.empty()) return model;

  optimal_segmentation::Segmenter segmenter(eps);
  size_t start = 0;
  for (size_t i = 0; i < keys.size(); i++) {
    if (!segmenter.add(keys[i], positions[i])) {
      model.segments.push_back(segmenter.line(keys[start]));
      segmenter.reset();
      segmenter.add(keys[i], positions[i]);
      start = i;
    }
  }
  model.segments.push_back(segmenter.line(keys[start]));
  return model;
}

inline SegmentedModel build_optimal_segmented_model(const std::vector<int64_t>& keys, int64_t eps) {
  std::vector<int64_t> positions(keys.size());
  for (size_t i = 0; i < keys.size(); i++) positions[i] = static_cast<int64_t>(i);
  return build_optimal_segmented_model(keys, positions, eps);
}
