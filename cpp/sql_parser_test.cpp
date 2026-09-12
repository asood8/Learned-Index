// Phase 9 test: parses every statement shape the grammar claims to
// support and checks the resulting AST fields directly -- same
// check()-and-count pattern as Phase 4's test suite. Also verifies
// genuinely malformed input actually throws, not just that valid
// input parses.
#include <cstdio>
#include <stdexcept>
#include <string>

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
  {
    auto stmt = parse_sql("CREATE TABLE users (id INT, name TEXT, age INT);");
    auto& ct = std::get<CreateTableStmt>(stmt);
    check(ct.table_name == "users", "CREATE TABLE: table name");
    check(ct.columns.size() == 3, "CREATE TABLE: column count");
    check(ct.columns[0].name == "id" && ct.columns[0].type == ColumnType::INT64, "CREATE TABLE: col 0 (id INT)");
    check(ct.columns[1].name == "name" && ct.columns[1].type == ColumnType::TEXT, "CREATE TABLE: col 1 (name TEXT)");
  }

  {
    auto stmt = parse_sql("INSERT INTO users VALUES (1, 'Alice', 30);");
    auto& ins = std::get<InsertStmt>(stmt);
    check(ins.table_name == "users", "INSERT: table name");
    check(ins.values.size() == 3, "INSERT: value count");
    check(ins.values[0].type == ColumnType::INT64 && ins.values[0].int_val == 1, "INSERT: int value");
    check(ins.values[1].type == ColumnType::TEXT && ins.values[1].text_val == "Alice", "INSERT: text value");
  }

  {
    auto stmt = parse_sql("SELECT * FROM users;");
    auto& sel = std::get<SelectStmt>(stmt);
    check(sel.table_name == "users" && !sel.has_where, "SELECT with no WHERE");
  }

  {
    auto stmt = parse_sql("SELECT * FROM users WHERE id = 42;");
    auto& sel = std::get<SelectStmt>(stmt);
    check(sel.has_where && sel.where.column == "id" && sel.where.op == CompareOp::EQ &&
              sel.where.value.int_val == 42,
          "SELECT WHERE col = int");
  }

  {
    auto stmt = parse_sql("SELECT * FROM users WHERE name = 'Bob';");
    auto& sel = std::get<SelectStmt>(stmt);
    check(sel.has_where && sel.where.op == CompareOp::EQ && sel.where.value.text_val == "Bob",
          "SELECT WHERE col = string");
  }

  {
    auto stmt = parse_sql("SELECT * FROM users WHERE age < 18;");
    check(std::get<SelectStmt>(stmt).where.op == CompareOp::LT, "SELECT WHERE col < value");
  }
  {
    auto stmt = parse_sql("SELECT * FROM users WHERE age > 18;");
    check(std::get<SelectStmt>(stmt).where.op == CompareOp::GT, "SELECT WHERE col > value");
  }
  {
    auto stmt = parse_sql("SELECT * FROM users WHERE age <= 18;");
    check(std::get<SelectStmt>(stmt).where.op == CompareOp::LE, "SELECT WHERE col <= value");
  }
  {
    auto stmt = parse_sql("SELECT * FROM users WHERE age >= 18;");
    check(std::get<SelectStmt>(stmt).where.op == CompareOp::GE, "SELECT WHERE col >= value");
  }

  {
    auto stmt = parse_sql("SELECT * FROM users WHERE age BETWEEN 18 AND 65;");
    auto& sel = std::get<SelectStmt>(stmt);
    check(sel.where.op == CompareOp::BETWEEN && sel.where.value.int_val == 18 && sel.where.value2.int_val == 65,
          "SELECT WHERE col BETWEEN a AND b");
  }

  {
    // lowercase keywords, irregular whitespace -- the tokenizer should not care
    auto stmt = parse_sql("select   *   from users\nwhere id=7;");
    auto& sel = std::get<SelectStmt>(stmt);
    check(sel.table_name == "users" && sel.where.value.int_val == 7,
          "case-insensitive keywords and irregular whitespace");
  }

  {
    auto stmt = parse_sql("INSERT INTO accounts VALUES (1, -500);");
    auto& ins = std::get<InsertStmt>(stmt);
    check(ins.values[1].int_val == -500, "negative integer literal");
  }

  {
    bool threw = false;
    try {
      parse_sql("SELECT * FRUM users;");  // deliberately misspelled keyword
    } catch (const std::runtime_error&) {
      threw = true;
    }
    check(threw, "malformed input actually throws, not silently misparses");
  }

  {
    bool threw = false;
    try {
      parse_sql("SELECT * FROM users WHERE id = ;");  // missing value
    } catch (const std::runtime_error&) {
      threw = true;
    }
    check(threw, "missing literal after operator throws");
  }

  std::printf("\n%d/%d tests passed\n", tests_passed, tests_run);
  return (tests_passed == tests_run) ? 0 : 1;
}
