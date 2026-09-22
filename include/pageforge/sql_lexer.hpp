#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace pageforge {

enum class SqlTokenKind {
  Identifier,
  Integer,
  String,
  Comma,
  Dot,
  LeftParen,
  RightParen,
  Star,
  Semicolon,
  Plus,
  Minus,
  Equal,
  NotEqual,
  Less,
  LessEqual,
  Greater,
  GreaterEqual,
  End,
};

struct SqlToken {
  SqlTokenKind kind;
  std::string text;
  std::size_t offset;
  std::size_t line;
  std::size_t column;

  bool operator==(const SqlToken&) const = default;
};

class SqlLexError final : public std::runtime_error {
 public:
  SqlLexError(std::size_t line, std::size_t column, std::string message);

  [[nodiscard]] std::size_t line() const noexcept { return line_; }
  [[nodiscard]] std::size_t column() const noexcept { return column_; }

 private:
  std::size_t line_;
  std::size_t column_;
};

[[nodiscard]] std::vector<SqlToken> lex_sql(std::string_view source);

}  // namespace pageforge
