// Phase 9 (extended for the stretch goals): a recursive-descent
// parser -- the same technique as writing a calculator/expression
// parser, just with a few more statement shapes. Works entirely off
// the token stream from sql_tokenizer.h and never touches raw
// characters.
#pragma once

#include <stdexcept>
#include <string>
#include <vector>

#include "sql_ast.h"
#include "sql_tokenizer.h"

class Parser {
 public:
  explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

  Statement parse_statement() {
    if (check_keyword("EXPLAIN")) {
      advance();
      // Scoped to SELECT only -- letting EXPLAIN wrap any statement
      // would need a Statement-inside-a-Statement AST node, which
      // means a variant containing itself, which needs pointer
      // indirection to even compile. Real access-method routing only
      // happens for SELECT anyway, so that's the only case worth
      // explaining, and this avoids that indirection entirely.
      if (!check_keyword("SELECT")) {
        throw std::runtime_error("EXPLAIN is only supported for SELECT statements");
      }
      SelectStmt stmt = parse_select();
      stmt.is_explain = true;
      return stmt;
    }
    if (check_keyword("CREATE")) {
      // peek past CREATE without consuming it yet, to see which kind
      const Token& next = tokens_[pos_ + 1];
      if (next.type == TokenType::KEYWORD && next.text == "INDEX") return parse_create_index();
      return parse_create_table();
    }
    if (check_keyword("INSERT")) return parse_insert();
    if (check_keyword("SELECT")) return parse_select();
    if (check_keyword("UPDATE")) return parse_update();
    if (check_keyword("DELETE")) return parse_delete();
    throw std::runtime_error("expected EXPLAIN, CREATE, INSERT, SELECT, UPDATE, or DELETE");
  }

 private:
  std::vector<Token> tokens_;
  size_t pos_ = 0;

  const Token& peek() const { return tokens_[pos_]; }
  const Token& advance() { return tokens_[pos_++]; }

  bool check_keyword(const std::string& kw) const {
    return peek().type == TokenType::KEYWORD && peek().text == kw;
  }
  bool check(TokenType t) const { return peek().type == t; }

  void expect_keyword(const std::string& kw) {
    if (!check_keyword(kw)) throw std::runtime_error("expected keyword " + kw + ", got '" + peek().text + "'");
    advance();
  }
  Token expect(TokenType t, const std::string& what) {
    if (!check(t)) throw std::runtime_error("expected " + what + ", got '" + peek().text + "'");
    return advance();
  }
  void skip_trailing_semicolon() {
    if (check(TokenType::SEMICOLON)) advance();
  }

  Value expect_literal() {
    if (check(TokenType::INT_LITERAL)) return Value::make_int(advance().int_value);
    if (check(TokenType::STRING_LITERAL)) return Value::make_text(advance().text);
    throw std::runtime_error("expected a literal value, got '" + peek().text + "'");
  }

  CreateTableStmt parse_create_table() {
    expect_keyword("CREATE");
    expect_keyword("TABLE");
    const std::string table_name = expect(TokenType::IDENTIFIER, "table name").text;
    expect(TokenType::LPAREN, "'('");

    std::vector<ColumnDefAst> columns;
    while (true) {
      const std::string col_name = expect(TokenType::IDENTIFIER, "column name").text;
      ColumnType type;
      if (check_keyword("INT")) {
        advance();
        type = ColumnType::INT64;
      } else if (check_keyword("TEXT")) {
        advance();
        type = ColumnType::TEXT;
      } else {
        throw std::runtime_error("expected column type INT or TEXT, got '" + peek().text + "'");
      }
      columns.push_back({col_name, type});
      if (check(TokenType::COMMA)) {
        advance();
        continue;
      }
      break;
    }
    expect(TokenType::RPAREN, "')'");
    skip_trailing_semicolon();
    return CreateTableStmt{table_name, columns};
  }

  InsertStmt parse_insert() {
    expect_keyword("INSERT");
    expect_keyword("INTO");
    const std::string table_name = expect(TokenType::IDENTIFIER, "table name").text;
    expect_keyword("VALUES");
    expect(TokenType::LPAREN, "'('");

    std::vector<Value> values;
    while (true) {
      values.push_back(expect_literal());
      if (check(TokenType::COMMA)) {
        advance();
        continue;
      }
      break;
    }
    expect(TokenType::RPAREN, "')'");
    skip_trailing_semicolon();
    return InsertStmt{table_name, values};
  }

