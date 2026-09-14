// A line-oriented front end for scripts. It reads one SQL statement per
// line from stdin and answers each with a machine-readable response on
// stdout. python/sqlite_diff_test.py uses it to compare this database
// against SQLite; the REPL is the front end for people.
//
// Responses, one per input line:
//   OK            a statement with nothing to return
//   OK <n>        UPDATE or DELETE, with the number of rows affected
//   ROWS <n>      a SELECT, followed by n lines, each row as a JSON array
//   ERR <message> an error or a parse error
//
// The line ".restart" throws away every in-memory structure and rebuilds
// them from the write-ahead log, the same way a new process would, so
// the differential test can check recovery too. The log file named on
// the command line is deleted at startup.
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>

#include "catalog.h"
#include "durable_store.h"
#include "executor.h"
#include "sql_parser.h"

std::string json_string(const std::string& s) {
  std::string out = "\"";
  for (const unsigned char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += static_cast<char>(c);
    } else if (c < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\u%04x", c);
      out += buf;
    } else {
      out += static_cast<char>(c);
    }
  }
  return out + "\"";
}

struct Database {
  DurableStore store;
  Catalog catalog;
  Executor exec;
  explicit Database(const std::string& wal_path)
      : store(wal_path, 0.7, 64), catalog(store, wal_path), exec(store, catalog) {}
};

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <wal-path>\n", argv[0]);
    return 1;
  }
  const std::string wal_path = argv[1];
  std::remove(wal_path.c_str());
  auto db = std::make_unique<Database>(wal_path);

  std::string line;
  while (std::getline(std::cin, line)) {
    if (line == ".restart") {
      db.reset();  // close the log before reopening it
      db = std::make_unique<Database>(wal_path);
      std::printf("OK\n");
      std::fflush(stdout);
      continue;
    }
    try {
      const Statement stmt = parse_sql(line);
      const QueryResult result = db->exec.execute(stmt);
      if (!result.success) {
        std::printf("ERR %s\n", result.error.c_str());
      } else if (std::holds_alternative<SelectStmt>(stmt) && !std::get<SelectStmt>(stmt).is_explain) {
        std::printf("ROWS %zu\n", result.rows.size());
        for (const auto& row : result.rows) {
          std::string out = "[";
          for (size_t i = 0; i < row.size(); i++) {
            if (i > 0) out += ",";
            out += (row[i].type == ColumnType::INT64) ? std::to_string(row[i].int_val) : json_string(row[i].text_val);
          }
          std::printf("%s]\n", out.c_str());
        }
      } else if (std::holds_alternative<UpdateStmt>(stmt) || std::holds_alternative<DeleteStmt>(stmt)) {
        std::printf("OK %d\n", result.rows_affected);
      } else {
        std::printf("OK\n");
      }
    } catch (const std::exception& e) {
      std::printf("ERR %s\n", e.what());
    }
    std::fflush(stdout);
  }
  return 0;
}
