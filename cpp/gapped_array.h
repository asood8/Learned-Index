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
// A much less frequent whole-array rebalance was kept as a periodic
// safety net, since a single insert's shift can occasionally still
// reach across a segment boundary (bounded by max_gap_scan_, but not
// perfectly confined to one segment's territory). v4 removed it.
//
// v3: it can store a value with each key. DurableStore used to keep
// rows in a separate std::unordered_map and use this array only to
// track which keys existed and in what order, so a point lookup
// searched the learned index and then the hash map, and the hash map
// could have answered by itself. BasicGappedArray<V> keeps each value
// in a parallel array next to its key, and every shift, removal, and
// rebalance moves the two together. GappedArray (V = NoValue) is the
// key-only version the benchmarks use; with NoValue, all of the value
// handling is compiled out by `if constexpr`.
//
// The model here is still fit with the pinned-anchor
// build_segmented_model(), not build_optimal_segmented_model(). The
// optimal fit needs about a quarter fewer segments on this layout (180
// vs 235 on 1M wide-range lognormal keys), but it takes 4-7x longer to
// fit, and this array refits a region every few dozen inserts. Before
// v4, when the whole model was refit every 512 inserts, switching made
// inserts about 2.7x slower. Measured again after v4, it made them 3-4x
// slower (1,233-1,717 vs 303-620 ns at 1M keys), with lookups about the
// same, so it still isn't worth it here.
//
// v4: inserts no longer cost O(n). The safety net rebuilt the whole
// array every 8*eps = 512 inserts whatever its size, and one rebuild
// took 10.8 ms at 1M keys, which by itself came to nearly all of the
// ~21 us an insert cost (and ~262 us per insert at 10M keys). It was
// needed because a shift that crossed into a neighboring segment wasn't
// counted against that neighbor. Now it is (see insert()), so each
// segment's count bounds how far its keys have drifted, and the safety
// net is gone. Three other things also forced full rebuilds and are
// fixed too, all in local_rebalance(): a crowded region, which now
// borrows room from its neighbors the way a packed-memory array does;
// keys appended past the end, which now grow the array; and segments
// long enough that refreshing one cost as much as rebuilding
// everything, now capped at kMaxSegmentKeys.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "segmented_model.h"

struct NoValue {};

struct GappedSearchDiagnostics {
  bool found = false;
  size_t index = 0;
  int64_t predicted_position = 0;
  int probes = 0;  // slots actually examined during the local scan
};

template <typename V>
class BasicGappedArray {
 public:
  static constexpr int64_t EMPTY_SLOT = INT64_MAX;
  static constexpr bool kStoresValues = !std::is_same_v<V, NoValue>;
  using SearchDiagnostics = GappedSearchDiagnostics;

  // Builds from a sorted, unique key list. target_density in (0,1]:
  // 0.7 means real keys occupy about 70% of the array, the rest left
  // as gaps. Any values start out empty.
  static BasicGappedArray build(const std::vector<int64_t>& sorted_keys, double target_density, int64_t eps) {
    BasicGappedArray ga;
    ga.density_ = target_density;
    ga.eps_ = eps;
    ga.layout(sorted_keys, nullptr);
    return ga;
  }

