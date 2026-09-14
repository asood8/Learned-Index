// Phase 4: a dedicated correctness test suite. Every previous phase's
// "does it actually work" checks were sample-based, run against real
// data. This phase specifically goes looking for the edge cases that
// random real-world data rarely exercises: empty structures, single
// elements, boundary keys, duplicate inserts, and larger randomized
// stress runs with fresh seeds.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "bplus_tree.h"
#include "durable_store.h"
#include "gapped_array.h"
#include "linear_model.h"
#include "segmented_model.h"

static int tests_run = 0;
static int tests_passed = 0;

void check(bool condition, const std::string& name) {
  tests_run++;
  if (condition) {
    tests_passed++;
    std::printf("  PASS  %s\n", name.c_str());
  } else {
    std::printf("  FAIL  %s\n", name.c_str());
  }
}

void test_bplus_tree() {
  std::printf("-- B+-tree --\n");

  BPlusTree empty_tree;
  int64_t val;
  check(!empty_tree.search(42, val), "empty tree: search returns not-found, no crash");

  BPlusTree one;
  one.insert(100, 7);
  check(one.search(100, val) && val == 7, "single insert: found with correct value");
  check(!one.search(101, val), "single insert: different key correctly not found");

  BPlusTree dup;
  dup.insert(5, 1);
  dup.insert(5, 2);  // duplicate key -- design is to overwrite, not add a second entry
  check(dup.search(5, val) && val == 2, "duplicate key insert overwrites value");
  check(dup.size() == 1, "duplicate key insert does not increase count");

  BPlusTree stress;
  std::vector<int64_t> keys;
  std::mt19937_64 rng(123);
  for (int i = 0; i < 50000; i++) keys.push_back(static_cast<int64_t>(rng()) & 0x7fffffffffff);
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
  for (size_t i = 0; i < keys.size(); i++) stress.insert(keys[i], static_cast<int64_t>(i));
  bool all_found = true;
  for (size_t i = 0; i < keys.size(); i++) {
    int64_t v;
    if (!stress.search(keys[i], v) || v != static_cast<int64_t>(i)) all_found = false;
  }
  check(all_found, "50k random inserts (new seed): every key found with correct value");
}

void test_linear_model() {
  std::printf("-- Phase 1: linear model --\n");

  LinearModel tiny_model = fit_linear_model({10});
  check(tiny_model.max_error == 0, "fitting on <2 points doesn't crash, returns safe default");

  std::vector<int64_t> keys;
  for (int64_t i = 0; i < 100000; i++) keys.push_back(i * 3);  // perfectly linear data
  LinearModel model = fit_linear_model(keys);
  bool every_point_within_bound = true;
  for (size_t i = 0; i < keys.size(); i++) {
    int64_t predicted = model.predict(keys[i]);
    if (std::llabs(predicted - static_cast<int64_t>(i)) > model.max_error) every_point_within_bound = false;
  }
  check(every_point_within_bound, "max_error genuinely bounds every training point's error");
  check(model.max_error <= 1, "perfectly linear data fits with ~zero error");
}

void test_segmented_model() {
  std::printf("-- Phase 2: segmented model --\n");

  SegmentedModel empty_model = build_segmented_model({}, 64);
  int64_t val;
  check(empty_model.segments.empty(), "building on zero keys produces zero segments");
  check(!segmented_search({}, empty_model, 5, val), "searching an empty segmented model returns false, no crash");

  std::vector<int64_t> one_key = {42};
  SegmentedModel one_model = build_segmented_model(one_key, 64);
  check(segmented_search(one_key, one_model, 42, val) && val == 0, "single-key segment found correctly");
  check(!segmented_search(one_key, one_model, 43, val), "single-key segment: absent key correctly not found");

  std::mt19937_64 rng(999);
  std::vector<int64_t> keys;
  int64_t k = 0;
  for (int i = 0; i < 200000; i++) {
    k += 1 + static_cast<int64_t>(rng() % 50);  // irregular spacing, on purpose
    keys.push_back(k);
  }
  const int64_t eps = 32;
  SegmentedModel model = build_segmented_model(keys, eps);
  bool every_point_within_eps = true;
  for (size_t i = 0; i < keys.size(); i++) {
    size_t seg_idx = find_segment_index(model.segments, keys[i]);
    const Segment& seg = model.segments[seg_idx];
    int64_t predicted = static_cast<int64_t>(std::llround(seg.slope * keys[i] + seg.intercept));
    if (std::llabs(predicted - static_cast<int64_t>(i)) > eps) every_point_within_eps = false;
  }
  check(every_point_within_eps, "the eps guarantee holds for every single point, not just a sample");

  bool all_found = true;
  for (size_t i = 0; i < keys.size(); i++) {
    int64_t v;
    if (!segmented_search(keys, model, keys[i], v) || v != static_cast<int64_t>(i)) all_found = false;
  }
  check(all_found, "irregularly-spaced data: every key found with correct position");
}

