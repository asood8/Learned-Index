// Phase 3: a gapped array. Real keys are spread across an array
// larger than strictly necessary, with empty slots deliberately left
// scattered through it, so a new insert can usually slide into a
// nearby gap instead of shifting everything after it.
//
// The segmented model from Phase 2 is reused completely unchanged --
// it's refit against each key's position *in this gapped array*
// rather than its dense rank, but the segmentation algorithm itself
// has no idea what the position numbers mean, so nothing about it
// needed to change.
//
// v2: rebalancing is now per-segment instead of whole-array. Each
// segment tracks its own insert count since it was last refreshed;
// once that hits the threshold, only *that segment's* physical
// region gets redistributed and refit -- not the entire structure.
// A much less frequent whole-array rebalance is kept as a periodic
// safety net, since a single insert's shift can occasionally still
// reach across a segment boundary (bounded by max_gap_scan_, but not
// perfectly confined to one segment's territory).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "segmented_model.h"

class GappedArray {
 public:
  static constexpr int64_t EMPTY_SLOT = INT64_MAX;

  // Builds from a sorted, unique key list. target_density in (0,1]:
  // 0.7 means real keys occupy about 70% of the array, the rest left
  // as gaps.
  static GappedArray build(const std::vector<int64_t>& sorted_keys, double target_density,
                            int64_t eps) {
    GappedArray ga;
    ga.density_ = target_density;
    ga.eps_ = eps;
    ga.layout(sorted_keys);
    return ga;
  }

  // A key is present iff the first real key >= it *is* it. Goes
  // through the same locate() walk as every other lookup below, so
  // there's one place where a prediction becomes an answer -- not
  // four near-copies that each have to get the edge cases right on
  // their own (see locate() for why that stopped being hypothetical).
  bool search(int64_t key, size_t& out_index) const {
    const size_t idx = lower_bound_index(key);
    if (idx == data_.size() || data_[idx] != key) return false;
    out_index = idx;
    return true;
  }

  // Removes a key by turning its slot back into a gap -- reusing the
  // exact concept that makes inserts cheap in the first place, rather
  // than needing a separate "tombstone" mechanism. The model isn't
  // touched: it was never a promise about which slots are occupied,
  // only where a given key's slot *would be* if present, and that's
  // still true for every other key after this one is gone.
  bool remove(int64_t key) {
    size_t idx;
    if (!search(key, idx)) return false;
    data_[idx] = EMPTY_SLOT;
    count_--;
    return true;
  }

  struct SearchDiagnostics {
    bool found = false;
    size_t index = 0;
    int64_t predicted_position = 0;
    int probes = 0;  // slots actually examined during the local scan
  };

  // Identical logic to search(), but tracks and returns the model's
  // raw prediction and how many slots the walk actually examined --
  // for Phase 12's EXPLAIN, kept as a separate method so the extra
  // bookkeeping never costs anything on the real query path.
  SearchDiagnostics search_explain(int64_t key) const {
    SearchDiagnostics diag;
    if (model_.segments.empty()) return diag;
    diag.predicted_position = predict(key);
    const size_t idx = locate(diag.predicted_position, key, &diag.probes);
    if (idx < data_.size() && data_[idx] == key) {
      diag.found = true;
      diag.index = idx;
    }
    return diag;
  }

  // Returns the physical index of the first real (non-empty) key >=
  // target, or capacity() if no such key exists. Range scans start
  // here; search() and insert() are built on the same locate() walk.
  size_t lower_bound_index(int64_t target) const {
    if (model_.segments.empty() || data_.empty()) return data_.size();
    return locate(predict(target), target, nullptr);
  }

  // All real keys in [lo, hi], inclusive, in sorted order. Correctness
  // comes from checking the actual stored key values against the
  // bounds -- the model only helps find where to *start* scanning.
  std::vector<int64_t> range_scan_keys(int64_t lo, int64_t hi) const {
    std::vector<int64_t> results;
    size_t idx = lower_bound_index(lo);
    const size_t n = data_.size();
    while (idx < n) {
      if (data_[idx] != EMPTY_SLOT) {
        if (data_[idx] > hi) break;  // a real key past the range: stop here
        results.push_back(data_[idx]);
      }
      idx++;  // a gap: skip it and keep going, don't stop the scan
    }
    return results;
  }

