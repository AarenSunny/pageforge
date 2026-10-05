#include "pageforge/record_store.hpp"

#include <stdexcept>
#include <utility>

namespace pageforge {

RecordStore::RecordStore(BufferPool& pool) : pool_(pool) {
  if (pool_.page_count() != 0) insertion_page_hint_ = pool_.page_count() - 1;
}

RecordCursor::RecordCursor(RecordCursor&& other) noexcept
    : pool_(other.pool_), next_page_(other.next_page_), next_slot_(other.next_slot_),
      end_page_(other.end_page_) {
  other.next_page_ = other.end_page_;
}

std::optional<Record> RecordCursor::next() {
  while (next_page_ < end_page_) {
    const auto guard = pool_.fetch(next_page_);
    while (next_slot_ < guard.page().slot_count()) {
      const auto slot_id = static_cast<SlotId>(next_slot_++);
      if (guard.page().contains(slot_id)) {
        return Record{{next_page_, slot_id}, guard.page().read(slot_id)};
      }
    }
    ++next_page_;
    next_slot_ = 0;
  }
  return std::nullopt;
}

RecordId RecordStore::insert(std::span<const std::byte> bytes) {
  if (bytes.empty()) throw std::invalid_argument("records must not be empty");
  if (bytes.size() > kPageSize - kPageHeaderSize - kSlotSize) {
    throw PageFull("record cannot fit on an empty page");
  }

  const auto page_count = pool_.page_count();
  const auto start_page = insertion_page_hint_ < page_count ? insertion_page_hint_ : PageId{0};
  for (PageId offset = 0; offset < page_count; ++offset) {
    const auto page_id = static_cast<PageId>(
        (static_cast<std::uint64_t>(start_page) + offset) % page_count);
    auto guard = pool_.fetch(page_id);
    try {
      const auto slot_id = guard.mutable_page().insert(bytes);
      insertion_page_hint_ = page_id;
      return {page_id, slot_id};
    } catch (const PageFull&) {
      // Another page may have room; no page is held pinned across the next fetch.
    }
  }

  auto guard = pool_.allocate();
  const auto slot_id = guard.mutable_page().insert(bytes);
  insertion_page_hint_ = guard.page_id();
  return {guard.page_id(), slot_id};
}

std::vector<std::byte> RecordStore::read(RecordId id) {
  const auto guard = pool_.fetch(id.page_id);
  return guard.page().read(id.slot_id);
}

void RecordStore::replace(RecordId id, std::span<const std::byte> bytes) {
  if (id.page_id >= pool_.page_count()) throw std::out_of_range("record page does not exist");
  auto guard = pool_.fetch(id.page_id);
  guard.mutable_page().replace(id.slot_id, bytes);
}

bool RecordStore::erase(RecordId id) {
  if (id.page_id >= pool_.page_count()) return false;
  auto guard = pool_.fetch(id.page_id);
  if (!guard.page().contains(id.slot_id)) return false;
  return guard.mutable_page().erase(id.slot_id);
}

RecordCursor RecordStore::cursor() { return RecordCursor(pool_); }

std::vector<Record> RecordStore::scan() {
  std::vector<Record> records;
  auto reader = cursor();
  while (auto record = reader.next()) records.push_back(std::move(*record));
  return records;
}

}  // namespace pageforge
