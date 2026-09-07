// Belady's algorithm: the provably optimal cache eviction policy --
// on a miss, evict whichever cached item's *next* use is farthest in
// the future (or never happens again). It needs to know the future,
// which is only available offline. Used two ways here: (1) to
// generate training labels for the Hawkeye-style classifier, since
// its whole idea is learning to approximate Belady's decisions using
// only information available online; (2) as the theoretical ceiling
// hit rate to compare LRU and the learned cache against.
#pragma once

#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

constexpr int64_t BELADY_NEVER_AGAIN = std::numeric_limits<int64_t>::max();

// For every position i, finds the index of trace[i]'s next occurrence
// (or BELADY_NEVER_AGAIN if it doesn't appear again). One backward
// pass, O(n).
std::vector<int64_t> next_occurrence_index(const std::vector<int64_t>& trace) {
  std::vector<int64_t> next_idx(trace.size());
  std::unordered_map<int64_t, int64_t> last_seen;
  for (int64_t i = static_cast<int64_t>(trace.size()) - 1; i >= 0; i--) {
    const int64_t key = trace[i];
    auto it = last_seen.find(key);
    next_idx[i] = (it == last_seen.end()) ? BELADY_NEVER_AGAIN : it->second;
    last_seen[key] = i;
  }
  return next_idx;
}

// Simulates Belady's optimal policy over a trace with full knowledge
// of the future, returning the achievable hit rate -- the ceiling no
// online policy (LRU, learned, or otherwise) can beat.
double belady_hit_rate(const std::vector<int64_t>& trace, size_t capacity) {
  const std::vector<int64_t> next_idx = next_occurrence_index(trace);
  std::unordered_map<int64_t, int64_t> cache;  // key -> its current next-occurrence index
  size_t hits = 0;

  for (size_t i = 0; i < trace.size(); i++) {
    const int64_t key = trace[i];
    auto it = cache.find(key);
    if (it != cache.end()) {
      hits++;
      it->second = next_idx[i];
      continue;
    }
    if (cache.size() >= capacity) {
      int64_t worst_key = -1;
      int64_t worst_dist = -1;
      for (const auto& [k, next] : cache) {
        if (next > worst_dist) {
          worst_dist = next;
          worst_key = k;
        }
      }
      cache.erase(worst_key);
    }
    cache[key] = next_idx[i];
  }
  return static_cast<double>(hits) / trace.size();
}