  // Inserts key in sorted order. Tries a nearby gap first; falls back
  // to a full rebuild only when the local area has run out of room --
  // the same "batch retrain" idea mentioned as the easy option, used
  // here only as a last resort instead of the default behavior.
  void insert(int64_t key) {
    if (model_.segments.empty()) {
      // nothing to route against yet -- bootstrap the structure from
      // scratch with just this one key, same fallback path used when
      // a local area is completely out of room
      rebalance_with(key);
      return;
    }

    // The insertion point is exactly the lower bound: the first real
    // key >= this one. This used to come from scanning the +/-eps
    // window and defaulting to "just past the window" when nothing in
    // it qualified -- which put the key out of sorted order whenever
    // its true spot lay outside the window (see locate()). Getting it
    // from locate() instead makes placement correct however far off
    // the prediction was, and doubles as the duplicate check,
    // replacing what used to be a separate search() call.
    const size_t owning_segment = find_segment_index(model_.segments, key);
    const int64_t insertion_point =
        static_cast<int64_t>(locate(predict_from(owning_segment, key), key, nullptr));
    if (insertion_point < static_cast<int64_t>(data_.size()) && data_[insertion_point] == key) {
      return;  // key already present: no-op, not a second copy
    }

    const int64_t gap = find_nearest_gap(insertion_point);
    if (gap < 0) {
      rebalance_with(key);
      return;
    }

    if (gap < insertion_point) {
      for (int64_t idx = gap; idx < insertion_point - 1; idx++) data_[idx] = data_[idx + 1];
      data_[insertion_point - 1] = key;
    } else {
      for (int64_t idx = gap; idx > insertion_point; idx--) data_[idx] = data_[idx - 1];
      data_[insertion_point] = key;
    }
    count_++;

    // Bump just the owning segment's counter. Once it crosses the
    // threshold, only that segment's physical region gets refreshed
    // -- cheap, since segments are typically a small slice of the
    // whole array. A much larger global counter still runs
    // underneath as a safety net (see below), since max_gap_scan_
    // means a shift can occasionally still land just across a
    // segment boundary, slightly outside what the owning segment's
    // own counter tracked.
    segment_insert_counts_[owning_segment]++;
    if (segment_insert_counts_[owning_segment] >= segment_threshold_) {
      local_rebalance(owning_segment);
    }

    inserts_since_rebalance_++;
    if (inserts_since_rebalance_ >= global_threshold_) {
      rebalance();
    }
  }

  size_t size() const { return count_; }
  size_t capacity() const { return data_.size(); }
  size_t rebalance_count() const { return rebalance_count_; }
  size_t local_rebalance_count() const { return local_rebalance_count_; }

 private:
  std::vector<int64_t> data_;
  SegmentedModel model_;
  std::vector<size_t> segment_insert_counts_;  // parallel to model_.segments
  double density_ = 0.7;
  int64_t eps_ = 0;
  size_t count_ = 0;
  size_t rebalance_count_ = 0;
  size_t local_rebalance_count_ = 0;
  size_t inserts_since_rebalance_ = 0;
  size_t segment_threshold_ = 1;   // per-segment inserts before a local rebalance
  size_t global_threshold_ = 1;    // total inserts before a full-array safety-net rebalance
  int64_t max_gap_scan_ = 1;       // bounds how far a single insert's shift can reach

  int64_t predict(int64_t key) const {
    return predict_from(find_segment_index(model_.segments, key), key);
  }

  // A segment's line was only fit to keys between its own anchor and
  // the next segment's anchor, so its prediction is clamped to that
  // physical stretch. Without this, a query for a value the segment
  // never saw -- age=77 in a segment whose last trained key is age=69,
  // when the next segment starts at an outlier age=99 -- extrapolates
  // the line straight past the next anchor: it predicted slot 1644 in
  // a 1430-slot array, for a key whose lower bound was slot 1407. The
  // clamp stops it at the next anchor instead, next to where the key
  // actually belongs. Anchors are hit exactly by construction
  // (build_segmented_model pins them), so this needs no extra storage.
  //
  // This is a model-level bound, but not a correctness guarantee on
  // its own -- keys drift between refits. locate() is what makes the
  // answer correct; this is what keeps locate()'s walk short.
  int64_t predict_from(size_t seg_idx, int64_t key) const {
    const Segment& seg = model_.segments[seg_idx];
    double p = seg.slope * static_cast<double>(key) + seg.intercept;
    if (key >= seg.start_key) p = std::max(p, anchor_position(seg));
    if (seg_idx + 1 < model_.segments.size()) p = std::min(p, anchor_position(model_.segments[seg_idx + 1]));
    // and never outside the array itself -- which also keeps llround
    // away from doubles too large to fit in an int64_t
    p = std::clamp(p, 0.0, static_cast<double>(data_.size()));
    return static_cast<int64_t>(std::llround(p));
  }

