#include "pageforge/sql_executor.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace pageforge {
namespace {

char ascii_lower(char value) {
  return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

bool equal_name(std::string_view left, std::string_view right) {
  if (left.size() != right.size()) return false;
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (ascii_lower(left[index]) != ascii_lower(right[index])) return false;
  }
  return true;
}

TableDefinition resolve_table(Catalog& catalog, std::string_view name) {
  std::optional<TableDefinition> result;
  for (const auto& table : catalog.list_tables()) {
    if (!equal_name(table.name, name)) continue;
    if (result) throw SqlBindError("ambiguous table name: " + std::string(name));
    result = table;
  }
  if (!result) throw SqlBindError("unknown table: " + std::string(name));
  return std::move(*result);
}

std::size_t resolve_column(const Schema& schema, std::string_view name) {
  std::optional<std::size_t> result;
  for (std::size_t index = 0; index < schema.size(); ++index) {
    if (!equal_name(schema[index].name, name)) continue;
    if (result) throw SqlBindError("ambiguous column name: " + std::string(name));
    result = index;
  }
  if (!result) throw SqlBindError("unknown column: " + std::string(name));
  return *result;
}

bool literal_matches(DataType type, const SqlLiteral& literal) {
  if (std::holds_alternative<std::monostate>(literal)) return true;
  switch (type) {
    case DataType::Integer: return std::holds_alternative<std::int64_t>(literal);
    case DataType::Text: return std::holds_alternative<std::string>(literal);
    case DataType::Boolean: return std::holds_alternative<bool>(literal);
  }
  return false;
}

template <typename Type>
bool compare(const Type& left, const Type& right, SqlComparison comparison) {
  switch (comparison) {
    case SqlComparison::Equal: return left == right;
    case SqlComparison::NotEqual: return left != right;
    case SqlComparison::Less: return left < right;
    case SqlComparison::LessEqual: return left <= right;
    case SqlComparison::Greater: return left > right;
    case SqlComparison::GreaterEqual: return left >= right;
  }
  return false;
}

RowPredicate bind_predicate(const Schema& schema, const SqlPredicate& predicate) {
  const auto column_index = resolve_column(schema, predicate.column);
  const auto type = schema[column_index].type;
  if (!literal_matches(type, predicate.literal)) {
    throw SqlBindError("literal type does not match column: " + predicate.column);
  }
  if (type == DataType::Boolean && predicate.comparison != SqlComparison::Equal &&
      predicate.comparison != SqlComparison::NotEqual) {
    throw SqlBindError("boolean columns support only equality comparisons");
  }

  const auto comparison = predicate.comparison;
  const Value literal = predicate.literal;
  return [column_index, type, comparison, literal](const TableRow& row) {
    const auto& value = row.values[column_index];
    if (std::holds_alternative<std::monostate>(value) ||
        std::holds_alternative<std::monostate>(literal)) {
      return false;
    }
    switch (type) {
      case DataType::Integer:
        return compare(std::get<std::int64_t>(value), std::get<std::int64_t>(literal), comparison);
      case DataType::Text:
        return compare(std::get<std::string>(value), std::get<std::string>(literal), comparison);
      case DataType::Boolean:
        return compare(std::get<bool>(value), std::get<bool>(literal), comparison);
    }
    return false;
  };
}

}  // namespace

BoundSelect bind_select(TableStore& tables, Catalog& catalog, const SelectPlan& plan) {
  if ((plan.select_all && !plan.columns.empty()) || (!plan.select_all && plan.columns.empty())) {
    throw SqlBindError("logical plan must select either '*' or one or more columns");
  }
  const auto table = resolve_table(catalog, plan.table);
  auto query = Query::from(tables, table.name);
  if (plan.predicate) query.filter(bind_predicate(table.schema, *plan.predicate));

  Schema output_schema;
  if (plan.select_all) {
    output_schema = table.schema;
  } else {
    std::vector<std::size_t> projection;
    projection.reserve(plan.columns.size());
    output_schema.reserve(plan.columns.size());
    for (const auto& name : plan.columns) {
      const auto index = resolve_column(table.schema, name);
      projection.push_back(index);
      output_schema.push_back(table.schema[index]);
    }
    query.project(std::move(projection));
  }
  if (plan.limit) query.limit(*plan.limit);
  return BoundSelect(std::move(query), std::move(output_schema));
}

BoundSelect execute_select_sql(TableStore& tables, Catalog& catalog, std::string_view sql) {
  return bind_select(tables, catalog, parse_select(sql));
}

}  // namespace pageforge
