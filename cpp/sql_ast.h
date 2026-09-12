// Phase 9: the AST (abstract syntax tree) types the parser builds and
// the executor (Phase 10) will walk. Deliberately small -- three
// statement kinds, matching exactly what the parser supports.
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

struct SelectStmt {
  std::string table_name;
  bool has_where = false;
  WhereClause where;
};

using Statement = std::variant<CreateTableStmt, InsertStmt, SelectStmt>;