  static double anchor_position(const Segment& seg) {
    return seg.slope * static_cast<double>(seg.start_key) + seg.intercept;
  }

  // The one place a prediction becomes an answer: the physical index
  // of the first real key >= target, or data_.size() if there isn't
  // one. Walks from the predicted slot toward the answer, in whichever
  // direction the stored keys say it lies:
  //   1. step left while the slot to the left is a gap or holds a key
  //      >= target -- the answer can't be to the right of either;
  //   2. step right past gaps and keys < target.
  // After step 1, every real key left of i is < target (the array is
  // sorted, and the walk stopped at one that is), so the first real
  // key >= target that step 2 reaches is the true lower bound. That
  // holds whatever the model predicted: the model only decides where
  // the walk starts, i.e. how long it is.
  //
  // This replaced a fixed +/-eps window. The eps guarantee only covers
  // keys the model was trained on; for any other key (one inserted
  // since the last refit, or a query for an absent one) nothing bounds
  // how far a prediction can miss, and each function that scanned the
  // window had its own way of being wrong when it did:
  // lower_bound_index() returned a later key than the true first one,
  // insert() placed keys out of sorted order, and search() had grown a
  // full-array fallback that made every absent-key lookup -- including
  // insert()'s own duplicate check -- O(n). The cost is now O(distance
  // from prediction to answer) rather than a fixed window.
  size_t locate(int64_t predicted, int64_t target, int* probes) const {
    const int64_t n = static_cast<int64_t>(data_.size());
    int64_t i = std::clamp<int64_t>(predicted, 0, n);
    int examined = 0;
    while (i > 0) {
      examined++;
      if (data_[i - 1] < target) break;  // gaps are EMPTY_SLOT = INT64_MAX, never < target
      i--;
    }
    while (i < n) {
      examined++;
      if (data_[i] != EMPTY_SLOT && data_[i] >= target) break;
      i++;
    }
    if (probes != nullptr) *probes = examined;
    return static_cast<size_t>(i);
  }

  int64_t find_nearest_gap(int64_t start) const {
    const int64_t n = static_cast<int64_t>(data_.size());
    for (int64_t d = 0; d <= max_gap_scan_; d++) {
      const int64_t right = start + d;
      if (right >= 0 && right < n && data_[right] == EMPTY_SLOT) return right;
      const int64_t left = start - 1 - d;
      if (left >= 0 && left < n && data_[left] == EMPTY_SLOT) return left;
    }
    return -1;  // essentially full nearby; caller should rebalance
  }

  // Lays a sorted, gap-free key list out into a fresh gapped array,
  // spacing keys proportionally across the larger capacity, then
  // refits the segmented model against these new gapped positions.
  // Used for the initial build and every full-array rebalance.
  void layout(const std::vector<int64_t>& sorted_keys) {
    const size_t n = sorted_keys.size();
    size_t capacity = (n == 0) ? 0 : static_cast<size_t>(std::ceil(static_cast<double>(n) / density_));
    capacity = std::max(capacity, n);

    data_.assign(capacity, EMPTY_SLOT);

    std::vector<int64_t> positions(n);
    int64_t last_slot = -1;
    for (size_t i = 0; i < n; i++) {
      int64_t target = static_cast<int64_t>((static_cast<double>(i) * static_cast<double>(capacity)) /
                                             static_cast<double>(n));
      if (target <= last_slot) target = last_slot + 1;  // same nudge-on-collision trick as data_gen.py's skewed generator
      target = std::min<int64_t>(target, static_cast<int64_t>(capacity) - 1);
      data_[target] = sorted_keys[i];
      positions[i] = target;
      last_slot = target;
    }

    model_ = build_segmented_model(sorted_keys, positions, eps_);
    count_ = n;
    inserts_since_rebalance_ = 0;
    segment_insert_counts_.assign(model_.segments.size(), 0);

    // Worst case, every insert since a segment was last refreshed
    // could have landed right on top of the same key, drifting it by
    // one position each time -- so both thresholds are tied to eps
    // itself (the budget being protected), not to array size. The
    // global one is set looser since local rebalancing now handles
    // the common case; it's a safety net, not the primary mechanism.
    segment_threshold_ = std::max<size_t>(1, static_cast<size_t>(eps_));
    global_threshold_ = std::max<size_t>(1, static_cast<size_t>(eps_) * 8);
    max_gap_scan_ = std::max<int64_t>(1, eps_ * 8);
  }

