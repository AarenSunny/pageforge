#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "pageforge/sql_lexer.hpp"

namespace pageforge {

using SqlLiteral = std::variant<std::monostate, std::int64_t, std::string, bool>;

enum class SqlComparison { Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual };

struct SqlPredicate {
  std::string column;
  SqlComparison comparison;
  SqlLiteral literal;

  bool operator==(const SqlPredicate&) const = default;
};

struct SelectPlan {
  std::string table;
  bool select_all = false;
  std::vector<std::string> columns;
  std::optional<SqlPredicate> predicate;
  std::optional<std::size_t> limit;

  bool operator==(const SelectPlan&) const = default;
};

class SqlParseError final : public std::runtime_error {
 public:
  SqlParseError(std::size_t line, std::size_t column, std::string message);

  [[nodiscard]] std::size_t line() const noexcept { return line_; }
  [[nodiscard]] std::size_t column() const noexcept { return column_; }

 private:
  std::size_t line_;
  std::size_t column_;
};

[[nodiscard]] SelectPlan parse_select(std::string_view source);

}  // namespace pageforge
