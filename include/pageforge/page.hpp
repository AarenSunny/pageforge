#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace pageforge {

using PageId = std::uint32_t;
using SlotId = std::uint16_t;

inline constexpr std::size_t kPageSize = 4096;
inline constexpr std::size_t kPageHeaderSize = 24;
inline constexpr std::size_t kSlotSize = 6;

class PageCorruption final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class PageFull final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class SlottedPage {
 public:
  using Bytes = std::array<std::byte, kPageSize>;

  static SlottedPage initialize(PageId page_id);
  static SlottedPage from_bytes(const Bytes& bytes);

  [[nodiscard]] PageId page_id() const;
  [[nodiscard]] std::size_t slot_count() const;
  [[nodiscard]] std::size_t live_records() const;
  [[nodiscard]] bool contains(SlotId slot_id) const;
  [[nodiscard]] std::size_t free_space() const;
  [[nodiscard]] std::size_t reclaimable_space() const;
  [[nodiscard]] const Bytes& bytes() const noexcept { return bytes_; }

  SlotId insert(std::span<const std::byte> record);
  [[nodiscard]] std::vector<std::byte> read(SlotId slot_id) const;
  bool erase(SlotId slot_id);
  void compact();
  void validate() const;

 private:
  explicit SlottedPage(Bytes bytes) : bytes_(std::move(bytes)) {}
  void update_checksum();

  Bytes bytes_{};
};

}  // namespace pageforge
