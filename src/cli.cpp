#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <unistd.h>

#include "pageforge/record_store.hpp"
#include "pageforge/sql_executor.hpp"

namespace {

void usage() {
  std::cerr << "Usage: pageforge <database> <command> [arguments]\n"
               "  init                              Create a new database\n"
               "  create-table <table> <name:type>... Create a typed table\n"
               "  insert <table> <value>...          Insert a typed row\n"
               "  query <select-sql>                 Execute a SELECT statement\n"
               "  shell                              Start the interactive SQL shell\n"
               "  put <text>                          Insert a raw text record\n"
               "  get <page:slot>                     Read a raw record\n"
               "  erase <page:slot>                   Delete a raw record\n"
               "  list                                List all raw records\n"
               "Types: int, text, bool; add ? for nullable (for example text?).\n";
}

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

bool is_sql_identifier(std::string_view value) {
  if (value.empty()) return false;
  const auto letter = [](char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           character == '_';
  };
  if (!letter(value.front())) return false;
  for (const auto character : value.substr(1)) {
    if (!letter(character) && (character < '0' || character > '9')) return false;
  }
  return true;
}

std::string lowercase(std::string_view value) {
  std::string result(value);
  for (auto& character : result) character = ascii_lower(character);
  return result;
}

template <typename Integer>
Integer parse_number(std::string_view input) {
  std::uint64_t value = 0;
  const auto [end, error] = std::from_chars(input.data(), input.data() + input.size(), value);
  if (input.empty() || error != std::errc{} || end != input.data() + input.size() ||
      value > std::numeric_limits<Integer>::max()) {
    throw std::invalid_argument("record ID must be a valid page:slot pair");
  }
  return static_cast<Integer>(value);
}

pageforge::RecordId parse_id(std::string_view input) {
  const auto separator = input.find(':');
  if (separator == std::string_view::npos) {
    throw std::invalid_argument("record ID must be a valid page:slot pair");
  }
  return {parse_number<pageforge::PageId>(input.substr(0, separator)),
          parse_number<pageforge::SlotId>(input.substr(separator + 1))};
}

void print_id(pageforge::RecordId id) { std::cout << id.page_id << ':' << id.slot_id; }

void print_bytes(const std::vector<std::byte>& bytes) {
  std::cout.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::vector<std::byte> as_bytes(std::string_view text) {
  std::vector<std::byte> result;
  result.reserve(text.size());
  for (unsigned char character : text) result.push_back(static_cast<std::byte>(character));
  return result;
}

pageforge::Column parse_column(std::string_view specification) {
  const auto separator = specification.find(':');
  if (separator == std::string_view::npos || specification.find(':', separator + 1) != std::string_view::npos) {
    throw std::invalid_argument("column must use name:type syntax");
  }
  const auto name = specification.substr(0, separator);
  if (!is_sql_identifier(name)) throw std::invalid_argument("column name is not a SQL identifier");
  auto type_name = lowercase(specification.substr(separator + 1));
  bool nullable = false;
  if (!type_name.empty() && type_name.back() == '?') {
    nullable = true;
    type_name.pop_back();
  }
  pageforge::DataType type;
  if (type_name == "int" || type_name == "integer") {
    type = pageforge::DataType::Integer;
  } else if (type_name == "text") {
    type = pageforge::DataType::Text;
  } else if (type_name == "bool" || type_name == "boolean") {
    type = pageforge::DataType::Boolean;
  } else {
    throw std::invalid_argument("unknown column type: " + type_name);
  }
  return {std::string(name), type, nullable};
}

pageforge::TableDefinition resolve_table(pageforge::Catalog& catalog, std::string_view name) {
  std::optional<pageforge::TableDefinition> found;
  for (const auto& table : catalog.list_tables()) {
    if (!equal_name(table.name, name)) continue;
    if (found) throw std::invalid_argument("ambiguous table name");
    found = table;
  }
  if (!found) throw std::out_of_range("table not found");
  return std::move(*found);
}

pageforge::Value parse_value(const pageforge::Column& column, std::string_view input) {
  if (input == "NULL") {
    if (!column.nullable) throw std::invalid_argument("NULL supplied for non-nullable column: " + column.name);
    return std::monostate{};
  }
  switch (column.type) {
    case pageforge::DataType::Integer: {
      std::int64_t value = 0;
      const bool explicit_plus = !input.empty() && input.front() == '+';
      const auto digits = explicit_plus ? input.substr(1) : input;
      const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
      if (digits.empty() || error != std::errc{} || end != digits.data() + digits.size()) {
        throw std::invalid_argument("invalid integer for column: " + column.name);
      }
      return value;
    }
    case pageforge::DataType::Text:
      return std::string(input);
    case pageforge::DataType::Boolean: {
      const auto value = lowercase(input);
      if (value == "true") return true;
      if (value == "false") return false;
      throw std::invalid_argument("invalid boolean for column: " + column.name);
    }
  }
  throw std::invalid_argument("unknown column type");
}

void print_escaped(std::string_view value) {
  for (const auto character : value) {
    switch (character) {
      case '\\': std::cout << "\\\\"; break;
      case '\t': std::cout << "\\t"; break;
      case '\n': std::cout << "\\n"; break;
      case '\r': std::cout << "\\r"; break;
      default: std::cout << character; break;
    }
  }
}

void print_value(const pageforge::Value& value) {
  if (std::holds_alternative<std::monostate>(value)) {
    std::cout << "NULL";
  } else if (std::holds_alternative<std::int64_t>(value)) {
    std::cout << std::get<std::int64_t>(value);
  } else if (std::holds_alternative<std::string>(value)) {
    print_escaped(std::get<std::string>(value));
  } else {
    std::cout << (std::get<bool>(value) ? "true" : "false");
  }
}

void print_query(pageforge::BoundSelect result) {
  for (std::size_t index = 0; index < result.output_schema().size(); ++index) {
    if (index != 0) std::cout << '\t';
    print_escaped(result.output_schema()[index].name);
  }
  std::cout << '\n';
  while (auto row = result.next()) {
    for (std::size_t index = 0; index < row->values.size(); ++index) {
      if (index != 0) std::cout << '\t';
      print_value(row->values[index]);
    }
    std::cout << '\n';
  }
}

std::string_view trim(std::string_view value) {
  const auto whitespace = [](char character) {
    return character == ' ' || character == '\t' || character == '\n' || character == '\r' ||
           character == '\f';
  };
  while (!value.empty() && whitespace(value.front())) value.remove_prefix(1);
  while (!value.empty() && whitespace(value.back())) value.remove_suffix(1);
  return value;
}

std::string_view type_name(pageforge::DataType type) {
  switch (type) {
    case pageforge::DataType::Integer: return "INTEGER";
    case pageforge::DataType::Text: return "TEXT";
    case pageforge::DataType::Boolean: return "BOOLEAN";
  }
  throw std::invalid_argument("unknown column type");
}

void print_schema(const pageforge::TableDefinition& table) {
  std::cout << table.name << '(';
  for (std::size_t index = 0; index < table.schema.size(); ++index) {
    if (index != 0) std::cout << ", ";
    const auto& column = table.schema[index];
    std::cout << column.name << ' ' << type_name(column.type)
              << (column.nullable ? " NULL" : " NOT NULL");
  }
  std::cout << ") [schema version " << table.schema_version << "]\n";
}

void print_shell_help() {
  std::cout << "Enter one SELECT statement per line. Shell commands:\n"
               "  .tables        List tables\n"
               "  .schema TABLE  Show a table schema\n"
               "  .help          Show this help\n"
               "  .quit          Exit the shell\n";
}

void run_shell(pageforge::TableStore& tables, pageforge::Catalog& catalog) {
  const bool interactive = ::isatty(STDIN_FILENO) != 0;
  if (interactive) {
    std::cout << "PageForge interactive shell. Type .help for help.\n";
  }

  std::string line;
  while (true) {
    if (interactive) {
      std::cout << "pageforge> " << std::flush;
    }
    if (!std::getline(std::cin, line)) {
      if (interactive) std::cout << '\n';
      return;
    }
    const auto input = trim(line);
    if (input.empty()) continue;
    try {
      if (input == ".quit" || input == ".exit") return;
      if (input == ".help") {
        print_shell_help();
      } else if (input == ".tables") {
        auto definitions = catalog.list_tables();
        std::sort(definitions.begin(), definitions.end(), [](const auto& left, const auto& right) {
          return lowercase(left.name) < lowercase(right.name);
        });
        for (const auto& table : definitions) std::cout << table.name << '\n';
      } else if (input == ".schema" || input.starts_with(".schema ") ||
                 input.starts_with(".schema\t")) {
        const auto name = trim(input.substr(std::string_view(".schema").size()));
        if (name.empty() || name.find_first_of(" \t\r\n\f") != std::string_view::npos) {
          throw std::invalid_argument("usage: .schema TABLE");
        }
        print_schema(resolve_table(catalog, name));
      } else if (input.front() == '.') {
        throw std::invalid_argument("unknown shell command; type .help for help");
      } else {
        print_query(pageforge::execute_select_sql(tables, catalog, input));
      }
    } catch (const std::exception& error) {
      std::cerr << "pageforge: " << error.what() << '\n';
    }
  }
}

int run(int argc, char* argv[]) {
  if (argc < 3) {
    usage();
    return 2;
  }

  const std::filesystem::path path(argv[1]);
  const std::string_view command(argv[2]);
  if (command == "init") {
    if (argc != 3) throw std::invalid_argument("init takes no argument");
    if (std::filesystem::symlink_status(path).type() != std::filesystem::file_type::not_found) {
      throw std::runtime_error("database path already exists");
    }
    auto heap = pageforge::HeapFile::create(path);
    std::cout << "initialized " << path.string() << '\n';
    return 0;
  }

  const bool one_argument = command == "put" || command == "get" || command == "erase" || command == "query";
  const bool no_argument = command == "list" || command == "shell";
  const bool variable_arguments = command == "create-table" || command == "insert";
  if ((!one_argument && !no_argument && !variable_arguments) || (one_argument && argc != 4) ||
      (no_argument && argc != 3) || (variable_arguments && argc < 5)) {
    usage();
    return 2;
  }

  auto heap = pageforge::HeapFile::open(path);
  pageforge::BufferPool pool(heap, 16);
  pageforge::RecordStore records(pool);
  pageforge::Catalog catalog(records);
  pageforge::TableStore tables(records, catalog);
  if (command == "put") {
    const auto payload = as_bytes(argv[3]);
    const auto id = records.insert(payload);
    pool.flush_all();
    print_id(id);
    std::cout << '\n';
  } else if (command == "get") {
    print_bytes(records.read(parse_id(argv[3])));
    std::cout << '\n';
  } else if (command == "erase") {
    const auto id = parse_id(argv[3]);
    if (!records.erase(id)) throw std::out_of_range("record not found");
    pool.flush_all();
    std::cout << "deleted ";
    print_id(id);
    std::cout << '\n';
  } else if (command == "list") {
    for (const auto& record : records.scan()) {
      print_id(record.id);
      std::cout << '\t';
      print_bytes(record.bytes);
      std::cout << '\n';
    }
  } else if (command == "create-table") {
    const std::string_view table_name(argv[3]);
    if (!is_sql_identifier(table_name)) throw std::invalid_argument("table name is not a SQL identifier");
    for (const auto& table : catalog.list_tables()) {
      if (equal_name(table.name, table_name)) throw std::invalid_argument("table already exists");
    }
    pageforge::Schema schema;
    for (int index = 4; index < argc; ++index) {
      auto column = parse_column(argv[index]);
      for (const auto& existing : schema) {
        if (equal_name(existing.name, column.name)) {
          throw std::invalid_argument("column names must be unique case-insensitively");
        }
      }
      schema.push_back(std::move(column));
    }
    (void)catalog.create_table({std::string(table_name), std::move(schema), 1});
    pool.flush_all();
    std::cout << "created table " << table_name << '\n';
  } else if (command == "insert") {
    const auto table = resolve_table(catalog, argv[3]);
    if (static_cast<std::size_t>(argc - 4) != table.schema.size()) {
      throw std::invalid_argument("insert value count does not match table schema");
    }
    pageforge::Tuple tuple;
    tuple.reserve(table.schema.size());
    for (std::size_t index = 0; index < table.schema.size(); ++index) {
      tuple.push_back(parse_value(table.schema[index], argv[static_cast<int>(index) + 4]));
    }
    const auto id = tables.insert(table.name, tuple);
    pool.flush_all();
    print_id(id);
    std::cout << '\n';
  } else if (command == "query") {
    print_query(pageforge::execute_select_sql(tables, catalog, argv[3]));
  } else {
    run_shell(tables, catalog);
  }
  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  try {
    return run(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << "pageforge: " << error.what() << '\n';
    return 1;
  }
}
