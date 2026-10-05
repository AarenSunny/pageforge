#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "pageforge/heap_file.hpp"
#include "pageforge/buffer_pool.hpp"
#include "pageforge/page.hpp"
#include "pageforge/record_store.hpp"
#include "pageforge/tuple.hpp"
#include "pageforge/catalog.hpp"
#include "pageforge/table_store.hpp"
#include "pageforge/query.hpp"
#include "pageforge/sql_lexer.hpp"
#include "pageforge/sql_parser.hpp"
#include "pageforge/sql_executor.hpp"
#include "pageforge/bplus_tree.hpp"

namespace {

using pageforge::HeapFile;
using pageforge::BufferPool;
using pageforge::BufferPoolExhausted;
using pageforge::PageCorruption;
using pageforge::PageFull;
using pageforge::RecordStore;
using pageforge::SlottedPage;
using pageforge::TupleCodec;
using pageforge::TupleCorruption;
using pageforge::Catalog;
using pageforge::CatalogCorruption;
using pageforge::IndexDefinition;
using pageforge::TableStore;
using pageforge::TableCorruption;
using pageforge::Query;
using pageforge::SqlLexError;
using pageforge::SqlTokenKind;
using pageforge::SqlParseError;
using pageforge::SqlBindError;
using pageforge::BPlusTreeIndex;
using pageforge::IndexCorruption;
using pageforge::RecordId;

std::vector<std::byte> bytes(std::string_view value) {
  const auto* begin = reinterpret_cast<const std::byte*>(value.data());
  return {begin, begin + value.size()};
}

std::string text(const std::vector<std::byte>& value) {
  return {reinterpret_cast<const char*>(value.data()), value.size()};
}

void check(bool condition, std::string_view message) {
  if (!condition) throw std::runtime_error(std::string(message));
}

template <typename Exception, typename Function>
void check_throws(Function function, std::string_view message) {
  try {
    function();
  } catch (const Exception&) {
    return;
  }
  throw std::runtime_error(std::string(message));
}

void slotted_page_round_trip() {
  auto page = SlottedPage::initialize(7);
  const auto alpha = page.insert(bytes("alpha"));
  const auto unicode = page.insert(bytes("database \xF0\x9F\x97\x83"));
  check(alpha == 0 && unicode == 1, "slot ids should be stable and sequential");
  check(text(page.read(alpha)) == "alpha", "first record should round-trip");
  check(text(page.read(unicode)) == "database \xF0\x9F\x97\x83", "UTF-8 bytes should round-trip");
  check(page.page_id() == 7 && page.live_records() == 2, "page metadata should be accurate");
  auto decoded = SlottedPage::from_bytes(page.bytes());
  check(text(decoded.read(unicode)) == "database \xF0\x9F\x97\x83", "encoded page should decode");
}

void deletion_compaction_and_slot_reuse() {
  auto page = SlottedPage::initialize(1);
  const auto first = page.insert(bytes(std::string(900, 'a')));
  const auto deleted = page.insert(bytes(std::string(1200, 'b')));
  const auto third = page.insert(bytes(std::string(900, 'c')));
  const auto before = page.free_space();
  check(page.erase(deleted), "live record should be deleted");
  check(page.reclaimable_space() > before, "deleted payload should become reclaimable");
  const auto replacement = page.insert(bytes(std::string(1100, 'd')));
  check(replacement == deleted, "a tombstoned slot should be reused");
  check(text(page.read(first)) == std::string(900, 'a'), "compaction must preserve the first slot");
  check(text(page.read(third)) == std::string(900, 'c'), "compaction must preserve later slots");
  check(text(page.read(replacement)) == std::string(1100, 'd'), "replacement should use reclaimed space");
  page.validate();
}

void replacement_preserves_slots_and_failure_atomicity() {
  auto page = SlottedPage::initialize(9);
  const auto first = page.insert(bytes(std::string(1000, 'a')));
  const auto second = page.insert(bytes(std::string(1000, 'b')));
  page.replace(first, bytes(std::string(2500, 'c')));
  check(text(page.read(first)) == std::string(2500, 'c') &&
            text(page.read(second)) == std::string(1000, 'b'),
        "replacement should preserve the target slot and neighboring records");
  page.replace(first, bytes("small"));
  check(text(page.read(first)) == "small", "replacement should support shrinking a record");

  check_throws<PageFull>([&] { page.replace(first, bytes(std::string(3500, 'x'))); },
                         "oversized replacement should fail");
  check(text(page.read(first)) == "small" && text(page.read(second)) == std::string(1000, 'b'),
        "failed replacement must leave the page unchanged");
  check_throws<std::out_of_range>([&] { page.replace(99, bytes("missing")); },
                                  "replacement should reject a missing slot");
  check_throws<std::invalid_argument>([&] { page.replace(first, {}); },
                                      "replacement should reject an empty record");
  page.validate();
}

void page_capacity_and_corruption() {
  auto page = SlottedPage::initialize(2);
  page.insert(bytes(std::string(4060, 'x')));
  check_throws<PageFull>([&] { page.insert(bytes("too large")); }, "full page should reject another record");
  auto corrupted = page.bytes();
  corrupted[4000] ^= std::byte{0x01};
  check_throws<PageCorruption>([&] { SlottedPage::from_bytes(corrupted); }, "checksum should detect bit flips");
  check_throws<std::invalid_argument>([&] { page.insert({}); }, "empty records should be rejected");
}

void heap_file_persists_pages() {
  const auto path = std::filesystem::temp_directory_path() / "pageforge-heap-test.db";
  std::filesystem::remove(path);
  {
    auto heap = HeapFile::create(path);
    auto page = heap.allocate_page();
    page.insert(bytes("persistent row"));
    heap.write_page(page);
    heap.allocate_page();
    heap.sync();
    check(heap.page_count() == 2, "heap should track allocated pages");
  }
  {
    auto heap = HeapFile::open(path);
    check(heap.page_count() == 2, "page count should survive reopen");
    check(text(heap.read_page(0).read(0)) == "persistent row", "record should survive reopen");
    check_throws<std::out_of_range>([&] { (void)heap.read_page(2); }, "out-of-range page reads should fail");
  }
  std::filesystem::remove(path);
}

void heap_file_detects_page_corruption() {
  const auto path = std::filesystem::temp_directory_path() / "pageforge-corruption-test.db";
  std::filesystem::remove(path);
  {
    auto heap = HeapFile::create(path);
    auto page = heap.allocate_page();
    page.insert(bytes("guarded"));
    heap.write_page(page);
    heap.sync();
  }
  {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    file.seekg(static_cast<std::streamoff>(pageforge::kPageSize + 100));
    char byte = 0;
    file.read(&byte, 1);
    file.clear();
    file.seekp(static_cast<std::streamoff>(pageforge::kPageSize + 100));
    byte ^= 0x1;
    file.write(&byte, 1);
  }
  auto heap = HeapFile::open(path);
  check_throws<PageCorruption>([&] { (void)heap.read_page(0); }, "corrupt disk page should fail checksum validation");
  std::filesystem::remove(path);
}

void heap_file_detects_header_corruption() {
  const auto path = std::filesystem::temp_directory_path() / "pageforge-header-test.db";
  std::filesystem::remove(path);
  { auto heap = HeapFile::create(path); }
  {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    file.seekg(100);
    char byte = 0;
    file.read(&byte, 1);
    file.clear();
    file.seekp(100);
    byte ^= 0x1;
    file.write(&byte, 1);
  }
  check_throws<PageCorruption>([&] { (void)HeapFile::open(path); }, "corrupt heap header should be rejected");
  std::filesystem::remove(path);
}

void heap_file_detects_truncation() {
  const auto path = std::filesystem::temp_directory_path() / "pageforge-truncation-test.db";
  std::filesystem::remove(path);
  {
    auto heap = HeapFile::create(path);
    heap.allocate_page();
    heap.sync();
  }
  std::filesystem::resize_file(path, pageforge::kPageSize + 100);
  check_throws<PageCorruption>([&] { (void)HeapFile::open(path); }, "truncated heap should be rejected");
  std::filesystem::remove(path);
}

void buffer_pool_caches_page_hits() {
  const auto path = std::filesystem::temp_directory_path() / "pageforge-buffer-hit-test.db";
  std::filesystem::remove(path);
  auto heap = HeapFile::create(path);
  auto disk_page = heap.allocate_page();
  disk_page.insert(bytes("cached"));
  heap.write_page(disk_page);
  heap.sync();

  BufferPool pool(heap, 2);
  {
    const auto guard = pool.fetch(0);
    check(text(guard.page().read(0)) == "cached", "buffer miss should load the disk page");
  }
  {
    const auto guard = pool.fetch(0);
    check(text(guard.page().read(0)) == "cached", "cache hit should return the resident page");
  }
  const auto stats = pool.stats();
  check(stats.misses == 1 && stats.hits == 1, "buffer metrics should distinguish hits and misses");
  check(pool.resident(0) && pool.resident_pages() == 1, "loaded page should remain resident");
  std::filesystem::remove(path);
}

void buffer_pool_writes_dirty_victims() {
  const auto path = std::filesystem::temp_directory_path() / "pageforge-buffer-eviction-test.db";
  std::filesystem::remove(path);
  auto heap = HeapFile::create(path);
  heap.allocate_page();
  heap.allocate_page();
  heap.sync();

  BufferPool pool(heap, 1);
  {
    auto guard = pool.fetch(0);
    guard.mutable_page().insert(bytes("dirty row"));
  }
  {
    const auto guard = pool.fetch(1);
    check(guard.page_id() == 1, "second page should replace the first frame");
  }
  const auto stats = pool.stats();
  check(stats.evictions == 1 && stats.dirty_writes == 1, "dirty eviction should be measured and written");
  check(text(heap.read_page(0).read(0)) == "dirty row", "dirty victim must reach the heap before eviction");
  std::filesystem::remove(path);
}

void buffer_pool_respects_pins() {
  const auto path = std::filesystem::temp_directory_path() / "pageforge-buffer-pin-test.db";
  std::filesystem::remove(path);
  auto heap = HeapFile::create(path);
  heap.allocate_page();
  heap.allocate_page();
  BufferPool pool(heap, 1);

  auto pinned = pool.fetch(0);
  check_throws<BufferPoolExhausted>([&] { (void)pool.fetch(1); }, "all-pinned pool should reject another fetch");
  pinned.release();
  const auto replacement = pool.fetch(1);
  check(replacement.page_id() == 1, "released frame should become evictable");
  std::filesystem::remove(path);
}

void buffer_pool_flushes_allocated_pages() {
  const auto path = std::filesystem::temp_directory_path() / "pageforge-buffer-allocate-test.db";
  std::filesystem::remove(path);
  auto heap = HeapFile::create(path);
  BufferPool pool(heap, 2);
  auto guard = pool.allocate();
  check(guard.page_id() == 0, "buffer allocation should return the new heap page");
  guard.mutable_page().insert(bytes("allocated through pool"));
  pool.flush_all();
  check(text(heap.read_page(0).read(0)) == "allocated through pool", "explicit flush should persist pinned pages");
  check(pool.stats().dirty_writes == 1, "flush should update dirty-write metrics");
  std::filesystem::remove(path);
}

void record_store_spans_pages_and_reopens() {
  const auto path = std::filesystem::temp_directory_path() / "pageforge-record-store-test.db";
  std::filesystem::remove(path);
  pageforge::RecordId first{};
  pageforge::RecordId second{};
  {
    auto heap = HeapFile::create(path);
    BufferPool pool(heap, 1);
    RecordStore records(pool);
    first = records.insert(bytes(std::string(3500, 'a')));
    second = records.insert(bytes(std::string(1000, 'b')));
    check(first.page_id == 0 && second.page_id == 1, "records should span pages when one is full");
    check(text(records.read(first)) == std::string(3500, 'a'), "evicted record should remain readable");
    check(text(records.read(second)) == std::string(1000, 'b'), "second-page record should be readable");
    const auto found = records.scan();
    check(found.size() == 2 && found[0].id == first && found[1].id == second,
          "scan should emit stable record ids in page and slot order");
    pool.flush_all();
  }
  {
    auto heap = HeapFile::open(path);
    BufferPool pool(heap, 1);
    RecordStore records(pool);
    check(text(records.read(first)) == std::string(3500, 'a'), "first record should survive reopen");
    check(text(records.read(second)) == std::string(1000, 'b'), "second record should survive reopen");
    check(records.scan().size() == 2, "scan should survive reopen");
  }
  std::filesystem::remove(path);
}

void record_store_reuses_deleted_space() {
  const auto path = std::filesystem::temp_directory_path() / "pageforge-record-reuse-test.db";
  std::filesystem::remove(path);
  {
    auto heap = HeapFile::create(path);
    BufferPool pool(heap, 1);
    RecordStore records(pool);
    const auto first = records.insert(bytes("first"));
    const auto deleted = records.insert(bytes(std::string(1200, 'x')));
    const auto last = records.insert(bytes("last"));
    check(records.erase(deleted), "live record should be deleted");
    check(!records.erase(deleted), "repeated deletion should return false");
    check_throws<std::out_of_range>([&] { (void)records.read(deleted); }, "deleted record must not be readable");
    const auto remaining = records.scan();
    check(remaining.size() == 2 && remaining[0].id == first && remaining[1].id == last,
          "scan should skip tombstones without renumbering other records");
    const auto replacement = records.insert(bytes(std::string(1100, 'y')));
    check(replacement == deleted, "insert should reuse the tombstoned record id");
    check(text(records.read(first)) == "first" && text(records.read(last)) == "last",
          "compaction and slot reuse must preserve other record ids");
    check(heap.page_count() == 1, "reused space should avoid a new page");
    pool.flush_all();
  }
  std::filesystem::remove(path);
}

void record_store_wraps_insertion_hint_to_reuse_space() {
  const auto path = std::filesystem::temp_directory_path() / "pageforge-record-hint-wrap-test.db";
  std::filesystem::remove(path);
  pageforge::RecordId reusable{};
  {
    auto heap = HeapFile::create(path);
    BufferPool pool(heap, 1);
    RecordStore records(pool);
    reusable = records.insert(bytes(std::string(3500, 'a')));
    (void)records.insert(bytes(std::string(1000, 'b')));
    check(records.erase(reusable), "setup should free space on the first page");
    pool.flush_all();
  }
  {
    auto heap = HeapFile::open(path);
    BufferPool pool(heap, 1);
    RecordStore records(pool);
    const auto replacement = records.insert(bytes(std::string(3500, 'c')));
    check(replacement == reusable,
          "a reopened store should wrap from its append hint and reuse earlier free space");
    check(heap.page_count() == 2, "hinted insertion should not allocate when an earlier page fits");
  }
  std::filesystem::remove(path);
}

void record_store_rejects_invalid_records() {
  const auto path = std::filesystem::temp_directory_path() / "pageforge-record-invalid-test.db";
  std::filesystem::remove(path);
  {
    auto heap = HeapFile::create(path);
    BufferPool pool(heap, 1);
    RecordStore records(pool);
    check_throws<std::invalid_argument>([&] { (void)records.insert({}); }, "empty record should be rejected");
    check_throws<PageFull>([&] { (void)records.insert(bytes(std::string(4067, 'z'))); },
                           "oversized record should be rejected before allocation");
    check(heap.page_count() == 0, "invalid records should not allocate pages");
    check(!records.erase({3, 0}), "deleting an invalid page should return false");
    check_throws<std::out_of_range>([&] { (void)records.read({3, 0}); },
                                    "reading an invalid page should fail");
  }
  std::filesystem::remove(path);
}

void record_cursor_streams_without_pins() {
  const auto path = std::filesystem::temp_directory_path() / "pageforge-record-cursor-test.db";
  std::filesystem::remove(path);
  {
    auto heap = HeapFile::create(path);
    BufferPool pool(heap, 1);
    RecordStore records(pool);
    const auto first = records.insert(bytes(std::string(4066, 'a')));
    const auto second = records.insert(bytes(std::string(4066, 'b')));
    auto reader = records.cursor();
    const auto one = reader.next();
    check(one && one->id == first && one->bytes.size() == 4066,
          "cursor should return the first page's record");
    {
      const auto guard = pool.fetch(1);
      check(guard.page_id() == second.page_id, "cursor must not retain a pin between next calls");
    }
    (void)records.insert(bytes(std::string(4066, 'c')));
    auto moved_reader = std::move(reader);
    check(!reader.next(), "moved-from cursor should be exhausted");
    const auto two = moved_reader.next();
    check(two && two->id == second && two->bytes.size() == 4066,
          "cursor should continue to the next page with a one-frame buffer pool");
    check(!moved_reader.next(), "cursor should not include pages allocated after its creation");
    check(!moved_reader.next(), "exhausted cursor should remain exhausted");
  }
  std::filesystem::remove(path);
}

pageforge::Schema people_schema() {
  return {{"id", pageforge::DataType::Integer, false},
          {"name", pageforge::DataType::Text, false},
          {"active", pageforge::DataType::Boolean, false},
          {"note", pageforge::DataType::Text, true}};
}

void tuple_codec_round_trips_typed_values() {
  const auto schema = people_schema();
  const pageforge::Tuple tuple{std::int64_t{-42}, std::string("Ada \xF0\x9F\x97\x83"), true, std::monostate{}};
  const auto encoded = TupleCodec::encode(schema, tuple);
  check(encoded.size() == 30, "tuple encoding should have deterministic size");
  check(encoded[0] == std::byte{'P'} && encoded[1] == std::byte{'F'} && encoded[2] == std::byte{'T'} &&
            encoded[3] == std::byte{'1'},
        "tuple encoding should have a portable format marker");
  check(encoded[8] == std::byte{0x08}, "null bitmap should identify the nullable fourth column");
  check(TupleCodec::decode(schema, encoded) == tuple, "mixed typed values should round-trip exactly");

  const pageforge::Schema extremes{{"low", pageforge::DataType::Integer, false},
                                   {"high", pageforge::DataType::Integer, false},
                                   {"empty", pageforge::DataType::Text, false},
                                   {"flag", pageforge::DataType::Boolean, false}};
  const pageforge::Tuple boundary{std::numeric_limits<std::int64_t>::min(),
                                  std::numeric_limits<std::int64_t>::max(), std::string{}, false};
  check(TupleCodec::decode(extremes, TupleCodec::encode(extremes, boundary)) == boundary,
        "integer boundaries and empty text should round-trip");
}

void tuple_codec_rejects_invalid_values() {
  const auto schema = people_schema();
  check_throws<std::invalid_argument>([&] { (void)TupleCodec::encode(schema, {std::int64_t{1}}); },
                                      "wrong tuple arity should fail");
  check_throws<std::invalid_argument>(
      [&] { (void)TupleCodec::encode(schema, {std::int64_t{1}, std::string("Ada"), std::monostate{},
                                              std::monostate{}}); },
      "null in a required column should fail");
  check_throws<std::invalid_argument>(
      [&] { (void)TupleCodec::encode(schema, {std::string("wrong"), std::string("Ada"), true,
                                              std::monostate{}}); },
      "wrong value type should fail");
  const pageforge::Schema unknown{{"mystery", static_cast<pageforge::DataType>(255), false}};
  check_throws<std::invalid_argument>([&] { (void)TupleCodec::encode(unknown, {std::int64_t{1}}); },
                                      "unknown schema type should fail");
}

void tuple_codec_detects_corruption() {
  const pageforge::Schema schema{{"flag", pageforge::DataType::Boolean, false}};
  const auto valid = TupleCodec::encode(schema, {true});
  for (std::size_t size = 0; size < valid.size(); ++size) {
    check_throws<TupleCorruption>([&] { (void)TupleCodec::decode(schema, std::span(valid).first(size)); },
                                  "every truncated prefix should fail");
  }
  auto corrupt = valid;
  corrupt[0] ^= std::byte{1};
  check_throws<TupleCorruption>([&] { (void)TupleCodec::decode(schema, corrupt); }, "bad magic should fail");
  corrupt = valid;
  corrupt[8] = std::byte{0x01};
  check_throws<TupleCorruption>([&] { (void)TupleCodec::decode(schema, corrupt); },
                                "null in a required column should fail decoding");
  corrupt = valid;
  corrupt[8] = std::byte{0x80};
  check_throws<TupleCorruption>([&] { (void)TupleCodec::decode(schema, corrupt); },
                                "unknown null-bitmap bits should fail");
  corrupt = valid;
  corrupt[9] = std::byte{2};
  check_throws<TupleCorruption>([&] { (void)TupleCodec::decode(schema, corrupt); },
                                "invalid boolean byte should fail");
  corrupt = valid;
  corrupt.push_back(std::byte{0});
  check_throws<TupleCorruption>([&] { (void)TupleCodec::decode(schema, corrupt); },
                                "trailing payload should fail");

  const pageforge::Schema text_schema{{"value", pageforge::DataType::Text, false}};
  corrupt = TupleCodec::encode(text_schema, {std::string("short")});
  corrupt[9] = std::byte{0xff};
  check_throws<TupleCorruption>([&] { (void)TupleCodec::decode(text_schema, corrupt); },
                                "text length beyond the payload should fail");
}

void typed_tuples_persist_in_record_store() {
  const auto path = std::filesystem::temp_directory_path() / "pageforge-tuple-store-test.db";
  std::filesystem::remove(path);
  const auto schema = people_schema();
  const pageforge::Tuple tuple{std::int64_t{7}, std::string("Grace"), false, std::string("compiler pioneer")};
  pageforge::RecordId id{};
  {
    auto heap = HeapFile::create(path);
    BufferPool pool(heap, 2);
    RecordStore records(pool);
    id = records.insert(TupleCodec::encode(schema, tuple));
    pool.flush_all();
  }
  {
    auto heap = HeapFile::open(path);
    BufferPool pool(heap, 2);
    RecordStore records(pool);
    check(TupleCodec::decode(schema, records.read(id)) == tuple,
          "typed tuple should survive the complete storage stack and reopen");
  }
  std::filesystem::remove(path);
}

void catalog_persists_table_schemas() {
  const auto path = std::filesystem::temp_directory_path() / "pageforge-catalog-test.db";
  std::filesystem::remove(path);
  const pageforge::TableDefinition people{"people", people_schema(), 3};
  const pageforge::TableDefinition events{
      "events", {{"event_id", pageforge::DataType::Integer, false},
                 {"description", pageforge::DataType::Text, true}},
      1};
  {
    auto heap = HeapFile::create(path);
    BufferPool pool(heap, 2);
    RecordStore records(pool);
    (void)records.insert(bytes("ordinary user record"));
    Catalog catalog(records);
    const auto people_id = catalog.create_table(people);
    const auto events_id = catalog.create_table(events);
    check(people_id.page_id == 0 && events_id.page_id == 0, "small catalog entries should share a page");
    check(catalog.find_table("people") == people, "created table should be immediately discoverable");
    check(!catalog.find_table("missing"), "unknown table should return no definition");
    const auto tables = catalog.list_tables();
    check(tables.size() == 2 && tables[0] == people && tables[1] == events,
          "catalog should ignore ordinary records and preserve creation order");
    pool.flush_all();
  }
  {
    auto heap = HeapFile::open(path);
    BufferPool pool(heap, 1);
    RecordStore records(pool);
    Catalog catalog(records);
    check(catalog.find_table("people") == people, "table schema and version should survive reopen");
    check(catalog.find_table("events") == events, "nullable columns should survive reopen");
  }
  std::filesystem::remove(path);
}

void catalog_validates_definitions() {
  const auto path = std::filesystem::temp_directory_path() / "pageforge-catalog-validation-test.db";
  std::filesystem::remove(path);
  {
    auto heap = HeapFile::create(path);
    BufferPool pool(heap, 2);
    RecordStore records(pool);
    Catalog catalog(records);
    check_throws<std::invalid_argument>([&] { (void)catalog.create_table({"", people_schema(), 1}); },
                                        "empty table name should fail");
    check_throws<std::invalid_argument>([&] { (void)catalog.create_table({"empty", {}, 1}); },
                                        "empty table schema should fail");
    check_throws<std::invalid_argument>(
        [&] {
          (void)catalog.create_table(
              {"duplicates", {{"id", pageforge::DataType::Integer, false},
                               {"id", pageforge::DataType::Text, false}}, 1});
        },
        "duplicate column names should fail");
    check_throws<std::invalid_argument>([&] { (void)catalog.create_table({"zero", people_schema(), 0}); },
                                        "zero schema version should fail");
    check_throws<std::invalid_argument>(
        [&] {
          (void)catalog.create_table(
              {"unknown", {{"value", static_cast<pageforge::DataType>(255), false}}, 1});
        },
        "unknown column type should fail");
    const pageforge::TableDefinition valid{"people", people_schema(), 1};
    (void)catalog.create_table(valid);
    check_throws<std::invalid_argument>([&] { (void)catalog.create_table(valid); },
                                        "duplicate table name should fail");
  }
  std::filesystem::remove(path);
}

void catalog_detects_corrupt_metadata() {
  const auto path = std::filesystem::temp_directory_path() / "pageforge-catalog-corruption-test.db";
  std::filesystem::remove(path);
  {
    auto heap = HeapFile::create(path);
    BufferPool pool(heap, 2);
    RecordStore records(pool);
    Catalog catalog(records);
    const pageforge::TableDefinition table{"people", people_schema(), 1};
    const auto id = catalog.create_table(table);
    const auto encoded = records.read(id);
    (void)records.insert(encoded);
    check_throws<CatalogCorruption>([&] { (void)catalog.list_tables(); },
                                    "duplicate persisted table names should be corruption");
  }
  std::filesystem::remove(path);

  const auto truncated_path = std::filesystem::temp_directory_path() / "pageforge-catalog-truncated-test.db";
  std::filesystem::remove(truncated_path);
  {
    auto heap = HeapFile::create(truncated_path);
    BufferPool pool(heap, 1);
    RecordStore records(pool);
    (void)records.insert(bytes("PFC1"));
    Catalog catalog(records);
    check_throws<CatalogCorruption>([&] { (void)catalog.list_tables(); },
                                    "truncated reserved catalog record should fail");
  }
  std::filesystem::remove(truncated_path);
}

void catalog_persists_index_ownership() {
  const auto path = std::filesystem::temp_directory_path() / "pageforge-index-catalog-test.db";
  std::filesystem::remove(path);
  RecordId header_id{};
  const IndexDefinition expected{"people_id_idx", "people", "id", {}};
  {
    auto heap = HeapFile::create(path);
    BufferPool pool(heap, 3);
    RecordStore records(pool);
    Catalog catalog(records);
    (void)catalog.create_table({"people", people_schema(), 1});
    auto index = BPlusTreeIndex::create(records);
    header_id = index.header_id();
    check(index.insert(7, {42, 1}) && index.insert(9, {42, 2}),
          "catalog-owned index should accept entries before registration");
    auto definition = expected;
    definition.header_id = header_id;
    (void)catalog.register_index(definition);
    check(catalog.find_index("people_id_idx") == definition,
          "registered index should be immediately discoverable");
    check(!catalog.find_index("missing"), "unknown index should return no definition");
    check(catalog.list_indexes() == std::vector<IndexDefinition>{definition},
          "index catalog should preserve creation order and ownership");
    pool.flush_all();
  }
  {
    auto heap = HeapFile::open(path);
    BufferPool pool(heap, 2);
    RecordStore records(pool);
    Catalog catalog(records);
    auto definition = expected;
    definition.header_id = header_id;
    check(catalog.find_index("people_id_idx") == definition,
          "index ownership should survive database reopen");
    auto index = BPlusTreeIndex::open(records, definition.header_id);
    check(index.find(7) == std::vector<RecordId>{{42, 1}} &&
              index.find(9) == std::vector<RecordId>{{42, 2}},
          "persisted catalog header should reopen the owned B+ tree");
  }
  std::filesystem::remove(path);
}

void catalog_validates_index_definitions() {
  const auto path = std::filesystem::temp_directory_path() / "pageforge-index-catalog-validation-test.db";
  std::filesystem::remove(path);
  {
    auto heap = HeapFile::create(path);
    BufferPool pool(heap, 3);
    RecordStore records(pool);
    Catalog catalog(records);
    (void)catalog.create_table({"people", people_schema(), 1});
    auto index = BPlusTreeIndex::create(records);
    const IndexDefinition valid{"people_id_idx", "people", "id", index.header_id()};
    check_throws<std::invalid_argument>(
        [&] { (void)catalog.register_index({"", "people", "id", index.header_id()}); },
        "empty index names should fail");
    check_throws<std::invalid_argument>(
        [&] { (void)catalog.register_index({"missing_table_idx", "missing", "id", index.header_id()}); },
        "indexes should reference an existing table");
    check_throws<std::invalid_argument>(
        [&] { (void)catalog.register_index({"missing_column_idx", "people", "missing", index.header_id()}); },
        "indexes should reference an existing column");
    check_throws<std::invalid_argument>(
        [&] { (void)catalog.register_index({"people_name_idx", "people", "name", index.header_id()}); },
        "B+ tree indexes should reject non-integer columns");
    check_throws<IndexCorruption>(
        [&] { (void)catalog.register_index({"bad_header_idx", "people", "id", {999, 0}}); },
        "index metadata should reject an invalid tree header");
    const auto metadata_id = catalog.register_index(valid);
    check_throws<std::invalid_argument>([&] { (void)catalog.register_index(valid); },
                                        "duplicate index names should fail");
    (void)records.insert(records.read(metadata_id));
    check_throws<CatalogCorruption>([&] { (void)catalog.list_indexes(); },
                                    "duplicate persisted index names should be corruption");
  }
  std::filesystem::remove(path);

  const auto truncated_path =
      std::filesystem::temp_directory_path() / "pageforge-index-catalog-truncated-test.db";
  std::filesystem::remove(truncated_path);
  {
    auto heap = HeapFile::create(truncated_path);
    BufferPool pool(heap, 1);
    RecordStore records(pool);
    (void)records.insert(bytes("PFX1"));
    Catalog catalog(records);
    check_throws<CatalogCorruption>([&] { (void)catalog.list_indexes(); },
                                    "truncated reserved index metadata should fail");
  }
  std::filesystem::remove(truncated_path);
}

void typed_tables_persist_and_isolate_rows() {
  const auto path = std::filesystem::temp_directory_path() / "pageforge-tables-test.db";
  std::filesystem::remove(path);
  const pageforge::Tuple ada{std::int64_t{1}, std::string("Ada"), true, std::monostate{}};
  const pageforge::Tuple grace{std::int64_t{2}, std::string("Grace"), false, std::string("compiler")};
  const pageforge::Tuple event{std::int64_t{9}, std::string("launch")};
  pageforge::RecordId ada_id{};
  pageforge::RecordId grace_id{};
  {
    auto heap = HeapFile::create(path);
    BufferPool pool(heap, 1);
    RecordStore records(pool);
    Catalog catalog(records);
    (void)catalog.create_table({"people", people_schema(), 1});
    (void)catalog.create_table({"events", {{"id", pageforge::DataType::Integer, false},
                                           {"title", pageforge::DataType::Text, false}}, 1});
    (void)records.insert(bytes("untyped record"));
    TableStore tables(records, catalog);
    ada_id = tables.insert("people", ada);
    grace_id = tables.insert("people", grace);
    const auto event_id = tables.insert("events", event);
    check(tables.read("people", ada_id) == ada, "typed table read should decode values");
    check(tables.read("events", event_id) == event, "second table should use its own schema");
    check_throws<std::invalid_argument>([&] { (void)tables.read("events", ada_id); },
                                        "a row must not be read under another table");
    const auto people = tables.scan("people");
    check(people.size() == 2 && people[0].id == ada_id && people[0].values == ada &&
              people[1].id == grace_id && people[1].values == grace,
          "table scan should ignore catalog entries, raw records, and other tables");
    check(tables.scan("events").size() == 1, "other table scan should be isolated");
    auto reader = tables.cursor("people");
    const auto first = reader.next();
    const auto second = reader.next();
    check(first && first->id == ada_id && first->values == ada && second &&
              second->id == grace_id && second->values == grace,
          "table cursor should stream only the named table's rows in record order");
    check(!reader.next(), "table cursor should signal exhaustion");
    pool.flush_all();
  }
  {
    auto heap = HeapFile::open(path);
    BufferPool pool(heap, 1);
    RecordStore records(pool);
    Catalog catalog(records);
    TableStore tables(records, catalog);
    check(tables.read("people", ada_id) == ada, "table row should survive reopen");
    check(tables.scan("people").size() == 2, "table scan should survive reopen");
    check(tables.erase("people", grace_id), "table row should be deletable");
    check(!tables.erase("people", grace_id), "deleting the same row twice should return false");
    check(tables.scan("people").size() == 1, "scan should omit deleted rows");
    pool.flush_all();
  }
  std::filesystem::remove(path);
}

void typed_tables_validate_input_and_ownership() {
  const auto path = std::filesystem::temp_directory_path() / "pageforge-tables-validation-test.db";
  std::filesystem::remove(path);
  {
    auto heap = HeapFile::create(path);
    BufferPool pool(heap, 2);
    RecordStore records(pool);
    Catalog catalog(records);
    (void)catalog.create_table({"people", people_schema(), 1});
    (void)catalog.create_table({"events", {{"id", pageforge::DataType::Integer, false}}, 1});
    TableStore tables(records, catalog);
    check_throws<std::out_of_range>([&] { (void)tables.insert("missing", {std::int64_t{1}}); },
                                    "unknown table should reject insertion");
    check_throws<std::invalid_argument>([&] { (void)tables.insert("people", {std::int64_t{1}}); },
                                        "tuple arity should be checked before insertion");
    check_throws<std::invalid_argument>(
        [&] { (void)tables.insert("events", {std::string("wrong type")}); },
        "tuple types should be checked before insertion");
    const auto id = tables.insert("events", {std::int64_t{7}});
    check_throws<std::invalid_argument>([&] { (void)tables.erase("people", id); },
                                        "delete must reject a row owned by another table");
    check(tables.read("events", id) == pageforge::Tuple{std::int64_t{7}},
          "wrong-table delete must leave the row intact");
  }
  std::filesystem::remove(path);
}

void typed_tables_maintain_registered_indexes() {
  const auto path = std::filesystem::temp_directory_path() / "pageforge-table-index-test.db";
  std::filesystem::remove(path);
  RecordId second_id{};
  {
    auto heap = HeapFile::create(path);
    BufferPool pool(heap, 4);
    RecordStore records(pool);
    Catalog catalog(records);
    (void)catalog.create_table({"items", {{"id", pageforge::DataType::Integer, false},
                                           {"score", pageforge::DataType::Integer, true},
                                           {"name", pageforge::DataType::Text, false}},
                                1});
    auto id_index = BPlusTreeIndex::create(records);
    auto score_index = BPlusTreeIndex::create(records);
    (void)catalog.register_index({"items_id_idx", "items", "id", id_index.header_id()});
    (void)catalog.register_index({"items_score_idx", "items", "score", score_index.header_id()});

    TableStore tables(records, catalog);
    const auto first_id = tables.insert("items", {std::int64_t{7}, std::int64_t{100},
                                                   std::string("seven")});
    second_id = tables.insert("items", {std::int64_t{8}, std::monostate{}, std::string("eight")});
    auto current_id_index = BPlusTreeIndex::open(records, id_index.header_id());
    auto current_score_index = BPlusTreeIndex::open(records, score_index.header_id());
    check(current_id_index.find(7) == std::vector<RecordId>{first_id} &&
              current_id_index.find(8) == std::vector<RecordId>{second_id},
          "table inserts should add each non-null integer key to its owned index");
    check(current_score_index.find(100) == std::vector<RecordId>{first_id} &&
              current_score_index.range(std::nullopt, std::nullopt).size() == 1,
          "nullable index columns should omit null values");
    check(tables.erase("items", first_id), "indexed table row should be deletable");
    auto deleted_id_index = BPlusTreeIndex::open(records, id_index.header_id());
    auto deleted_score_index = BPlusTreeIndex::open(records, score_index.header_id());
    check(deleted_id_index.find(7).empty() && deleted_score_index.find(100).empty(),
          "table deletion should remove every indexed key for the row");
    pool.flush_all();
  }
  {
    auto heap = HeapFile::open(path);
    BufferPool pool(heap, 3);
    RecordStore records(pool);
    Catalog catalog(records);
    const auto definition = catalog.find_index("items_id_idx");
    check(definition.has_value(), "owned index metadata should survive with indexed rows");
    auto id_index = BPlusTreeIndex::open(records, definition->header_id);
    check(id_index.find(8) == std::vector<RecordId>{second_id},
          "automatic index maintenance should survive database reopen");
    TableStore tables(records, catalog);
    check(tables.erase("items", second_id), "reopened table should maintain its index on delete");
    auto empty_id_index = BPlusTreeIndex::open(records, definition->header_id);
    check(empty_id_index.range(std::nullopt, std::nullopt).empty(),
          "reopened index should be empty after deleting its final row");
  }
  std::filesystem::remove(path);
}

void typed_tables_build_indexes_for_existing_rows() {
  const auto path = std::filesystem::temp_directory_path() / "pageforge-table-index-build-test.db";
  std::filesystem::remove(path);
  {
    auto heap = HeapFile::create(path);
    BufferPool pool(heap, 4);
    RecordStore records(pool);
    Catalog catalog(records);
    (void)catalog.create_table({"items", {{"score", pageforge::DataType::Integer, true},
                                           {"name", pageforge::DataType::Text, false}},
                                1});
    TableStore tables(records, catalog);
    const auto first = tables.insert("items", {std::int64_t{7}, std::string("first")});
    (void)tables.insert("items", {std::monostate{}, std::string("unscored")});
    const auto second = tables.insert("items", {std::int64_t{7}, std::string("second")});

    (void)tables.create_index("items_score_idx", "items", "score");
    const auto definition = catalog.find_index("items_score_idx");
    check(definition && definition->table == "items" && definition->column == "score",
          "built index should become catalog-owned after existing rows are populated");
    const auto matches = tables.lookup_index("items_score_idx", 7);
    check(matches.size() == 2 && matches[0].id == first && matches[1].id == second,
          "index build should include duplicate existing keys and omit nulls");

    const auto third = tables.insert("items", {std::int64_t{7}, std::string("third")});
    const auto updated = tables.lookup_index("items_score_idx", 7);
    check(updated.size() == 3 && updated.back().id == third,
          "writes after index creation should continue automatic maintenance");
    check_throws<std::invalid_argument>(
        [&] { (void)tables.create_index("items_score_idx", "items", "score"); },
        "building a duplicate index should fail before allocating another tree");
    check_throws<std::invalid_argument>(
        [&] { (void)tables.create_index("", "items", "score"); },
        "index builder should validate its name before allocating a tree");
    check_throws<std::invalid_argument>(
        [&] { (void)tables.create_index("items_name_idx", "items", "name"); },
        "index builder should reject non-integer columns");
  }
  std::filesystem::remove(path);
}

void typed_tables_reject_corrupt_owned_indexes_before_writes() {
  const auto path = std::filesystem::temp_directory_path() / "pageforge-table-index-corruption-test.db";
  std::filesystem::remove(path);
  {
    auto heap = HeapFile::create(path);
    BufferPool pool(heap, 3);
    RecordStore records(pool);
    Catalog catalog(records);
    (void)catalog.create_table({"events", {{"id", pageforge::DataType::Integer, false}}, 1});
    auto index = BPlusTreeIndex::create(records);
    (void)catalog.register_index({"events_id_idx", "events", "id", index.header_id()});
    TableStore tables(records, catalog);
    const auto existing = tables.insert("events", {std::int64_t{1}});
    records.replace(index.header_id(), bytes("broken index header"));
    check_throws<IndexCorruption>([&] { (void)tables.insert("events", {std::int64_t{2}}); },
                                  "insert should validate every owned index before writing a row");
    check(tables.scan("events").size() == 1,
          "rejected indexed insert should not leave an unindexed table row");
    check_throws<IndexCorruption>([&] { (void)tables.erase("events", existing); },
                                  "delete should validate every owned index before changing data");
    check(tables.read("events", existing) == pageforge::Tuple{std::int64_t{1}},
          "rejected indexed delete should leave the table row intact");
  }
  std::filesystem::remove(path);
}

void typed_tables_detect_corrupt_envelopes() {
  const auto path = std::filesystem::temp_directory_path() / "pageforge-tables-corruption-test.db";
  std::filesystem::remove(path);
  {
    auto heap = HeapFile::create(path);
    BufferPool pool(heap, 2);
    RecordStore records(pool);
    Catalog catalog(records);
    (void)catalog.create_table({"events", {{"id", pageforge::DataType::Integer, false}}, 1});
    TableStore tables(records, catalog);
    (void)records.insert(bytes("PFR1"));
    check_throws<TableCorruption>([&] { (void)tables.scan("events"); },
                                  "truncated row envelope should fail scanning");
  }
  std::filesystem::remove(path);

  const auto version_path = std::filesystem::temp_directory_path() / "pageforge-tables-version-test.db";
  std::filesystem::remove(version_path);
  {
    auto heap = HeapFile::create(version_path);
    BufferPool pool(heap, 2);
    RecordStore records(pool);
    Catalog catalog(records);
    (void)catalog.create_table({"events", {{"id", pageforge::DataType::Integer, false}}, 1});
    TableStore tables(records, catalog);
    const auto id = tables.insert("events", {std::int64_t{7}});
    auto encoded = records.read(id);
    encoded[6] = std::byte{2};  // Encoded schema version starts after magic and envelope version.
    check(records.erase(id), "original row should be removable before corruption test");
    (void)records.insert(encoded);
    check_throws<TableCorruption>([&] { (void)tables.scan("events"); },
                                  "row with unknown schema version should fail decoding");
  }
  std::filesystem::remove(version_path);
}

void query_pipeline_filters_projects_and_limits() {
  const auto path = std::filesystem::temp_directory_path() / "pageforge-query-test.db";
  std::filesystem::remove(path);
  {
    auto heap = HeapFile::create(path);
    BufferPool pool(heap, 1);
    RecordStore records(pool);
    Catalog catalog(records);
    (void)catalog.create_table({"people", people_schema(), 1});
    TableStore tables(records, catalog);
    const auto ada_id = tables.insert("people", {std::int64_t{1}, std::string("Ada"), true,
                                                std::monostate{}});
    (void)tables.insert("people", {std::int64_t{2}, std::string("Bob"), false,
                                   std::string("analyst")});
    (void)tables.insert("people", {std::int64_t{3}, std::string("Grace"), true,
                                   std::monostate{}});
    const auto second_ada_id = tables.insert(
        "people", {std::int64_t{4}, std::string("Ada"), false, std::string("historian")});

    std::size_t examined = 0;
    auto query = Query::from(tables, "people");
    query.filter([&](const pageforge::TableRow& row) {
           ++examined;
           return std::holds_alternative<std::monostate>(row.values[3]);
         })
        .project({1, 0, 1})
        .limit(1);
    check(query.column_count() == 3, "projection should update the query's output width");
    const auto first = query.next();
    check(first && first->id == ada_id &&
              first->values == pageforge::Tuple{std::string("Ada"), std::int64_t{1}, std::string("Ada")},
          "query should filter nulls, reorder columns, and permit repeated columns");
    check(examined == 1, "limit should not pull extra rows before yielding the first result");
    check(!query.next() && examined == 1, "exhausted limit must not pull upstream rows");

    auto active = Query::from(tables, "people");
    active.filter([](const pageforge::TableRow& row) { return std::get<bool>(row.values[2]); })
        .project({1});
    const auto a = active.next();
    const auto g = active.next();
    check(a && a->values == pageforge::Tuple{std::string("Ada")} &&
              g && g->values == pageforge::Tuple{std::string("Grace")} && !active.next(),
          "filter and projection should stream matching rows in stable order");

    auto sorted = Query::from(tables, "people");
    sorted.sort([](const pageforge::TableRow& left, const pageforge::TableRow& right) {
            return std::get<std::string>(left.values[1]) >
                   std::get<std::string>(right.values[1]);
          })
        .project({1});
    const auto grace = sorted.next();
    const auto bob = sorted.next();
    const auto ada = sorted.next();
    const auto second_ada = sorted.next();
    check(grace && grace->values == pageforge::Tuple{std::string("Grace")} &&
              bob && bob->values == pageforge::Tuple{std::string("Bob")} &&
              ada && ada->id == ada_id && ada->values == pageforge::Tuple{std::string("Ada")} &&
              second_ada && second_ada->id == second_ada_id &&
              second_ada->values == pageforge::Tuple{std::string("Ada")} && !sorted.next(),
          "sort should materialize once, order rows, and retain scan order for ties");
  }
  std::filesystem::remove(path);
}

void query_pipeline_validates_and_short_circuits() {
  const auto path = std::filesystem::temp_directory_path() / "pageforge-query-validation-test.db";
  std::filesystem::remove(path);
  {
    auto heap = HeapFile::create(path);
    BufferPool pool(heap, 1);
    RecordStore records(pool);
    Catalog catalog(records);
    (void)catalog.create_table({"items", {{"id", pageforge::DataType::Integer, false}}, 1});
    TableStore tables(records, catalog);
    (void)tables.insert("items", {std::int64_t{7}});

    check_throws<std::out_of_range>([&] { (void)Query::from(tables, "missing"); },
                                    "query should reject an unknown table");
    auto invalid = Query::from(tables, "items");
    check_throws<std::invalid_argument>([&] { invalid.filter({}); },
                                        "query should reject an empty predicate");
    check_throws<std::invalid_argument>([&] { invalid.sort({}); },
                                        "query should reject an empty sort comparator");
    check_throws<std::out_of_range>([&] { invalid.project({1}); },
                                    "projection should validate indices before consuming rows");
    check(invalid.column_count() == 1 && invalid.next(),
          "failed query construction should leave the original scan usable");

    std::size_t examined = 0;
    auto zero = Query::from(tables, "items");
    zero.filter([&](const pageforge::TableRow&) {
          ++examined;
          return true;
        })
        .limit(0);
    check(!zero.next() && examined == 0, "limit zero must not pull any input rows");

    auto chained = Query::from(tables, "items");
    chained.project({0, 0});
    check_throws<std::out_of_range>([&] { chained.project({2}); },
                                    "later projections should validate against the current row width");
    check(chained.next()->values == pageforge::Tuple{std::int64_t{7}, std::int64_t{7}},
          "valid projection should remain usable after a rejected projection");
  }
  std::filesystem::remove(path);
}

void sql_lexer_recognizes_literals_and_positions() {
  const auto tokens = pageforge::lex_sql(
      "\nSELECT name, age FROM people\r\nWHERE age >= -18 AND name <> 'O''Neil'; -- tail\n");
  const std::vector<SqlTokenKind> expected{
      SqlTokenKind::Identifier, SqlTokenKind::Identifier, SqlTokenKind::Comma,
      SqlTokenKind::Identifier, SqlTokenKind::Identifier, SqlTokenKind::Identifier,
      SqlTokenKind::Identifier, SqlTokenKind::Identifier, SqlTokenKind::GreaterEqual,
      SqlTokenKind::Minus, SqlTokenKind::Integer, SqlTokenKind::Identifier,
      SqlTokenKind::Identifier, SqlTokenKind::NotEqual, SqlTokenKind::String,
      SqlTokenKind::Semicolon, SqlTokenKind::End};
  std::vector<SqlTokenKind> actual;
  for (const auto& token : tokens) actual.push_back(token.kind);
  check(actual == expected, "SQL lexer should recognize names, numbers, comparisons, and punctuation");
  check(tokens[0].text == "SELECT" && tokens[0].offset == 1 && tokens[0].line == 2 &&
            tokens[0].column == 1,
        "first token should retain its source location");
  check(tokens[6].text == "WHERE" && tokens[6].line == 3 && tokens[6].column == 1,
        "CRLF should count as one line break");
  check(tokens[14].text == "O'Neil", "doubled apostrophes should decode inside string literals");
  check(tokens.back().line == 4 && tokens.back().column == 1,
        "end token should point after the final line comment");
}

void sql_lexer_handles_comments_and_operators() {
  const auto tokens = pageforge::lex_sql("/* a\n b */ a.b + 1 <= 2 != 3 > 0 = 1 < 4 *;\n-- done");
  const std::vector<SqlTokenKind> expected{
      SqlTokenKind::Identifier, SqlTokenKind::Dot, SqlTokenKind::Identifier,
      SqlTokenKind::Plus, SqlTokenKind::Integer, SqlTokenKind::LessEqual,
      SqlTokenKind::Integer, SqlTokenKind::NotEqual, SqlTokenKind::Integer,
      SqlTokenKind::Greater, SqlTokenKind::Integer, SqlTokenKind::Equal,
      SqlTokenKind::Integer, SqlTokenKind::Less, SqlTokenKind::Integer,
      SqlTokenKind::Star, SqlTokenKind::Semicolon, SqlTokenKind::End};
  std::vector<SqlTokenKind> actual;
  for (const auto& token : tokens) actual.push_back(token.kind);
  check(actual == expected, "SQL lexer should skip comments and recognize all supported symbols");
  check(tokens[0].line == 2 && tokens[0].column == 7,
        "block-comment newlines should update source positions");
  check(tokens[7].text == "!=", "alternate inequality spelling should be preserved");
  const auto string_tokens = pageforge::lex_sql("'' 'line\nnext'");
  check(string_tokens[0].text.empty() && string_tokens[1].text == "line\nnext",
        "empty and multiline strings should decode exactly");
}

void sql_lexer_rejects_malformed_input() {
  const auto empty = pageforge::lex_sql("");
  check(empty.size() == 1 && empty[0].kind == SqlTokenKind::End && empty[0].offset == 0,
        "empty SQL should produce only an end token");
  check_throws<SqlLexError>([&] { (void)pageforge::lex_sql("'unfinished"); },
                            "unterminated string should fail");
  check_throws<SqlLexError>([&] { (void)pageforge::lex_sql("/* unfinished"); },
                            "unterminated block comment should fail");
  check_throws<SqlLexError>([&] { (void)pageforge::lex_sql("SELECT ! value"); },
                            "bare exclamation point should fail");
  check_throws<SqlLexError>([&] { (void)pageforge::lex_sql(std::string("a\0b", 3)); },
                            "NUL outside a string should fail");
  try {
    (void)pageforge::lex_sql(std::string("'a\0b'", 5));
    throw std::runtime_error("NUL inside a string should have failed");
  } catch (const SqlLexError& error) {
    check(error.line() == 1 && error.column() == 3,
          "NUL error should report the offending character, not just the string start");
  }
  try {
    (void)pageforge::lex_sql("SELECT\n$bad");
    throw std::runtime_error("invalid symbol should have raised a lexical error");
  } catch (const SqlLexError& error) {
    check(error.line() == 2 && error.column() == 1,
          "lexical error should expose the offending source position");
  }
}

void sql_parser_builds_select_plans() {
  const auto plan = pageforge::parse_select(
      "SeLeCt Name, age FROM People WHERE age >= -9223372036854775808 LIMIT 10;");
  check(plan.table == "People" && !plan.select_all &&
            plan.columns == std::vector<std::string>{"Name", "age"},
        "parser should preserve identifier spelling and projection order");
  check(plan.predicates.size() == 1 && plan.predicates[0].column == "age" &&
            plan.predicates[0].comparison == pageforge::SqlComparison::GreaterEqual &&
            plan.predicates[0].literal ==
                pageforge::SqlLiteral{std::numeric_limits<std::int64_t>::min()},
        "parser should build a comparison with the minimum signed integer");
  check(plan.limit == 10, "parser should record LIMIT");

  const auto all = pageforge::parse_select("SELECT * FROM flags WHERE active = TRUE");
  check(all.select_all && all.columns.empty() && all.predicates.size() == 1 &&
            all.predicates[0].literal == pageforge::SqlLiteral{true} && !all.limit,
        "parser should support wildcard projection and boolean literals");

  const auto string_plan = pageforge::parse_select(
      "SELECT name FROM people WHERE name <> 'O''Neil' LIMIT 0");
  check(string_plan.predicates.size() == 1 &&
            string_plan.predicates[0].comparison == pageforge::SqlComparison::NotEqual &&
            string_plan.predicates[0].literal == pageforge::SqlLiteral{std::string("O'Neil")} &&
            string_plan.limit == 0,
        "parser should carry decoded string literals and zero limits into the plan");

  const auto null_plan = pageforge::parse_select("SELECT note FROM people WHERE note = NULL;");
  check(null_plan.predicates.size() == 1 &&
            std::holds_alternative<std::monostate>(null_plan.predicates[0].literal),
        "parser should represent NULL distinctly");

  const auto conjunction = pageforge::parse_select(
      "SELECT name FROM people WHERE active = TRUE AND note IS NOT NULL AND id >= 2;");
  check(conjunction.predicates.size() == 3 &&
            conjunction.predicates[0].kind == pageforge::SqlPredicateKind::Comparison &&
            conjunction.predicates[1].column == "note" &&
            conjunction.predicates[1].kind == pageforge::SqlPredicateKind::IsNotNull &&
            conjunction.predicates[2].column == "id" &&
            conjunction.predicates[2].comparison == pageforge::SqlComparison::GreaterEqual,
        "parser should retain ordered AND predicates and null tests");

  const auto null_test = pageforge::parse_select("SELECT * FROM people WHERE note IS NULL");
  check(null_test.predicates.size() == 1 &&
            null_test.predicates[0].kind == pageforge::SqlPredicateKind::IsNull,
        "parser should represent IS NULL separately from comparison with NULL");

  const auto ordered = pageforge::parse_select(
      "SELECT name FROM people WHERE active = TRUE ORDER BY id DESC LIMIT 2;");
  check(ordered.order && ordered.order->column == "id" && ordered.order->descending &&
            ordered.limit == 2,
        "parser should record descending ORDER BY before LIMIT");
  const auto ascending = pageforge::parse_select("SELECT * FROM people ORDER BY name ASC");
  check(ascending.order && ascending.order->column == "name" && !ascending.order->descending,
        "parser should treat explicit ASC as ascending");
  const auto default_order = pageforge::parse_select("SELECT * FROM people ORDER BY id");
  check(default_order.order && !default_order.order->descending,
        "parser should default ORDER BY to ascending");
}

void sql_parser_maps_comparisons_and_signs() {
  const std::vector<std::pair<std::string, pageforge::SqlComparison>> comparisons{
      {"=", pageforge::SqlComparison::Equal}, {"!=", pageforge::SqlComparison::NotEqual},
      {"<>", pageforge::SqlComparison::NotEqual}, {"<", pageforge::SqlComparison::Less},
      {"<=", pageforge::SqlComparison::LessEqual}, {">", pageforge::SqlComparison::Greater},
      {">=", pageforge::SqlComparison::GreaterEqual}};
  for (const auto& [symbol, expected] : comparisons) {
    const auto plan = pageforge::parse_select("SELECT x FROM t WHERE x " + symbol + " +42");
    check(plan.predicates.size() == 1 && plan.predicates[0].comparison == expected &&
              plan.predicates[0].literal == pageforge::SqlLiteral{std::int64_t{42}},
          "parser should map every comparison and explicit positive sign");
  }
  const auto false_plan = pageforge::parse_select("SELECT x FROM t WHERE x = false;");
  check(false_plan.predicates.size() == 1 &&
            false_plan.predicates[0].literal == pageforge::SqlLiteral{false},
        "boolean keywords should be case-insensitive");
}

void sql_parser_builds_explain_statements() {
  const auto statement = pageforge::parse_sql_statement(
      "eXpLaIn SELECT name FROM People WHERE id = 7 ORDER BY name LIMIT 1;");
  check(std::holds_alternative<pageforge::ExplainPlan>(statement),
        "statement parser should distinguish EXPLAIN from executable SELECT");
  const auto& select = std::get<pageforge::ExplainPlan>(statement).select;
  check(select.table == "People" && select.columns == std::vector<std::string>{"name"} &&
            select.predicates.size() == 1 && select.order && select.limit == 1,
        "EXPLAIN should retain the complete nested SELECT plan");

  const auto ordinary = pageforge::parse_sql_statement("SELECT * FROM People");
  check(std::holds_alternative<pageforge::SelectPlan>(ordinary),
        "statement parser should retain ordinary SELECT statements");
  check_throws<SqlParseError>(
      [] { (void)pageforge::parse_select("EXPLAIN SELECT * FROM people"); },
      "SELECT-only parser should continue rejecting EXPLAIN");
  check_throws<SqlParseError>(
      [] { (void)pageforge::parse_sql_statement("EXPLAIN DELETE FROM people"); },
      "EXPLAIN should accept only the supported SELECT grammar");
}

void sql_parser_rejects_invalid_statements() {
  const std::vector<std::string> invalid{
      "", "DELETE FROM people", "SELECT FROM people", "SELECT a, FROM people",
      "SELECT * people", "SELECT * FROM", "SELECT * FROM t WHERE x",
      "SELECT * FROM t WHERE x AND 1", "SELECT * FROM t WHERE x = unknown",
      "SELECT * FROM t LIMIT -1", "SELECT * FROM t LIMIT",
      "SELECT * FROM t LIMIT 99999999999999999999999999999999999999",
      "SELECT * FROM t WHERE x = 9223372036854775808",
      "SELECT * FROM t WHERE x = -9223372036854775809",
      "SELECT * FROM t; SELECT * FROM u", "SELECT * FROM t;;",
      "SELECT * FROM t LIMIT 1 WHERE x = 2", "SELECT * FROM t WHERE x IS",
      "SELECT * FROM t WHERE x IS FALSE", "SELECT * FROM t WHERE x = 1 AND",
      "SELECT * FROM t WHERE x = 1 OR x = 2", "SELECT * FROM t ORDER x",
      "SELECT * FROM t ORDER BY", "SELECT * FROM t ORDER BY x SIDEWAYS",
      "SELECT * FROM t LIMIT 1 ORDER BY x"};
  for (const auto& sql : invalid) {
    check_throws<SqlParseError>([&] { (void)pageforge::parse_select(sql); },
                                "invalid SELECT syntax should fail");
  }

  try {
    (void)pageforge::parse_select("SELECT x\nFROM t\nWHERE x");
    throw std::runtime_error("missing comparison should have failed");
  } catch (const SqlParseError& error) {
    check(error.line() == 3 && error.column() == 8,
          "parse errors should retain the current token's line and column");
  }
}

void sql_execution_binds_and_streams_results() {
  const auto path = std::filesystem::temp_directory_path() / "pageforge-sql-execution-test.db";
  std::filesystem::remove(path);
  {
    auto heap = HeapFile::create(path);
    BufferPool pool(heap, 1);
    RecordStore records(pool);
    Catalog catalog(records);
    (void)catalog.create_table({"People", people_schema(), 1});
    TableStore tables(records, catalog);
    const auto ada_id = tables.insert("People", {std::int64_t{1}, std::string("Ada"), true,
                                                 std::monostate{}});
    (void)tables.insert("People", {std::int64_t{2}, std::string("Bob"), false,
                                   std::string("analyst")});
    (void)tables.insert("People", {std::int64_t{3}, std::string("Grace"), true,
                                   std::string("pioneer")});

    auto selected = pageforge::execute_select_sql(
        tables, catalog, "SELECT NAME, id, name FROM people WHERE ACTIVE = TRUE LIMIT 1;");
    check(selected.output_schema() == pageforge::Schema{{"name", pageforge::DataType::Text, false},
                                                        {"id", pageforge::DataType::Integer, false},
                                                        {"name", pageforge::DataType::Text, false}},
          "binding should resolve identifiers case-insensitively and preserve catalog metadata");
    const auto row = selected.next();
    check(row && row->id == ada_id &&
              row->values == pageforge::Tuple{std::string("Ada"), std::int64_t{1}, std::string("Ada")} &&
              !selected.next(),
          "SQL should execute filter, repeated projection, and limit lazily");

    auto names = pageforge::execute_select_sql(
        tables, catalog, "SELECT name FROM People WHERE name > 'Bob'");
    const auto grace = names.next();
    check(grace && grace->values == pageforge::Tuple{std::string("Grace")} && !names.next(),
          "text comparisons should use stable bytewise ordering");

    auto integers = pageforge::execute_select_sql(
        tables, catalog, "SELECT id FROM People WHERE id >= 2");
    const auto two = integers.next();
    const auto three = integers.next();
    check(two && two->values == pageforge::Tuple{std::int64_t{2}} &&
              three && three->values == pageforge::Tuple{std::int64_t{3}} && !integers.next(),
          "integer comparisons should stream all matching rows in record order");

    auto null_comparison = pageforge::execute_select_sql(
        tables, catalog, "SELECT * FROM People WHERE note = NULL");
    check(null_comparison.output_schema() == people_schema() && !null_comparison.next(),
          "NULL comparison should evaluate as unknown and be removed by WHERE");

    auto non_null = pageforge::execute_select_sql(
        tables, catalog, "SELECT name FROM People WHERE note != 'analyst'");
    const auto non_null_row = non_null.next();
    check(non_null_row && non_null_row->values == pageforge::Tuple{std::string("Grace")} && !non_null.next(),
          "nullable column comparisons should skip null rows and compare non-null values");

    auto null_rows = pageforge::execute_select_sql(
        tables, catalog, "SELECT name FROM People WHERE active = TRUE AND note IS NULL");
    const auto ada = null_rows.next();
    check(ada && ada->values == pageforge::Tuple{std::string("Ada")} && !null_rows.next(),
          "AND filters and IS NULL should stream only rows satisfying every predicate");

    auto present_notes = pageforge::execute_select_sql(
        tables, catalog, "SELECT name FROM People WHERE note IS NOT NULL AND id >= 3");
    const auto grace_with_note = present_notes.next();
    check(grace_with_note &&
              grace_with_note->values == pageforge::Tuple{std::string("Grace")} &&
              !present_notes.next(),
          "IS NOT NULL should compose with typed comparisons");

    auto descending = pageforge::execute_select_sql(
        tables, catalog, "SELECT name FROM People ORDER BY id DESC LIMIT 2");
    const auto grace_first = descending.next();
    const auto bob_second = descending.next();
    check(grace_first && grace_first->values == pageforge::Tuple{std::string("Grace")} &&
              bob_second && bob_second->values == pageforge::Tuple{std::string("Bob")} &&
              !descending.next(),
          "ORDER BY should bind unprojected columns and run before LIMIT");

    auto nulls_last = pageforge::execute_select_sql(
        tables, catalog, "SELECT name FROM People ORDER BY note ASC");
    const auto bob_by_note = nulls_last.next();
    const auto grace_by_note = nulls_last.next();
    const auto ada_by_note = nulls_last.next();
    check(bob_by_note && bob_by_note->values == pageforge::Tuple{std::string("Bob")} &&
              grace_by_note && grace_by_note->values == pageforge::Tuple{std::string("Grace")} &&
              ada_by_note && ada_by_note->values == pageforge::Tuple{std::string("Ada")} &&
              !nulls_last.next(),
          "ascending ORDER BY should use typed values and place nulls last");
  }
  std::filesystem::remove(path);
}

void sql_execution_rejects_binding_errors() {
  const auto path = std::filesystem::temp_directory_path() / "pageforge-sql-binding-test.db";
  std::filesystem::remove(path);
  {
    auto heap = HeapFile::create(path);
    BufferPool pool(heap, 2);
    RecordStore records(pool);
    Catalog catalog(records);
    (void)catalog.create_table({"people", people_schema(), 1});
    (void)catalog.create_table({"Things", {{"id", pageforge::DataType::Integer, false}}, 1});
    (void)catalog.create_table({"things", {{"id", pageforge::DataType::Integer, false}}, 1});
    (void)catalog.create_table({"ambiguous", {{"Name", pageforge::DataType::Text, false},
                                              {"name", pageforge::DataType::Text, false}}, 1});
    TableStore tables(records, catalog);

    const std::vector<std::string> invalid{
        "SELECT * FROM missing",
        "SELECT missing FROM people",
        "SELECT * FROM people WHERE missing = 1",
        "SELECT * FROM people WHERE id = 'wrong'",
        "SELECT * FROM people WHERE name = 7",
        "SELECT * FROM people WHERE active > TRUE",
        "SELECT * FROM people WHERE id >= 1 AND missing IS NULL",
        "SELECT * FROM people ORDER BY missing",
        "SELECT * FROM THINGS",
        "SELECT NAME FROM ambiguous"};
    for (const auto& sql : invalid) {
      check_throws<SqlBindError>([&] { (void)pageforge::execute_select_sql(tables, catalog, sql); },
                                 "invalid SQL names or types should fail during binding");
    }

    check_throws<SqlParseError>(
        [&] { (void)pageforge::execute_select_sql(tables, catalog, "SELECT FROM people"); },
        "execution entry point should preserve parse errors");
    pageforge::SelectPlan malformed;
    malformed.table = "people";
    check_throws<SqlBindError>([&] { (void)pageforge::bind_select(tables, catalog, malformed); },
                               "binder should reject malformed hand-built logical plans");
    auto zero = pageforge::execute_select_sql(tables, catalog, "SELECT * FROM people LIMIT 0");
    check(zero.output_schema() == people_schema() && !zero.next(),
          "bound LIMIT zero should retain output metadata without scanning rows");

    auto index = BPlusTreeIndex::create(records);
    (void)catalog.register_index({"people_id_idx", "people", "id", index.header_id()});
    records.replace(index.header_id(), bytes("broken index header"));
    check_throws<SqlBindError>(
        [&] {
          (void)pageforge::execute_select_sql(
              tables, catalog, "SELECT missing FROM people WHERE id = 1");
        },
        "all names should bind before an eager index lookup reads storage");
  }
  std::filesystem::remove(path);
}

void sql_execution_uses_indexed_equality_lookups() {
  const auto path = std::filesystem::temp_directory_path() / "pageforge-sql-index-test.db";
  std::filesystem::remove(path);
  {
    auto heap = HeapFile::create(path);
    BufferPool pool(heap, 4);
    RecordStore records(pool);
    Catalog catalog(records);
    (void)catalog.create_table({"Items", {{"id", pageforge::DataType::Integer, false},
                                           {"name", pageforge::DataType::Text, false},
                                           {"active", pageforge::DataType::Boolean, false}},
                                1});
    auto index = BPlusTreeIndex::create(records);
    (void)catalog.register_index({"items_id_idx", "Items", "id", index.header_id()});
    TableStore tables(records, catalog);
    const auto one_id = tables.insert("Items", {std::int64_t{1}, std::string("one"), true});
    (void)tables.insert("Items", {std::int64_t{2}, std::string("two-b"), false});
    const auto two_id = tables.insert("Items", {std::int64_t{2}, std::string("two-a"), true});

    auto selected = pageforge::execute_select_sql(
        tables, catalog,
        "SELECT name FROM items WHERE ID = 2 AND active = TRUE ORDER BY name ASC");
    check(selected.access_path() == pageforge::SelectAccessPath::IndexLookup &&
              selected.index_name() == std::optional<std::string>{"items_id_idx"},
          "integer equality should choose the matching catalog-owned index");
    const auto row = selected.next();
    check(row && row->id == two_id && row->values == pageforge::Tuple{std::string("two-a")} &&
              !selected.next(),
          "indexed candidates should still pass residual filters, sorting, and projection");

    auto text_scan = pageforge::execute_select_sql(
        tables, catalog, "SELECT id FROM Items WHERE name = 'one'");
    check(text_scan.access_path() == pageforge::SelectAccessPath::TableScan && !text_scan.index_name(),
          "an equality predicate without a matching index should retain a table scan");
    check(text_scan.next()->id == one_id && !text_scan.next(),
          "table-scan fallback should preserve SQL results");

    auto range_scan = pageforge::execute_select_sql(
        tables, catalog, "SELECT id FROM Items WHERE id >= 2");
    check(range_scan.access_path() == pageforge::SelectAccessPath::TableScan,
          "range predicates should not claim unsupported index planning");

    check(index.insert(999, one_id), "test should be able to inject a stale logical index entry");
    check_throws<TableCorruption>([&] { (void)tables.lookup_index("items_id_idx", 999); },
                                  "index lookup should reject keys that disagree with table rows");
  }
  std::filesystem::remove(path);
}

void sql_explain_reports_access_paths_without_running_rows() {
  const auto path = std::filesystem::temp_directory_path() / "pageforge-sql-explain-test.db";
  std::filesystem::remove(path);
  {
    auto heap = HeapFile::create(path);
    BufferPool pool(heap, 4);
    RecordStore records(pool);
    Catalog catalog(records);
    (void)catalog.create_table({"Items", {{"id", pageforge::DataType::Integer, false},
                                           {"name", pageforge::DataType::Text, false}},
                                1});
    TableStore tables(records, catalog);
    (void)tables.insert("Items", {std::int64_t{7}, std::string("seven")});
    const auto plan = pageforge::parse_select(
        "SELECT name FROM items WHERE id = 7 ORDER BY name LIMIT 1");

    const auto scan = pageforge::explain_select(catalog, plan);
    check(scan.table == "Items" && scan.access_path == pageforge::SelectAccessPath::TableScan &&
              !scan.index_name && !scan.lookup_key && scan.predicate_count == 1 &&
              scan.sorts_rows && scan.limit == 1,
          "EXPLAIN should report validated table-scan planning metadata");

    (void)tables.create_index("items_id_idx", "Items", "id");
    const auto indexed = pageforge::explain_select(catalog, plan);
    check(indexed.access_path == pageforge::SelectAccessPath::IndexLookup &&
              indexed.index_name == std::optional<std::string>{"items_id_idx"} &&
              indexed.lookup_key == std::optional<std::int64_t>{7},
          "EXPLAIN should identify the chosen equality index and lookup key");

    auto execution = pageforge::execute_sql(
        tables, catalog, "EXPLAIN SELECT name FROM items WHERE id = 7");
    check(std::holds_alternative<pageforge::SelectExplanation>(execution) &&
              std::get<pageforge::SelectExplanation>(execution).access_path ==
                  pageforge::SelectAccessPath::IndexLookup,
          "general SQL execution should dispatch EXPLAIN without producing rows");
    auto selected = pageforge::execute_sql(
        tables, catalog, "SELECT name FROM items WHERE id = 7");
    check(std::holds_alternative<pageforge::BoundSelect>(selected),
          "general SQL execution should retain ordinary SELECT results");
    auto& rows = std::get<pageforge::BoundSelect>(selected);
    const auto row = rows.next();
    check(row && row->values == pageforge::Tuple{std::string("seven")} && !rows.next(),
          "EXPLAIN support should not change SELECT results");
  }
  std::filesystem::remove(path);
}

void bplus_tree_routes_splits_ranges_and_reopens() {
  const auto path = std::filesystem::temp_directory_path() / "pageforge-bplus-leaves-test.db";
  std::filesystem::remove(path);
  RecordId header_id{};
  {
    auto heap = HeapFile::create(path);
    BufferPool pool(heap, 4);
    RecordStore records(pool);
    auto index = BPlusTreeIndex::create(records);
    header_id = index.header_id();
    check(index.height() == 1 && index.root_id() == RecordId{0, 0},
          "new index should begin as a single root leaf");

    for (std::int64_t position = 0; position < 400; ++position) {
      const auto key = (position * 137) % 400;
      check(index.insert(key, {static_cast<pageforge::PageId>(10 + key / 50),
                               static_cast<pageforge::SlotId>(key % 50)}),
            "new index entry should be inserted");
    }
    check(index.insert(42, {99, 2}) && index.insert(42, {99, 1}),
          "duplicate keys with distinct record IDs should be retained");
    check(!index.insert(42, {99, 1}), "an exact index entry should be idempotent");
    check(index.insert(std::numeric_limits<std::int64_t>::min(), {200, 1}) &&
              index.insert(std::numeric_limits<std::int64_t>::max(), {200, 2}),
          "signed key extremes should be ordered without narrowing");
    check(index.leaf_count() >= 4, "capacity overflow should split the persistent leaf chain");
    check(index.height() == 2 && index.root_id() != RecordId{0, 0},
          "the first leaf split should promote a durable internal root");

    const auto matches = index.find(42);
    check(matches == std::vector<RecordId>{{10, 42}, {99, 1}, {99, 2}},
          "point lookup should return duplicate-key record IDs in stable order");
    const auto range = index.range(120, 135);
    check(range.size() == 16 && range.front().key == 120 && range.back().key == 135,
          "inclusive range lookup should cross split leaf boundaries in key order");
    for (std::size_t offset = 1; offset < range.size(); ++offset) {
      check(range[offset - 1].key + 1 == range[offset].key,
            "range lookup should not skip or reorder keys");
    }
    const auto all = index.range(std::nullopt, std::nullopt);
    check(all.size() == 404 && all.front().key == std::numeric_limits<std::int64_t>::min() &&
              all.back().key == std::numeric_limits<std::int64_t>::max(),
          "unbounded range should cover every ordered entry");
    check_throws<std::invalid_argument>([&] { (void)index.range(8, 7); },
                                        "range lookup should reject reversed bounds");
    pool.flush_all();
  }

  {
    auto heap = HeapFile::open(path);
    BufferPool pool(heap, 2);
    RecordStore records(pool);
    auto index = BPlusTreeIndex::open(records, header_id);
    check(index.height() == 2 && index.leaf_count() >= 4 && index.find(42).size() == 3,
          "internal root, leaf links, and duplicate keys should survive reopen");
    const auto tail = index.range(398, std::nullopt);
    check(tail.size() == 3 && tail[0].key == 398 && tail[1].key == 399 &&
              tail[2].key == std::numeric_limits<std::int64_t>::max(),
          "reopened index should preserve upper-tail range traversal");
    check(index.insert(42, {100, 3}) && index.find(42).size() == 4,
          "reopened internal routing should accept and expose new duplicate-key entries");
    pool.flush_all();
  }

  {
    auto heap = HeapFile::open(path);
    BufferPool pool(heap, 2);
    RecordStore records(pool);
    auto index = BPlusTreeIndex::open(records, header_id);
    check(index.height() == 2 && index.find(42).size() == 4,
          "internal-root updates should remain valid after a second reopen");
  }
  std::filesystem::remove(path);
}

void bplus_tree_detects_corruption() {
  const auto path = std::filesystem::temp_directory_path() / "pageforge-bplus-corruption-test.db";
  std::filesystem::remove(path);
  {
    auto heap = HeapFile::create(path);
    BufferPool pool(heap, 2);
    RecordStore records(pool);
    auto index = BPlusTreeIndex::create(records);
    records.replace({0, 0}, bytes("not an index leaf"));
    check_throws<IndexCorruption>([&] { (void)index.leaf_count(); },
                                  "malformed index leaves should be rejected");
    records.replace(index.header_id(), bytes("not an index header"));
    check_throws<IndexCorruption>([&] { (void)BPlusTreeIndex::open(records, index.header_id()); },
                                  "malformed index headers should be rejected");
  }
  std::filesystem::remove(path);

  const auto internal_path =
      std::filesystem::temp_directory_path() / "pageforge-bplus-internal-corruption-test.db";
  std::filesystem::remove(internal_path);
  {
    auto heap = HeapFile::create(internal_path);
    BufferPool pool(heap, 3);
    RecordStore records(pool);
    auto index = BPlusTreeIndex::create(records);
    for (std::int64_t key = 0; key <= static_cast<std::int64_t>(pageforge::kBPlusLeafCapacity);
         ++key) {
      (void)index.insert(key, {300, static_cast<pageforge::SlotId>(key)});
    }
    check(index.height() == 2, "leaf overflow should create an internal root before corruption test");
    records.replace(index.root_id(), bytes("not an internal node"));
    check_throws<IndexCorruption>([&] { (void)index.leaf_count(); },
                                  "malformed internal roots should be rejected");
  }
  std::filesystem::remove(internal_path);

  const auto missing_path =
      std::filesystem::temp_directory_path() / "pageforge-bplus-missing-header-test.db";
  std::filesystem::remove(missing_path);
  {
    auto heap = HeapFile::create(missing_path);
    BufferPool pool(heap, 1);
    RecordStore records(pool);
    check_throws<IndexCorruption>([&] { (void)BPlusTreeIndex::open(records, {0, 0}); },
                                  "missing index headers should become index corruption errors");
  }
  std::filesystem::remove(missing_path);
}

void bplus_tree_deletes_repairs_and_collapses() {
  const auto path = std::filesystem::temp_directory_path() / "pageforge-bplus-delete-test.db";
  std::filesystem::remove(path);
  RecordId header_id{};
  std::size_t removed = 0;
  {
    auto heap = HeapFile::create(path);
    BufferPool pool(heap, 4);
    RecordStore records(pool);
    auto index = BPlusTreeIndex::create(records);
    header_id = index.header_id();
    for (std::int64_t key = 0; key < 400; ++key) {
      (void)index.insert(key, {400, static_cast<pageforge::SlotId>(key)});
    }
    const auto initial_leaves = index.leaf_count();
    check(index.height() == 2 && initial_leaves > 2,
          "deletion test should begin with a multi-leaf internal root");
    while (index.leaf_count() == initial_leaves) {
      check(index.erase(static_cast<std::int64_t>(removed),
                        {400, static_cast<pageforge::SlotId>(removed)}),
            "existing first-leaf entry should be erased");
      ++removed;
    }
    const auto remaining = index.range(std::nullopt, std::nullopt);
    check(index.height() == 2 && index.leaf_count() == initial_leaves - 1 &&
              remaining.size() == 400 - removed &&
              remaining.front().key == static_cast<std::int64_t>(removed),
          "empty first leaf should be unlinked and root children repaired");
    pool.flush_all();
  }

  {
    auto heap = HeapFile::open(path);
    BufferPool pool(heap, 4);
    RecordStore records(pool);
    auto index = BPlusTreeIndex::open(records, header_id);
    const auto remaining = index.range(std::nullopt, std::nullopt);
    for (const auto& entry : remaining) {
      check(index.erase(entry.key, entry.value),
            "reopened index should erase every remaining routed entry");
    }
    check(index.height() == 1 && index.leaf_count() == 1 &&
              index.range(std::nullopt, std::nullopt).empty(),
          "deleting through underflow should collapse the root to one empty leaf");
    check(!index.erase(7, {400, 7}), "erasing a missing exact entry should be idempotent");
    check(index.insert(-1, {401, 1}) && index.find(-1) == std::vector<RecordId>{{401, 1}},
          "collapsed empty tree should accept new entries");
    pool.flush_all();
  }

  {
    auto heap = HeapFile::open(path);
    BufferPool pool(heap, 2);
    RecordStore records(pool);
    auto index = BPlusTreeIndex::open(records, header_id);
    check(index.height() == 1 && index.find(-1) == std::vector<RecordId>{{401, 1}},
          "collapsed root and subsequent insert should survive reopen");
  }
  std::filesystem::remove(path);
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"slotted page round-trip", slotted_page_round_trip},
      {"deletion, compaction, and slot reuse", deletion_compaction_and_slot_reuse},
      {"stable record replacement", replacement_preserves_slots_and_failure_atomicity},
      {"page capacity and corruption", page_capacity_and_corruption},
      {"heap file persistence", heap_file_persists_pages},
      {"heap file corruption detection", heap_file_detects_page_corruption},
      {"heap header corruption detection", heap_file_detects_header_corruption},
      {"heap truncation detection", heap_file_detects_truncation},
      {"buffer pool cache hits", buffer_pool_caches_page_hits},
      {"buffer pool dirty eviction", buffer_pool_writes_dirty_victims},
      {"buffer pool pin safety", buffer_pool_respects_pins},
      {"buffer pool allocation and flush", buffer_pool_flushes_allocated_pages},
      {"record store multi-page persistence", record_store_spans_pages_and_reopens},
      {"record store deletion and reuse", record_store_reuses_deleted_space},
      {"record store insertion hint wraparound", record_store_wraps_insertion_hint_to_reuse_space},
      {"record store input validation", record_store_rejects_invalid_records},
      {"record cursor streaming and pin safety", record_cursor_streams_without_pins},
      {"typed tuple round-trip", tuple_codec_round_trips_typed_values},
      {"typed tuple validation", tuple_codec_rejects_invalid_values},
      {"typed tuple corruption detection", tuple_codec_detects_corruption},
      {"typed tuple storage persistence", typed_tuples_persist_in_record_store},
      {"catalog schema persistence", catalog_persists_table_schemas},
      {"catalog definition validation", catalog_validates_definitions},
      {"catalog corruption detection", catalog_detects_corrupt_metadata},
      {"catalog index ownership persistence", catalog_persists_index_ownership},
      {"catalog index definition validation", catalog_validates_index_definitions},
      {"typed table persistence and isolation", typed_tables_persist_and_isolate_rows},
      {"typed table input and ownership validation", typed_tables_validate_input_and_ownership},
      {"typed table automatic index maintenance", typed_tables_maintain_registered_indexes},
      {"typed table existing-row index build", typed_tables_build_indexes_for_existing_rows},
      {"typed table index corruption safety", typed_tables_reject_corrupt_owned_indexes_before_writes},
      {"typed table corruption detection", typed_tables_detect_corrupt_envelopes},
      {"query filter, projection, and limit", query_pipeline_filters_projects_and_limits},
      {"query validation and lazy limit", query_pipeline_validates_and_short_circuits},
      {"SQL lexer tokens and positions", sql_lexer_recognizes_literals_and_positions},
      {"SQL lexer comments and operators", sql_lexer_handles_comments_and_operators},
      {"SQL lexer malformed input", sql_lexer_rejects_malformed_input},
      {"SQL parser SELECT plans", sql_parser_builds_select_plans},
      {"SQL parser comparisons and signs", sql_parser_maps_comparisons_and_signs},
      {"SQL parser EXPLAIN statements", sql_parser_builds_explain_statements},
      {"SQL parser invalid statements", sql_parser_rejects_invalid_statements},
      {"SQL binding and execution", sql_execution_binds_and_streams_results},
      {"SQL binding validation", sql_execution_rejects_binding_errors},
      {"SQL indexed equality execution", sql_execution_uses_indexed_equality_lookups},
      {"SQL EXPLAIN access paths", sql_explain_reports_access_paths_without_running_rows},
      {"B+ tree routing, splits, ranges, and reopen", bplus_tree_routes_splits_ranges_and_reopens},
      {"B+ tree corruption detection", bplus_tree_detects_corruption},
      {"B+ tree deletion and root collapse", bplus_tree_deletes_repairs_and_collapses},
  };
  std::size_t passed = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      ++passed;
      std::cout << "PASS  " << name << '\n';
    } catch (const std::exception& error) {
      std::cerr << "FAIL  " << name << ": " << error.what() << '\n';
    }
  }
  std::cout << passed << "/" << tests.size() << " tests passed\n";
  return passed == tests.size() ? 0 : 1;
}
