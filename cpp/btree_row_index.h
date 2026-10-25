// tlx's B+-tree, wrapped in the interface DurableStore uses from the
// gapped array, so that the whole database (parser, executor, catalog,
// write-ahead log, snapshots) can be built on either index with nothing
// else changing. Build with -DLIDB_BTREE_INDEX to use it; see
// durable_store.h. It exists to measure what the learned index is worth
// inside the engine, rather than on its own.
//
// Rows are stored in the tree's leaves, next to their keys, the same way
// the gapped array keeps them in a parallel array.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <tlx/container/btree_map.hpp>

#include "gapped_array.h"  // GappedSearchDiagnostics, which EXPLAIN reads

class BTreeRowIndex {
 public:
  using SearchDiagnostics = GappedSearchDiagnostics;

  // Same signatures as BasicGappedArray::build(); the density and error
  // bound mean nothing to a B+-tree.
  static BTreeRowIndex build(const std::vector<int64_t>& sorted_keys, double, int64_t) {
    return build(sorted_keys, std::vector<std::string>(sorted_keys.size()), 0.0, 0);
  }
  static BTreeRowIndex build(const std::vector<int64_t>& sorted_keys, std::vector<std::string> values, double,
                             int64_t) {
    // tlx's bulk_load copies every element it's given, and copying a row
    // allocates it a second time, so the keys go in with empty rows and
    // the rows are moved into place afterwards.
    std::vector<std::pair<int64_t, std::string>> pairs;
    pairs.reserve(sorted_keys.size());
    for (int64_t key : sorted_keys) pairs.emplace_back(key, std::string());
    BTreeRowIndex index;
    index.tree_.bulk_load(pairs.begin(), pairs.end());
    size_t i = 0;
    for (auto it = index.tree_.begin(); it != index.tree_.end(); ++it) it->second = std::move(values[i++]);
    return index;
  }

  bool search(int64_t key, size_t& out_index) const {
    if (tree_.find(key) == tree_.end()) return false;
    out_index = 0;  // a tree has no slot numbers; callers only use this as "found"
    return true;
  }

  bool get(int64_t key, std::string& out) const {
    const auto it = tree_.find(key);
    if (it == tree_.end()) return false;
    out = it->second;
    return true;
  }

  // Inserts, or replaces the value if the key is already there.
  void insert(int64_t key, std::string value) { tree_[key] = std::move(value); }

  bool remove(int64_t key) { return tree_.erase(key) > 0; }

  // EXPLAIN only gets found / not found from a tree.
  SearchDiagnostics search_explain(int64_t key) const {
    SearchDiagnostics diag;
    diag.found = tree_.find(key) != tree_.end();
    return diag;
  }

  std::vector<std::pair<int64_t, std::string>> range_scan(int64_t lo, int64_t hi) const {
    std::vector<std::pair<int64_t, std::string>> results;
    for (auto it = tree_.lower_bound(lo); it != tree_.end() && it->first <= hi; ++it) {
      results.emplace_back(it->first, it->second);
    }
    return results;
  }

  size_t size() const { return tree_.size(); }

 private:
  tlx::btree_map<int64_t, std::string> tree_;
};
