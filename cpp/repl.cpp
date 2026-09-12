// Phase 11: the REPL. Read a line, parse it, execute it, print the
// result, loop -- a tiny sqlite3-style command line. This is the
// payoff for everything before it: the difference between a
// benchmark script and someone typing
// `SELECT * FROM users WHERE id = 42;` and watching it come back.
//
// The database persists across runs: it's backed by a real WAL file
// on disk (default results/repl.wal, or pass a path as argv[1]), so
// closing and reopening the REPL is a real restart, using the same
// durability machinery proven back in Phase 5/8.
#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "catalog.h"
#include "durable_store.h"
#include "executor.h"
#include "sql_parser.h"

std::string trim(const std::string& s) {
  size_t start = s.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return "";
  size_t end = s.find_last_not_of(" \t\r\n");
  return s.substr(start, end - start + 1);
}

std::string value_to_string(const Value& v) {
  return (v.type == ColumnType::INT64) ? std::to_string(v.int_val) : v.text_val;
}

void print_select_result(const QueryResult& result) {
  if (result.rows.empty()) {
    std::printf("(0 rows) [%s]\n", result.access_method.c_str());
    return;
  }

  std::vector<size_t> widths(result.column_names.size());
  for (size_t c = 0; c < result.column_names.size(); c++) widths[c] = result.column_names[c].size();

  std::vector<std::vector<std::string>> formatted;
  for (const auto& row : result.rows) {
    std::vector<std::string> cells;
    for (size_t c = 0; c < row.size(); c++) {
      std::string s = value_to_string(row[c]);
      widths[c] = std::max(widths[c], s.size());
      cells.push_back(s);
    }
    formatted.push_back(std::move(cells));
  }

  for (size_t c = 0; c < result.column_names.size(); c++) std::printf("%-*s  ", (int)widths[c], result.column_names[c].c_str());
  std::printf("\n");
  for (size_t c = 0; c < result.column_names.size(); c++) std::printf("%s  ", std::string(widths[c], '-').c_str());
  std::printf("\n");
  for (const auto& row : formatted) {
    for (size_t c = 0; c < row.size(); c++) std::printf("%-*s  ", (int)widths[c], row[c].c_str());
    std::printf("\n");
  }
  std::printf("(%zu row%s) [%s]\n", result.rows.size(), result.rows.size() == 1 ? "" : "s",
              result.access_method.c_str());
}

int main(int argc, char** argv) {
  const std::string wal_path = (argc > 1) ? argv[1] : "results/repl.wal";

  DurableStore store(wal_path, 0.7, 64);
  Catalog catalog(store, wal_path);
  Executor exec(store, catalog);

  std::printf("learned-index-db -- type SQL ending in ';', or .exit to quit.\n");
  std::printf("database file: %s\n\n", wal_path.c_str());

  std::string line;
  while (true) {
    std::printf("db> ");
    std::fflush(stdout);
    if (!std::getline(std::cin, line)) break;  // EOF (Ctrl-D / piped input exhausted)

    const std::string trimmed = trim(line);
    if (trimmed.empty()) continue;
    if (trimmed == ".exit" || trimmed == ".quit") break;

    if (trimmed == ".tables") {
      auto names = catalog.table_names();
      std::sort(names.begin(), names.end());
      for (const auto& name : names) std::printf("%s\n", name.c_str());
      continue;
    }

    try {
      Statement stmt = parse_sql(trimmed);
      QueryResult result = exec.execute(stmt);
      if (!result.success) {
        std::printf("Error: %s\n", result.error.c_str());
      } else if (std::holds_alternative<SelectStmt>(stmt) && std::get<SelectStmt>(stmt).is_explain) {
        std::printf("%s\n", result.explain_text.c_str());
      } else if (std::holds_alternative<SelectStmt>(stmt)) {
        print_select_result(result);
      } else if (std::holds_alternative<UpdateStmt>(stmt) || std::holds_alternative<DeleteStmt>(stmt)) {
        std::printf("OK (%d row%s affected)\n", result.rows_affected, result.rows_affected == 1 ? "" : "s");
      } else {
        std::printf("OK\n");
      }
    } catch (const std::exception& e) {
      std::printf("Parse error: %s\n", e.what());
    }
  }

  std::printf("bye.\n");
  return 0;
}
