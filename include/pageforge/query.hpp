#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "pageforge/table_store.hpp"

namespace pageforge {

using RowPredicate = std::function<bool(const TableRow&)>;

class RowOperator {
 public:
  virtual ~RowOperator() = default;
  [[nodiscard]] virtual std::optional<TableRow> next() = 0;
};

class Query {
 public:
  [[nodiscard]] static Query from(TableStore& tables, std::string_view table_name);

  Query(const Query&) = delete;
  Query& operator=(const Query&) = delete;
  Query(Query&&) noexcept = default;
  Query& operator=(Query&&) noexcept = default;

  Query& filter(RowPredicate predicate);
  Query& project(std::vector<std::size_t> columns);
  Query& limit(std::size_t count);
  [[nodiscard]] std::optional<TableRow> next();
  [[nodiscard]] std::size_t column_count() const noexcept { return column_count_; }

 private:
  Query(std::unique_ptr<RowOperator> root, std::size_t column_count)
      : root_(std::move(root)), column_count_(column_count) {}

  std::unique_ptr<RowOperator> root_;
  std::size_t column_count_;
};

}  // namespace pageforge