// An exact but slow way to decide whether one line can fit points
// [from, to) within eps. If any line fits, a corner of the region of
// fitting lines does too, and every corner is a line through two of the
// points' gap endpoints, so trying every such pair is enough.
static bool run_fits_brute_force(const std::vector<int64_t>& xs, const std::vector<int64_t>& ys, size_t from,
                                 size_t to, int64_t eps) {
  if (to - from == 1) return true;
  for (size_t a = from; a < to; a++) {
    for (size_t b = a + 1; b < to; b++) {
      for (int64_t da : {-eps, eps}) {
        for (int64_t db : {-eps, eps}) {
          const __int128 xa = xs[a], ya = ys[a] + da, xb = xs[b], yb = ys[b] + db;
          bool all_fit = true;
          for (size_t i = from; i < to && all_fit; i++) {
            const __int128 scaled = ya * (xb - xa) + (yb - ya) * (xs[i] - xa);  // the line at xs[i], times (xb - xa)
            all_fit = (ys[i] - eps) * (xb - xa) <= scaled && scaled <= (ys[i] + eps) * (xb - xa);
          }
          if (all_fit) return true;
        }
      }
    }
  }
  return false;
}

void test_optimal_segmentation() {
  std::printf("-- optimal segmentation --\n");

  // Greedy extension with an exact fits-or-not test gives the minimum
  // number of segments, so the fast version has to match it exactly.
  int mismatches = 0;
  for (int t = 0; t < 400; t++) {
    std::mt19937_64 rng(3000 + t);
    const int64_t eps = 1 + static_cast<int64_t>(rng() % 3);
    const size_t n = 5 + rng() % 40;
    std::vector<int64_t> xs(n), ys(n);
    int64_t x = 0, y = 0;
    for (size_t i = 0; i < n; i++) {
      x += 1 + static_cast<int64_t>(rng() % 8 == 0 ? rng() % 200 : rng() % 10);
      // positions that only go up (the real use), or wander both ways
      y += (t % 2 == 0) ? 1 + static_cast<int64_t>(rng() % 3) : static_cast<int64_t>(rng() % 11) - 5;
      xs[i] = x;
      ys[i] = y;
    }
    std::vector<int64_t> expected_starts;
    for (size_t from = 0; from < n;) {
      size_t to = from + 1;
      while (to < n && run_fits_brute_force(xs, ys, from, to + 1, eps)) to++;
      expected_starts.push_back(xs[from]);
      from = to;
    }
    std::vector<int64_t> starts;
    for (const Segment& s : build_optimal_segmented_model(xs, ys, eps).segments) starts.push_back(s.start_key);
    if (starts != expected_starts) mismatches++;
  }
  check(mismatches == 0, "400 small random inputs: exactly the segments a brute-force greedy search finds");

  // On a big input, every key's prediction -- computed in doubles, the
  // way lookups compute it -- has to land within eps.
  std::mt19937_64 rng(4242);
  std::vector<int64_t> keys;
  int64_t k = 0;
  for (int i = 0; i < 200000; i++) {
    k += 1 + static_cast<int64_t>(rng() % 50);
    if (rng() % 5000 == 0) k += static_cast<int64_t>(rng() % 100000000);  // occasional huge jumps
    keys.push_back(k);
  }
  const int64_t eps = 32;
  const SegmentedModel optimal = build_optimal_segmented_model(keys, eps);
  const SegmentedModel pinned = build_segmented_model(keys, eps);
  bool within_eps = true;
  for (size_t i = 0; i < keys.size(); i++) {
    const Segment& seg = optimal.segments[find_segment_index(optimal.segments, keys[i])];
    const int64_t predicted = static_cast<int64_t>(std::llround(seg.slope * static_cast<double>(keys[i]) + seg.intercept));
    if (std::llabs(predicted - static_cast<int64_t>(i)) > eps) within_eps = false;
  }
  check(within_eps, "optimal model: every one of 200k keys predicted within eps");
  bool all_found = true;
  for (size_t i = 0; i < keys.size(); i++) {
    int64_t v;
    if (!segmented_search(keys, optimal, keys[i], v) || v != static_cast<int64_t>(i)) all_found = false;
  }
  check(all_found, "optimal model: every key found at its exact position");
  std::printf("    (200k irregular keys, eps=32: %zu segments optimal, %zu pinned)\n", optimal.segments.size(),
              pinned.segments.size());
  check(optimal.segments.size() <= pinned.segments.size(), "optimal model never needs more segments than pinned");
}

