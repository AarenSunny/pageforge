#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "pageforge/record_store.hpp"
#include "pageforge/tuple.hpp"

namespace pageforge {

struct TableDefinition {
  std::string name;
  Schema schema;
  std::uint32_t schema_version = 1;

  bool operator==(const TableDefinition&) const = default;
};

class CatalogCorruption final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class Catalog {
 public:
  explicit Catalog(RecordStore& records) : records_(records) {}

  [[nodiscard]] RecordId create_table(const TableDefinition& table);
  [[nodiscard]] std::optional<TableDefinition> find_table(std::string_view name);
  [[nodiscard]] std::vector<TableDefinition> list_tables();

 private:
  RecordStore& records_;
};

}  // namespace pageforge
