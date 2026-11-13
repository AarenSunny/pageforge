#include "pageforge/catalog.hpp"

#include "pageforge/bplus_tree.hpp"
#include "sql_identifier.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <span>
#include <unordered_set>
#include <utility>

namespace pageforge {
namespace {

constexpr std::array<std::byte, 4> kTableMagic{std::byte{'P'}, std::byte{'F'}, std::byte{'C'},
                                               std::byte{'1'}};
constexpr std::array<std::byte, 4> kIndexMagic{std::byte{'P'}, std::byte{'F'}, std::byte{'X'},
                                               std::byte{'1'}};
constexpr std::uint16_t kFormatVersion = 1;
constexpr std::uint8_t kNullable = 1;

char ascii_lower(char value) {
  return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

std::string identifier_key(std::string_view value) {
  std::string key;
  key.reserve(value.size());
  for (const auto character : value) key.push_back(ascii_lower(character));
  return key;
}

void append_u16(std::vector<std::byte>& output, std::uint16_t value) {
  output.push_back(static_cast<std::byte>(value & 0xffU));
  output.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
}

void append_u32(std::vector<std::byte>& output, std::uint32_t value) {
  for (std::size_t index = 0; index < 4; ++index) {
    output.push_back(static_cast<std::byte>((value >> (index * 8U)) & 0xffU));
  }
}

void append_text(std::vector<std::byte>& output, std::string_view text) {
  if (text.size() > std::numeric_limits<std::uint16_t>::max()) {
    throw std::invalid_argument("catalog identifier is too long");
  }
  append_u16(output, static_cast<std::uint16_t>(text.size()));
  const auto* begin = reinterpret_cast<const std::byte*>(text.data());
  output.insert(output.end(), begin, begin + text.size());
}

void require_bytes(std::span<const std::byte> bytes, std::size_t offset, std::size_t count) {
  if (offset > bytes.size() || count > bytes.size() - offset) {
    throw CatalogCorruption("catalog entry is truncated");
  }
}

std::uint16_t read_u16(std::span<const std::byte> bytes, std::size_t& offset) {
  require_bytes(bytes, offset, 2);
  const auto value = static_cast<std::uint16_t>(std::to_integer<unsigned>(bytes[offset])) |
                     static_cast<std::uint16_t>(std::to_integer<unsigned>(bytes[offset + 1]) << 8U);
  offset += 2;
  return value;
}

std::uint32_t read_u32(std::span<const std::byte> bytes, std::size_t& offset) {
  require_bytes(bytes, offset, 4);
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(std::to_integer<unsigned>(bytes[offset + index])) << (index * 8U);
  }
  offset += 4;
  return value;
}

std::string read_text(std::span<const std::byte> bytes, std::size_t& offset) {
  const auto length = read_u16(bytes, offset);
  require_bytes(bytes, offset, length);
  const auto* begin = reinterpret_cast<const char*>(bytes.data() + offset);
  std::string result(begin, length);
  offset += length;
  return result;
}

void validate_type(DataType type) {
  if (type != DataType::Integer && type != DataType::Text && type != DataType::Boolean) {
    throw std::invalid_argument("catalog column has an unknown type");
  }
}

void validate_table(const TableDefinition& table) {
  if (!detail::is_sql_identifier(table.name)) {
    throw std::invalid_argument("table name is not a SQL identifier");
  }
  if (table.name.size() > std::numeric_limits<std::uint16_t>::max()) {
    throw std::invalid_argument("table name is too long");
  }
  if (table.schema.empty()) throw std::invalid_argument("table must have at least one column");
  if (table.schema.size() > std::numeric_limits<std::uint16_t>::max()) {
    throw std::invalid_argument("table has too many columns");
  }
  if (table.schema_version == 0) throw std::invalid_argument("schema version must be positive");

  std::unordered_set<std::string> names;
  for (const auto& column : table.schema) {
    if (!detail::is_sql_identifier(column.name)) {
      throw std::invalid_argument("column name is not a SQL identifier");
    }
    if (column.name.size() > std::numeric_limits<std::uint16_t>::max()) {
      throw std::invalid_argument("column name is too long");
    }
    validate_type(column.type);
    if (!names.insert(identifier_key(column.name)).second) {
      throw std::invalid_argument("column names must be unique case-insensitively");
    }
  }
}

std::uint8_t encode_type(DataType type) {
  switch (type) {
    case DataType::Integer:
      return 1;
    case DataType::Text:
      return 2;
    case DataType::Boolean:
      return 3;
    default:
      throw std::invalid_argument("catalog column has an unknown type");
  }
}

DataType decode_type(std::byte byte) {
  switch (std::to_integer<unsigned>(byte)) {
    case 1:
      return DataType::Integer;
    case 2:
      return DataType::Text;
    case 3:
      return DataType::Boolean;
    default:
      throw CatalogCorruption("catalog column has an unknown type tag");
  }
}

std::vector<std::byte> encode(const TableDefinition& table) {
  validate_table(table);
  std::vector<std::byte> output(kTableMagic.begin(), kTableMagic.end());
  append_u16(output, kFormatVersion);
  append_u32(output, table.schema_version);
  append_text(output, table.name);
  append_u16(output, static_cast<std::uint16_t>(table.schema.size()));
  for (const auto& column : table.schema) {
    append_text(output, column.name);
    output.push_back(static_cast<std::byte>(encode_type(column.type)));
    output.push_back(column.nullable ? static_cast<std::byte>(kNullable) : std::byte{0});
  }
  return output;
}

bool has_magic(std::span<const std::byte> bytes, std::span<const std::byte> magic) {
  return bytes.size() >= magic.size() && std::equal(magic.begin(), magic.end(), bytes.begin());
}

std::uint64_t record_key(RecordId id) {
  return (static_cast<std::uint64_t>(id.page_id) << 16U) | id.slot_id;
}

TableDefinition decode(std::span<const std::byte> bytes) {
  std::size_t offset = kTableMagic.size();
  if (read_u16(bytes, offset) != kFormatVersion) throw CatalogCorruption("unsupported catalog format version");
  TableDefinition table;
  table.schema_version = read_u32(bytes, offset);
  if (table.schema_version == 0) throw CatalogCorruption("catalog schema version must be positive");
  table.name = read_text(bytes, offset);
  if (!detail::is_sql_identifier(table.name)) {
    throw CatalogCorruption("catalog table name is not a SQL identifier");
  }
  const auto column_count = read_u16(bytes, offset);
  if (column_count == 0) throw CatalogCorruption("catalog table has no columns");
  table.schema.reserve(column_count);
  std::unordered_set<std::string> names;
  for (std::size_t index = 0; index < column_count; ++index) {
    auto name = read_text(bytes, offset);
    if (!detail::is_sql_identifier(name)) {
      throw CatalogCorruption("catalog column name is not a SQL identifier");
    }
    if (!names.insert(identifier_key(name)).second) {
      throw CatalogCorruption("catalog has case-insensitive duplicate column names");
    }
    require_bytes(bytes, offset, 2);
    const auto type = decode_type(bytes[offset++]);
    const auto flags = std::to_integer<unsigned>(bytes[offset++]);
    if ((flags & ~kNullable) != 0) throw CatalogCorruption("catalog column has unknown flags");
    table.schema.push_back({std::move(name), type, (flags & kNullable) != 0});
  }
  if (offset != bytes.size()) throw CatalogCorruption("catalog entry has trailing bytes");
  return table;
}

void validate_index_identifiers(const IndexDefinition& index) {
  const std::array<std::pair<std::string_view, std::string_view>, 3> identifiers{{
      {index.name, "index name"}, {index.table, "index table name"}, {index.column, "index column name"}}};
  for (const auto& [value, label] : identifiers) {
    if (!detail::is_sql_identifier(value)) {
      throw std::invalid_argument(std::string(label) + " is not a SQL identifier");
    }
    if (value.size() > std::numeric_limits<std::uint16_t>::max()) {
      throw std::invalid_argument(std::string(label) + " is too long");
    }
  }
}

std::vector<std::byte> encode_index(const IndexDefinition& index) {
  validate_index_identifiers(index);
  std::vector<std::byte> output(kIndexMagic.begin(), kIndexMagic.end());
  append_u16(output, kFormatVersion);
  append_u16(output, 0);
  append_u32(output, index.header_id.page_id);
  append_u16(output, index.header_id.slot_id);
  append_u16(output, 0);
  append_text(output, index.name);
  append_text(output, index.table);
  append_text(output, index.column);
  return output;
}

IndexDefinition decode_index(std::span<const std::byte> bytes) {
  std::size_t offset = kIndexMagic.size();
  if (read_u16(bytes, offset) != kFormatVersion) {
    throw CatalogCorruption("unsupported index catalog format version");
  }
  if (read_u16(bytes, offset) != 0) throw CatalogCorruption("index catalog entry has unknown flags");
  const auto page_id = read_u32(bytes, offset);
  const auto slot_id = read_u16(bytes, offset);
  if (read_u16(bytes, offset) != 0) throw CatalogCorruption("index catalog entry has invalid padding");
  IndexDefinition index{read_text(bytes, offset), read_text(bytes, offset), read_text(bytes, offset),
                        {page_id, slot_id}};
  if (!detail::is_sql_identifier(index.name) || !detail::is_sql_identifier(index.table) ||
      !detail::is_sql_identifier(index.column)) {
    throw CatalogCorruption("index catalog entry has a non-SQL identifier");
  }
  if (offset != bytes.size()) throw CatalogCorruption("index catalog entry has trailing bytes");
  return index;
}

IndexDefinition resolve_index_reference(IndexDefinition index,
                                        std::span<const TableDefinition> tables) {
  const auto table_key = identifier_key(index.table);
  const auto table = std::find_if(tables.begin(), tables.end(), [&](const auto& candidate) {
    return identifier_key(candidate.name) == table_key;
  });
  if (table == tables.end()) {
    throw CatalogCorruption("index catalog entry references a missing table");
  }
  const auto column_key = identifier_key(index.column);
  const auto column = std::find_if(table->schema.begin(), table->schema.end(),
                                   [&](const auto& candidate) {
                                     return identifier_key(candidate.name) == column_key;
                                   });
  if (column == table->schema.end()) {
    throw CatalogCorruption("index catalog entry references a missing column");
  }
  if (column->type != DataType::Integer) {
    throw CatalogCorruption("index catalog entry references a non-integer column");
  }
  index.table = table->name;
  index.column = column->name;
  return index;
}

}  // namespace

RecordId Catalog::create_table(const TableDefinition& table) {
  validate_table(table);
  const auto key = identifier_key(table.name);
  for (const auto& existing : list_tables()) {
    if (identifier_key(existing.name) == key) throw std::invalid_argument("table already exists");
  }
  const auto bytes = encode(table);
  return records_.insert(bytes);
}

std::optional<TableDefinition> Catalog::find_table(std::string_view name) {
  const auto key = identifier_key(name);
  std::optional<TableDefinition> found;
  std::unordered_set<std::string> names;
  auto cursor = records_.cursor();
  while (auto record = cursor.next()) {
    if (!has_magic(record->bytes, kTableMagic)) continue;
    auto table = decode(record->bytes);
    if (!names.insert(identifier_key(table.name)).second) {
      throw CatalogCorruption("catalog contains case-insensitive duplicate table names");
    }
    if (identifier_key(table.name) == key) found = std::move(table);
  }
  return found;
}

std::vector<TableDefinition> Catalog::list_tables() {
  std::vector<TableDefinition> tables;
  std::unordered_set<std::string> names;
  auto cursor = records_.cursor();
  while (auto record = cursor.next()) {
    if (!has_magic(record->bytes, kTableMagic)) continue;
    auto table = decode(record->bytes);
    if (!names.insert(identifier_key(table.name)).second) {
      throw CatalogCorruption("catalog contains case-insensitive duplicate table names");
    }
    tables.push_back(std::move(table));
  }
  return tables;
}

RecordId Catalog::register_index(const IndexDefinition& index) {
  validate_index_identifiers(index);
  const auto name_key = identifier_key(index.name);
  for (const auto& existing : list_indexes()) {
    if (identifier_key(existing.name) == name_key) {
      throw std::invalid_argument("index already exists");
    }
    if (existing.header_id == index.header_id) {
      throw std::invalid_argument("index tree is already registered");
    }
  }
  const auto table = find_table(index.table);
  if (!table) throw std::invalid_argument("index table does not exist");
  const auto column_key = identifier_key(index.column);
  const auto column = std::find_if(table->schema.begin(), table->schema.end(), [&](const auto& candidate) {
    return identifier_key(candidate.name) == column_key;
  });
  if (column == table->schema.end()) throw std::invalid_argument("index column does not exist");
  if (column->type != DataType::Integer) {
    throw std::invalid_argument("B+ tree indexes require an integer column");
  }
  (void)BPlusTreeIndex::open(records_, index.header_id);
  auto normalized = index;
  normalized.table = table->name;
  normalized.column = column->name;
  return records_.insert(encode_index(normalized));
}

std::optional<IndexDefinition> Catalog::find_index(std::string_view name) {
  const auto key = identifier_key(name);
  const auto tables = list_tables();
  std::optional<IndexDefinition> found;
  std::unordered_set<std::string> names;
  std::unordered_set<std::uint64_t> header_ids;
  auto cursor = records_.cursor();
  while (auto record = cursor.next()) {
    if (!has_magic(record->bytes, kIndexMagic)) continue;
    auto index = resolve_index_reference(decode_index(record->bytes), tables);
    if (!names.insert(identifier_key(index.name)).second) {
      throw CatalogCorruption("catalog contains case-insensitive duplicate index names");
    }
    if (!header_ids.insert(record_key(index.header_id)).second) {
      throw CatalogCorruption("catalog contains shared index tree ownership");
    }
    if (identifier_key(index.name) == key) found = std::move(index);
  }
  return found;
}

std::vector<IndexDefinition> Catalog::list_indexes() {
  const auto tables = list_tables();
  std::vector<IndexDefinition> indexes;
  std::unordered_set<std::string> names;
  std::unordered_set<std::uint64_t> header_ids;
  auto cursor = records_.cursor();
  while (auto record = cursor.next()) {
    if (!has_magic(record->bytes, kIndexMagic)) continue;
    auto index = resolve_index_reference(decode_index(record->bytes), tables);
    if (!names.insert(identifier_key(index.name)).second) {
      throw CatalogCorruption("catalog contains case-insensitive duplicate index names");
    }
    if (!header_ids.insert(record_key(index.header_id)).second) {
      throw CatalogCorruption("catalog contains shared index tree ownership");
    }
    indexes.push_back(std::move(index));
  }
  return indexes;
}

}  // namespace pageforge