void test_gapped_array() {
  std::printf("-- Phase 3: gapped array --\n");

  GappedArray empty_ga = GappedArray::build({}, 0.7, 64);
  size_t idx;
  check(!empty_ga.search(5, idx), "searching an empty gapped array returns false, no crash");

  GappedArray boot_ga = GappedArray::build({}, 0.7, 64);
  boot_ga.insert(7);  // inserting into a genuinely empty structure -- bootstrapping case
  check(boot_ga.search(7, idx), "insert into an empty gapped array bootstraps correctly");

  GappedArray dup_ga = GappedArray::build({10, 20, 30}, 0.7, 64);
  size_t before = dup_ga.size();
  dup_ga.insert(20);  // key already present
  check(dup_ga.size() == before, "inserting a duplicate key is a no-op, not a second copy");

  std::vector<int64_t> base;
  for (int64_t i = 0; i < 500000; i++) base.push_back(i * 2);
  GappedArray ga = GappedArray::build(base, 0.7, 64);

  std::mt19937_64 rng(2024);  // deliberately different seed than Phase 3's own test
  std::vector<int64_t> to_insert;
  for (int i = 0; i < 100000; i++) to_insert.push_back(1 + i * 2);  // fills in every odd gap
  std::shuffle(to_insert.begin(), to_insert.end(), rng);
  for (int64_t key : to_insert) ga.insert(key);

  bool all_found = true;
  for (int64_t key : base) {
    if (!ga.search(key, idx)) all_found = false;
  }
  for (int64_t key : to_insert) {
    if (!ga.search(key, idx)) all_found = false;
  }
  check(all_found, "100k shuffled-order inserts (new seed): every original and inserted key found");
}

