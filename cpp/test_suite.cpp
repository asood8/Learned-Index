// Phase 4: a dedicated correctness test suite. Every previous phase's
// "does it actually work" checks were sample-based, run against real
// data. This phase specifically goes looking for the edge cases that
// random real-world data rarely exercises: empty structures, single
// elements, boundary keys, duplicate inserts, and larger randomized
// stress runs with fresh seeds.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "bplus_tree.h"
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

int main() {
  test_bplus_tree();
  test_linear_model();
  test_segmented_model();
  test_gapped_array();

  std::printf("\n%d/%d tests passed\n", tests_passed, tests_run);
  return (tests_passed == tests_run) ? 0 : 1;
}