  // Same, for an array that stores values: values[i] goes with
  // sorted_keys[i]. Loading a checkpoint snapshot uses this, which lays
  // everything out in one pass instead of inserting keys one at a time.
  static BasicGappedArray build(const std::vector<int64_t>& sorted_keys, std::vector<V> values,
                                double target_density, int64_t eps) {
    static_assert(kStoresValues, "this build() needs a gapped array that stores values");
    BasicGappedArray ga;
    ga.density_ = target_density;
    ga.eps_ = eps;
    ga.layout(sorted_keys, &values);
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

  // The value stored with `key`, if the key is present.
  bool get(int64_t key, V& out) const {
    static_assert(kStoresValues, "get() needs a gapped array that stores values");
    size_t idx;
    if (!search(key, idx)) return false;
    out = vals_[idx];
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
    if constexpr (kStoresValues) {
      V empty{};
      std::swap(vals_[idx], empty);  // frees the old value's memory now, not at the next rebalance
    }
    count_--;
    return true;
  }

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

  // Same as lower_bound_index(), but starting from any slot instead of
  // the model's prediction. A lookup is supposed to be correct wherever
  // it starts, and this lets tests check that directly, along with what
  // a bad starting guess costs (`probes` = slots examined).
  size_t lower_bound_from(int64_t start, int64_t target, int* probes = nullptr) const {
    int examined = 0;
    const size_t idx = locate(start, target, &examined);
    if (probes != nullptr) *probes = examined;
    return idx;
  }

  // All real keys in [lo, hi], inclusive, in sorted order. Correctness
  // comes from checking the actual stored key values against the
  // bounds -- the model only helps find where to *start* scanning.
  std::vector<int64_t> range_scan_keys(int64_t lo, int64_t hi) const {
    std::vector<int64_t> results;
    scan(lo, hi, [&](size_t idx) { results.push_back(data_[idx]); });
    return results;
  }

  // Same, with each key's value.
  std::vector<std::pair<int64_t, V>> range_scan(int64_t lo, int64_t hi) const {
    static_assert(kStoresValues, "range_scan() needs a gapped array that stores values");
    std::vector<std::pair<int64_t, V>> results;
    scan(lo, hi, [&](size_t idx) { results.emplace_back(data_[idx], vals_[idx]); });
    return results;
  }

  // Inserts key in sorted order. Tries a nearby gap first; when there's
  // none within reach, refreshes the surrounding region, and rebuilds
  // the whole array only when all of it is too full -- the same "batch
  // retrain" idea mentioned as the easy option, used here only as a last
  // resort instead of the default behavior. If the
  // key is already present, a key-only array does nothing and one that
  // stores values replaces the value, so insert() doubles as update.
  void insert(int64_t key, V value = V()) {
    if (model_.segments.empty()) {
      // nothing to route against yet -- bootstrap the structure from
      // scratch with just this one key, same fallback path used when
      // a local area is completely out of room
      rebalance_with(key, std::move(value));
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
    size_t owning_segment = find_segment_index(model_.segments, key);
    int64_t insertion_point = static_cast<int64_t>(locate(predict_from(owning_segment, key), key, nullptr));
    if (insertion_point < static_cast<int64_t>(data_.size()) && data_[insertion_point] == key) {
      if constexpr (kStoresValues) vals_[insertion_point] = std::move(value);
      return;  // key already present: never a second copy
    }

    int64_t gap = find_nearest_gap(insertion_point);
    if (gap < 0) {
      // No gap within reach. Refreshing the segment spreads its gaps out
      // again, borrowing room from its neighbors or growing the array if
      // it has to (see local_rebalance()), which is far cheaper than the
      // full rebuild this used to go straight to. Then the spot has to be
      // found again, since that moved keys and may have split the segment.
      local_rebalance(owning_segment);
      owning_segment = find_segment_index(model_.segments, key);
      insertion_point = static_cast<int64_t>(locate(predict_from(owning_segment, key), key, nullptr));
      gap = find_nearest_gap(insertion_point);
    }
    if (gap < 0) {
      rebalance_with(key, std::move(value));
      return;
    }

    if (gap < insertion_point) {
      for (int64_t idx = gap; idx < insertion_point - 1; idx++) move_slot(idx + 1, idx);
      place(insertion_point - 1, key, std::move(value));
    } else {
      for (int64_t idx = gap; idx > insertion_point; idx--) move_slot(idx - 1, idx);
      place(insertion_point, key, std::move(value));
    }
    count_++;

    // Count this insert against every segment it changed: the one that
    // gained the key, and any neighbor whose keys the shift pushed one
    // slot over. The gap search doesn't stop at segment boundaries (it
    // looks up to max_gap_scan_ slots each way), so a shift can cross
    // one. Those crossings used to go uncounted, and a full rebuild every
    // 512 inserts covered for them (see v4 at the top). Each segment
    // whose count reaches the threshold gets its region refreshed.
    const std::vector<Segment>& segs = model_.segments;
    size_t first = owning_segment, last = owning_segment;
    if (gap > insertion_point && last + 1 < segs.size() && data_[gap] >= segs[last + 1].start_key) {
      last = find_segment_index(segs, data_[gap]);  // the largest key that moved right is now at slot gap
    }
    if (gap < insertion_point - 1 && data_[gap] < segs[first].start_key) {
      first = find_segment_index(segs, data_[gap]);  // the smallest key that moved left is now at slot gap
    }
    for (size_t si = first; si <= last; si++) segment_changes_[si]++;
    refresh_segments(first, last);
  }

  size_t size() const { return count_; }
  size_t capacity() const { return data_.size(); }
  size_t segment_count() const { return model_.segments.size(); }
  size_t rebalance_count() const { return rebalance_count_; }
  size_t local_rebalance_count() const { return local_rebalance_count_; }

  // Checks the bookkeeping, for tests: real keys strictly increasing and
  // as many as size() says, segment start keys strictly increasing, and
  // every array that runs parallel to another really in step with it.
  // Lookups only rely on the keys being sorted (see locate()), so a
  // broken count or a stale segment would make them slower, not wrong,
  // and has to be checked for on its own.
  bool consistent() const {
    const std::vector<Segment>& segs = model_.segments;
    if (segment_changes_.size() != segs.size()) return false;
    if (kStoresValues && vals_.size() != data_.size()) return false;
    for (size_t i = 1; i < segs.size(); i++) {
      if (segs[i].start_key <= segs[i - 1].start_key) return false;
    }
    size_t real = 0;
    int64_t prev = INT64_MIN;
    for (int64_t k : data_) {
      if (k == EMPTY_SLOT) continue;
      if (real > 0 && k <= prev) return false;
      prev = k;
      real++;
    }
    return real == count_;
  }

  // No segment covers more keys than this; see build_segmented_model().
  static constexpr size_t kMaxSegmentKeys = 4096;
  // How full a segment's region may get before refreshing it needs room
  // from its neighbors, and how full the whole array may get before it's
  // rebuilt bigger; windows in between get limits in between. See
  // too_dense().
  static constexpr double kSegmentDensityLimit = 0.9;
  static constexpr double kArrayDensityLimit = 0.8;

 private:
  std::vector<int64_t> data_;
  std::vector<V> vals_;  // parallel to data_; stays empty when V is NoValue
  SegmentedModel model_;
  // Parallel to model_.segments: how many inserts since the segment was
  // last fit either added a key to it or moved one of its keys.
  std::vector<size_t> segment_changes_;
  double density_ = 0.7;
  int64_t eps_ = 0;
  size_t count_ = 0;
  size_t rebalance_count_ = 0;
  size_t local_rebalance_count_ = 0;
  size_t segment_threshold_ = 1;   // changes to a segment before its region is refreshed
  int64_t max_gap_scan_ = 1;       // bounds how far a single insert's shift can reach

  // Every slot move goes through these two, so a value can't be left
  // behind when its key moves.
  void move_slot(int64_t from, int64_t to) {
    data_[to] = data_[from];
    if constexpr (kStoresValues) vals_[to] = std::move(vals_[from]);
  }
  void place(int64_t idx, int64_t key, [[maybe_unused]] V&& value) {
    data_[idx] = key;
    if constexpr (kStoresValues) vals_[idx] = std::move(value);
  }

  // Calls visit(slot) for every real key in [lo, hi], in sorted order.
  template <typename Visit>
  void scan(int64_t lo, int64_t hi, Visit visit) const {
    size_t idx = lower_bound_index(lo);
    const size_t n = data_.size();
    while (idx < n) {
      if (data_[idx] != EMPTY_SLOT) {
        if (data_[idx] > hi) break;  // a real key past the range: stop here
        visit(idx);
      }
      idx++;  // a gap: skip it and keep going, don't stop the scan
    }
  }

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
  // one. It starts at the predicted slot and moves toward the answer:
  //   1. step left while the slot to the left is a gap or holds a key
  //      >= target -- the answer can't be to the right of either;
  //   2. step right past gaps and keys < target.
  // After step 1, every real key left of i is < target (the array is
  // sorted, and the walk stopped at one that is), so the first real
  // key >= target that step 2 reaches is the true lower bound. That
  // holds whatever the model predicted: the model only decides where
  // the walk starts, i.e. how long it is.
  //
  // Stepping one slot at a time is cheap when the guess is close, since
  // neighboring slots share cache lines, but on its own it made a
  // far-off guess cost O(distance), and O(n) at worst. So each direction
  // only steps eps slots, the model's own error bound: a key the model
  // was trained on is about that close, so going further means the model
  // is outside its promise. Past that, gallop() takes over and finishes
  // in O(log distance). Galloping sooner was measured and made ordinary
  // lookups slower (157 vs 134 ns at a limit of 1), because binary
  // search jumps around memory while the walk reads neighboring slots;
  // at a limit of 64 ordinary lookups cost about the same as the plain
  // walk, and a lookup started at the wrong end of a 1.3M-slot array
  // fell from about 0.6 ms to under 0.2 microseconds.
  //
  // This replaced a fixed +/-eps window. The eps guarantee only covers
  // keys the model was trained on; for any other key (one inserted
  // since the last refit, or a query for an absent one) nothing bounds
  // how far a prediction can miss, and each function that scanned the
  // window had its own way of being wrong when it did:
  // lower_bound_index() returned a later key than the true first one,
  // insert() placed keys out of sorted order, and search() had grown a
  // full-array fallback that made every absent-key lookup -- including
  // insert()'s own duplicate check -- O(n).
  size_t locate(int64_t predicted, int64_t target, int* probes) const {
    const int64_t n = static_cast<int64_t>(data_.size());
    int64_t i = std::clamp<int64_t>(predicted, 0, n);
    const int64_t walk_limit = std::max<int64_t>(eps_, 1);
    int examined = 0;
    bool far = false;
    // The stopping slot is worked out once, so each step does the same
    // work as an unlimited walk. Checking a step counter on every step
    // instead made ordinary lookups a few percent slower (2-11% across
    // runs), which isn't worth paying for a case that's rare.
    const int64_t left_stop = std::max<int64_t>(i - walk_limit, 0);
    while (i > left_stop) {
      examined++;
      if (data_[i - 1] < target) break;  // gaps are EMPTY_SLOT = INT64_MAX, never < target
      i--;
    }
    if (i == left_stop && i > 0 && data_[i - 1] >= target) far = true;  // out of steps, answer still to the left
    if (!far) {
      const int64_t right_stop = std::min<int64_t>(i + walk_limit, n);
      while (i < right_stop) {
        examined++;
        if (data_[i] != EMPTY_SLOT && data_[i] >= target) break;
        i++;
      }
      if (i == right_stop && i < n && (data_[i] == EMPTY_SLOT || data_[i] < target)) far = true;  // still to the right
    }
    if (far) i = gallop(i, target, examined);
    if (probes != nullptr) *probes = examined;
    return static_cast<size_t>(i);
  }

  // Is the first real key at or after slot j at least `target`? At
  // j == data_.size() the answer is yes by definition. Since the array
  // is sorted, this flips from no to yes exactly once as j grows, and
  // the lower bound is the first real key at or after that flip. That's
  // what lets gallop() binary search even though gaps are scattered
  // through the array.
  bool reaches(int64_t j, int64_t target, int& examined) const {
    const int64_t n = static_cast<int64_t>(data_.size());
    while (j < n && data_[j] == EMPTY_SLOT) {
      j++;
      examined++;
    }
    if (j == n) return true;
    examined++;
    return data_[j] >= target;
  }

  // The far-off case. Jumps 1, 2, 4, 8, ... slots from i, in whichever
  // direction the answer is, until the flip in reaches() is bracketed,
  // then binary searches inside the bracket. That's O(log distance)
  // calls to reaches(), each of which also skips any empty slots it
  // lands on; those runs are short after a rebalance, but deletes can
  // leave longer ones until the next.
  int64_t gallop(int64_t i, int64_t target, int& examined) const {
    const int64_t n = static_cast<int64_t>(data_.size());
    int64_t lo, hi;  // reaches(lo) is false (lo == -1 counts as false); reaches(hi) is true
    if (reaches(i, target, examined)) {
      hi = i;
      for (int64_t step = 1;; step *= 2) {
        lo = hi - step;
        if (lo < 0) {
          lo = -1;
          break;
        }
        if (!reaches(lo, target, examined)) break;
        hi = lo;
      }
    } else {
      lo = i;
      for (int64_t step = 1;; step *= 2) {
        hi = lo + step;
        if (hi >= n) {
          hi = n;
          break;
        }
        if (reaches(hi, target, examined)) break;
        lo = hi;
      }
    }
    while (hi - lo > 1) {
      const int64_t mid = lo + (hi - lo) / 2;
      if (reaches(mid, target, examined)) {
        hi = mid;
      } else {
        lo = mid;
      }
    }
    while (hi < n && data_[hi] == EMPTY_SLOT) {
      hi++;
      examined++;
    }
    return hi;
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

  // Lays a sorted, gap-free key list (and its values, if given) out
  // into a fresh gapped array, spacing keys proportionally across the
  // larger capacity, then refits the segmented model against these new
  // gapped positions. Used for the initial build and every full-array
  // rebalance.
  void layout(const std::vector<int64_t>& sorted_keys, [[maybe_unused]] std::vector<V>* values) {
    const size_t n = sorted_keys.size();
    size_t capacity = (n == 0) ? 0 : static_cast<size_t>(std::ceil(static_cast<double>(n) / density_));
    capacity = std::max(capacity, n);

    data_.assign(capacity, EMPTY_SLOT);
    if constexpr (kStoresValues) vals_.assign(capacity, V());

    std::vector<int64_t> positions(n);
    int64_t last_slot = -1;
    for (size_t i = 0; i < n; i++) {
      int64_t target = static_cast<int64_t>((static_cast<double>(i) * static_cast<double>(capacity)) /
                                             static_cast<double>(n));
      if (target <= last_slot) target = last_slot + 1;  // same nudge-on-collision trick as data_gen.py's skewed generator
      target = std::min<int64_t>(target, static_cast<int64_t>(capacity) - 1);
      data_[target] = sorted_keys[i];
      if constexpr (kStoresValues) {
        if (values != nullptr) vals_[target] = std::move((*values)[i]);
      }
      positions[i] = target;
      last_slot = target;
    }

    model_ = build_segmented_model(sorted_keys, positions, eps_, segment_cap(n));
    count_ = n;
    segment_changes_.assign(model_.segments.size(), 0);

    // A change counted against a segment moves each of its keys by at
    // most one slot, so refreshing a segment once its count reaches eps
    // keeps every key it was fit on within 2*eps of its prediction: eps
    // from the fit itself, and less than eps of drift since. The
    // threshold is tied to eps (the budget being protected), not to
    // array size.
    segment_threshold_ = std::max<size_t>(1, static_cast<size_t>(eps_));
    max_gap_scan_ = std::max<int64_t>(1, eps_ * 8);
  }

  // Pulls every real key (and its value) out of the array, in order.
  void collect_all(std::vector<int64_t>& keys, [[maybe_unused]] std::vector<V>& values) {
    keys.reserve(count_ + 1);
    if constexpr (kStoresValues) values.reserve(count_ + 1);
    for (size_t i = 0; i < data_.size(); i++) {
      if (data_[i] == EMPTY_SLOT) continue;
      keys.push_back(data_[i]);
      if constexpr (kStoresValues) values.push_back(std::move(vals_[i]));
    }
  }

  // Refreshes the layout and model from whatever keys are currently
  // in the array: the full rebuild, which local_rebalance() falls back to
  // only when the whole array is too full.
  void rebalance() {
    std::vector<int64_t> all_keys;
    std::vector<V> all_values;
    collect_all(all_keys, all_values);
    layout(all_keys, &all_values);
    rebalance_count_++;
  }

  void rebalance_with(int64_t new_key, [[maybe_unused]] V value) {
    std::vector<int64_t> all_keys;
    std::vector<V> all_values;
    collect_all(all_keys, all_values);
    const auto pos = std::lower_bound(all_keys.begin(), all_keys.end(), new_key) - all_keys.begin();
    all_keys.insert(all_keys.begin() + pos, new_key);
    if constexpr (kStoresValues) all_values.insert(all_values.begin() + pos, std::move(value));

    layout(all_keys, &all_values);
    rebalance_count_++;
  }

  // Refreshes every segment in [first, last] whose count has reached the
  // threshold. Highest index first: a refresh can split a segment into
  // several, which shifts the indices after it but never the ones before.
  void refresh_segments(size_t first, size_t last) {
    for (size_t si = last + 1; si-- > first;) {
      if (segment_changes_[si] < segment_threshold_) continue;
      const size_t full_rebuilds = rebalance_count_;
      local_rebalance(si);
      if (rebalance_count_ != full_rebuilds) return;  // fell back to a full rebuild, which refreshed everything
    }
  }

  // Refreshes segment si by re-laying a window of segments around it:
  // just si if its region has room, otherwise a wider window, the way a
  // packed-memory array rebalances (Itai, Konheim and Rodeh 1981; Bender,
  // Demaine and Farach-Colton 2000). The window doubles, to the aligned
  // pair of segments containing si, then four, then eight, until it's
  // under its density limit, so a crowded region borrows gaps from its
  // neighbors. Only a whole array over its limit gets a full rebuild,
  // which also makes the array bigger. A crowded region used to go
  // straight to a full rebuild, so a hot spot (inserts concentrated in
  // one key range) set one off every few dozen inserts.
  //
  // The last segment is the exception: its region ends where the array
  // does, so it can get more slots on the end instead, and no other key
  // has to move. That's what keeps appends cheap. Every key larger than
  // all the others lands there (an auto-increment id, say), and filling
  // that region used to set off a full rebuild every few hundred inserts,
  // 18-41 us per append at 1M keys. std::vector grows its storage
  // geometrically, so growing a little at a time is still amortized O(1)
  // per key.
  void local_rebalance(size_t si) {
    const size_t segment_total = model_.segments.size();
    size_t lo = si, hi = si;
    auto [begin, end] = region(lo, hi);
    size_t keys = count_keys(begin, end);
    if (hi + 1 == segment_total && too_dense(keys, end - begin, 0)) {
      const size_t wanted = std::max(keys + 1, static_cast<size_t>(std::ceil(keys / density_)));
      if (begin + wanted > data_.size()) {
        const size_t new_size = begin + wanted;
        // Grow the storage by a quarter at a time. Still geometric, so
        // still amortized O(1), but std::vector's own doubling could leave
        // half of it unused: 100.6 vs 92.4 bytes per row after loading 1M
        // rows by INSERT, at the same append speed.
        if (new_size > data_.capacity()) {
          const size_t grown = std::max(new_size, data_.size() + data_.size() / 4);
          data_.reserve(grown);
          if constexpr (kStoresValues) vals_.reserve(grown);
        }
        data_.resize(new_size, EMPTY_SLOT);
        if constexpr (kStoresValues) vals_.resize(data_.size());
      }
      end = data_.size();
    } else {
      for (size_t level = 1; too_dense(keys, end - begin, level - 1); level++) {
        if (lo == 0 && hi + 1 == segment_total) {
          rebalance();  // the whole array is too full: rebuild it bigger
          return;
        }
        const size_t width = size_t{1} << level;
        lo = si / width * width;
        hi = std::min(lo + width, segment_total) - 1;
        std::tie(begin, end) = region(lo, hi);
        keys = count_keys(begin, end);
      }
    }
    relay(lo, hi, begin, end, keys);
    local_rebalance_count_++;
  }

  // The slots holding segments lo..hi: from the first key at or above
  // segment lo's start up to the first key of segment hi + 1. The first
  // segment's region starts at slot 0 instead, since it also owns any key
  // smaller than its start, and the last one's runs to the end of the
  // array. Going by key value rather than looking up each anchor key
  // keeps a region well defined after its anchor has been removed; that
  // lookup used to fail and fall back to a full rebuild.
  std::pair<size_t, size_t> region(size_t lo, size_t hi) const {
    const size_t begin = (lo == 0) ? 0 : lower_bound_index(model_.segments[lo].start_key);
    const size_t end = (hi + 1 == model_.segments.size()) ? data_.size()
                                                          : lower_bound_index(model_.segments[hi + 1].start_key);
    return {begin, end};
  }

  size_t count_keys(size_t begin, size_t end) const {
    size_t n = 0;
    for (size_t idx = begin; idx < end; idx++) n += (data_[idx] != EMPTY_SLOT);
    return n;
  }

  // Is a window at this level (0 is one segment; each level up doubles
  // the window) too full to re-lay with room to spare? The limit falls
  // from kSegmentDensityLimit for one segment to kArrayDensityLimit for
  // the whole array. Wider windows get lower limits so that re-laying one
  // leaves every smaller window inside it well under its own limit, which
  // is what keeps the wider, costlier refreshes rare.
  bool too_dense(size_t keys, size_t slots, size_t level) const {
    if (keys == 0) return false;
    size_t height = 0;  // the level at which one window covers every segment
    while ((size_t{1} << height) < model_.segments.size()) height++;
    const double t = (height == 0) ? 0.0 : std::min(1.0, static_cast<double>(level) / static_cast<double>(height));
    const double limit = kSegmentDensityLimit - (kSegmentDensityLimit - kArrayDensityLimit) * t;
    return static_cast<double>(keys) >= limit * static_cast<double>(slots);
  }

  // Re-lays the keys in slots [begin, end) evenly across those slots and
  // replaces segments lo..hi with a fresh fit of them.
  void relay(size_t lo, size_t hi, size_t begin, size_t end, size_t keys) {
    std::vector<int64_t> local_keys;
    std::vector<V> local_values;
    local_keys.reserve(keys);
    if constexpr (kStoresValues) local_values.reserve(keys);
    for (size_t idx = begin; idx < end; idx++) {
      if (data_[idx] != EMPTY_SLOT) {
        local_keys.push_back(data_[idx]);
        if constexpr (kStoresValues) local_values.push_back(std::move(vals_[idx]));
      }
      data_[idx] = EMPTY_SLOT;
      if constexpr (kStoresValues) vals_[idx] = V();
    }

    const size_t slots = end - begin;
    std::vector<int64_t> local_positions(local_keys.size());
    int64_t last_slot = static_cast<int64_t>(begin) - 1;
    for (size_t i = 0; i < local_keys.size(); i++) {
      int64_t target = static_cast<int64_t>(begin) +
                       static_cast<int64_t>((static_cast<double>(i) * static_cast<double>(slots)) /
                                            static_cast<double>(local_keys.size()));
      if (target <= last_slot) target = last_slot + 1;
      target = std::min<int64_t>(target, static_cast<int64_t>(end) - 1);
      data_[target] = local_keys[i];
      if constexpr (kStoresValues) vals_[target] = std::move(local_values[i]);
      local_positions[i] = target;
      last_slot = target;
    }

    SegmentedModel local_model =
        build_segmented_model(local_keys, local_positions, eps_, segment_cap(local_keys.size()));
    model_.segments.erase(model_.segments.begin() + lo, model_.segments.begin() + hi + 1);
    model_.segments.insert(model_.segments.begin() + lo, local_model.segments.begin(), local_model.segments.end());
    segment_changes_.erase(segment_changes_.begin() + lo, segment_changes_.begin() + hi + 1);
    segment_changes_.insert(segment_changes_.begin() + lo, local_model.segments.size(), 0);
  }

  // The segment cap for fitting n keys. Where the cap is what splits
  // them, the pieces come out about equal: capping 4,160 keys at 4,096
  // would leave a 64-key segment at the end, small enough that a few
  // dozen inserts fill its region.
  static size_t segment_cap(size_t n) {
    const size_t pieces = (n + kMaxSegmentKeys - 1) / kMaxSegmentKeys;
    return (pieces <= 1) ? kMaxSegmentKeys : (n + pieces - 1) / pieces;
  }
};

using GappedArray = BasicGappedArray<NoValue>;
