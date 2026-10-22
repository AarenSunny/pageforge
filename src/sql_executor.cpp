#include "pageforge/sql_executor.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
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

struct IndexAccess {
  SelectAccessPath path;
  std::string name;
  std::string column;
  std::optional<std::int64_t> key;
  std::optional<std::int64_t> lower;
  std::optional<std::int64_t> upper;
  std::size_t covered_predicates;
  std::size_t selected_predicates;
  bool empty;
};

std::optional<IndexAccess> choose_index(Catalog& catalog, const TableDefinition& table,
                                        const std::vector<SqlPredicate>& predicates,
                                        const std::optional<SqlOrder>& order) {
  const auto indexes = catalog.list_indexes();
  const auto matching_index = [&](const SqlPredicate& predicate) -> std::optional<IndexDefinition> {
    if (predicate.kind != SqlPredicateKind::Comparison ||
        !std::holds_alternative<std::int64_t>(predicate.literal)) {
      return std::nullopt;
    }
    const auto column_index = resolve_column(table.schema, predicate.column);
    if (table.schema[column_index].type != DataType::Integer) return std::nullopt;
    for (const auto& index : indexes) {
      if (index.table == table.name && index.column == table.schema[column_index].name) {
        return index;
      }
    }
    return std::nullopt;
  };

  std::vector<IndexAccess> lookups;
  for (const auto& predicate : predicates) {
    if (predicate.comparison != SqlComparison::Equal) continue;
    if (auto index = matching_index(predicate)) {
      const auto existing = std::find_if(lookups.begin(), lookups.end(), [&](const auto& candidate) {
        return candidate.name == index->name;
      });
      if (existing == lookups.end()) {
        lookups.push_back({SelectAccessPath::IndexLookup, std::move(index->name),
                           std::move(index->column),
                           std::get<std::int64_t>(predicate.literal), std::nullopt, std::nullopt, 0,
                           0, false});
      }
    }
  }
  for (auto& lookup : lookups) {
    for (const auto& predicate : predicates) {
      const auto index = matching_index(predicate);
      if (!index || index->name != lookup.name) continue;
      ++lookup.selected_predicates;
      ++lookup.covered_predicates;
      const auto literal = std::get<std::int64_t>(predicate.literal);
      if (!compare(*lookup.key, literal, predicate.comparison)) lookup.empty = true;
    }
  }
  if (!lookups.empty()) {
    const auto reuses_order = [&](const IndexAccess& access) {
      return order && equal_name(access.column, order->column);
    };
    return *std::max_element(lookups.begin(), lookups.end(), [&](const auto& left, const auto& right) {
      if (left.empty != right.empty) return !left.empty;
      if (left.selected_predicates != right.selected_predicates) {
        return left.selected_predicates < right.selected_predicates;
      }
      return reuses_order(left) < reuses_order(right);
    });
  }

  std::vector<IndexAccess> ranges;
  for (const auto& predicate : predicates) {
    if (predicate.comparison == SqlComparison::Equal ||
        predicate.comparison == SqlComparison::NotEqual) {
      continue;
    }
    if (auto index = matching_index(predicate)) {
      const auto key = std::get<std::int64_t>(predicate.literal);
      const bool has_lower = predicate.comparison == SqlComparison::Greater ||
                             predicate.comparison == SqlComparison::GreaterEqual;
      const bool strict = predicate.comparison == SqlComparison::Greater ||
                          predicate.comparison == SqlComparison::Less;
      const bool impossible =
          (predicate.comparison == SqlComparison::Greater &&
           key == std::numeric_limits<std::int64_t>::max()) ||
          (predicate.comparison == SqlComparison::Less &&
           key == std::numeric_limits<std::int64_t>::min());
      const auto bound = impossible || !strict
                             ? key
                             : predicate.comparison == SqlComparison::Greater ? key + 1 : key - 1;
      auto range = std::find_if(ranges.begin(), ranges.end(), [&](const auto& candidate) {
        return candidate.name == index->name;
      });
      if (range == ranges.end()) {
        ranges.push_back(
            {SelectAccessPath::IndexRange, std::move(index->name), std::move(index->column),
             std::nullopt,
             has_lower ? std::optional<std::int64_t>{bound} : std::nullopt,
             has_lower ? std::nullopt : std::optional<std::int64_t>{bound}, 1, 1, impossible});
        continue;
      }
      ++range->selected_predicates;
      if (range->empty) {
        range->covered_predicates = range->selected_predicates;
        continue;
      }
      if (impossible) {
        range->empty = true;
        range->covered_predicates = range->selected_predicates;
        continue;
      }

      auto lower = range->lower;
      auto upper = range->upper;
      if (has_lower && (!lower || bound > *lower)) lower = bound;
      if (!has_lower && (!upper || bound < *upper)) upper = bound;
      range->lower = lower;
      range->upper = upper;
      if (lower && upper && *lower > *upper) {
        range->empty = true;
        range->covered_predicates = range->selected_predicates;
        continue;
      }
      ++range->covered_predicates;
    }
  }
  if (ranges.empty()) return std::nullopt;
  const auto bound_count = [](const IndexAccess& access) {
    return static_cast<std::size_t>(access.lower.has_value()) +
           static_cast<std::size_t>(access.upper.has_value());
  };
  const auto reuses_order = [&](const IndexAccess& access) {
    return order && equal_name(access.column, order->column);
  };
  return *std::max_element(ranges.begin(), ranges.end(), [&](const auto& left, const auto& right) {
    if (left.empty != right.empty) return !left.empty;
    if (bound_count(left) != bound_count(right)) return bound_count(left) < bound_count(right);
    if (left.selected_predicates != right.selected_predicates) {
      return left.selected_predicates < right.selected_predicates;
    }
    return reuses_order(left) < reuses_order(right);
  });
}

