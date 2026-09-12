// Phase 9 (extended for the stretch goals): the AST (abstract syntax
// tree) types the parser builds and the executor (Phase 10) will
// walk. Five statement kinds now: the original three, plus UPDATE
// and DELETE.
#pragma once

#include <string>
#include <variant>
#include <vector>

#include "row_format.h"  // ColumnType, Value

struct ColumnDefAst {
  std::string name;
  ColumnType type;
};

struct CreateTableStmt {
  std::string table_name;
  std::vector<ColumnDefAst> columns;
};

struct InsertStmt {
  std::string table_name;
  std::vector<Value> values;
};

enum class CompareOp { EQ, LT, GT, LE, GE, BETWEEN };

struct WhereClause {
  std::string column;
  CompareOp op;
  Value value;
  Value value2;  // only meaningful when op == BETWEEN (the upper bound)
};

enum class SelectTarget { STAR, COUNT_STAR, SUM };

struct SelectStmt {
  SelectTarget target = SelectTarget::STAR;
  std::string sum_column;  // only meaningful when target == SUM
  std::string table_name;
  bool has_where = false;
  WhereClause where;
  bool is_explain = false;  // set when the statement was `EXPLAIN SELECT ...`

  bool has_order_by = false;
  std::string order_by_column;
  bool order_desc = false;  // false = ASC (the default)

  bool has_limit = false;
  int64_t limit_count = 0;
};

struct Assignment {
  std::string column;
  Value value;
};

struct UpdateStmt {
  std::string table_name;
  std::vector<Assignment> assignments;
  bool has_where = false;
  WhereClause where;
};

struct DeleteStmt {
  std::string table_name;
  bool has_where = false;
  WhereClause where;
};

using Statement = std::variant<CreateTableStmt, InsertStmt, SelectStmt, UpdateStmt, DeleteStmt>;
