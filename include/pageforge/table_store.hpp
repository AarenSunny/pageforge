#pragma once

#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "pageforge/catalog.hpp"

namespace pageforge {

struct TableRow {
  RecordId id;
  Tuple values;
};

class TableCorruption final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class TableCursor {
 public:
  TableCursor(const TableCursor&) = delete;
  TableCursor& operator=(const TableCursor&) = delete;
  TableCursor(TableCursor&&) = default;

  [[nodiscard]] std::optional<TableRow> next();

 private:
  friend class TableStore;
  TableCursor(RecordCursor cursor, TableDefinition table);

  RecordCursor cursor_;
  TableDefinition table_;
};

class TableStore {
 public:
  TableStore(RecordStore& records, Catalog& catalog) : records_(records), catalog_(catalog) {}

  [[nodiscard]] RecordId insert(std::string_view table_name, const Tuple& values);
  [[nodiscard]] Tuple read(std::string_view table_name, RecordId id);
  [[nodiscard]] TableCursor cursor(std::string_view table_name);
  [[nodiscard]] std::vector<TableRow> scan(std::string_view table_name);
  bool erase(std::string_view table_name, RecordId id);

 private:
  [[nodiscard]] TableDefinition require_table(std::string_view name);

  RecordStore& records_;
  Catalog& catalog_;
};

}  // namespace pageforge
