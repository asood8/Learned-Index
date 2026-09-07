// Standard least-recently-used cache: on a miss with the cache full,
// evict whichever entry was touched longest ago. Uses a linear scan
// over the cache to find the LRU entry rather than a proper O(1)
// linked-list implementation -- a deliberate simplification, since
// this comparison is about eviction *decision quality* (LRU vs.
// learned vs. Belady's optimal), not raw cache-management speed, and
// our cache sizes (hundreds of entries) make the scan cost trivial.
#pragma once

#include <cstdint>
#include <unordered_map>

class LRUCache {
 public:
  explicit LRUCache(size_t capacity) : capacity_(capacity) {}

  // Returns true on a hit, false on a miss (and performs the
  // eviction if the cache was full).
  bool access(int64_t key, size_t current_time) {
    auto it = entries_.find(key);
    if (it != entries_.end()) {
      it->second = current_time;
      return true;
    }
    if (entries_.size() >= capacity_) {
      int64_t oldest_key = -1;
      size_t oldest_time = SIZE_MAX;
      for (const auto& [k, last_used] : entries_) {
        if (last_used < oldest_time) {
          oldest_time = last_used;
          oldest_key = k;
        }
      }
      entries_.erase(oldest_key);
    }
    entries_[key] = current_time;
    return false;
  }

 private:
  size_t capacity_;
  std::unordered_map<int64_t, size_t> entries_;  // key -> last access time
};
