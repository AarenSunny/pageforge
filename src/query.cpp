#include "pageforge/query.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

namespace pageforge {
namespace {

class ScanOperator final : public RowOperator {
 public:
  explicit ScanOperator(TableCursor cursor) : cursor_(std::move(cursor)) {}
  std::optional<TableRow> next() override { return cursor_.next(); }

 private:
  TableCursor cursor_;
};

class FilterOperator final : public RowOperator {
 public:
  FilterOperator(std::unique_ptr<RowOperator> input, RowPredicate predicate)
      : input_(std::move(input)), predicate_(std::move(predicate)) {}

  std::optional<TableRow> next() override {
    while (auto row = input_->next()) {
      if (predicate_(*row)) return row;
    }
    return std::nullopt;
  }

 private:
  std::unique_ptr<RowOperator> input_;
  RowPredicate predicate_;
};

class ProjectOperator final : public RowOperator {
 public:
  ProjectOperator(std::unique_ptr<RowOperator> input, std::vector<std::size_t> columns)
      : input_(std::move(input)), columns_(std::move(columns)) {}

  std::optional<TableRow> next() override {
    auto row = input_->next();
    if (!row) return std::nullopt;
    Tuple projected;
    projected.reserve(columns_.size());
    for (const auto column : columns_) projected.push_back(row->values[column]);
    row->values = std::move(projected);
    return row;
  }

 private:
  std::unique_ptr<RowOperator> input_;
  std::vector<std::size_t> columns_;
};

class SortOperator final : public RowOperator {
 public:
  SortOperator(std::unique_ptr<RowOperator> input, RowLess less)
      : input_(std::move(input)), less_(std::move(less)) {}

  std::optional<TableRow> next() override {
    if (!loaded_) load();
    if (next_ == rows_.size()) return std::nullopt;
    return std::move(rows_[next_++]);
  }

 private:
  void load() {
    while (auto row = input_->next()) rows_.push_back(std::move(*row));
    std::stable_sort(rows_.begin(), rows_.end(), less_);
    input_.reset();
    loaded_ = true;
  }

  std::unique_ptr<RowOperator> input_;
  RowLess less_;
  std::vector<TableRow> rows_;
  std::size_t next_ = 0;
  bool loaded_ = false;
};

class LimitOperator final : public RowOperator {
 public:
  LimitOperator(std::unique_ptr<RowOperator> input, std::size_t count)
      : input_(std::move(input)), remaining_(count) {}

  std::optional<TableRow> next() override {
    if (remaining_ == 0) return std::nullopt;
    auto row = input_->next();
    if (!row) return std::nullopt;
    --remaining_;
    return row;
  }

 private:
  std::unique_ptr<RowOperator> input_;
  std::size_t remaining_;
};

}  // namespace

Query Query::from(TableStore& tables, std::string_view table_name) {
  auto cursor = tables.cursor(table_name);
  const auto column_count = cursor.column_count();
  return Query(std::make_unique<ScanOperator>(std::move(cursor)), column_count);
}

Query& Query::filter(RowPredicate predicate) {
  if (!predicate) throw std::invalid_argument("filter predicate must not be empty");
  root_ = std::make_unique<FilterOperator>(std::move(root_), std::move(predicate));
  return *this;
}

Query& Query::sort(RowLess less) {
  if (!less) throw std::invalid_argument("sort comparator must not be empty");
  root_ = std::make_unique<SortOperator>(std::move(root_), std::move(less));
  return *this;
}

Query& Query::project(std::vector<std::size_t> columns) {
  for (const auto column : columns) {
    if (column >= column_count_) throw std::out_of_range("projection column is outside the current row");
  }
  const auto projected_count = columns.size();
  root_ = std::make_unique<ProjectOperator>(std::move(root_), std::move(columns));
  column_count_ = projected_count;
  return *this;
}

Query& Query::limit(std::size_t count) {
  root_ = std::make_unique<LimitOperator>(std::move(root_), count);
  return *this;
}

std::optional<TableRow> Query::next() { return root_->next(); }

}  // namespace pageforge
