#pragma once

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "pageforge/heap_file.hpp"

namespace pageforge {

class BufferPool;

class BufferPoolExhausted final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct BufferPoolStats {
  std::size_t hits = 0;
  std::size_t misses = 0;
  std::size_t evictions = 0;
  std::size_t dirty_writes = 0;
};

class PageGuard {
 public:
  PageGuard(const PageGuard&) = delete;
  PageGuard& operator=(const PageGuard&) = delete;
  PageGuard(PageGuard&& other) noexcept;
  PageGuard& operator=(PageGuard&& other) noexcept;
  ~PageGuard();

  [[nodiscard]] PageId page_id() const;
  [[nodiscard]] const SlottedPage& page() const;
  SlottedPage& mutable_page();
  void release() noexcept;

 private:
  friend class BufferPool;
  PageGuard(BufferPool* pool, std::size_t frame_index) : pool_(pool), frame_index_(frame_index) {}

  BufferPool* pool_ = nullptr;
  std::size_t frame_index_ = 0;
};

class BufferPool {
 public:
  BufferPool(HeapFile& heap, std::size_t capacity);
  BufferPool(const BufferPool&) = delete;
  BufferPool& operator=(const BufferPool&) = delete;
  ~BufferPool();

  [[nodiscard]] PageGuard fetch(PageId page_id);
  [[nodiscard]] PageGuard allocate();
  void flush(PageId page_id);
  void flush_all();

  [[nodiscard]] std::size_t capacity() const noexcept { return frames_.size(); }
  [[nodiscard]] PageId page_count() const noexcept { return heap_.page_count(); }
  [[nodiscard]] std::size_t resident_pages() const noexcept { return page_table_.size(); }
  [[nodiscard]] bool resident(PageId page_id) const { return page_table_.contains(page_id); }
  [[nodiscard]] BufferPoolStats stats() const noexcept { return stats_; }

 private:
  friend class PageGuard;

  struct Frame {
    std::optional<SlottedPage> page;
    std::size_t pin_count = 0;
    bool referenced = false;
    bool dirty = false;
  };

  [[nodiscard]] std::size_t choose_victim();
  void flush_frame(std::size_t frame_index);
  void install(std::size_t frame_index, SlottedPage page);
  void unpin(std::size_t frame_index) noexcept;
  void mark_dirty(std::size_t frame_index);
  [[nodiscard]] SlottedPage& frame_page(std::size_t frame_index);
  [[nodiscard]] const SlottedPage& frame_page(std::size_t frame_index) const;

  HeapFile& heap_;
  std::vector<Frame> frames_;
  std::unordered_map<PageId, std::size_t> page_table_;
  std::size_t clock_hand_ = 0;
  BufferPoolStats stats_{};
};

}  // namespace pageforge