struct AnalyzedSelect {
  TableDefinition table;
  std::vector<RowPredicate> predicates;
  std::optional<RowLess> order;
  Schema output_schema;
  std::optional<std::vector<std::size_t>> projection;
  std::optional<IndexAccess> access;
  bool sorts_rows;
  bool reverses_index_order;
  std::optional<std::size_t> candidate_limit;
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

  auto access = choose_index(catalog, table, plan.predicates, plan.order);
  bool sorts_rows = plan.order.has_value();
  const bool matching_index_order =
      plan.order && access && equal_name(access->column, plan.order->column) &&
      (access->path == SelectAccessPath::IndexLookup ||
       access->path == SelectAccessPath::IndexRange);
  const bool reverses_index_order =
      matching_index_order && plan.order->descending &&
      access->path == SelectAccessPath::IndexRange && !access->empty;
  if (plan.order && access &&
      (access->empty || matching_index_order)) {
    order.reset();
    sorts_rows = false;
  }
  std::optional<std::size_t> candidate_limit;
  if (plan.limit && *plan.limit > 0 && access && !access->empty && !sorts_rows &&
      access->covered_predicates == plan.predicates.size()) {
    candidate_limit = plan.limit;
  }
  return {std::move(table), std::move(predicates), std::move(order), std::move(output_schema),
          std::move(projection), std::move(access), sorts_rows, reverses_index_order,
          candidate_limit};
}

}  // namespace

BoundSelect bind_select(TableStore& tables, Catalog& catalog, const SelectPlan& plan) {
  auto analyzed = analyze_select(catalog, plan);
  auto make_query = [&]() {
    if (plan.limit && *plan.limit == 0) {
      return Query::from_rows({}, analyzed.table.schema.size());
    }
    if (!analyzed.access) return Query::from(tables, analyzed.table.name);
    if (analyzed.access->empty) {
      return Query::from_rows({}, analyzed.table.schema.size());
    }
    if (analyzed.access->path == SelectAccessPath::IndexLookup) {
      return Query::from_rows(
          tables.lookup_index(analyzed.access->name, *analyzed.access->key,
                              analyzed.candidate_limit),
          analyzed.table.schema.size());
    }
    return Query::from_rows(
        tables.lookup_index_range(analyzed.access->name, analyzed.access->lower,
                                  analyzed.access->upper, analyzed.reverses_index_order,
                                  analyzed.candidate_limit),
        analyzed.table.schema.size());
  };
  auto query = make_query();
  for (auto& predicate : analyzed.predicates) query.filter(std::move(predicate));
  if (analyzed.order) query.sort(std::move(*analyzed.order));
  if (analyzed.projection) query.project(std::move(*analyzed.projection));
  if (plan.limit) query.limit(*plan.limit);
  return BoundSelect(
      std::move(query), std::move(analyzed.output_schema),
      analyzed.access ? analyzed.access->path : SelectAccessPath::TableScan,
      analyzed.access ? std::optional<std::string>{analyzed.access->name} : std::nullopt);
}

SelectExplanation explain_select(Catalog& catalog, const SelectPlan& plan) {
  auto analyzed = analyze_select(catalog, plan);
  SelectExplanation explanation;
  explanation.table = std::move(analyzed.table.name);
  explanation.access_path = analyzed.access ? analyzed.access->path : SelectAccessPath::TableScan;
  if (analyzed.access) {
    explanation.index_name = std::move(analyzed.access->name);
    explanation.lookup_key = analyzed.access->key;
    explanation.lower_bound = analyzed.access->lower;
    explanation.upper_bound = analyzed.access->upper;
    if (analyzed.access->path == SelectAccessPath::IndexRange && !analyzed.access->empty) {
      explanation.index_scan_direction = analyzed.reverses_index_order
                                             ? IndexScanDirection::Reverse
                                             : IndexScanDirection::Forward;
    }
  }
  explanation.empty_result = (plan.limit && *plan.limit == 0) ||
                             (analyzed.access && analyzed.access->empty);
  explanation.predicate_count = plan.predicates.size();
  explanation.residual_predicate_count = plan.predicates.size() -
                                         (analyzed.access ? analyzed.access->covered_predicates : 0);
  explanation.sorts_rows = analyzed.sorts_rows;
  explanation.limit = plan.limit;
  explanation.candidate_limit = analyzed.candidate_limit;
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
