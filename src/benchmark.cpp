#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

#include "pageforge/sql_executor.hpp"

namespace {

using Clock = std::chrono::steady_clock;

std::size_t parse_rows(std::string_view input) {
  std::size_t rows = 0;
  const auto [end, error] = std::from_chars(input.data(), input.data() + input.size(), rows);
  constexpr std::size_t kMaximumRows = 10'000'000;
  if (input.empty() || error != std::errc{} || end != input.data() + input.size() || rows == 0 ||
      rows > kMaximumRows) {
    throw std::invalid_argument("row count must be between 1 and 10000000");
  }
  return rows;
}

std::int64_t elapsed_microseconds(Clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
}

double rows_per_second(std::size_t rows, std::int64_t microseconds) {
  const auto duration = static_cast<double>(microseconds > 0 ? microseconds : 1);
  return static_cast<double>(rows) * 1'000'000.0 / duration;
}

struct Metrics {
  std::size_t rows = 0;
  std::size_t matched_rows = 0;
  std::int64_t insert_microseconds = 0;
  std::int64_t select_microseconds = 0;
  std::int64_t reopen_scan_microseconds = 0;
  std::uintmax_t database_bytes = 0;
  std::uint64_t checksum = 0;
};

Metrics benchmark(const std::filesystem::path& path, std::size_t row_count) {
  if (std::filesystem::symlink_status(path).type() != std::filesystem::file_type::not_found) {
    throw std::runtime_error("benchmark database path already exists");
  }

  Metrics metrics;
  metrics.rows = row_count;
  {
    auto heap = pageforge::HeapFile::create(path);
    pageforge::BufferPool pool(heap, 64);
    pageforge::RecordStore records(pool);
    pageforge::Catalog catalog(records);
    (void)catalog.create_table(
        {"benchmark_rows",
         {{"id", pageforge::DataType::Integer, false},
          {"payload", pageforge::DataType::Text, false},
          {"active", pageforge::DataType::Boolean, false}},
         1});
    pageforge::TableStore tables(records, catalog);

    const auto insert_start = Clock::now();
    for (std::size_t index = 0; index < row_count; ++index) {
      (void)tables.insert(
          "benchmark_rows",
          {static_cast<std::int64_t>(index), std::string("row-") + std::to_string(index), index % 2 == 0});
    }
    pool.flush_all();
    metrics.insert_microseconds = elapsed_microseconds(insert_start);

    const auto select_start = Clock::now();
    auto selected = pageforge::execute_select_sql(
        tables, catalog, "SELECT id, payload FROM benchmark_rows WHERE active = TRUE");
    while (auto row = selected.next()) {
      ++metrics.matched_rows;
      metrics.checksum += static_cast<std::uint64_t>(std::get<std::int64_t>(row->values[0]));
    }
    metrics.select_microseconds = elapsed_microseconds(select_start);
    const auto expected_matches = (row_count + 1) / 2;
    const auto expected_selected_checksum =
        static_cast<std::uint64_t>(expected_matches) * (expected_matches - 1);
    if (metrics.matched_rows != expected_matches || metrics.checksum != expected_selected_checksum) {
      throw std::runtime_error("SQL filter did not reproduce the expected rows");
    }
  }

  {
    auto heap = pageforge::HeapFile::open(path);
    pageforge::BufferPool pool(heap, 64);
    pageforge::RecordStore records(pool);
    pageforge::Catalog catalog(records);
    pageforge::TableStore tables(records, catalog);
    const auto scan_start = Clock::now();
    auto cursor = tables.cursor("benchmark_rows");
    std::size_t scanned = 0;
    std::uint64_t scan_checksum = 0;
    while (auto row = cursor.next()) {
      ++scanned;
      scan_checksum += static_cast<std::uint64_t>(std::get<std::int64_t>(row->values[0]));
    }
    metrics.reopen_scan_microseconds = elapsed_microseconds(scan_start);
    const auto expected_checksum = static_cast<std::uint64_t>(row_count) * (row_count - 1) / 2;
    if (scanned != row_count || scan_checksum != expected_checksum) {
      throw std::runtime_error("reopened scan did not reproduce every inserted row");
    }
  }

  metrics.database_bytes = std::filesystem::file_size(path);
  return metrics;
}

void print(const Metrics& metrics) {
  std::cout << std::fixed << std::setprecision(2)
            << "{\"rows\":" << metrics.rows << ",\"matched_rows\":" << metrics.matched_rows
            << ",\"insert_microseconds\":" << metrics.insert_microseconds
            << ",\"select_microseconds\":" << metrics.select_microseconds
            << ",\"reopen_scan_microseconds\":" << metrics.reopen_scan_microseconds
            << ",\"insert_rows_per_second\":"
            << rows_per_second(metrics.rows, metrics.insert_microseconds)
            << ",\"select_input_rows_per_second\":"
            << rows_per_second(metrics.rows, metrics.select_microseconds)
            << ",\"reopen_scan_rows_per_second\":"
            << rows_per_second(metrics.rows, metrics.reopen_scan_microseconds)
            << ",\"database_bytes\":" << metrics.database_bytes
            << ",\"selected_id_checksum\":" << metrics.checksum << "}\n";
}

}  // namespace

int main(int argc, char* argv[]) {
  try {
    if (argc != 3) {
      std::cerr << "Usage: pageforge_bench <database> <rows>\n";
      return 2;
    }
    print(benchmark(argv[1], parse_rows(argv[2])));
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "pageforge_bench: " << error.what() << '\n';
    return 1;
  }
}
