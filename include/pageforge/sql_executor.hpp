#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "pageforge/query.hpp"
#include "pageforge/sql_parser.hpp"

namespace pageforge {

class SqlBindError final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class BoundSelect {
 public:
  BoundSelect(const BoundSelect&) = delete;
  BoundSelect& operator=(const BoundSelect&) = delete;
  BoundSelect(BoundSelect&&) noexcept = default;
  BoundSelect& operator=(BoundSelect&&) noexcept = default;

  [[nodiscard]] std::optional<TableRow> next() { return query_.next(); }
  [[nodiscard]] const Schema& output_schema() const noexcept { return output_schema_; }

 private:
  friend BoundSelect bind_select(TableStore&, Catalog&, const SelectPlan&);
  BoundSelect(Query query, Schema output_schema)
      : query_(std::move(query)), output_schema_(std::move(output_schema)) {}

  Query query_;
  Schema output_schema_;
};

[[nodiscard]] BoundSelect bind_select(TableStore& tables, Catalog& catalog, const SelectPlan& plan);
[[nodiscard]] BoundSelect execute_select_sql(TableStore& tables, Catalog& catalog, std::string_view sql);

}  // namespace pageforge
