#include "pageforge/buffer_pool.hpp"

#include <utility>

namespace pageforge {

PageGuard::PageGuard(PageGuard&& other) noexcept
    : pool_(std::exchange(other.pool_, nullptr)), frame_index_(other.frame_index_) {}

PageGuard& PageGuard::operator=(PageGuard&& other) noexcept {
  if (this != &other) {
    release();
    pool_ = std::exchange(other.pool_, nullptr);
    frame_index_ = other.frame_index_;
  }
  return *this;
}

PageGuard::~PageGuard() { release(); }

PageId PageGuard::page_id() const { return page().page_id(); }

const SlottedPage& PageGuard::page() const {
  if (!pool_) throw std::logic_error("page guard has been released");
  return pool_->frame_page(frame_index_);
}

SlottedPage& PageGuard::mutable_page() {
  if (!pool_) throw std::logic_error("page guard has been released");
  pool_->mark_dirty(frame_index_);
  return pool_->frame_page(frame_index_);
}

void PageGuard::release() noexcept {
  if (!pool_) return;
  pool_->unpin(frame_index_);
  pool_ = nullptr;
}

BufferPool::BufferPool(HeapFile& heap, std::size_t capacity) : heap_(heap), frames_(capacity) {
  if (capacity == 0) throw std::invalid_argument("buffer pool capacity must be positive");
}

BufferPool::~BufferPool() {
  try {
    flush_all();
  } catch (...) {
    // Destructors cannot report I/O failures. Call flush_all explicitly when the
    // caller needs durability errors to propagate.
  }
}

PageGuard BufferPool::fetch(PageId page_id) {
  if (const auto found = page_table_.find(page_id); found != page_table_.end()) {
    auto& frame = frames_[found->second];
    ++frame.pin_count;
    frame.referenced = true;
    ++stats_.hits;
    return PageGuard(this, found->second);
  }

  const auto frame_index = choose_victim();
  auto incoming = heap_.read_page(page_id);
  flush_frame(frame_index);
  install(frame_index, std::move(incoming));
  ++stats_.misses;
  return PageGuard(this, frame_index);
}

PageGuard BufferPool::allocate() {
  const auto frame_index = choose_victim();
  flush_frame(frame_index);
  auto page = heap_.allocate_page();
  install(frame_index, std::move(page));
  ++stats_.misses;
  return PageGuard(this, frame_index);
}

void BufferPool::flush(PageId page_id) {
  const auto found = page_table_.find(page_id);
  if (found == page_table_.end()) return;
  flush_frame(found->second);
  heap_.sync();
}

void BufferPool::flush_all() {
  for (std::size_t index = 0; index < frames_.size(); ++index) flush_frame(index);
  heap_.sync();
}

std::size_t BufferPool::choose_victim() {
  for (std::size_t examined = 0; examined < frames_.size() * 2; ++examined) {
    const auto candidate = clock_hand_;
    clock_hand_ = (clock_hand_ + 1) % frames_.size();
    auto& frame = frames_[candidate];
    if (!frame.page) return candidate;
    if (frame.pin_count != 0) continue;
    if (frame.referenced) {
      frame.referenced = false;
      continue;
    }
    return candidate;
  }
  throw BufferPoolExhausted("every buffer frame is pinned");
}

void BufferPool::flush_frame(std::size_t frame_index) {
  auto& frame = frames_.at(frame_index);
  if (!frame.page || !frame.dirty) return;
  heap_.write_page(*frame.page);
  frame.dirty = false;
  ++stats_.dirty_writes;
}

void BufferPool::install(std::size_t frame_index, SlottedPage page) {
  auto& frame = frames_.at(frame_index);
  if (frame.page) {
    page_table_.erase(frame.page->page_id());
    ++stats_.evictions;
  }
  const auto page_id = page.page_id();
  frame.page = std::move(page);
  frame.pin_count = 1;
  frame.referenced = true;
  frame.dirty = false;
  page_table_.emplace(page_id, frame_index);
}

void BufferPool::unpin(std::size_t frame_index) noexcept {
  auto& frame = frames_[frame_index];
  if (frame.pin_count > 0) --frame.pin_count;
}

void BufferPool::mark_dirty(std::size_t frame_index) { frames_.at(frame_index).dirty = true; }

SlottedPage& BufferPool::frame_page(std::size_t frame_index) {
  auto& frame = frames_.at(frame_index);
  if (!frame.page) throw std::logic_error("buffer frame is empty");
  return *frame.page;
}

const SlottedPage& BufferPool::frame_page(std::size_t frame_index) const {
  const auto& frame = frames_.at(frame_index);
  if (!frame.page) throw std::logic_error("buffer frame is empty");
  return *frame.page;
}

}  // namespace pageforge