// The secondary-index bug, reproduced at the storage layer with no SQL
// involved: composite (value << 28 | pk) keys form a staircase, an
// outlier value stretches a segment's key range, and new keys are
// inserted with values the model was never trained on. Checked only
// through the public API, against a std::set ground truth: a full-range
// scan catches any key placed out of sorted order, and a per-value scan
// catches a lower bound that lands past the real first match
// (range_scan_keys stops at the first key above its upper bound).
void test_untrained_keys() {
  std::printf("-- untrained keys in outlier-stretched segments --\n");
  auto composite = [](int64_t value, int64_t pk) { return (value << 28) | pk; };
  auto scan_all = [](const GappedArray& g) { return g.range_scan_keys(INT64_MIN, INT64_MAX - 1); };

  // the exact shape of the failing executor test: ages 20..69, one row
  // updated to 99, then new rows at ages in between
  std::set<int64_t> truth;
  for (int64_t i = 0; i < 1000; i++) truth.insert(composite(20 + i % 50, i));
  truth.insert(composite(99, 1000));
  GappedArray ga = GappedArray::build(std::vector<int64_t>(truth.begin(), truth.end()), 0.7, 64);
  bool each_found = true;
  for (int64_t v = 70; v <= 98; v++) {
    const int64_t k = composite(v, 5000 + v);
    ga.insert(k);
    truth.insert(k);
    if (ga.range_scan_keys(composite(v, 0), composite(v, (1 << 28) - 1)) != std::vector<int64_t>{k}) {
      each_found = false;
    }
  }
  check(each_found, "values inserted between the data and an outlier: each found by its own range scan");
  check(scan_all(ga) == std::vector<int64_t>(truth.begin(), truth.end()),
        "values inserted between the data and an outlier: full scan still sorted and complete");

  int bad_trials = 0;
  for (int t = 0; t < 100; t++) {
    std::mt19937_64 rng(5000 + t);
    const int64_t eps = std::vector<int64_t>{4, 16, 64}[rng() % 3];
    const int64_t distinct = 20 + static_cast<int64_t>(rng() % 50);
    std::set<int64_t> keys;
    int64_t pk = 1;
    for (int64_t v = 0; v < distinct; v++) {
      const int rows = 1 + static_cast<int>(rng() % 40);
      for (int r = 0; r < rows; r++) keys.insert(composite(v, pk++));
    }
    const int outliers = 1 + static_cast<int>(rng() % 3);
    for (int o = 0; o < outliers; o++) keys.insert(composite(distinct + 20 + static_cast<int64_t>(rng() % 5000), pk++));

    GappedArray g = GappedArray::build(std::vector<int64_t>(keys.begin(), keys.end()), 0.7, eps);
    bool ok = true;
    for (int op = 0; op < 300 && ok; op++) {
      if (rng() % 5 == 0) {
        auto it = std::next(keys.begin(), static_cast<long>(rng() % keys.size()));
        g.remove(*it);
        keys.erase(it);
      } else {
        const int64_t k = composite(static_cast<int64_t>(rng() % (distinct + 5000)), pk++);
        g.insert(k);
        keys.insert(k);
      }
      ok = scan_all(g) == std::vector<int64_t>(keys.begin(), keys.end());
    }
    size_t idx;
    for (int64_t k : keys) {
      if (!g.search(k, idx)) ok = false;
    }
    if (g.search(composite(999999, 0), idx)) ok = false;
    if (!ok) bad_trials++;
  }
  check(bad_trials == 0, "100 randomized outlier trials (inserts + removes): scans and lookups match ground truth");
}

// The gapped array can carry a value for each key, and DurableStore keeps
// its rows that way. Random inserts, overwrites, and removes are checked
// against a std::map, with a small eps in half the trials so local and
// full rebalances happen constantly, to make sure every value stays with
// its key through shifts and rebalances.
void test_gapped_array_values() {
  std::printf("-- gapped array with values --\n");
  using ValueArray = BasicGappedArray<std::string>;
  using Pairs = std::vector<std::pair<int64_t, std::string>>;
  int bad_trials = 0;
  size_t rebalances = 0;
  for (int t = 0; t < 30; t++) {
    std::mt19937_64 rng(9000 + t);
    ValueArray ga = ValueArray::build({}, 0.7, (t % 2 == 0) ? 4 : 64);
    const int64_t spread = (t % 3 == 0) ? 1000003 : 1;  // some trials use widely spaced keys
    std::map<int64_t, std::string> truth;
    bool ok = true;
    for (int op = 0; op < 3000 && ok; op++) {
      const int64_t k = static_cast<int64_t>(rng() % 2000) * spread;
      if (rng() % 4 == 0) {
        ga.remove(k);
        truth.erase(k);
      } else {
        const std::string v = "value_" + std::to_string(rng() % 100000);
        ga.insert(k, v);
        truth[k] = v;
      }
      if (op % 100 == 99) ok = ga.range_scan(INT64_MIN, INT64_MAX - 1) == Pairs(truth.begin(), truth.end());
    }
    std::string v;
    for (const auto& [k, want] : truth) {
      if (!ga.get(k, v) || v != want) ok = false;
    }
    if (ga.get(-1, v) || ga.size() != truth.size()) ok = false;
    rebalances += ga.rebalance_count() + ga.local_rebalance_count();
    if (!ok) bad_trials++;
  }
  check(bad_trials == 0, "30 randomized trials: every value stays with its key through inserts, overwrites, "
                         "removes, and rebalances");
  check(rebalances > 0, "rebalances actually happened during those trials");
}

