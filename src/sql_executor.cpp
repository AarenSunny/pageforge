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
  if (predicate.kind != SqlPredicateKind::Comparison) {
    const bool expect_null = predicate.kind == SqlPredicateKind::IsNull;
    return [column_index, expect_null](const TableRow& row) {
      return std::holds_alternative<std::monostate>(row.values[column_index]) == expect_null;
    };
  }
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

RowLess bind_order(const Schema& schema, const SqlOrder& order) {
  const auto column_index = resolve_column(schema, order.column);
  const auto type = schema[column_index].type;
  const auto descending = order.descending;
  return [column_index, type, descending](const TableRow& left, const TableRow& right) {
    const auto& left_value = left.values[column_index];
    const auto& right_value = right.values[column_index];
    const bool left_null = std::holds_alternative<std::monostate>(left_value);
    const bool right_null = std::holds_alternative<std::monostate>(right_value);
    if (left_null || right_null) {
      if (left_null == right_null) return false;
      return !left_null;  // Nulls sort last in both directions.
    }

    const auto less = [&](const Value& first, const Value& second) {
      switch (type) {
        case DataType::Integer:
          return std::get<std::int64_t>(first) < std::get<std::int64_t>(second);
        case DataType::Text:
          return std::get<std::string>(first) < std::get<std::string>(second);
        case DataType::Boolean: return std::get<bool>(first) < std::get<bool>(second);
      }
      return false;
    };
    return descending ? less(right_value, left_value) : less(left_value, right_value);
  };
}

struct IndexLookup {
  std::string name;
  std::int64_t key;
};

std::optional<IndexLookup> choose_index(Catalog& catalog, const TableDefinition& table,
                                        const std::vector<SqlPredicate>& predicates) {
  const auto indexes = catalog.list_indexes();
  for (const auto& predicate : predicates) {
    if (predicate.kind != SqlPredicateKind::Comparison ||
        predicate.comparison != SqlComparison::Equal ||
        !std::holds_alternative<std::int64_t>(predicate.literal)) {
      continue;
    }
    const auto column_index = resolve_column(table.schema, predicate.column);
    if (table.schema[column_index].type != DataType::Integer) continue;
    for (const auto& index : indexes) {
      if (index.table == table.name && index.column == table.schema[column_index].name) {
        return IndexLookup{index.name, std::get<std::int64_t>(predicate.literal)};
      }
    }
  }
  return std::nullopt;
}

struct AnalyzedSelect {
  TableDefinition table;
  std::vector<RowPredicate> predicates;
  std::optional<RowLess> order;
  Schema output_schema;
  std::optional<std::vector<std::size_t>> projection;
  std::optional<IndexLookup> lookup;
};

AnalyzedSelect analyze_select(Catalog& catalog, const SelectPlan& plan) {
  if ((plan.select_all && !plan.columns.empty()) || (!plan.select_all && plan.columns.empty())) {
    throw SqlBindError("logical plan must select either '*' or one or more columns");
  }
  auto table = resolve_table(catalog, plan.table);
  std::vector<RowPredicate> predicates;
  predicates.reserve(plan.predicates.size());
  for (const auto& predicate : plan.predicates) {
    predicates.push_back(bind_predicate(table.schema, predicate));
  }
  std::optional<RowLess> order;
  if (plan.order) order = bind_order(table.schema, *plan.order);

  Schema output_schema;
  std::optional<std::vector<std::size_t>> projection;
  if (plan.select_all) {
    output_schema = table.schema;
  } else {
    projection.emplace();
    projection->reserve(plan.columns.size());
    output_schema.reserve(plan.columns.size());
    for (const auto& name : plan.columns) {
      const auto index = resolve_column(table.schema, name);
      projection->push_back(index);
      output_schema.push_back(table.schema[index]);
    }
  }

  auto lookup = choose_index(catalog, table, plan.predicates);
  return {std::move(table), std::move(predicates), std::move(order), std::move(output_schema),
          std::move(projection), std::move(lookup)};
}

}  // namespace

BoundSelect bind_select(TableStore& tables, Catalog& catalog, const SelectPlan& plan) {
  auto analyzed = analyze_select(catalog, plan);
  auto query = analyzed.lookup
                   ? Query::from_rows(
                         tables.lookup_index(analyzed.lookup->name, analyzed.lookup->key),
                         analyzed.table.schema.size())
                   : Query::from(tables, analyzed.table.name);
  for (auto& predicate : analyzed.predicates) query.filter(std::move(predicate));
  if (analyzed.order) query.sort(std::move(*analyzed.order));
  if (analyzed.projection) query.project(std::move(*analyzed.projection));
  if (plan.limit) query.limit(*plan.limit);
  return BoundSelect(
      std::move(query), std::move(analyzed.output_schema),
      analyzed.lookup ? SelectAccessPath::IndexLookup : SelectAccessPath::TableScan,
      analyzed.lookup ? std::optional<std::string>{analyzed.lookup->name} : std::nullopt);
}

SelectExplanation explain_select(Catalog& catalog, const SelectPlan& plan) {
  auto analyzed = analyze_select(catalog, plan);
  SelectExplanation explanation;
  explanation.table = std::move(analyzed.table.name);
  explanation.access_path =
      analyzed.lookup ? SelectAccessPath::IndexLookup : SelectAccessPath::TableScan;
  if (analyzed.lookup) {
    explanation.index_name = std::move(analyzed.lookup->name);
    explanation.lookup_key = analyzed.lookup->key;
  }
  explanation.predicate_count = plan.predicates.size();
  explanation.residual_predicate_count =
      plan.predicates.size() - static_cast<std::size_t>(analyzed.lookup.has_value());
  explanation.sorts_rows = plan.order.has_value();
  explanation.limit = plan.limit;
  return explanation;
}

BoundSelect execute_select_sql(TableStore& tables, Catalog& catalog, std::string_view sql) {
  return bind_select(tables, catalog, parse_select(sql));
}

SqlExecution execute_sql(TableStore& tables, Catalog& catalog, std::string_view sql) {
  auto statement = parse_sql_statement(sql);
  if (std::holds_alternative<SelectPlan>(statement)) {
    return bind_select(tables, catalog, std::get<SelectPlan>(statement));
  }
  return explain_select(catalog, std::get<ExplainPlan>(statement).select);
}

}  // namespace pageforge
