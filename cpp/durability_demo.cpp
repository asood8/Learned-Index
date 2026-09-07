// Phase 5 demo: this doesn't just claim durability works -- it forces
// a real restart. A DurableStore is built, written to, and destroyed
// (closing its file descriptor, discarding all in-memory state).
// Then a brand new DurableStore is constructed from the exact same
// WAL file on disk, the same way it would be after a real process
// crash and restart, and every originally-inserted key is checked
// for real. Also measures the actual cost of durability: put() with
// a real fsync vs. a plain in-memory insert with none.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "durable_store.h"
#include "gapped_array.h"

int main() {
  const std::string wal_path = "results/phase5_demo.wal";
  std::remove(wal_path.c_str());  // clean slate for a reproducible run

  const int64_t eps = 64;
  const double density = 0.7;
  const int n_inserts = 5000;

  std::mt19937_64 rng(55);
  std::vector<int64_t> keys;
  {
    int64_t k = 0;
    for (int i = 0; i < n_inserts; i++) {
      k += 1 + static_cast<int64_t>(rng() % 100);
      keys.push_back(k);
    }
  }

  double put_ns;
  {
    DurableStore store(wal_path, density, eps);
    std::printf("fresh store: recovered %zu keys from an empty log (expected 0)\n",
                store.recovered_count());

    auto t0 = std::chrono::steady_clock::now();
    for (int64_t key : keys) store.put(key);
    auto t1 = std::chrono::steady_clock::now();
    put_ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / n_inserts;

    std::printf("wrote %d keys via put() (each logged + fsync'd before applying)\n", n_inserts);
    // store goes out of scope here -- destructor closes the WAL file
    // descriptor, and all in-memory state (the GappedArray) is gone
  }

  std::printf("\n-- simulated restart: constructing a brand new DurableStore --\n");
  bool all_recovered_correctly;
  {
    DurableStore restarted(wal_path, density, eps);
    std::printf("recovered %zu keys by replaying the log\n", restarted.recovered_count());

    all_recovered_correctly = (restarted.recovered_count() == static_cast<size_t>(n_inserts));
    for (int64_t key : keys) {
      size_t idx;
      if (!restarted.get(key, idx)) all_recovered_correctly = false;
    }
  }

  if (!all_recovered_correctly) {
    std::fprintf(stderr, "DURABILITY CHECK FAILED: not every key survived the restart\n");
    return 1;
  }
  std::printf("durability check passed: every one of %d keys survived the restart\n\n", n_inserts);

  // measure the actual cost of durability: same insert, no WAL
  GappedArray plain = GappedArray::build({}, density, eps);
  std::vector<int64_t> plain_keys;
  {
    int64_t k = -1;
    for (int i = 0; i < 100000; i++) {
      k -= 1 + static_cast<int64_t>(rng() % 100);  // disjoint key range from `keys`
      plain_keys.push_back(k);
    }
  }
  auto t2 = std::chrono::steady_clock::now();
  for (int64_t key : plain_keys) plain.insert(key);
  auto t3 = std::chrono::steady_clock::now();
  double plain_ns = std::chrono::duration<double, std::nano>(t3 - t2).count() / plain_keys.size();

  std::printf("durable put() (log + fsync + insert): %10.1f ns/op\n", put_ns);
  std::printf("plain insert() (no WAL at all):        %10.1f ns/op\n", plain_ns);
  std::printf("durability overhead: ~%.0fx\n", put_ns / plain_ns);

  return 0;
}
