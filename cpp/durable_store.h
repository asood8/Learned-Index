// Phase 5: wraps the Phase 3 gapped array with a write-ahead log.
// put() logs first, then applies -- on construction, any existing
// log is replayed into a fresh gapped array before anything else
// happens, so state survives a restart.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "gapped_array.h"
#include "write_ahead_log.h"

class DurableStore {
 public:
  DurableStore(const std::string& wal_path, double density, int64_t eps)
      : wal_(wal_path), data_(GappedArray::build({}, density, eps)) {
    std::vector<int64_t> recovered = WriteAheadLog::replay(wal_path);
    for (int64_t key : recovered) data_.insert(key);
    recovered_count_ = recovered.size();
  }

  // Durably inserts key: logged (and fsync'd) to disk first, then
  // applied to the in-memory structure. If the process dies between
  // these two lines, the next startup's replay still recovers it.
  void put(int64_t key) {
    wal_.log_insert(key);
    data_.insert(key);
  }

  bool get(int64_t key, size_t& out_index) const { return data_.search(key, out_index); }

  size_t recovered_count() const { return recovered_count_; }
  size_t size() const { return data_.size(); }

 private:
  WriteAheadLog wal_;
  GappedArray data_;
  size_t recovered_count_ = 0;
};
