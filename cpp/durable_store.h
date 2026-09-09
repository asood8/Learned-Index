// Phase 5 (extended in Phase 8): wraps the Phase 3 gapped array (for
// fast key existence/position) with a real value store and the
// write-ahead log, giving genuine key -> value storage instead of
// just key existence. put() logs first, then applies -- on
// construction, any existing log is replayed before anything else
// happens, so state survives a restart.
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
    std::vector<std::pair<int64_t, std::string>> recovered = WriteAheadLog::replay(wal_path);
    for (auto& [key, value] : recovered) {
      // GappedArray::insert is already a no-op for a key that's
      // already indexed (the Phase 4 duplicate-key fix), so this
      // correctly handles both fresh keys and updates to existing
      // ones without any special-casing here -- last value written
      // for a given key wins, matching the log's own write order.
      index_.insert(key);
      values_[key] = value;
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

  bool get(int64_t key, std::string& out_value) const {
    size_t idx;
    if (!index_.search(key, idx)) return false;
    auto it = values_.find(key);
    if (it == values_.end()) return false;  // shouldn't happen if index_ and values_ stay consistent
    out_value = it->second;
    return true;
  }

  // Number of log *entries* replayed on startup -- not necessarily
  // the number of unique keys, since an update writes a second entry
  // for the same key. Use size() for the actual key count.
  size_t recovered_entries() const { return recovered_entries_; }
  size_t size() const { return index_.size(); }

 private:
  WriteAheadLog wal_;
  GappedArray index_;
  std::unordered_map<int64_t, std::string> values_;
  size_t recovered_entries_ = 0;
};
