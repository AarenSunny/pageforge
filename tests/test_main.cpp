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
#include <vector>

#include "pageforge/heap_file.hpp"
#include "pageforge/buffer_pool.hpp"
#include "pageforge/page.hpp"
#include "pageforge/record_store.hpp"

namespace {

using pageforge::HeapFile;
using pageforge::BufferPool;
using pageforge::BufferPoolExhausted;
using pageforge::PageCorruption;
using pageforge::PageFull;
using pageforge::RecordStore;
using pageforge::SlottedPage;

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
