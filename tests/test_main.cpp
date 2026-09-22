#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
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
using pageforge::TableStore;
using pageforge::TableCorruption;
using pageforge::Query;
using pageforge::SqlLexError;
using pageforge::SqlTokenKind;

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

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"slotted page round-trip", slotted_page_round_trip},
      {"deletion, compaction, and slot reuse", deletion_compaction_and_slot_reuse},
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
      {"record store input validation", record_store_rejects_invalid_records},
      {"record cursor streaming and pin safety", record_cursor_streams_without_pins},
      {"typed tuple round-trip", tuple_codec_round_trips_typed_values},
      {"typed tuple validation", tuple_codec_rejects_invalid_values},
      {"typed tuple corruption detection", tuple_codec_detects_corruption},
      {"typed tuple storage persistence", typed_tuples_persist_in_record_store},
      {"catalog schema persistence", catalog_persists_table_schemas},
      {"catalog definition validation", catalog_validates_definitions},
      {"catalog corruption detection", catalog_detects_corrupt_metadata},
      {"typed table persistence and isolation", typed_tables_persist_and_isolate_rows},
      {"typed table input and ownership validation", typed_tables_validate_input_and_ownership},
      {"typed table corruption detection", typed_tables_detect_corrupt_envelopes},
      {"query filter, projection, and limit", query_pipeline_filters_projects_and_limits},
      {"query validation and lazy limit", query_pipeline_validates_and_short_circuits},
      {"SQL lexer tokens and positions", sql_lexer_recognizes_literals_and_positions},
      {"SQL lexer comments and operators", sql_lexer_handles_comments_and_operators},
      {"SQL lexer malformed input", sql_lexer_rejects_malformed_input},
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
