// Phase 5 (extended in Phase 8, extended again for the DELETE stretch
// goal): wraps the Phase 3 gapped array (for fast key existence/
// position) with a real value store and the write-ahead log, giving
// genuine key -> value storage instead of just key existence. put()
// and remove() both log first, then apply -- on construction, any
// existing log is replayed before anything else happens, so state
// survives a restart.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "gapped_array.h"
#include "write_ahead_log.h"

class DurableStore {
 public:
  DurableStore(const std::string& wal_path, double density, int64_t eps)
      : wal_(wal_path), index_(GappedArray::build({}, density, eps)) {
    const std::vector<WalRecord> recovered = WriteAheadLog::replay(wal_path);
    for (const WalRecord& rec : recovered) {
      if (rec.type == WalRecordType::PUT) {
        // GappedArray::insert is already a no-op for a key that's
        // already indexed (the Phase 4 duplicate-key fix), so this
        // correctly handles both fresh keys and updates to existing
        // ones without any special-casing here -- last value written
        // for a given key wins, matching the log's own write order.
        index_.insert(rec.key);
        values_[rec.key] = rec.value;
      } else {
        index_.remove(rec.key);
        values_.erase(rec.key);
      }
    }
    recovered_entries_ = recovered.size();
  }

  // Durably stores key -> value: logged (and fsync'd) to disk first,
  // then applied in memory. If the process dies between these two
  // lines, the next startup's replay still recovers it.
  void put(int64_t key, const std::string& value) {
    wal_.log_put(key, value);
    index_.insert(key);
    values_[key] = value;
  }

  // Durably removes key: logged first (so a crash right after this
  // call still remembers the deletion on replay), then applied.
  // Returns false if the key wasn't present.
  bool remove(int64_t key) {
    size_t idx;
    if (!index_.search(key, idx)) return false;
    wal_.log_delete(key);
    index_.remove(key);
    values_.erase(key);
    return true;
  }

  bool get(int64_t key, std::string& out_value) const {
    size_t idx;
    if (!index_.search(key, idx)) return false;
    auto it = values_.find(key);
    if (it == values_.end()) return false;  // shouldn't happen if index_ and values_ stay consistent
    out_value = it->second;
    return true;
  }

  // Diagnostic-only variant for Phase 12's EXPLAIN -- reuses the
  // index's own instrumented search rather than duplicating its logic.
  GappedArray::SearchDiagnostics get_explain(int64_t key) const { return index_.search_explain(key); }

  // All (key, value) pairs with key in [lo, hi], inclusive, in sorted
  // order. Used for both a full table scan (bounds covering the
  // table's entire primary-key range) and a genuine BETWEEN range
  // scan (bounds from the WHERE clause) -- the executor doesn't need
  // two different code paths for those, just different bounds.
  std::vector<std::pair<int64_t, std::string>> range_scan(int64_t lo, int64_t hi) const {
    std::vector<std::pair<int64_t, std::string>> results;
    for (int64_t key : index_.range_scan_keys(lo, hi)) {
      auto it = values_.find(key);
      if (it != values_.end()) results.emplace_back(key, it->second);
    }
    return results;
  }

  // Number of log *entries* (PUT and DELETE combined) replayed on
  // startup -- not the number of live keys, since an update or a
  // delete writes an additional entry for the same key. Use size()
  // for the actual live key count.
  size_t recovered_entries() const { return recovered_entries_; }
  size_t size() const { return index_.size(); }

 private:
  WriteAheadLog wal_;
  GappedArray index_;
  std::unordered_map<int64_t, std::string> values_;
  size_t recovered_entries_ = 0;
};
