#include "pageforge/tuple.hpp"

#include <bit>
#include <limits>
#include <string_view>

namespace pageforge {
namespace {

constexpr std::uint32_t kMagic = 0x31544650;  // "PFT1" in little endian.
constexpr std::uint16_t kVersion = 1;
constexpr std::size_t kHeaderSize = 8;

void append_u16(std::vector<std::byte>& output, std::uint16_t value) {
  output.push_back(static_cast<std::byte>(value & 0xffU));
  output.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
}

void append_u32(std::vector<std::byte>& output, std::uint32_t value) {
  for (std::size_t index = 0; index < 4; ++index) {
    output.push_back(static_cast<std::byte>((value >> (index * 8U)) & 0xffU));
  }
}

void append_u64(std::vector<std::byte>& output, std::uint64_t value) {
  for (std::size_t index = 0; index < 8; ++index) {
    output.push_back(static_cast<std::byte>((value >> (index * 8U)) & 0xffU));
  }
}

void require_bytes(std::span<const std::byte> bytes, std::size_t offset, std::size_t count) {
  if (offset > bytes.size() || count > bytes.size() - offset) {
    throw TupleCorruption("tuple payload is truncated");
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

std::uint64_t read_u64(std::span<const std::byte> bytes, std::size_t& offset) {
  require_bytes(bytes, offset, 8);
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(std::to_integer<unsigned>(bytes[offset + index])) << (index * 8U);
  }
  offset += 8;
  return value;
}

bool is_null(const Value& value) { return std::holds_alternative<std::monostate>(value); }

void validate_schema(const Schema& schema) {
  if (schema.size() > std::numeric_limits<std::uint16_t>::max()) {
    throw std::invalid_argument("schema has too many columns");
  }
  for (const auto& column : schema) {
    if (column.type != DataType::Integer && column.type != DataType::Text && column.type != DataType::Boolean) {
      throw std::invalid_argument("schema contains an unknown data type");
    }
  }
}

}  // namespace

std::vector<std::byte> TupleCodec::encode(const Schema& schema, const Tuple& tuple) {
  validate_schema(schema);
  if (tuple.size() != schema.size()) throw std::invalid_argument("tuple does not match schema arity");

  const auto bitmap_size = (schema.size() + 7) / 8;
  std::vector<std::byte> output;
  output.reserve(kHeaderSize + bitmap_size + schema.size() * 8);
  append_u32(output, kMagic);
  append_u16(output, kVersion);
  append_u16(output, static_cast<std::uint16_t>(schema.size()));
  const auto bitmap_offset = output.size();
  output.resize(output.size() + bitmap_size);

  for (std::size_t index = 0; index < schema.size(); ++index) {
    const auto& column = schema[index];
    const auto& value = tuple[index];
    if (is_null(value)) {
      if (!column.nullable) throw std::invalid_argument("null supplied for non-nullable column");
      output[bitmap_offset + index / 8] |= static_cast<std::byte>(1U << (index % 8));
      continue;
    }

    switch (column.type) {
      case DataType::Integer:
        if (!std::holds_alternative<std::int64_t>(value)) throw std::invalid_argument("tuple value type mismatch");
        append_u64(output, std::bit_cast<std::uint64_t>(std::get<std::int64_t>(value)));
        break;
      case DataType::Text: {
        if (!std::holds_alternative<std::string>(value)) throw std::invalid_argument("tuple value type mismatch");
        const auto& text = std::get<std::string>(value);
        if (text.size() > std::numeric_limits<std::uint32_t>::max()) {
          throw std::invalid_argument("text value is too large");
        }
        append_u32(output, static_cast<std::uint32_t>(text.size()));
        const auto* begin = reinterpret_cast<const std::byte*>(text.data());
        output.insert(output.end(), begin, begin + text.size());
        break;
      }
      case DataType::Boolean:
        if (!std::holds_alternative<bool>(value)) throw std::invalid_argument("tuple value type mismatch");
        output.push_back(std::get<bool>(value) ? std::byte{1} : std::byte{0});
        break;
      default:
        throw std::invalid_argument("schema contains an unknown data type");
    }
  }
  return output;
}

Tuple TupleCodec::decode(const Schema& schema, std::span<const std::byte> bytes) {
  validate_schema(schema);
  std::size_t offset = 0;
  if (read_u32(bytes, offset) != kMagic) throw TupleCorruption("invalid tuple magic");
  if (read_u16(bytes, offset) != kVersion) throw TupleCorruption("unsupported tuple version");
  if (read_u16(bytes, offset) != schema.size()) throw TupleCorruption("tuple column count does not match schema");

  const auto bitmap_size = (schema.size() + 7) / 8;
  require_bytes(bytes, offset, bitmap_size);
  const auto bitmap = bytes.subspan(offset, bitmap_size);
  offset += bitmap_size;
  if (!bitmap.empty() && schema.size() % 8 != 0) {
    const auto unused_mask = static_cast<unsigned>(0xffU << (schema.size() % 8));
    if ((std::to_integer<unsigned>(bitmap.back()) & unused_mask) != 0) {
      throw TupleCorruption("tuple null bitmap has unknown bits set");
    }
  }

  Tuple tuple;
  tuple.reserve(schema.size());
  for (std::size_t index = 0; index < schema.size(); ++index) {
    const bool null = (std::to_integer<unsigned>(bitmap[index / 8]) & (1U << (index % 8))) != 0;
    if (null) {
      if (!schema[index].nullable) throw TupleCorruption("non-nullable column is encoded as null");
      tuple.emplace_back(std::monostate{});
      continue;
    }

    switch (schema[index].type) {
      case DataType::Integer:
        tuple.emplace_back(std::bit_cast<std::int64_t>(read_u64(bytes, offset)));
        break;
      case DataType::Text: {
        const auto length = read_u32(bytes, offset);
        require_bytes(bytes, offset, length);
        const auto* begin = reinterpret_cast<const char*>(bytes.data() + offset);
        tuple.emplace_back(std::string(begin, length));
        offset += length;
        break;
      }
      case DataType::Boolean:
        require_bytes(bytes, offset, 1);
        if (bytes[offset] != std::byte{0} && bytes[offset] != std::byte{1}) {
          throw TupleCorruption("invalid boolean encoding");
        }
        tuple.emplace_back(bytes[offset++] == std::byte{1});
        break;
      default:
        throw std::invalid_argument("schema contains an unknown data type");
    }
  }
  if (offset != bytes.size()) throw TupleCorruption("tuple payload has trailing bytes");
  return tuple;
}

}  // namespace pageforge
