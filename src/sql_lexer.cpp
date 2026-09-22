#include "pageforge/sql_lexer.hpp"

#include <utility>

namespace pageforge {
namespace {

bool is_letter(char value) {
  return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || value == '_';
}

bool is_digit(char value) { return value >= '0' && value <= '9'; }

bool is_space(char value) {
  return value == ' ' || value == '\t' || value == '\n' || value == '\r' || value == '\f';
}

class Lexer {
 public:
  explicit Lexer(std::string_view source) : source_(source) {}

  std::vector<SqlToken> run() {
    std::vector<SqlToken> tokens;
    while (true) {
      skip_ignored();
      const auto start = mark();
      if (at_end()) {
        tokens.push_back({SqlTokenKind::End, "", start.offset, start.line, start.column});
        return tokens;
      }

      const char current = peek();
      if (is_letter(current)) {
        tokens.push_back(identifier(start));
      } else if (is_digit(current)) {
        tokens.push_back(integer(start));
      } else if (current == '\'') {
        tokens.push_back(string_literal(start));
      } else {
        tokens.push_back(symbol(start));
      }
    }
  }

 private:
  struct Mark {
    std::size_t offset;
    std::size_t line;
    std::size_t column;
  };

  [[nodiscard]] Mark mark() const { return {offset_, line_, column_}; }
  [[nodiscard]] bool at_end() const { return offset_ == source_.size(); }
  [[nodiscard]] char peek(std::size_t ahead = 0) const {
    return ahead < source_.size() - offset_ ? source_[offset_ + ahead] : '\0';
  }

  char advance() {
    const char value = source_[offset_++];
    if (value == '\r') {
      ++line_;
      column_ = 1;
    } else if (value == '\n') {
      if (offset_ < 2 || source_[offset_ - 2] != '\r') ++line_;
      column_ = 1;
    } else {
      ++column_;
    }
    return value;
  }

  [[nodiscard]] SqlToken token(SqlTokenKind kind, Mark start, std::string text) const {
    return {kind, std::move(text), start.offset, start.line, start.column};
  }

  void skip_ignored() {
    while (!at_end()) {
      if (is_space(peek())) {
        advance();
      } else if (peek() == '-' && peek(1) == '-') {
        advance();
        advance();
        while (!at_end() && peek() != '\n' && peek() != '\r') advance();
      } else if (peek() == '/' && peek(1) == '*') {
        const auto start = mark();
        advance();
        advance();
        bool closed = false;
        while (!at_end()) {
          if (peek() == '*' && peek(1) == '/') {
            advance();
            advance();
            closed = true;
            break;
          }
          advance();
        }
        if (!closed) throw SqlLexError(start.line, start.column, "unterminated block comment");
      } else {
        return;
      }
    }
  }

  [[nodiscard]] SqlToken identifier(Mark start) {
    while (!at_end() && (is_letter(peek()) || is_digit(peek()))) advance();
    return token(SqlTokenKind::Identifier, start, std::string(source_.substr(start.offset, offset_ - start.offset)));
  }

  [[nodiscard]] SqlToken integer(Mark start) {
    while (!at_end() && is_digit(peek())) advance();
    return token(SqlTokenKind::Integer, start, std::string(source_.substr(start.offset, offset_ - start.offset)));
  }

  [[nodiscard]] SqlToken string_literal(Mark start) {
    advance();  // Opening apostrophe.
    std::string decoded;
    while (!at_end()) {
      const auto current = mark();
      const char value = advance();
      if (value == '\'') {
        if (peek() == '\'') {
          advance();
          decoded.push_back('\'');
        } else {
          return token(SqlTokenKind::String, start, std::move(decoded));
        }
      } else if (value == '\0') {
        throw SqlLexError(current.line, current.column, "NUL byte in string literal");
      } else {
        decoded.push_back(value);
      }
    }
    throw SqlLexError(start.line, start.column, "unterminated string literal");
  }

  [[nodiscard]] SqlToken symbol(Mark start) {
    const char value = advance();
    auto one = [&](SqlTokenKind kind) { return token(kind, start, std::string(1, value)); };
    switch (value) {
      case ',': return one(SqlTokenKind::Comma);
      case '.': return one(SqlTokenKind::Dot);
      case '(': return one(SqlTokenKind::LeftParen);
      case ')': return one(SqlTokenKind::RightParen);
      case '*': return one(SqlTokenKind::Star);
      case ';': return one(SqlTokenKind::Semicolon);
      case '+': return one(SqlTokenKind::Plus);
      case '-': return one(SqlTokenKind::Minus);
      case '=': return one(SqlTokenKind::Equal);
      case '<':
        if (peek() == '=') {
          advance();
          return token(SqlTokenKind::LessEqual, start, "<=");
        }
        if (peek() == '>') {
          advance();
          return token(SqlTokenKind::NotEqual, start, "<>");
        }
        return one(SqlTokenKind::Less);
      case '>':
        if (peek() == '=') {
          advance();
          return token(SqlTokenKind::GreaterEqual, start, ">=");
        }
        return one(SqlTokenKind::Greater);
      case '!':
        if (peek() == '=') {
          advance();
          return token(SqlTokenKind::NotEqual, start, "!=");
        }
        break;
      default:
        break;
    }
    throw SqlLexError(start.line, start.column, "unexpected character");
  }

  std::string_view source_;
  std::size_t offset_ = 0;
  std::size_t line_ = 1;
  std::size_t column_ = 1;
};

}  // namespace

SqlLexError::SqlLexError(std::size_t line, std::size_t column, std::string message)
    : std::runtime_error("SQL lex error at " + std::to_string(line) + ':' + std::to_string(column) +
                         ": " + message),
      line_(line), column_(column) {}

std::vector<SqlToken> lex_sql(std::string_view source) { return Lexer(source).run(); }

}  // namespace pageforge
