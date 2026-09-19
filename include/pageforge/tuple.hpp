#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace pageforge {

enum class DataType : std::uint8_t { Integer, Text, Boolean };

struct Column {
  std::string name;
  DataType type;
  bool nullable = false;
};

using Schema = std::vector<Column>;
using Value = std::variant<std::monostate, std::int64_t, std::string, bool>;
using Tuple = std::vector<Value>;

class TupleCorruption final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class TupleCodec {
 public:
  [[nodiscard]] static std::vector<std::byte> encode(const Schema& schema, const Tuple& tuple);
  [[nodiscard]] static Tuple decode(const Schema& schema, std::span<const std::byte> bytes);
};

}  // namespace pageforge
