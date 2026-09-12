// Phase 10 test: builds a real table, inserts 1000 rows, then runs
// one query through each of the executor's paths and checks the
// actual returned rows -- not just that execution didn't crash.
// Prints which access_method each query took, which is the whole
// point of this phase made visible.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "catalog.h"
#include "durable_store.h"
#include "executor.h"
#include "sql_parser.h"

static int tests_run = 0;
static int tests_passed = 0;

void check(bool condition, const std::string& name) {
  tests_run++;
  if (condition) {
    tests_passed++;
    std::printf("  PASS  %s\n", name.c_str());
  } else {
    std::printf("  FAIL  %s\n", name.c_str());
  }
}

int main() {
  const std::string wal_path = "results/phase10_demo.wal";
  std::remove(wal_path.c_str());

  DurableStore store(wal_path, 0.7, 64);
  Catalog catalog(store, wal_path);
  Executor exec(store, catalog);

  auto run = [&](const std::string& sql) { return exec.execute(parse_sql(sql)); };

  auto r = run("CREATE TABLE users (id INT, name TEXT, age INT);");
  check(r.success, "CREATE TABLE users");

  for (int64_t i = 1; i <= 1000; i++) {
    std::string sql = "INSERT INTO users VALUES (" + std::to_string(i) + ", 'user_" + std::to_string(i) +
                       "', " + std::to_string(20 + (i % 50)) + ");";
    auto ins = run(sql);
    if (!ins.success) {
      std::printf("insert failed at i=%lld: %s\n", static_cast<long long>(i), ins.error.c_str());
      break;
    }
  }
  std::printf("inserted 1000 rows\n\n");

  {
    auto res = run("SELECT * FROM users WHERE id = 42;");
    check(res.access_method == "point lookup", "WHERE id = 42 uses a point lookup");
    check(res.rows.size() == 1 && res.rows[0][0].int_val == 42 && res.rows[0][1].text_val == "user_42" &&
              res.rows[0][2].int_val == 20 + (42 % 50),
          "point lookup returns the exact correct row");
  }

  {
    auto res = run("SELECT * FROM users WHERE id BETWEEN 100 AND 110;");
    check(res.access_method == "range scan", "WHERE id BETWEEN uses a range scan");
    check(res.rows.size() == 11, "range scan returns exactly 11 rows (100..110 inclusive)");
    bool ids_correct = true;
    for (size_t i = 0; i < res.rows.size(); i++) {
      if (res.rows[i][0].int_val != 100 + static_cast<int64_t>(i)) ids_correct = false;
    }
    check(ids_correct, "range scan rows are the correct ids, in sorted order");
  }

  {
    auto res = run("SELECT * FROM users WHERE age = 30;");
    check(res.access_method == "full scan (filtered)", "WHERE on a non-key column uses a filtered full scan");
    int expected = 0;
    for (int64_t i = 1; i <= 1000; i++) {
      if (20 + (i % 50) == 30) expected++;
    }
    check(static_cast<int>(res.rows.size()) == expected, "filtered scan returns the correct row count");
    bool all_match = true;
    for (auto& row : res.rows) {
      if (row[2].int_val != 30) all_match = false;
    }
    check(all_match, "every returned row actually has age = 30");
  }

  {
    auto res = run("SELECT * FROM users;");
    check(res.access_method == "full scan", "no WHERE uses a full scan");
    std::printf("    (full scan returned %zu rows, expected 1000)\n", res.rows.size());
    check(res.rows.size() == 1000, "full scan returns all 1000 rows");
  }

  {
    auto res = run("SELECT * FROM ghosts;");
    check(!res.success, "querying a nonexistent table fails");
  }
  {
    auto res = run("INSERT INTO users VALUES (1, 'only two values');");
    check(!res.success, "wrong column count fails");
  }
  {
    auto res = run("INSERT INTO users VALUES ('not an int', 'x', 1);");
    check(!res.success, "non-INT primary key fails");
  }

  // EXPLAIN: point lookup should report a genuine predicted position
  // and probe count, not just the access method name
  {
    auto res = run("EXPLAIN SELECT * FROM users WHERE id = 42;");
    check(res.success, "EXPLAIN point lookup succeeds");
    check(res.explain_text.find("point lookup") != std::string::npos, "EXPLAIN mentions point lookup");
    check(res.explain_text.find("predicted position") != std::string::npos, "EXPLAIN reports a predicted position");
    check(res.explain_text.find("key found") != std::string::npos, "EXPLAIN reports the key was found");
  }
  {
    auto res = run("EXPLAIN SELECT * FROM users WHERE id = 999999;");  // doesn't exist
    check(res.explain_text.find("key not found") != std::string::npos,
          "EXPLAIN correctly reports a missing key as not found");
  }
  {
    auto res = run("EXPLAIN SELECT * FROM users WHERE id BETWEEN 100 AND 110;");
    check(res.explain_text.find("range scan") != std::string::npos &&
              res.explain_text.find("11") != std::string::npos,
          "EXPLAIN range scan reports the correct row count");
  }
  {
    auto res = run("EXPLAIN SELECT * FROM users WHERE age = 30;");
    check(res.explain_text.find("full table scan") != std::string::npos,
          "EXPLAIN on a non-key column reports a full table scan");
  }

  {
    bool threw = false;
    try {
      run("EXPLAIN INSERT INTO users VALUES (1, 'x', 1);");
    } catch (const std::runtime_error&) {
      threw = true;
    }
    check(threw, "EXPLAIN on a non-SELECT statement is rejected");
  }

  // UPDATE: simple non-key column change
  {
    auto res = run("UPDATE users SET age = 99 WHERE id = 42;");
    check(res.success && res.rows_affected == 1, "UPDATE by primary key affects exactly 1 row");
    auto check_res = run("SELECT * FROM users WHERE id = 42;");
    check(check_res.rows[0][2].int_val == 99, "UPDATE actually changed the value");
  }

  // UPDATE: multi-row via a non-key WHERE
  {
    auto res = run("UPDATE users SET name = 'renamed' WHERE age = 99;");
    check(res.success && res.rows_affected == 1, "UPDATE via full scan affects the correct row count");
  }

  // UPDATE: reassigning the primary key itself -- the row's storage
  // key has to move, not just its value
  {
    auto before = run("SELECT * FROM users WHERE id = 500;");
    check(before.rows.size() == 1, "sanity: row 500 exists before the PK update");
    auto res = run("UPDATE users SET id = 5000 WHERE id = 500;");
    check(res.success && res.rows_affected == 1, "UPDATE reassigning the primary key succeeds");
    auto old_lookup = run("SELECT * FROM users WHERE id = 500;");
    check(old_lookup.rows.empty(), "old primary key is gone after being reassigned");
    auto new_lookup = run("SELECT * FROM users WHERE id = 5000;");
    check(new_lookup.rows.size() == 1 && new_lookup.rows[0][0].int_val == 5000,
          "row is findable under its new primary key");
  }

  // DELETE by primary key
  {
    auto res = run("DELETE FROM users WHERE id = 1;");
    check(res.success && res.rows_affected == 1, "DELETE by primary key affects exactly 1 row");
    auto lookup = run("SELECT * FROM users WHERE id = 1;");
    check(lookup.rows.empty(), "deleted row is genuinely gone");
    auto full = run("SELECT * FROM users;");
    check(full.rows.size() == 999, "full scan reflects the deletion (999 rows remain)");
  }

  // DELETE with a non-key WHERE, multi-row
  {
    auto res = run("DELETE FROM users WHERE age = 22;");
    int expected = 0;
    for (int64_t i = 1; i <= 1000; i++) {
      if (i != 1 && i != 500 && 20 + (i % 50) == 22) expected++;  // account for earlier updates/deletes
    }
    check(res.rows_affected == expected, "DELETE via full scan affects the correct row count");
  }

  // DELETE and durability: a deletion has to survive a restart too,
  // not just an insert -- otherwise replay would resurrect the row
  {
    auto res = run("DELETE FROM users WHERE id = 2;");
    check(res.success, "setup: delete row 2 before restart check");
  }
  {
    DurableStore restarted(wal_path, 0.7, 64);
    Catalog restarted_catalog(restarted, wal_path);
    Executor restarted_exec(restarted, restarted_catalog);
    auto res = restarted_exec.execute(parse_sql("SELECT * FROM users WHERE id = 2;"));
    check(res.rows.empty(), "a deletion survives a full restart, not just the current session");
    auto res2 = restarted_exec.execute(parse_sql("SELECT * FROM users WHERE id = 5000;"));
    check(res2.rows.size() == 1, "an UPDATE's new primary key also survives a restart");
  }

  // ORDER BY
  {
    auto res = run("SELECT * FROM users WHERE id BETWEEN 3 AND 12 ORDER BY age DESC;");
    check(res.success && res.rows.size() == 10, "ORDER BY doesn't change the row count");
    bool sorted_desc = true;
    for (size_t i = 1; i < res.rows.size(); i++) {
      if (res.rows[i - 1][2].int_val < res.rows[i][2].int_val) sorted_desc = false;
    }
    check(sorted_desc, "ORDER BY age DESC actually sorts descending");
  }
  {
    auto res = run("SELECT * FROM users WHERE id BETWEEN 3 AND 12 ORDER BY name ASC;");
    bool sorted_asc = true;
    for (size_t i = 1; i < res.rows.size(); i++) {
      if (res.rows[i - 1][1].text_val > res.rows[i][1].text_val) sorted_asc = false;
    }
    check(sorted_asc, "ORDER BY on a TEXT column sorts correctly");
  }

  // LIMIT
  {
    auto res = run("SELECT * FROM users LIMIT 5;");
    check(res.rows.size() == 5, "LIMIT truncates to the requested count");
  }
  {
    auto res = run("SELECT * FROM users WHERE id BETWEEN 3 AND 6 LIMIT 100;");
    check(res.rows.size() == 4, "LIMIT larger than the result set doesn't pad or crash");
  }
  {
    auto res = run("SELECT * FROM users WHERE id BETWEEN 3 AND 25 ORDER BY age DESC LIMIT 3;");
    check(res.rows.size() == 3, "ORDER BY + LIMIT together: correct final count");
    bool sorted_desc = res.rows[0][2].int_val >= res.rows[1][2].int_val &&
                        res.rows[1][2].int_val >= res.rows[2][2].int_val;
    check(sorted_desc, "ORDER BY + LIMIT together: LIMIT applies after sorting, not before");
  }

  // aggregates
  {
    auto full = run("SELECT * FROM users;");
    auto res = run("SELECT COUNT(*) FROM users;");
    check(res.success && res.rows.size() == 1 && res.column_names[0] == "COUNT(*)",
          "COUNT(*) returns a single summary row");
    check(res.rows[0][0].int_val == static_cast<int64_t>(full.rows.size()),
          "COUNT(*) matches the actual row count");
  }
  {
    auto res = run("SELECT COUNT(*) FROM users WHERE id BETWEEN 3 AND 12;");
    check(res.rows[0][0].int_val == 10, "COUNT(*) respects a WHERE clause");
  }
  {
    auto full = run("SELECT * FROM users WHERE id BETWEEN 3 AND 7;");
    int64_t expected_sum = 0;
    for (auto& row : full.rows) expected_sum += row[2].int_val;
    auto res = run("SELECT SUM(age) FROM users WHERE id BETWEEN 3 AND 7;");
    check(res.success && res.rows[0][0].int_val == expected_sum, "SUM(column) computes the correct total");
  }
  {
    auto res = run("SELECT SUM(name) FROM users;");
    check(!res.success, "SUM on a TEXT column is rejected");
  }

  // Secondary index: create on an existing table (must backfill)
  {
    auto res = run("CREATE INDEX idx_age ON users(age);");
    check(res.success, "CREATE INDEX on an existing table succeeds (backfill)");
  }
  {
    auto res = run("CREATE INDEX idx_age ON users(age);");
    check(!res.success, "creating a duplicate-named index is rejected");
  }
  {
    auto res = run("CREATE INDEX idx_name ON users(name);");
    check(!res.success, "secondary index on a TEXT column is rejected");
  }

  // correctness: index lookup must return the exact same rows a full
  // scan would, not just *some* rows
  {
    auto filtered = run("SELECT * FROM users WHERE age = 45;");
    check(filtered.access_method == "secondary index lookup", "WHERE on an indexed column uses the index now");

    // build the expected set by temporarily reasoning about the data directly
    auto full = run("SELECT * FROM users;");
    int expected = 0;
    for (auto& row : full.rows) {
      if (row[2].int_val == 45) expected++;
    }
    check(static_cast<int>(filtered.rows.size()) == expected, "index lookup returns the correct row count");
    std::printf("    (debug: index lookup returned %zu rows, full-scan-derived expected %d)\n", filtered.rows.size(),
                expected);
    bool all_match = true;
    for (auto& row : filtered.rows) {
      if (row[2].int_val != 45) all_match = false;
    }
    check(all_match, "every row from the index lookup actually has age = 45");
  }

  // the index has to stay in sync with writes, not just reflect
  // whatever existed at CREATE INDEX time
  {
    run("INSERT INTO users VALUES (9001, 'fresh_insert', 45);");
    auto res = run("SELECT * FROM users WHERE age = 45;");
    bool found_new = false;
    for (auto& row : res.rows) {
      if (row[0].int_val == 9001) found_new = true;
    }
    check(found_new, "a new INSERT is immediately visible via the secondary index");
  }
  {
    run("UPDATE users SET age = 77 WHERE id = 9001;");
    auto old_val = run("SELECT * FROM users WHERE age = 45;");
    bool still_there_at_old = false;
    for (auto& row : old_val.rows) {
      if (row[0].int_val == 9001) still_there_at_old = true;
    }
    check(!still_there_at_old, "UPDATE removes the stale entry from the secondary index");
    auto new_val = run("SELECT * FROM users WHERE age = 77;");
    bool found_at_new = false;
    for (auto& row : new_val.rows) {
      if (row[0].int_val == 9001) found_at_new = true;
    }
    check(found_at_new, "UPDATE adds the correct new entry to the secondary index");
  }
  {
    run("DELETE FROM users WHERE id = 9001;");
    auto res = run("SELECT * FROM users WHERE age = 77;");
    bool still_present = false;
    for (auto& row : res.rows) {
      if (row[0].int_val == 9001) still_present = true;
    }
    check(!still_present, "DELETE removes the entry from the secondary index too");
  }

  // durability: the index itself has to survive a restart, not just
  // the base table
  {
    DurableStore restarted(wal_path, 0.7, 64);
    Catalog restarted_catalog(restarted, wal_path);
    Executor restarted_exec(restarted, restarted_catalog);
    auto res = restarted_exec.execute(parse_sql("SELECT * FROM users WHERE age = 45;"));
    check(res.access_method == "secondary index lookup", "the secondary index itself survives a restart");
  }

  // the actual point of this stretch goal: prove the speedup is real
  {
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 200; i++) run("SELECT * FROM users WHERE age = 33;");
    auto t1 = std::chrono::steady_clock::now();
    double indexed_ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / 200;

    auto t2 = std::chrono::steady_clock::now();
    for (int i = 0; i < 200; i++) run("SELECT * FROM users WHERE name = 'user_500';");  // no index on name
    auto t3 = std::chrono::steady_clock::now();
    double full_scan_ns = std::chrono::duration<double, std::nano>(t3 - t2).count() / 200;

    std::printf("    (indexed lookup: %.0f ns, full scan: %.0f ns, speedup: %.1fx)\n", indexed_ns, full_scan_ns,
                full_scan_ns / indexed_ns);
    check(indexed_ns < full_scan_ns, "the secondary index is actually faster than a full scan, not just correct");
  }

  std::printf("\n%d/%d tests passed\n", tests_passed, tests_run);
  return (tests_passed == tests_run) ? 0 : 1;
}
