#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "pageforge/record_store.hpp"

namespace {

void usage() {
  std::cerr << "Usage: pageforge <database> <command> [argument]\n"
               "  init             Create a database; refuse an existing path\n"
               "  put <text>       Insert a nonempty text record\n"
               "  get <page:slot>  Read a record\n"
               "  erase <page:slot> Delete a record\n"
               "  list             List live records\n";
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

  const bool needs_argument = command == "put" || command == "get" || command == "erase";
  if (argc != (needs_argument ? 4 : 3)) {
    usage();
    return 2;
  }
  if (command != "put" && command != "get" && command != "erase" && command != "list") {
    usage();
    return 2;
  }

  auto heap = pageforge::HeapFile::open(path);
  pageforge::BufferPool pool(heap, 16);
  pageforge::RecordStore records(pool);
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
  } else {
    for (const auto& record : records.scan()) {
      print_id(record.id);
      std::cout << '\t';
      print_bytes(record.bytes);
      std::cout << '\n';
    }
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
