#include "pageforge/catalog.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <span>
#include <unordered_set>
#include <utility>

namespace pageforge {
namespace {

constexpr std::array<std::byte, 4> kMagic{std::byte{'P'}, std::byte{'F'}, std::byte{'C'}, std::byte{'1'}};
constexpr std::uint16_t kFormatVersion = 1;
constexpr std::uint8_t kNullable = 1;

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
  if (table.name.empty()) throw std::invalid_argument("table name must not be empty");
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
    if (column.name.empty()) throw std::invalid_argument("column name must not be empty");
    if (column.name.size() > std::numeric_limits<std::uint16_t>::max()) {
      throw std::invalid_argument("column name is too long");
    }
    validate_type(column.type);
    if (!names.insert(column.name).second) throw std::invalid_argument("column names must be unique");
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
  std::vector<std::byte> output(kMagic.begin(), kMagic.end());
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

bool has_magic(std::span<const std::byte> bytes) {
  return bytes.size() >= kMagic.size() && std::equal(kMagic.begin(), kMagic.end(), bytes.begin());
}

TableDefinition decode(std::span<const std::byte> bytes) {
  std::size_t offset = kMagic.size();
  if (read_u16(bytes, offset) != kFormatVersion) throw CatalogCorruption("unsupported catalog format version");
  TableDefinition table;
  table.schema_version = read_u32(bytes, offset);
  if (table.schema_version == 0) throw CatalogCorruption("catalog schema version must be positive");
  table.name = read_text(bytes, offset);
  if (table.name.empty()) throw CatalogCorruption("catalog table name is empty");
  const auto column_count = read_u16(bytes, offset);
  if (column_count == 0) throw CatalogCorruption("catalog table has no columns");
  table.schema.reserve(column_count);
  std::unordered_set<std::string> names;
  for (std::size_t index = 0; index < column_count; ++index) {
    auto name = read_text(bytes, offset);
    if (name.empty()) throw CatalogCorruption("catalog column name is empty");
    if (!names.insert(name).second) throw CatalogCorruption("catalog has duplicate column names");
    require_bytes(bytes, offset, 2);
    const auto type = decode_type(bytes[offset++]);
    const auto flags = std::to_integer<unsigned>(bytes[offset++]);
    if ((flags & ~kNullable) != 0) throw CatalogCorruption("catalog column has unknown flags");
    table.schema.push_back({std::move(name), type, (flags & kNullable) != 0});
  }
  if (offset != bytes.size()) throw CatalogCorruption("catalog entry has trailing bytes");
  return table;
}

}  // namespace

RecordId Catalog::create_table(const TableDefinition& table) {
  validate_table(table);
  if (find_table(table.name)) throw std::invalid_argument("table already exists");
  const auto bytes = encode(table);
  return records_.insert(bytes);
}

std::optional<TableDefinition> Catalog::find_table(std::string_view name) {
  const auto tables = list_tables();
  for (const auto& table : tables) {
    if (table.name == name) return table;
  }
  return std::nullopt;
}

std::vector<TableDefinition> Catalog::list_tables() {
  std::vector<TableDefinition> tables;
  std::unordered_set<std::string> names;
  for (const auto& record : records_.scan()) {
    if (!has_magic(record.bytes)) continue;
    auto table = decode(record.bytes);
    if (!names.insert(table.name).second) throw CatalogCorruption("catalog contains duplicate table names");
    tables.push_back(std::move(table));
  }
  return tables;
}

}  // namespace pageforge
