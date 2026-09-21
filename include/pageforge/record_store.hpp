#pragma once

#include <cstddef>
#include <optional>
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

class RecordCursor {
 public:
  RecordCursor(const RecordCursor&) = delete;
  RecordCursor& operator=(const RecordCursor&) = delete;
  RecordCursor(RecordCursor&& other) noexcept;

  [[nodiscard]] std::optional<Record> next();

 private:
  friend class RecordStore;
  explicit RecordCursor(BufferPool& pool) : pool_(pool), end_page_(pool.page_count()) {}

  BufferPool& pool_;
  PageId next_page_ = 0;
  std::size_t next_slot_ = 0;
  PageId end_page_;
};

class RecordStore {
 public:
  explicit RecordStore(BufferPool& pool) : pool_(pool) {}

  [[nodiscard]] RecordId insert(std::span<const std::byte> bytes);
  [[nodiscard]] std::vector<std::byte> read(RecordId id);
  bool erase(RecordId id);
  [[nodiscard]] RecordCursor cursor();
  [[nodiscard]] std::vector<Record> scan();

 private:
  BufferPool& pool_;
};

}  // namespace pageforge