  // Refreshes the layout and model from whatever keys are currently
  // in the array -- the full-array safety net, and the fallback when
  // a local gap search comes up completely empty.
  void rebalance() {
    std::vector<int64_t> all_keys;
    all_keys.reserve(count_);
    for (int64_t v : data_) {
      if (v != EMPTY_SLOT) all_keys.push_back(v);
    }
    layout(all_keys);
    rebalance_count_++;
  }

  void rebalance_with(int64_t new_key) {
    std::vector<int64_t> all_keys;
    all_keys.reserve(count_ + 1);
    for (int64_t v : data_) {
      if (v != EMPTY_SLOT) all_keys.push_back(v);
    }
    auto it = std::lower_bound(all_keys.begin(), all_keys.end(), new_key);
    all_keys.insert(it, new_key);

    layout(all_keys);
    rebalance_count_++;
  }

  // Refreshes just one segment's physical region: finds its current
  // slot boundaries (via search, since the segment's own anchor key
  // and the next segment's anchor key are always real, findable keys),
  // redistributes its keys within that same slot range, and refits a
  // fresh local model. If that region is too full to leave reasonable
  // breathing room, falls back to the always-correct full rebalance
  // instead of forcing a bad local fit.
  void local_rebalance(size_t si) {
    const int64_t lo_key = model_.segments[si].start_key;
    const bool is_last = (si + 1 == model_.segments.size());

    size_t phys_lo;
    if (!search(lo_key, phys_lo)) {
      rebalance();
      return;
    }

    size_t phys_hi_exclusive;
    if (is_last) {
      phys_hi_exclusive = data_.size();
    } else {
      const int64_t hi_key = model_.segments[si + 1].start_key;
      if (!search(hi_key, phys_hi_exclusive)) {
        rebalance();
        return;
      }
    }

    std::vector<int64_t> local_keys;
    for (size_t idx = phys_lo; idx < phys_hi_exclusive; idx++) {
      if (data_[idx] != EMPTY_SLOT) local_keys.push_back(data_[idx]);
    }

    const size_t region_capacity = phys_hi_exclusive - phys_lo;
    // if this region has grown too dense, a local fix won't leave
    // enough room to be worth it -- fall back to the full rebalance
    if (region_capacity == 0 || local_keys.size() >= static_cast<size_t>(0.9 * region_capacity)) {
      rebalance();
      return;
    }

    for (size_t idx = phys_lo; idx < phys_hi_exclusive; idx++) data_[idx] = EMPTY_SLOT;

    std::vector<int64_t> local_positions(local_keys.size());
    int64_t last_slot = static_cast<int64_t>(phys_lo) - 1;
    for (size_t i = 0; i < local_keys.size(); i++) {
      int64_t target =
          static_cast<int64_t>(phys_lo) +
          static_cast<int64_t>((static_cast<double>(i) * static_cast<double>(region_capacity)) /
                                static_cast<double>(local_keys.size()));
      if (target <= last_slot) target = last_slot + 1;
      target = std::min<int64_t>(target, static_cast<int64_t>(phys_hi_exclusive) - 1);
      data_[target] = local_keys[i];
      local_positions[i] = target;
      last_slot = target;
    }

    SegmentedModel local_model = build_segmented_model(local_keys, local_positions, eps_);

    model_.segments.erase(model_.segments.begin() + si);
    model_.segments.insert(model_.segments.begin() + si, local_model.segments.begin(),
                            local_model.segments.end());

    segment_insert_counts_.erase(segment_insert_counts_.begin() + si);
    segment_insert_counts_.insert(segment_insert_counts_.begin() + si, local_model.segments.size(), 0);

    local_rebalance_count_++;
  }
};
