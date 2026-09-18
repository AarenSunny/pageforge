#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "pageforge/buffer_pool.hpp"

namespace pageforge {

struct RecordId {
  PageId page_id;
  SlotId slot_id;

  bool operator==(const RecordId&) const = default;
};

struct Record {
  RecordId id;
  std::vector<std::byte> bytes;
};

class RecordStore {
 public:
  explicit RecordStore(BufferPool& pool) : pool_(pool) {}

  [[nodiscard]] RecordId insert(std::span<const std::byte> bytes);
  [[nodiscard]] std::vector<std::byte> read(RecordId id);
  bool erase(RecordId id);
  [[nodiscard]] std::vector<Record> scan();

 private:
  BufferPool& pool_;
};

}  // namespace pageforge