// Checkpointing: the whole store goes into a snapshot and the log is
// emptied. Every case restarts from disk and compares the result with a
// std::map that saw the same writes.
void test_checkpointing() {
  std::printf("-- checkpointing --\n");
  const std::string path = "results/test_checkpoint.wal";
  using Pairs = std::vector<std::pair<int64_t, std::string>>;
  std::map<int64_t, std::string> truth;
  auto put = [&](DurableStore& s, int64_t k, const std::string& v) {
    s.put(k, v);
    truth[k] = v;
  };
  auto del = [&](DurableStore& s, int64_t k) {
    s.remove(k);
    truth.erase(k);
  };
  auto matches_truth = [&](const DurableStore& s) {
    return s.range_scan(INT64_MIN, INT64_MAX - 1) == Pairs(truth.begin(), truth.end());
  };
  auto read_bytes = [](const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  };
  auto write_bytes = [](const std::string& p, const std::string& bytes) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << bytes;
  };

  DurableStore::destroy(path);
  {
    DurableStore s(path, 0.7, 64, 0);  // automatic checkpoints off for these cases
    for (int64_t k = 1; k <= 300; k++) put(s, k, "first_" + std::to_string(k));
    for (int64_t k = 1; k <= 300; k += 3) del(s, k);
    s.checkpoint();
    check(std::filesystem::file_size(path) == WriteAheadLog::MAGIC_LEN && std::filesystem::exists(path + ".snapshot"),
          "a checkpoint writes a snapshot and empties the log");
    // after the checkpoint: overwrite some keys, delete some, add new ones
    for (int64_t k = 2; k <= 300; k += 3) put(s, k, "second_" + std::to_string(k));
    for (int64_t k = 3; k <= 60; k += 3) del(s, k);
    for (int64_t k = 301; k <= 320; k++) put(s, k, "second_" + std::to_string(k));
  }
  {
    DurableStore s(path, 0.7, 64, 0);
    check(s.snapshot_entries() == 200 && matches_truth(s),
          "restart = the snapshot plus the overwrites, deletes, and new keys logged after it");
  }

  // A crash after the new snapshot is renamed into place but before the
  // log is emptied leaves both behind. Replaying that log on top of the
  // snapshot has to end in the same state.
  {
    std::string unemptied_log;
    {
      DurableStore s(path, 0.7, 64, 0);
      unemptied_log = read_bytes(path);
      s.checkpoint();
    }
    write_bytes(path, unemptied_log);
    DurableStore s(path, 0.7, 64, 0);
    check(matches_truth(s), "a crash between writing the snapshot and emptying the log loses nothing");
  }

  // A crash partway through writing a snapshot leaves only the temporary
  // file. Startup ignores it, and the next checkpoint replaces it.
  write_bytes(path + ".snapshot.tmp", "half of a snapshot");
  {
    DurableStore s(path, 0.7, 64, 0);
    bool ok = matches_truth(s);
    put(s, 999, "after");
    s.checkpoint();
    ok = ok && matches_truth(s);
    check(ok, "a leftover temporary snapshot is ignored, and the next checkpoint writes over it");
  }
  {
    DurableStore s(path, 0.7, 64, 0);
    check(matches_truth(s), "...and the state after that checkpoint comes back intact");
  }

  // A damaged snapshot has to be refused: skipping it would quietly lose
  // everything the emptied log no longer has.
  {
    const std::string snap = path + ".snapshot";
    const auto middle = static_cast<std::streamoff>(std::filesystem::file_size(snap) / 2);
    std::fstream f(snap, std::ios::binary | std::ios::in | std::ios::out);
    f.seekg(middle);
    const char byte = static_cast<char>(f.get());
    f.seekp(middle);
    f.put(static_cast<char>(byte ^ 0xFF));
  }
  bool refused = false;
  try {
    DurableStore s(path, 0.7, 64, 0);
  } catch (const std::exception&) {
    refused = true;
  }
  check(refused, "a damaged snapshot is refused rather than silently skipped");

  // Automatic checkpoints: one every 50 writes.
  DurableStore::destroy(path);
  truth.clear();
  {
    DurableStore s(path, 0.7, 64, 50);
    for (int64_t i = 0; i < 175; i++) put(s, i % 60, "auto_" + std::to_string(i));
    check(s.checkpoints() == 3, "automatic checkpoints: 3 of them after 175 writes at one per 50");
  }
  {
    DurableStore s(path, 0.7, 64, 50);
    check(matches_truth(s) && s.snapshot_entries() == 60 && s.recovered_entries() == 25,
          "restart loads the last snapshot and replays only the 25 writes after it");
  }
  DurableStore::destroy(path);
}

