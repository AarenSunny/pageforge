#pragma once

#include <string_view>

namespace pageforge::detail {

inline bool is_sql_identifier(std::string_view value) {
  if (value.empty()) return false;
  const auto is_initial = [](char character) {
    return (character >= 'A' && character <= 'Z') ||
           (character >= 'a' && character <= 'z') || character == '_';
  };
  if (!is_initial(value.front())) return false;
  for (const auto character : value.substr(1)) {
    if (!is_initial(character) && (character < '0' || character > '9')) return false;
  }
  return true;
}

}  // namespace pageforge::detail
