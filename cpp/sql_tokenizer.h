// Phase 9: a SQL tokenizer. Splits raw text into a flat stream of
// tokens -- the same first step any parser needs, whether it's SQL,
// a calculator, or a programming language. The parser (sql_parser.h)
// works entirely off this token stream and never looks at raw
// characters again.
#pragma once

#include <cctype>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

enum class TokenType {
  KEYWORD,
  IDENTIFIER,
  INT_LITERAL,
  STRING_LITERAL,
  LPAREN,
  RPAREN,
  COMMA,
  SEMICOLON,
  STAR,
  EQ,
  LT,
  GT,
  LE,
  GE,
  END_OF_INPUT
};

struct Token {
  TokenType type;
  std::string text;       // keyword (uppercased), identifier, or string literal value
  int64_t int_value = 0;  // valid when type == INT_LITERAL
};

// Recognized keywords -- anything else that looks like an identifier
// just becomes one. Matching is case-insensitive (SQL convention),
// but the stored text is always uppercased so the parser can compare
// against plain string literals without normalizing every time.
inline bool is_keyword(const std::string& upper) {
  static const std::vector<std::string> kKeywords = {
      "CREATE", "TABLE",  "INSERT", "INTO",   "VALUES", "SELECT", "FROM",
      "WHERE",  "AND",    "BETWEEN", "INT",   "TEXT",   "EXPLAIN",
      "UPDATE", "SET",    "DELETE", "ORDER",  "BY",     "ASC",
      "DESC",   "LIMIT",  "COUNT",  "SUM"};
  for (const auto& kw : kKeywords) {
    if (upper == kw) return true;
  }
  return false;
}

inline std::vector<Token> tokenize(const std::string& sql) {
  std::vector<Token> tokens;
  size_t i = 0;
  const size_t n = sql.size();

  while (i < n) {
    const char c = sql[i];

    if (std::isspace(static_cast<unsigned char>(c))) {
      i++;
      continue;
    }

    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
      size_t start = i;
      while (i < n && (std::isalnum(static_cast<unsigned char>(sql[i])) || sql[i] == '_')) i++;
      std::string word = sql.substr(start, i - start);
      std::string upper = word;
      for (char& ch : upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
      if (is_keyword(upper)) {
        tokens.push_back({TokenType::KEYWORD, upper});
      } else {
        tokens.push_back({TokenType::IDENTIFIER, word});
      }
      continue;
    }

    if (std::isdigit(static_cast<unsigned char>(c)) ||
        (c == '-' && i + 1 < n && std::isdigit(static_cast<unsigned char>(sql[i + 1])))) {
      size_t start = i;
      i++;
      while (i < n && std::isdigit(static_cast<unsigned char>(sql[i]))) i++;
      int64_t value = std::stoll(sql.substr(start, i - start));
      tokens.push_back({TokenType::INT_LITERAL, "", value});
      continue;
    }

    if (c == '\'') {
      i++;  // skip opening quote
      size_t start = i;
      while (i < n && sql[i] != '\'') i++;
      if (i >= n) throw std::runtime_error("unterminated string literal");
      std::string value = sql.substr(start, i - start);
      i++;  // skip closing quote
      tokens.push_back({TokenType::STRING_LITERAL, value});
      continue;
    }

    switch (c) {
      case '(': tokens.push_back({TokenType::LPAREN, "("}); i++; continue;
      case ')': tokens.push_back({TokenType::RPAREN, ")"}); i++; continue;
      case ',': tokens.push_back({TokenType::COMMA, ","}); i++; continue;
      case ';': tokens.push_back({TokenType::SEMICOLON, ";"}); i++; continue;
      case '*': tokens.push_back({TokenType::STAR, "*"}); i++; continue;
      case '=': tokens.push_back({TokenType::EQ, "="}); i++; continue;
      case '<':
        if (i + 1 < n && sql[i + 1] == '=') { tokens.push_back({TokenType::LE, "<="}); i += 2; }
        else { tokens.push_back({TokenType::LT, "<"}); i++; }
        continue;
      case '>':
        if (i + 1 < n && sql[i + 1] == '=') { tokens.push_back({TokenType::GE, ">="}); i += 2; }
        else { tokens.push_back({TokenType::GT, ">"}); i++; }
        continue;
      default:
        throw std::runtime_error(std::string("unexpected character: ") + c);
    }
  }

  tokens.push_back({TokenType::END_OF_INPUT, ""});
  return tokens;
}
