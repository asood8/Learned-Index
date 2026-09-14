// Startup time with and without a checkpoint.
//
// Without checkpoints every startup replays every write ever logged, so
// a store whose rows keep getting updated gets slower to open even
// though it isn't getting any bigger. This builds a store of `rows`
// rows, each one written `rounds` times, then times opening it: once by
// replaying the whole log, and once after a checkpoint, from the
// snapshot. It checks every row's final value both times.
//
// Every write is fsync'd, so building the store takes a while (about
// 40 s for the defaults on WSL's ext4). Point it at a real local disk.
// On WSL that means somewhere in the Linux home directory: not /mnt/c,
// where fsync is very slow, and not /tmp, which is a tmpfs where fsync
// does nothing.
//
// usage: checkpoint_demo [dir] [rows] [rounds]
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "durable_store.h"

volatile size_t g_sink = 0;

static std::string value_for(int64_t row, int round) {
  // about 60 bytes, a typical small row
  return "row " + std::to_string(row) + ", version " + std::to_string(round) + std::string(40, '.');
}

template <typename F>
double median_ms(int reps, F f) {
  std::vector<double> samples;
  for (int i = 0; i < reps; i++) {
    const auto t0 = std::chrono::steady_clock::now();
    f();
    const auto t1 = std::chrono::steady_clock::now();
    samples.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
  }
  std::sort(samples.begin(), samples.end());
  return samples[samples.size() / 2];
}

int main(int argc, char** argv) {
  const std::string dir = (argc > 1) ? argv[1] : "results";
  const int64_t rows = (argc > 2) ? std::atoll(argv[2]) : 2000;
  const int rounds = (argc > 3) ? std::atoi(argv[3]) : 10;
  const std::string path = dir + "/checkpoint_demo.wal";
  DurableStore::destroy(path);

  const auto t0 = std::chrono::steady_clock::now();
  {
    DurableStore s(path, 0.7, 64, 0);  // no automatic checkpoints: the long log is the point
    for (int r = 0; r < rounds; r++) {
      for (int64_t k = 0; k < rows; k++) s.put(k, value_for(k, r));
    }
  }
  const double build_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

  auto open_store = [&] {
    DurableStore s(path, 0.7, 64, 0);
    g_sink = s.size();
  };
  auto all_rows_correct = [&](size_t& from_snapshot, size_t& from_log) {
    DurableStore s(path, 0.7, 64, 0);
    from_snapshot = s.snapshot_entries();
    from_log = s.recovered_entries();
    if (s.size() != static_cast<size_t>(rows)) return false;
    std::string v;
    for (int64_t k = 0; k < rows; k++) {
      if (!s.get(k, v) || v != value_for(k, rounds - 1)) return false;
    }
    return true;
  };

  size_t snap_rows = 0, log_records = 0;
  const bool ok_before = all_rows_correct(snap_rows, log_records);
  const double open_before = median_ms(5, open_store);
  const auto log_bytes_before = std::filesystem::file_size(path);

  {
    DurableStore s(path, 0.7, 64, 0);
    s.checkpoint();
  }
  size_t snap_rows_after = 0, log_records_after = 0;
  const bool ok_after = all_rows_correct(snap_rows_after, log_records_after);
  const double open_after = median_ms(5, open_store);
  const auto log_bytes_after = std::filesystem::file_size(path);
  const auto snapshot_bytes = std::filesystem::file_size(path + ".snapshot");

  std::printf("%lld rows x %d rounds = %lld writes, each fsync'd, in %.1f s\n", static_cast<long long>(rows), rounds,
              static_cast<long long>(rows) * rounds, build_s);
  std::printf("before checkpoint: log %9ju bytes; opening replays %zu log records:   %8.2f ms (median of 5); rows correct: %s\n",
              static_cast<uintmax_t>(log_bytes_before), log_records, open_before, ok_before ? "yes" : "NO");
  std::printf("after checkpoint:  log %9ju bytes, snapshot %ju bytes; opening loads %zu rows, replays %zu: %8.2f ms (median of 5); rows correct: %s\n",
              static_cast<uintmax_t>(log_bytes_after), static_cast<uintmax_t>(snapshot_bytes), snap_rows_after,
              log_records_after, open_after, ok_after ? "yes" : "NO");

  DurableStore::destroy(path);
  return (ok_before && ok_after) ? 0 : 1;
}
