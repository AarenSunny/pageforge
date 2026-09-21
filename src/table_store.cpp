#include "pageforge/table_store.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <utility>

namespace pageforge {
namespace {

constexpr std::array<std::byte, 4> kMagic{std::byte{'P'}, std::byte{'F'}, std::byte{'R'}, std::byte{'1'}};
constexpr std::uint16_t kVersion = 1;

void append_u16(std::vector<std::byte>& output, std::uint16_t value) {
  output.push_back(static_cast<std::byte>(value & 0xffU));
  output.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
}

void append_u32(std::vector<std::byte>& output, std::uint32_t value) {
  for (std::size_t index = 0; index < 4; ++index) {
    output.push_back(static_cast<std::byte>((value >> (index * 8U)) & 0xffU));
  }
}

void require_bytes(std::span<const std::byte> bytes, std::size_t offset, std::size_t count) {
  if (offset > bytes.size() || count > bytes.size() - offset) {
    throw TableCorruption("table row envelope is truncated");
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

bool has_magic(std::span<const std::byte> bytes) {
  return bytes.size() >= kMagic.size() && std::equal(kMagic.begin(), kMagic.end(), bytes.begin());
}

struct RowEnvelope {
  std::string table_name;
  std::uint32_t schema_version;
  std::span<const std::byte> tuple_bytes;
};

RowEnvelope parse(std::span<const std::byte> bytes) {
  if (!has_magic(bytes)) throw std::invalid_argument("record is not a typed table row");
  std::size_t offset = kMagic.size();
  if (read_u16(bytes, offset) != kVersion) throw TableCorruption("unsupported table row version");
  const auto schema_version = read_u32(bytes, offset);
  if (schema_version == 0) throw TableCorruption("table row has invalid schema version");
  const auto name_length = read_u16(bytes, offset);
  if (name_length == 0) throw TableCorruption("table row has no table name");
  require_bytes(bytes, offset, name_length);
  const auto* begin = reinterpret_cast<const char*>(bytes.data() + offset);
  std::string table_name(begin, name_length);
  offset += name_length;
  return {std::move(table_name), schema_version, bytes.subspan(offset)};
}

std::vector<std::byte> wrap(const TableDefinition& table, std::span<const std::byte> tuple_bytes) {
  if (table.name.size() > std::numeric_limits<std::uint16_t>::max()) {
    throw std::invalid_argument("table name is too long");
  }
  std::vector<std::byte> output(kMagic.begin(), kMagic.end());
  append_u16(output, kVersion);
  append_u32(output, table.schema_version);
  append_u16(output, static_cast<std::uint16_t>(table.name.size()));
  const auto* begin = reinterpret_cast<const std::byte*>(table.name.data());
  output.insert(output.end(), begin, begin + table.name.size());
  output.insert(output.end(), tuple_bytes.begin(), tuple_bytes.end());
  return output;
}

Tuple decode_row(const TableDefinition& table, const RowEnvelope& envelope) {
  if (envelope.schema_version != table.schema_version) {
    throw TableCorruption("row schema version does not match the catalog");
  }
  try {
    return TupleCodec::decode(table.schema, envelope.tuple_bytes);
  } catch (const TupleCorruption& error) {
    throw TableCorruption(std::string("invalid tuple in table row: ") + error.what());
  }
}

}  // namespace

TableDefinition TableStore::require_table(std::string_view name) {
  auto table = catalog_.find_table(name);
  if (!table) throw std::out_of_range("table does not exist");
  return std::move(*table);
}

RecordId TableStore::insert(std::string_view table_name, const Tuple& values) {
  const auto table = require_table(table_name);
  const auto tuple_bytes = TupleCodec::encode(table.schema, values);
  return records_.insert(wrap(table, tuple_bytes));
}

Tuple TableStore::read(std::string_view table_name, RecordId id) {
  const auto table = require_table(table_name);
  const auto bytes = records_.read(id);
  const auto envelope = parse(bytes);
  if (envelope.table_name != table.name) throw std::invalid_argument("record belongs to another table");
  return decode_row(table, envelope);
}

std::vector<TableRow> TableStore::scan(std::string_view table_name) {
  const auto table = require_table(table_name);
  std::vector<TableRow> rows;
  for (const auto& record : records_.scan()) {
    if (!has_magic(record.bytes)) continue;
    const auto envelope = parse(record.bytes);
    if (envelope.table_name == table.name) rows.push_back({record.id, decode_row(table, envelope)});
  }
  return rows;
}

bool TableStore::erase(std::string_view table_name, RecordId id) {
  const auto table = require_table(table_name);
  std::vector<std::byte> bytes;
  try {
    bytes = records_.read(id);
  } catch (const std::out_of_range&) {
    return false;
  }
  const auto envelope = parse(bytes);
  if (envelope.table_name != table.name) throw std::invalid_argument("record belongs to another table");
  (void)decode_row(table, envelope);
  return records_.erase(id);
}

}  // namespace pageforge
