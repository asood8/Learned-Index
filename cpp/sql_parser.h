// Phase 9: a recursive-descent parser -- the same technique as
// writing a calculator/expression parser, just with a few more
// statement shapes. Works entirely off the token stream from
// sql_tokenizer.h and never touches raw characters.
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
    if (check_keyword("CREATE")) return parse_create_table();
    if (check_keyword("INSERT")) return parse_insert();
    if (check_keyword("SELECT")) return parse_select();
    throw std::runtime_error("expected CREATE, INSERT, or SELECT");
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
    expect(TokenType::STAR, "'*' (only SELECT * is supported)");
    expect_keyword("FROM");
    SelectStmt stmt;
    stmt.table_name = expect(TokenType::IDENTIFIER, "table name").text;

    if (check_keyword("WHERE")) {
      advance();
      stmt.has_where = true;
      stmt.where.column = expect(TokenType::IDENTIFIER, "column name").text;

      if (check_keyword("BETWEEN")) {
        advance();
        stmt.where.op = CompareOp::BETWEEN;
        stmt.where.value = expect_literal();
        expect_keyword("AND");
        stmt.where.value2 = expect_literal();
      } else {
        stmt.where.op = parse_compare_op();
        stmt.where.value = expect_literal();
      }
    }
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
