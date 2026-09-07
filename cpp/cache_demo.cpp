// Trains the Hawkeye-style classifier on one Zipfian trace, then
// evaluates LRU, the learned cache, and Belady's optimal (the
// unreachable ceiling, since it needs the future) on a *separate*
// trace with a different seed -- training and evaluating on the same
// trace would be the caching equivalent of grading your own homework.
#include <cstdio>
#include <vector>

#include "belady.h"
#include "hawkeye_cache.h"
#include "lru_cache.h"
#include "zipf_generator.h"

int main() {
  const size_t universe_size = 10000;
  const double skew = 1.0;
  const size_t capacity = 500;
  const size_t trace_length = 200000;

  ZipfGenerator train_gen(universe_size, skew, /*seed=*/1);
  std::vector<int64_t> train_trace = train_gen.generate_trace(trace_length);

  ZipfGenerator eval_gen(universe_size, skew, /*seed=*/2);  // different seed: genuinely held out
  std::vector<int64_t> eval_trace = eval_gen.generate_trace(trace_length);

  std::printf("universe: %zu keys, skew=%.1f, cache capacity: %zu (%.1f%% of universe)\n", universe_size,
              skew, capacity, 100.0 * capacity / universe_size);

  std::vector<CacheTrainingExample> training_data = build_training_data(train_trace, capacity);
  HawkeyeClassifier model = HawkeyeClassifier::train(training_data, /*epochs=*/5, /*lr=*/0.1, /*seed=*/3);
  std::printf("trained on %zu labeled accesses from a separate training trace\n\n", training_data.size());

  LRUCache lru(capacity);
  size_t lru_hits = 0;
  for (size_t i = 0; i < eval_trace.size(); i++) {
    if (lru.access(eval_trace[i], i)) lru_hits++;
  }
  const double lru_hit_rate = static_cast<double>(lru_hits) / eval_trace.size();

  HawkeyeCache learned(capacity, model);
  size_t learned_hits = 0;
  for (size_t i = 0; i < eval_trace.size(); i++) {
    if (learned.access(eval_trace[i], i)) learned_hits++;
  }
  const double learned_hit_rate = static_cast<double>(learned_hits) / eval_trace.size();

  const double opt_hit_rate = belady_hit_rate(eval_trace, capacity);

  std::printf("hit rate on a held-out evaluation trace (%zu accesses):\n", eval_trace.size());
  std::printf("  LRU:              %.2f%%\n", lru_hit_rate * 100.0);
  std::printf("  learned (Hawkeye): %.2f%%\n", learned_hit_rate * 100.0);
  std::printf("  Belady's optimal:  %.2f%%  (ceiling -- needs the future, unreachable online)\n",
              opt_hit_rate * 100.0);

  const double gap_closed = (learned_hit_rate - lru_hit_rate) / (opt_hit_rate - lru_hit_rate) * 100.0;
  std::printf("\nlearned cache closed %.1f%% of the gap between LRU and the theoretical ceiling\n", gap_closed);

  return 0;
}
