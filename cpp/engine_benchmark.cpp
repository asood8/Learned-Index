// The whole database, timed from SQL in to rows out, on the learned
// index or, built with -DLIDB_BTREE_INDEX, on tlx's B+-tree. Nothing else
// changes between the two builds: same parser, executor, catalog,
// write-ahead log and snapshots (see durable_store.h). The other
// benchmarks time the index on its own; this is what's left of the
// difference once a query also has to be parsed, planned, logged and
// turned back into a row.
//
// The two builds run as separate programs, so they can't take turns
// within one run the way the other benchmarks do. scripts/run_benchmarks.sh
// runs them alternately, several times, to even that out.
//
// usage: engine_benchmark <keys.bin> <fast-dir> <disk-dir>
//   fast-dir  where the log goes for everything that measures CPU time.
//             Use a tmpfs (WSL's /tmp is one), where fsync costs nothing,
//             so the log's cost is just the write() calls.
//   disk-dir  a real disk, for the durable-write test, which measures
//             fsync.
//
// Rows are (id INT, val INT), with ids from keys.bin. Primary keys have to
// fit in 48 bits, so keys that don't are shifted right until they do,
// which keeps the shape of the distribution (and any key that then
// collides with its neighbor is dropped).
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <malloc.h>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "catalog.h"
#include "csv_out.h"
#include "durable_store.h"
#include "executor.h"
#include "sql_parser.h"
#include "table_key.h"

struct Database {
  DurableStore store;
  Catalog catalog;
  Executor exec;
  Database(const std::string& wal_path, size_t sync_every = 1)
      : store(wal_path, 0.7, 64, 0, sync_every), catalog(store), exec(store, catalog) {}
};

std::vector<int64_t> load_keys(const std::string& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) throw std::runtime_error("could not open " + path);
  const std::streamsize size = f.tellg();
  f.seekg(0, std::ios::beg);
  std::vector<int64_t> keys(size / sizeof(int64_t));
  if (!f.read(reinterpret_cast<char*>(keys.data()), size)) throw std::runtime_error("failed to read " + path);
  return keys;
}

size_t heap_bytes() {
  const struct mallinfo2 mi = mallinfo2();
  return mi.uordblks + mi.hblkhd;  // small allocations + big ones mmap'd separately
}