  SelectStmt parse_select() {
    expect_keyword("SELECT");
    SelectStmt stmt;

    if (check(TokenType::STAR)) {
      advance();
      stmt.target = SelectTarget::STAR;
    } else if (check_keyword("COUNT")) {
      advance();
      expect(TokenType::LPAREN, "'('");
      expect(TokenType::STAR, "'*' (only COUNT(*) is supported)");
      expect(TokenType::RPAREN, "')'");
      stmt.target = SelectTarget::COUNT_STAR;
    } else if (check_keyword("SUM")) {
      advance();
      expect(TokenType::LPAREN, "'('");
      stmt.sum_column = expect(TokenType::IDENTIFIER, "column name").text;
      expect(TokenType::RPAREN, "')'");
      stmt.target = SelectTarget::SUM;
    } else {
      throw std::runtime_error("expected '*', COUNT(*), or SUM(col), got '" + peek().text + "'");
    }

    expect_keyword("FROM");
    stmt.table_name = expect(TokenType::IDENTIFIER, "table name").text;

    if (check_keyword("WHERE")) {
      advance();
      stmt.has_where = true;
      stmt.where = parse_where_clause();
    }
    if (check_keyword("ORDER")) {
      advance();
      expect_keyword("BY");
      stmt.has_order_by = true;
      stmt.order_by_column = expect(TokenType::IDENTIFIER, "column name").text;
      if (check_keyword("DESC")) {
        advance();
        stmt.order_desc = true;
      } else if (check_keyword("ASC")) {
        advance();
      }
    }
    if (check_keyword("LIMIT")) {
      advance();
      stmt.has_limit = true;
      stmt.limit_count = expect(TokenType::INT_LITERAL, "a number").int_value;
    }
    skip_trailing_semicolon();
    return stmt;
  }

  // Assumes "WHERE" has already been consumed by the caller. Shared
  // by SELECT, UPDATE, and DELETE -- all three support the same
  // WHERE grammar, so this is the one place that grammar is written.
  WhereClause parse_where_clause() {
    WhereClause where;
    where.column = expect(TokenType::IDENTIFIER, "column name").text;
    if (check_keyword("BETWEEN")) {
      advance();
      where.op = CompareOp::BETWEEN;
      where.value = expect_literal();
      expect_keyword("AND");
      where.value2 = expect_literal();
    } else {
      where.op = parse_compare_op();
      where.value = expect_literal();
    }
    return where;
  }

  UpdateStmt parse_update() {
    expect_keyword("UPDATE");
    UpdateStmt stmt;
    stmt.table_name = expect(TokenType::IDENTIFIER, "table name").text;
    expect_keyword("SET");

    while (true) {
      const std::string col = expect(TokenType::IDENTIFIER, "column name").text;
      expect(TokenType::EQ, "'='");
      stmt.assignments.push_back({col, expect_literal()});
      if (check(TokenType::COMMA)) {
        advance();
        continue;
      }
      break;
    }
    if (check_keyword("WHERE")) {
      advance();
      stmt.has_where = true;
      stmt.where = parse_where_clause();
    }
    skip_trailing_semicolon();
    return stmt;
  }

  DeleteStmt parse_delete() {
    expect_keyword("DELETE");
    expect_keyword("FROM");
    DeleteStmt stmt;
    stmt.table_name = expect(TokenType::IDENTIFIER, "table name").text;
    if (check_keyword("WHERE")) {
      advance();
      stmt.has_where = true;
      stmt.where = parse_where_clause();
    }
    skip_trailing_semicolon();
    return stmt;
  }

  CreateIndexStmt parse_create_index() {
    expect_keyword("CREATE");
    expect_keyword("INDEX");
    CreateIndexStmt stmt;
    stmt.index_name = expect(TokenType::IDENTIFIER, "index name").text;
    expect_keyword("ON");
    stmt.table_name = expect(TokenType::IDENTIFIER, "table name").text;
    expect(TokenType::LPAREN, "'('");
    stmt.column_name = expect(TokenType::IDENTIFIER, "column name").text;
    expect(TokenType::RPAREN, "')'");
    skip_trailing_semicolon();
    return stmt;
  }

  CompareOp parse_compare_op() {
    if (check(TokenType::EQ)) { advance(); return CompareOp::EQ; }
    if (check(TokenType::LT)) { advance(); return CompareOp::LT; }
    if (check(TokenType::GT)) { advance(); return CompareOp::GT; }
    if (check(TokenType::LE)) { advance(); return CompareOp::LE; }
    if (check(TokenType::GE)) { advance(); return CompareOp::GE; }
    throw std::runtime_error("expected a comparison operator (=, <, >, <=, >=), got '" + peek().text + "'");
  }
};

inline Statement parse_sql(const std::string& sql) {
  Parser parser(tokenize(sql));
  return parser.parse_statement();
}
