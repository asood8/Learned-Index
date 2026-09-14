// Phase 5 (extended in Phase 8, extended again for the DELETE stretch
// goal): the gapped array plus the write-ahead log, giving real
// key -> value storage. put() and remove() both log first, then apply
// -- on construction, any existing log is replayed before anything
// else happens, so state survives a restart.
//
// Rows are stored in the gapped array itself, next to their keys. They
// used to live in a separate std::unordered_map, with the gapped array
// only tracking which keys existed and in what order. That made the
// learned index optional for point lookups: get() searched it and then
// the hash map, and the hash map could have answered on its own. Now
// the learned index is the only place a row lives.
//
// Checkpoints. Without them the log only grows, and every startup
// replays every write ever made, even for a store that isn't getting
// bigger because the same rows keep being updated. checkpoint() writes
// the whole store to a snapshot file (snapshot.h) and then empties the
// log, and startup loads the snapshot in one pass before replaying
// whatever the log has gathered since. One also happens automatically
// every `checkpoint_every` writes (0 turns that off). A checkpoint
// makes that one write slow, since it rewrites the whole store;
// a production database would do it in the background.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "gapped_array.h"
#include "snapshot.h"
#include "write_ahead_log.h"

class DurableStore {
 public:
  using RowArray = BasicGappedArray<std::string>;

  DurableStore(const std::string& wal_path, double density, int64_t eps, size_t checkpoint_every = 100000)
      : wal_(wal_path),
        snapshot_path_(wal_path + ".snapshot"),
        checkpoint_every_(checkpoint_every),
        index_(RowArray::build({}, density, eps)) {
    std::vector<int64_t> keys;
    std::vector<std::string> values;
    if (read_snapshot(snapshot_path_, keys, values)) {
      snapshot_entries_ = keys.size();
      index_ = RowArray::build(keys, std::move(values), density, eps);
    }
    const std::vector<WalRecord> recovered = WriteAheadLog::replay(wal_path);
    for (const WalRecord& rec : recovered) {
      if (rec.type == WalRecordType::PUT) {
        // insert() replaces the value when the key is already present,
        // so a key that was written several times ends up with the last
        // value logged for it, matching the log's own order.
        index_.insert(rec.key, rec.value);
      } else {
        index_.remove(rec.key);
      }
    }
    recovered_entries_ = recovered.size();
    writes_since_checkpoint_ = recovered.size();
  }

  // Deletes a store's files (log, snapshot, and any leftover temporary
  // snapshot), for tests and demos that want a fresh start. Deleting
  // only the log would leave an old snapshot behind to be loaded.
  static void destroy(const std::string& wal_path) {
    std::remove(wal_path.c_str());
    std::remove((wal_path + ".snapshot").c_str());
    std::remove((wal_path + ".snapshot.tmp").c_str());
  }

  // Durably stores key -> value: logged (and fsync'd) to disk first,
  // then applied in memory. If the process dies between these two
  // lines, the next startup's replay still recovers it.
  void put(int64_t key, const std::string& value) {
    wal_.log_put(key, value);
    index_.insert(key, value);
    after_write();
  }

  // Durably removes key: logged first (so a crash right after this
  // call still remembers the deletion on replay), then applied.
  // Returns false if the key wasn't present.
  bool remove(int64_t key) {
    size_t idx;
    if (!index_.search(key, idx)) return false;
    wal_.log_delete(key);
    index_.remove(key);
    after_write();
    return true;
  }

  // Writes everything to the snapshot file, then empties the log. The
  // order is what makes it crash-safe. The snapshot goes to a temporary
  // file and is renamed into place only once it's complete, and the log
  // is emptied only after that. A crash in between leaves the new
  // snapshot plus a log of writes it already contains, and replaying
  // those on top of it ends in the same state, since each key ends up
  // with whatever its last logged write said.
  void checkpoint() {
    write_snapshot(snapshot_path_, index_.range_scan(INT64_MIN, INT64_MAX - 1));
    wal_.reset();
    writes_since_checkpoint_ = 0;
    checkpoints_++;
  }

  bool get(int64_t key, std::string& out_value) const { return index_.get(key, out_value); }

  // Diagnostic-only variant for Phase 12's EXPLAIN -- reuses the
  // index's own instrumented search rather than duplicating its logic.
  GappedSearchDiagnostics get_explain(int64_t key) const { return index_.search_explain(key); }

  // All (key, value) pairs with key in [lo, hi], inclusive, in sorted
  // order. Used for both a full table scan (bounds covering the
  // table's entire primary-key range) and a genuine BETWEEN range
  // scan (bounds from the WHERE clause) -- the executor doesn't need
  // two different code paths for those, just different bounds.
  std::vector<std::pair<int64_t, std::string>> range_scan(int64_t lo, int64_t hi) const {
    return index_.range_scan(lo, hi);
  }

  // Number of log *entries* (PUT and DELETE combined) replayed on
  // startup -- not the number of live keys, since an update or a
  // delete writes an additional entry for the same key. Use size()
  // for the actual live key count.
  size_t recovered_entries() const { return recovered_entries_; }
  size_t snapshot_entries() const { return snapshot_entries_; }  // rows loaded from a snapshot on startup
  size_t checkpoints() const { return checkpoints_; }            // checkpoints taken by this instance
  uint64_t log_bytes() const { return wal_.size_bytes(); }
  size_t size() const { return index_.size(); }

 private:
  WriteAheadLog wal_;
  std::string snapshot_path_;
  size_t checkpoint_every_;
  RowArray index_;
  size_t recovered_entries_ = 0;
  size_t snapshot_entries_ = 0;
  size_t writes_since_checkpoint_ = 0;
  size_t checkpoints_ = 0;

  void after_write() {
    if (checkpoint_every_ > 0 && ++writes_since_checkpoint_ >= checkpoint_every_) checkpoint();
  }
};