double now_ns() {
  return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  const size_t n = v.size();
  return (n % 2 == 1) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

void must(const QueryResult& r, const char* what) {
  if (!r.success) throw std::runtime_error(std::string(what) + " failed: " + r.error);
}

std::string insert_sql(int64_t id, int64_t val) {
  return "INSERT INTO t VALUES (" + std::to_string(id) + ", " + std::to_string(val) + ")";
}

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: %s <keys.bin> <fast-dir> <disk-dir>\n", argv[0]);
    return 1;
  }
  const char* index_name = DurableStore::kLearnedIndex ? "learned index" : "B+-tree (tlx)";
  const std::string fast_dir = argv[2], disk_dir = argv[3];
  const int passes = 7;

  std::vector<int64_t> ids = load_keys(argv[1]);
  int shift = 0;
  while ((ids.back() >> shift) > PRIMARY_KEY_MASK) shift++;
  if (shift > 0) {
    for (int64_t& k : ids) k >>= shift;
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  }
  std::mt19937_64 split_rng(7);
  std::vector<int64_t> initial, holdout;
  for (int64_t k : ids) (split_rng() % 10 == 0 ? holdout : initial).push_back(k);
  std::printf("== %s, %s: %zu rows loaded, %zu held out for inserts", index_name, argv[1], initial.size(),
              holdout.size());
  if (shift > 0) std::printf(" (keys shifted right %d bits to fit 48)", shift);
  std::printf(" ==\n");

  std::ofstream csv = open_csv("engine.csv", "path,index,metric,value");
  auto report = [&](const char* metric, double value, const char* unit) {
    std::printf("  %-44s %12.1f %s\n", metric, value, unit);
    csv << csv_field(argv[1]) << "," << csv_field(index_name) << "," << csv_field(metric) << "," << value << "\n";
  };

  // ---- load, in random order, one INSERT statement per row ----
  const std::string wal = fast_dir + "/engine_benchmark.wal";
  DurableStore::destroy(wal);
  std::vector<int64_t> load_order = initial;
  std::shuffle(load_order.begin(), load_order.end(), std::mt19937_64(11));
  const size_t heap_before = heap_bytes();
  auto db = std::make_unique<Database>(wal);
  must(db->exec.execute(parse_sql("CREATE TABLE t (id INT, val INT)")), "CREATE TABLE");
  double t0 = now_ns();
  for (int64_t id : load_order) must(db->exec.execute(parse_sql(insert_sql(id, id % 1000))), "load");
  report("load, random order (ns/row)", (now_ns() - t0) / load_order.size(), "ns");
  report("memory after the load (bytes/row)", static_cast<double>(heap_bytes() - heap_before) / initial.size(), "B");

  // ---- startup: from the log, then from a snapshot ----
  db.reset();
  t0 = now_ns();
  db = std::make_unique<Database>(wal);
  report("open from the log (ms)", (now_ns() - t0) / 1e6, "ms");
  db->store.checkpoint();
  db.reset();
  const size_t heap_closed = heap_bytes();
  t0 = now_ns();
  db = std::make_unique<Database>(wal);
  report("open from a snapshot (ms)", (now_ns() - t0) / 1e6, "ms");
  // after random-order inserts a B+-tree's leaves are partly empty; built
  // from a snapshot in one pass, both indexes are as compact as they get
  report("memory after opening the snapshot (bytes/row)",
         static_cast<double>(heap_bytes() - heap_closed) / initial.size(), "B");
  if (db->store.size() != initial.size() + 1) throw std::runtime_error("wrong row count after restart");

  // Everything from here on is timed on the store as a snapshot left it:
  // laid out in one pass, the same starting point for both indexes.
  Executor& exec = db->exec;
  const int32_t table_id = db->catalog.get_table("t")->table_id;

  // ---- point lookups ----
  std::mt19937_64 rng(42);
  std::uniform_int_distribution<size_t> pick(0, initial.size() - 1), pick_miss(0, holdout.size() - 1);
  std::vector<int64_t> hit_ids(100000), miss_ids(100000);
  for (auto& id : hit_ids) id = initial[pick(rng)];
  for (auto& id : miss_ids) id = holdout[pick_miss(rng)];
  std::vector<std::string> hit_sql, miss_sql;
  std::vector<Statement> hit_stmts, miss_stmts;
  for (int64_t id : hit_ids) hit_sql.push_back("SELECT * FROM t WHERE id = " + std::to_string(id));
  for (int64_t id : miss_ids) miss_sql.push_back("SELECT * FROM t WHERE id = " + std::to_string(id));
  for (const auto& s : hit_sql) hit_stmts.push_back(parse_sql(s));
  for (const auto& s : miss_sql) miss_stmts.push_back(parse_sql(s));
  for (size_t i = 0; i < hit_ids.size(); i++) {
    const QueryResult r = exec.execute(hit_stmts[i]);
    if (r.rows.size() != 1 || r.rows[0][0].int_val != hit_ids[i] || r.rows[0][1].int_val != hit_ids[i] % 1000) {
      throw std::runtime_error("wrong row for a point lookup");
    }
    if (!exec.execute(miss_stmts[i]).rows.empty()) throw std::runtime_error("a missing id returned a row");
  }
  std::vector<double> t_hit, t_miss, t_parsed;
  size_t sink = 0;
  for (int p = 0; p < passes; p++) {
    double a = now_ns();
    for (const auto& s : hit_stmts) sink += exec.execute(s).rows.size();
    t_hit.push_back((now_ns() - a) / hit_stmts.size());
    a = now_ns();
    for (const auto& s : miss_stmts) sink += exec.execute(s).rows.size();
    t_miss.push_back((now_ns() - a) / miss_stmts.size());
    a = now_ns();
    for (const auto& s : hit_sql) sink += exec.execute(parse_sql(s)).rows.size();
    t_parsed.push_back((now_ns() - a) / hit_sql.size());
  }
  report("point lookup, parsed ahead of time (ns)", median(t_hit), "ns");
  report("point lookup for a missing id (ns)", median(t_miss), "ns");
  report("point lookup, parsed each time (ns)", median(t_parsed), "ns");
  double index_ns = 0;
  {
    std::vector<double> t_index;
    std::string raw;
    for (int p = 0; p < passes; p++) {
      const double a = now_ns();
      for (int64_t id : hit_ids) sink += db->store.get(pack_key(table_id, id), raw);
      t_index.push_back((now_ns() - a) / hit_ids.size());
    }
    index_ns = median(t_index);
    report("  of which the store's own lookup (ns)", index_ns, "ns");
  }

  // ---- range scans ----
  for (size_t len : {10, 100, 1000}) {
    std::vector<Statement> scans;
    std::vector<size_t> expect;
    const size_t count = (len == 1000) ? 2000 : 10000;
    std::uniform_int_distribution<size_t> start(0, initial.size() - len);
    for (size_t i = 0; i < count; i++) {
      const size_t s = start(rng);
      scans.push_back(parse_sql("SELECT * FROM t WHERE id BETWEEN " + std::to_string(initial[s]) + " AND " +
                                std::to_string(initial[s + len - 1])));
      expect.push_back(len);
    }
    for (size_t i = 0; i < count; i++) {
      if (exec.execute(scans[i]).rows.size() != expect[i]) throw std::runtime_error("range scan returned wrong rows");
    }
    std::vector<double> t;
    for (int p = 0; p < passes; p++) {
      const double a = now_ns();
      for (const auto& s : scans) sink += exec.execute(s).rows.size();
      t.push_back((now_ns() - a) / scans.size());
    }
    const std::string name = "range scan of " + std::to_string(len) + " rows (ns)";
    report(name.c_str(), median(t), "ns");
  }

  // ---- inserts: random order, then appends past the end ----
  std::vector<int64_t> shuffled = holdout;
  std::shuffle(shuffled.begin(), shuffled.end(), std::mt19937_64(99));
  t0 = now_ns();
  for (int64_t id : shuffled) must(exec.execute(parse_sql(insert_sql(id, id % 1000))), "insert");
  report("insert, random order (ns)", (now_ns() - t0) / shuffled.size(), "ns");
  const int64_t top = ids.back();
  const size_t append_count = 100000;
  t0 = now_ns();
  for (size_t i = 1; i <= append_count; i++) {
    must(exec.execute(parse_sql(insert_sql(top + static_cast<int64_t>(i), 7))), "append");
  }
  report("insert, appended past the end (ns)", (now_ns() - t0) / append_count, "ns");
  if (db->store.size() != ids.size() + append_count + 1) throw std::runtime_error("wrong row count after inserts");

  // ---- mixed reads and inserts ----
  // Reads pick ids already in the table. New ids for the inserts come one
  // of two ways. Following the data: a random id between two neighboring
  // ids already in the table, so new keys arrive where the data is dense,
  // the way the held-out ids did. Spread evenly: a random id anywhere in
  // the key range. On uniform data the two are the same thing; on skewed
  // data the second puts most inserts where the table has few keys, a
  // shift in the distribution that a learned index has to absorb.
  std::uniform_int_distribution<size_t> pick_gap(0, initial.size() - 2);
  std::uniform_int_distribution<int64_t> anywhere(0, top);
  auto absent = [&](int64_t id) {
    std::string raw;
    return !db->store.get(pack_key(table_id, id), raw);
  };
  auto id_following_data = [&]() {
    for (;;) {
      const size_t i = pick_gap(rng);
      const int64_t gap = initial[i + 1] - initial[i];
      if (gap < 2) continue;
      const int64_t id = initial[i] + 1 + static_cast<int64_t>(rng() % static_cast<uint64_t>(gap - 1));
      if (absent(id)) return id;
    }
  };
  auto id_anywhere = [&]() {
    for (;;) {
      const int64_t id = anywhere(rng);
      if (absent(id)) return id;
    }
  };
  struct Mix {
    int read_pct;
    bool follow_data;
    const char* name;
  };
  for (const Mix& mix : {Mix{95, true, "95% reads, 5% inserts following the data (ns/op)"},
                         Mix{50, true, "50% reads, 50% inserts following the data (ns/op)"},
                         Mix{50, false, "50% reads, 50% inserts spread evenly (ns/op)"}}) {
    std::vector<Statement> ops;
    std::uniform_int_distribution<int> coin(0, 99);
    for (int i = 0; i < 200000; i++) {
      if (coin(rng) < mix.read_pct) {
        ops.push_back(parse_sql("SELECT * FROM t WHERE id = " + std::to_string(initial[pick(rng)])));
      } else {
        // an id picked twice in one workload just overwrites its row
        ops.push_back(parse_sql(insert_sql(mix.follow_data ? id_following_data() : id_anywhere(), 1)));
      }
    }
    t0 = now_ns();
    for (const auto& op : ops) must(exec.execute(op), "mixed workload");
    report(mix.name, (now_ns() - t0) / ops.size(), "ns");
  }
  db.reset();
  DurableStore::destroy(wal);

  // ---- durable inserts on a real disk, with batched fsync ----
  const std::string disk_wal = disk_dir + "/engine_benchmark_durable.wal";
  for (size_t sync_every : {1, 8, 64, 512}) {
    DurableStore::destroy(disk_wal);
    Database d(disk_wal, sync_every);
    must(d.exec.execute(parse_sql("CREATE TABLE t (id INT, val INT)")), "CREATE TABLE");
    const size_t n = std::min<size_t>(200 * sync_every, 20000);
    std::vector<int64_t> order(initial.begin(), initial.begin() + static_cast<std::ptrdiff_t>(n));
    std::shuffle(order.begin(), order.end(), std::mt19937_64(5));
    const double a = now_ns();
    for (int64_t id : order) must(d.exec.execute(parse_sql(insert_sql(id, 1))), "durable insert");
    d.store.sync();
    const std::string name = "durable insert, fsync every " + std::to_string(sync_every) + " (ns)";
    report(name.c_str(), (now_ns() - a) / n, "ns");
  }
  DurableStore::destroy(disk_wal);

  if (sink == 42) std::printf(" ");  // keeps the timed loops from being optimized away
  return 0;
}