// Crash recovery for the write-ahead log. Each case writes a real log
// through DurableStore, damages the file the way a crash or a bad disk
// can, and restarts from it.
void test_wal_recovery() {
  std::printf("-- write-ahead log crash recovery --\n");
  const std::string path = "results/test_wal_recovery.wal";
  auto fresh_log = [&](int n) {
    std::remove(path.c_str());
    DurableStore s(path, 0.7, 64);
    for (int k = 1; k <= n; k++) s.put(k, "value_" + std::to_string(k));
  };
  // how many of keys 1..n come back with exactly the value written
  auto intact_count = [](DurableStore& s, int n) {
    int count = 0;
    std::string v;
    for (int k = 1; k <= n; k++) {
      if (s.get(k, v) && v == "value_" + std::to_string(k)) count++;
    }
    return count;
  };

  // A crash in the middle of an append leaves a torn last record. The
  // restart after it has to drop that record, and writes made after
  // that restart have to survive the next one.
  fresh_log(100);
  std::filesystem::resize_file(path, std::filesystem::file_size(path) - 3);
  {
    DurableStore s(path, 0.7, 64);
    check(s.size() == 99 && intact_count(s, 99) == 99, "torn last record: dropped, everything before it kept");
    s.put(1000, "written_after_recovery");
  }
  {
    DurableStore s(path, 0.7, 64);
    std::string v;
    check(s.get(1000, v) && v == "written_after_recovery" && s.size() == 100 && intact_count(s, 99) == 99,
          "torn last record: writes made after recovery survive the next restart");
  }

  // Some filesystems can leave zero-filled blocks at the end of a file
  // after a crash. Zeros must not be read back as records.
  fresh_log(10);
  {
    std::ofstream f(path, std::ios::binary | std::ios::app);
    f << std::string(64, '\0');
  }
  {
    DurableStore s(path, 0.7, 64);
    std::string v;
    check(s.size() == 10 && !s.get(0, v), "zero-filled tail is not read back as records");
  }

  // One damaged byte in the middle of the log. Whatever comes back has
  // to be exactly what was written; nothing past the damage is trusted.
  fresh_log(100);
  {
    const auto middle = static_cast<std::streamoff>(std::filesystem::file_size(path) / 2);
    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
    f.seekg(middle);
    const char byte = static_cast<char>(f.get());
    f.seekp(middle);
    f.put(static_cast<char>(byte ^ 0xFF));
  }
  {
    DurableStore s(path, 0.7, 64);
    check(s.size() < 100 && intact_count(s, 100) == static_cast<int>(s.size()),
          "damaged byte mid-log: no wrong values, nothing after the damage kept");
  }

  // A file that isn't a log in the current format has to be refused
  // and left alone -- not treated as one long torn record and cut to
  // nothing.
  std::remove(path.c_str());
  {
    std::ofstream f(path, std::ios::binary);
    f << "definitely not a write-ahead log";
  }
  const auto size_before = std::filesystem::file_size(path);
  bool refused = false;
  try {
    DurableStore s(path, 0.7, 64);
  } catch (const std::exception&) {
    refused = true;
  }
  check(refused && std::filesystem::file_size(path) == size_before,
        "a file in an unrecognized format is refused and left untouched");
  std::remove(path.c_str());
}

int main() {
  test_bplus_tree();
  test_linear_model();
  test_segmented_model();
  test_optimal_segmentation();
  test_gapped_array();
  test_untrained_keys();
  test_gapped_array_values();
  test_checkpointing();
  test_wal_recovery();

  std::printf("\n%d/%d tests passed\n", tests_passed, tests_run);
  return (tests_passed == tests_run) ? 0 : 1;
}
