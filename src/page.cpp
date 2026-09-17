#include "pageforge/page.hpp"

#include <algorithm>
#include <limits>
#include <string>

namespace pageforge {
namespace {

constexpr std::uint32_t kMagic = 0x52464750;  // "PGFR" in little endian.
constexpr std::uint16_t kVersion = 1;
constexpr std::uint16_t kLive = 1;
constexpr std::size_t kMagicOffset = 0;
constexpr std::size_t kVersionOffset = 4;
constexpr std::size_t kPageIdOffset = 8;
constexpr std::size_t kSlotCountOffset = 12;
constexpr std::size_t kFreeStartOffset = 14;
constexpr std::size_t kFreeEndOffset = 16;
constexpr std::size_t kChecksumOffset = 20;

std::uint16_t read_u16(const SlottedPage::Bytes& bytes, std::size_t offset) {
  return static_cast<std::uint16_t>(std::to_integer<unsigned>(bytes[offset])) |
         static_cast<std::uint16_t>(std::to_integer<unsigned>(bytes[offset + 1]) << 8U);
}

std::uint32_t read_u32(const SlottedPage::Bytes& bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(std::to_integer<unsigned>(bytes[offset + index])) << (index * 8U);
  }
  return value;
}

void write_u16(SlottedPage::Bytes& bytes, std::size_t offset, std::uint16_t value) {
  bytes[offset] = static_cast<std::byte>(value & 0xffU);
  bytes[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xffU);
}

void write_u32(SlottedPage::Bytes& bytes, std::size_t offset, std::uint32_t value) {
  for (std::size_t index = 0; index < 4; ++index) {
    bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
  }
}

std::uint32_t checksum(const SlottedPage::Bytes& bytes) {
  std::uint32_t hash = 2166136261U;
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    const unsigned value = index >= kChecksumOffset && index < kChecksumOffset + 4
                               ? 0U
                               : std::to_integer<unsigned>(bytes[index]);
    hash ^= value;
    hash *= 16777619U;
  }
  return hash;
}

std::size_t slot_offset(SlotId slot_id) {
  return kPageHeaderSize + static_cast<std::size_t>(slot_id) * kSlotSize;
}

struct Slot {
  std::uint16_t offset;
  std::uint16_t length;
  std::uint16_t flags;
};

Slot read_slot(const SlottedPage::Bytes& bytes, SlotId slot_id) {
  const auto offset = slot_offset(slot_id);
  return {read_u16(bytes, offset), read_u16(bytes, offset + 2), read_u16(bytes, offset + 4)};
}

void write_slot(SlottedPage::Bytes& bytes, SlotId slot_id, Slot slot) {
  const auto offset = slot_offset(slot_id);
  write_u16(bytes, offset, slot.offset);
  write_u16(bytes, offset + 2, slot.length);
  write_u16(bytes, offset + 4, slot.flags);
}

}  // namespace

SlottedPage SlottedPage::initialize(PageId page_id) {
  Bytes bytes{};
  write_u32(bytes, kMagicOffset, kMagic);
  write_u16(bytes, kVersionOffset, kVersion);
  write_u32(bytes, kPageIdOffset, page_id);
  write_u16(bytes, kSlotCountOffset, 0);
  write_u16(bytes, kFreeStartOffset, static_cast<std::uint16_t>(kPageHeaderSize));
  write_u16(bytes, kFreeEndOffset, static_cast<std::uint16_t>(kPageSize));
  SlottedPage page(std::move(bytes));
  page.update_checksum();
  return page;
}

SlottedPage SlottedPage::from_bytes(const Bytes& bytes) {
  SlottedPage page(bytes);
  page.validate();
  return page;
}

PageId SlottedPage::page_id() const { return read_u32(bytes_, kPageIdOffset); }

std::size_t SlottedPage::slot_count() const { return read_u16(bytes_, kSlotCountOffset); }

std::size_t SlottedPage::live_records() const {
  std::size_t count = 0;
  for (SlotId slot = 0; slot < slot_count(); ++slot) {
    if ((read_slot(bytes_, slot).flags & kLive) != 0) ++count;
  }
  return count;
}

std::size_t SlottedPage::free_space() const {
  return read_u16(bytes_, kFreeEndOffset) - read_u16(bytes_, kFreeStartOffset);
}

std::size_t SlottedPage::reclaimable_space() const {
  std::size_t used = 0;
  for (SlotId slot = 0; slot < slot_count(); ++slot) {
    const auto entry = read_slot(bytes_, slot);
    if ((entry.flags & kLive) != 0) used += entry.length;
  }
  return kPageSize - (kPageHeaderSize + slot_count() * kSlotSize) - used;
}

