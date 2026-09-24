#include "pageforge/sql_parser.hpp"

#include <charconv>
#include <limits>
#include <utility>

namespace pageforge {
namespace {

char ascii_lower(char value) {
  return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

bool keyword(const SqlToken& token, std::string_view expected) {
  if (token.kind != SqlTokenKind::Identifier || token.text.size() != expected.size()) return false;
  for (std::size_t index = 0; index < expected.size(); ++index) {
    if (ascii_lower(token.text[index]) != ascii_lower(expected[index])) return false;
  }
  return true;
}

class Parser {
 public:
  explicit Parser(std::vector<SqlToken> tokens) : tokens_(std::move(tokens)) {}

  SelectPlan parse() {
    expect_keyword("SELECT");
    SelectPlan plan;
    if (match(SqlTokenKind::Star)) {
      plan.select_all = true;
    } else {
      plan.columns.push_back(expect_identifier("expected a selected column"));
      while (match(SqlTokenKind::Comma)) {
        plan.columns.push_back(expect_identifier("expected a column after ','"));
      }
    }

    expect_keyword("FROM");
    plan.table = expect_identifier("expected a table name after FROM");
    if (match_keyword("WHERE")) {
      plan.predicates.push_back(parse_predicate());
      while (match_keyword("AND")) plan.predicates.push_back(parse_predicate());
    }
    if (match_keyword("LIMIT")) plan.limit = parse_limit();
    (void)match(SqlTokenKind::Semicolon);
    if (current().kind != SqlTokenKind::End) fail(current(), "unexpected token after SELECT statement");
    return plan;
  }

 private:
  [[nodiscard]] const SqlToken& current() const { return tokens_.at(index_); }

  bool match(SqlTokenKind kind) {
    if (current().kind != kind) return false;
    ++index_;
    return true;
  }

  bool match_keyword(std::string_view value) {
    if (!keyword(current(), value)) return false;
    ++index_;
    return true;
  }

  void expect_keyword(std::string_view value) {
    if (!match_keyword(value)) fail(current(), "expected " + std::string(value));
  }

  std::string expect_identifier(std::string_view message) {
    if (current().kind != SqlTokenKind::Identifier) fail(current(), std::string(message));
    return tokens_[index_++].text;
  }

  SqlPredicate parse_predicate() {
    SqlPredicate predicate;
    predicate.column = expect_identifier("expected a column after WHERE");
    if (match_keyword("IS")) {
      const bool negated = match_keyword("NOT");
      expect_keyword("NULL");
      predicate.kind = negated ? SqlPredicateKind::IsNotNull : SqlPredicateKind::IsNull;
      predicate.literal = std::monostate{};
      return predicate;
    }
    predicate.comparison = parse_comparison();
    predicate.literal = parse_literal();
    return predicate;
  }

  SqlComparison parse_comparison() {
    const auto kind = current().kind;
    ++index_;
    switch (kind) {
      case SqlTokenKind::Equal: return SqlComparison::Equal;
      case SqlTokenKind::NotEqual: return SqlComparison::NotEqual;
      case SqlTokenKind::Less: return SqlComparison::Less;
      case SqlTokenKind::LessEqual: return SqlComparison::LessEqual;
      case SqlTokenKind::Greater: return SqlComparison::Greater;
      case SqlTokenKind::GreaterEqual: return SqlComparison::GreaterEqual;
      default:
        --index_;
        fail(current(), "expected a comparison operator");
    }
  }

  SqlLiteral parse_literal() {
    if (current().kind == SqlTokenKind::String) return tokens_[index_++].text;
    if (keyword(current(), "TRUE")) {
      ++index_;
      return true;
    }
    if (keyword(current(), "FALSE")) {
      ++index_;
      return false;
    }
    if (keyword(current(), "NULL")) {
      ++index_;
      return std::monostate{};
    }

    bool negative = false;
    const auto start = current();
    if (match(SqlTokenKind::Plus)) {
      negative = false;
    } else if (match(SqlTokenKind::Minus)) {
      negative = true;
    }
    if (current().kind != SqlTokenKind::Integer) fail(current(), "expected a literal value");
    const auto token = tokens_[index_++];
    std::uint64_t magnitude = 0;
    const auto [end, error] = std::from_chars(token.text.data(), token.text.data() + token.text.size(), magnitude);
    if (error != std::errc{} || end != token.text.data() + token.text.size()) {
      fail(start, "integer literal is outside the signed 64-bit range");
    }
    const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (!negative) {
      if (magnitude > maximum) fail(start, "integer literal is outside the signed 64-bit range");
      return static_cast<std::int64_t>(magnitude);
    }
    if (magnitude > maximum + 1) fail(start, "integer literal is outside the signed 64-bit range");
    if (magnitude == maximum + 1) return std::numeric_limits<std::int64_t>::min();
    return -static_cast<std::int64_t>(magnitude);
  }

  std::size_t parse_limit() {
    const auto token = current();
    if (!match(SqlTokenKind::Integer)) fail(token, "expected a nonnegative integer after LIMIT");
    std::size_t value = 0;
    const auto [end, error] = std::from_chars(token.text.data(), token.text.data() + token.text.size(), value);
    if (error != std::errc{} || end != token.text.data() + token.text.size()) {
      fail(token, "LIMIT is outside the platform size range");
    }
    return value;
  }

  [[noreturn]] static void fail(const SqlToken& token, std::string message) {
    throw SqlParseError(token.line, token.column, std::move(message));
  }

  std::vector<SqlToken> tokens_;
  std::size_t index_ = 0;
};

}  // namespace

SqlParseError::SqlParseError(std::size_t line, std::size_t column, std::string message)
    : std::runtime_error("SQL parse error at " + std::to_string(line) + ':' + std::to_string(column) +
                         ": " + message),
      line_(line), column_(column) {}

SelectPlan parse_select(std::string_view source) { return Parser(lex_sql(source)).parse(); }

}  // namespace pageforge
