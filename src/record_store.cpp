#include "pageforge/record_store.hpp"

#include <stdexcept>

namespace pageforge {

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

std::vector<Record> RecordStore::scan() {
  std::vector<Record> records;
  for (PageId page_id = 0; page_id < pool_.page_count(); ++page_id) {
    const auto guard = pool_.fetch(page_id);
    for (std::size_t slot = 0; slot < guard.page().slot_count(); ++slot) {
      const auto slot_id = static_cast<SlotId>(slot);
      if (guard.page().contains(slot_id)) {
        records.push_back({{page_id, slot_id}, guard.page().read(slot_id)});
      }
    }
  }
  return records;
}

}  // namespace pageforge