SlotId SlottedPage::insert(std::span<const std::byte> record) {
  if (record.empty()) throw std::invalid_argument("records must not be empty");
  if (record.size() > std::numeric_limits<std::uint16_t>::max()) throw PageFull("record is too large");

  SlotId target = static_cast<SlotId>(slot_count());
  bool reusing = false;
  for (SlotId slot = 0; slot < slot_count(); ++slot) {
    if ((read_slot(bytes_, slot).flags & kLive) == 0) {
      target = slot;
      reusing = true;
      break;
    }
  }
  const std::size_t directory_bytes = reusing ? 0 : kSlotSize;
  if (record.size() + directory_bytes > free_space()) compact();
  if (record.size() + directory_bytes > free_space()) throw PageFull("page has insufficient free space");

  const auto new_end = static_cast<std::uint16_t>(read_u16(bytes_, kFreeEndOffset) - record.size());
  std::copy(record.begin(), record.end(), bytes_.begin() + new_end);
  write_slot(bytes_, target, {new_end, static_cast<std::uint16_t>(record.size()), kLive});
  write_u16(bytes_, kFreeEndOffset, new_end);
  if (!reusing) {
    write_u16(bytes_, kSlotCountOffset, static_cast<std::uint16_t>(slot_count() + 1));
    write_u16(bytes_, kFreeStartOffset, static_cast<std::uint16_t>(kPageHeaderSize + slot_count() * kSlotSize));
  }
  update_checksum();
  return target;
}

std::vector<std::byte> SlottedPage::read(SlotId slot_id) const {
  if (slot_id >= slot_count()) throw std::out_of_range("slot does not exist");
  const auto slot = read_slot(bytes_, slot_id);
  if ((slot.flags & kLive) == 0) throw std::out_of_range("slot has been deleted");
  return {bytes_.begin() + slot.offset, bytes_.begin() + slot.offset + slot.length};
}

bool SlottedPage::erase(SlotId slot_id) {
  if (slot_id >= slot_count()) return false;
  const auto slot = read_slot(bytes_, slot_id);
  if ((slot.flags & kLive) == 0) return false;
  write_slot(bytes_, slot_id, {0, 0, 0});
  update_checksum();
  return true;
}

void SlottedPage::compact() {
  Bytes compacted{};
  std::copy(bytes_.begin(), bytes_.begin() + kPageHeaderSize, compacted.begin());
  std::uint16_t free_end = static_cast<std::uint16_t>(kPageSize);
  for (SlotId slot_id = 0; slot_id < slot_count(); ++slot_id) {
    const auto slot = read_slot(bytes_, slot_id);
    if ((slot.flags & kLive) == 0) {
      write_slot(compacted, slot_id, {0, 0, 0});
      continue;
    }
    free_end = static_cast<std::uint16_t>(free_end - slot.length);
    std::copy(bytes_.begin() + slot.offset, bytes_.begin() + slot.offset + slot.length,
              compacted.begin() + free_end);
    write_slot(compacted, slot_id, {free_end, slot.length, kLive});
  }
  write_u16(compacted, kFreeStartOffset, static_cast<std::uint16_t>(kPageHeaderSize + slot_count() * kSlotSize));
  write_u16(compacted, kFreeEndOffset, free_end);
  bytes_ = std::move(compacted);
  update_checksum();
}

void SlottedPage::validate() const {
  if (read_u32(bytes_, kMagicOffset) != kMagic) throw PageCorruption("invalid page magic");
  if (read_u16(bytes_, kVersionOffset) != kVersion) throw PageCorruption("unsupported page version");
  if (read_u32(bytes_, kChecksumOffset) != checksum(bytes_)) throw PageCorruption("page checksum mismatch");

  const auto slots = slot_count();
  const auto free_start = read_u16(bytes_, kFreeStartOffset);
  const auto free_end = read_u16(bytes_, kFreeEndOffset);
  if (free_start != kPageHeaderSize + slots * kSlotSize || free_start > free_end || free_end > kPageSize) {
    throw PageCorruption("invalid free-space boundaries");
  }
  std::array<bool, kPageSize> occupied{};
  for (SlotId slot_id = 0; slot_id < slots; ++slot_id) {
    const auto slot = read_slot(bytes_, slot_id);
    if ((slot.flags & kLive) == 0) continue;
    if (slot.length == 0 || slot.offset < free_end || static_cast<std::size_t>(slot.offset) + slot.length > kPageSize) {
      throw PageCorruption("slot points outside the record region");
    }
    for (std::size_t index = slot.offset; index < static_cast<std::size_t>(slot.offset) + slot.length; ++index) {
      if (occupied[index]) throw PageCorruption("record payloads overlap");
      occupied[index] = true;
    }
  }
}

void SlottedPage::update_checksum() { write_u32(bytes_, kChecksumOffset, checksum(bytes_)); }

}  // namespace pageforge
