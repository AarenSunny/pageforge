#include "pageforge/record_store.hpp"

#include <stdexcept>
#include <utility>

namespace pageforge {

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

  for (PageId page_id = 0; page_id < pool_.page_count(); ++page_id) {
    auto guard = pool_.fetch(page_id);
    try {
      return {page_id, guard.mutable_page().insert(bytes)};
    } catch (const PageFull&) {
      // A later page may have room; no page is held pinned across the next fetch.
    }
  }

  auto guard = pool_.allocate();
  return {guard.page_id(), guard.mutable_page().insert(bytes)};
}

std::vector<std::byte> RecordStore::read(RecordId id) {
  const auto guard = pool_.fetch(id.page_id);
  return guard.page().read(id.slot_id);
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
