#include "pageforge/bplus_tree.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <span>
#include <string>
#include <unordered_set>

namespace pageforge {
namespace {

constexpr std::array<std::byte, 4> kHeaderMagic{
    std::byte{'P'}, std::byte{'F'}, std::byte{'I'}, std::byte{'H'}};
constexpr std::array<std::byte, 4> kLeafMagic{
    std::byte{'P'}, std::byte{'F'}, std::byte{'I'}, std::byte{'L'}};
constexpr std::uint16_t kFormatVersion = 1;
constexpr std::size_t kNodeHeaderBytes = 16;
constexpr std::size_t kEntryBytes = 14;
constexpr std::size_t kLeafBytes = kNodeHeaderBytes + kBPlusLeafCapacity * kEntryBytes;
constexpr PageId kNoPage = std::numeric_limits<PageId>::max();
constexpr SlotId kNoSlot = std::numeric_limits<SlotId>::max();

struct Leaf {
  std::optional<RecordId> next;
  std::vector<IndexEntry> entries;
};

struct LoadedLeaf {
  RecordId id;
  Leaf leaf;
};

void write_u16(std::vector<std::byte>& output, std::size_t offset, std::uint16_t value) {
  output[offset] = static_cast<std::byte>(value & 0xffU);
  output[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xffU);
}

void write_u32(std::vector<std::byte>& output, std::size_t offset, std::uint32_t value) {
  for (std::size_t index = 0; index < 4; ++index) {
    output[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
  }
}

void write_u64(std::vector<std::byte>& output, std::size_t offset, std::uint64_t value) {
  for (std::size_t index = 0; index < 8; ++index) {
    output[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
  }
}

std::uint16_t read_u16(std::span<const std::byte> input, std::size_t offset) {
  return static_cast<std::uint16_t>(std::to_integer<unsigned>(input[offset])) |
         static_cast<std::uint16_t>(std::to_integer<unsigned>(input[offset + 1]) << 8U);
}

std::uint32_t read_u32(std::span<const std::byte> input, std::size_t offset) {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(std::to_integer<unsigned>(input[offset + index]))
             << (index * 8U);
  }
  return value;
}

std::uint64_t read_u64(std::span<const std::byte> input, std::size_t offset) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(std::to_integer<unsigned>(input[offset + index]))
             << (index * 8U);
  }
  return value;
}

bool has_magic(std::span<const std::byte> input, const std::array<std::byte, 4>& magic) {
  return input.size() >= magic.size() && std::equal(magic.begin(), magic.end(), input.begin());
}

bool entry_less(const IndexEntry& left, const IndexEntry& right) {
  if (left.key != right.key) return left.key < right.key;
  if (left.value.page_id != right.value.page_id) return left.value.page_id < right.value.page_id;
  return left.value.slot_id < right.value.slot_id;
}

std::uint64_t record_key(RecordId id) {
  return (static_cast<std::uint64_t>(id.page_id) << 16U) | id.slot_id;
}

std::vector<std::byte> encode_header(RecordId first_leaf) {
  std::vector<std::byte> output(kNodeHeaderBytes);
  std::copy(kHeaderMagic.begin(), kHeaderMagic.end(), output.begin());
  write_u16(output, 4, kFormatVersion);
  write_u32(output, 8, first_leaf.page_id);
  write_u16(output, 12, first_leaf.slot_id);
  return output;
}

RecordId decode_header(std::span<const std::byte> input) {
  if (input.size() != kNodeHeaderBytes || !has_magic(input, kHeaderMagic)) {
    throw IndexCorruption("invalid index header record");
  }
  if (read_u16(input, 4) != kFormatVersion) {
    throw IndexCorruption("unsupported index header version");
  }
  if (read_u16(input, 6) != 0 || read_u16(input, 14) != 0) {
    throw IndexCorruption("index header has nonzero reserved fields");
  }
  return {read_u32(input, 8), read_u16(input, 12)};
}

std::vector<std::byte> encode_leaf(const Leaf& leaf) {
  if (leaf.entries.size() > kBPlusLeafCapacity) {
    throw std::invalid_argument("index leaf exceeds its fixed capacity");
  }
  for (std::size_t index = 1; index < leaf.entries.size(); ++index) {
    if (!entry_less(leaf.entries[index - 1], leaf.entries[index])) {
      throw std::invalid_argument("index leaf entries must be strictly ordered");
    }
  }

  std::vector<std::byte> output(kLeafBytes);
  std::copy(kLeafMagic.begin(), kLeafMagic.end(), output.begin());
  write_u16(output, 4, kFormatVersion);
  write_u32(output, 8, leaf.next ? leaf.next->page_id : kNoPage);
  write_u16(output, 12, leaf.next ? leaf.next->slot_id : kNoSlot);
  write_u16(output, 14, static_cast<std::uint16_t>(leaf.entries.size()));
  for (std::size_t index = 0; index < leaf.entries.size(); ++index) {
    const auto offset = kNodeHeaderBytes + index * kEntryBytes;
    write_u64(output, offset, std::bit_cast<std::uint64_t>(leaf.entries[index].key));
    write_u32(output, offset + 8, leaf.entries[index].value.page_id);
    write_u16(output, offset + 12, leaf.entries[index].value.slot_id);
  }
  return output;
}

Leaf decode_leaf(std::span<const std::byte> input) {
  if (input.size() != kLeafBytes || !has_magic(input, kLeafMagic)) {
    throw IndexCorruption("invalid index leaf record");
  }
  if (read_u16(input, 4) != kFormatVersion) {
    throw IndexCorruption("unsupported index leaf version");
  }
  if (read_u16(input, 6) != 0) throw IndexCorruption("index leaf has nonzero reserved flags");
  const auto count = read_u16(input, 14);
  if (count > kBPlusLeafCapacity) throw IndexCorruption("index leaf entry count exceeds capacity");

  Leaf leaf;
  const auto next_page = read_u32(input, 8);
  const auto next_slot = read_u16(input, 12);
  if ((next_page == kNoPage) != (next_slot == kNoSlot)) {
    throw IndexCorruption("index leaf has an invalid next pointer");
  }
  if (next_page != kNoPage) leaf.next = RecordId{next_page, next_slot};
  leaf.entries.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    const auto offset = kNodeHeaderBytes + index * kEntryBytes;
    leaf.entries.push_back(
        {std::bit_cast<std::int64_t>(read_u64(input, offset)),
         {read_u32(input, offset + 8), read_u16(input, offset + 12)}});
    if (index != 0 && !entry_less(leaf.entries[index - 1], leaf.entries[index])) {
      throw IndexCorruption("index leaf entries are not strictly ordered");
    }
  }
  const auto used = kNodeHeaderBytes + static_cast<std::size_t>(count) * kEntryBytes;
  for (std::size_t offset = used; offset < input.size(); ++offset) {
    if (input[offset] != std::byte{0}) throw IndexCorruption("index leaf padding is not zero");
  }
  return leaf;
}

std::vector<LoadedLeaf> load_chain(RecordStore& records, RecordId first_leaf) {
  std::vector<LoadedLeaf> leaves;
  std::unordered_set<std::uint64_t> visited;
  std::optional<IndexEntry> previous;
  auto current = std::optional<RecordId>{first_leaf};
  while (current) {
    if (!visited.insert(record_key(*current)).second) {
      throw IndexCorruption("index leaf chain contains a cycle");
    }
    Leaf leaf;
    try {
      leaf = decode_leaf(records.read(*current));
    } catch (const IndexCorruption&) {
      throw;
    } catch (const std::exception& error) {
      throw IndexCorruption(std::string("could not read index leaf: ") + error.what());
    }
    if (leaf.entries.empty() && (!leaves.empty() || leaf.next)) {
      throw IndexCorruption("only a single root leaf may be empty");
    }
    if (previous && !leaf.entries.empty() && !entry_less(*previous, leaf.entries.front())) {
      throw IndexCorruption("index leaf chain is not globally ordered");
    }
    if (!leaf.entries.empty()) previous = leaf.entries.back();
    const auto next = leaf.next;
    leaves.push_back({*current, std::move(leaf)});
    current = next;
  }
  if (leaves.empty()) throw IndexCorruption("index has no leaf nodes");
  return leaves;
}

}  // namespace

BPlusTreeIndex BPlusTreeIndex::create(RecordStore& records) {
  const auto leaf_id = records.insert(encode_leaf({}));
  const auto header_id = records.insert(encode_header(leaf_id));
  return BPlusTreeIndex(records, header_id, leaf_id);
}

BPlusTreeIndex BPlusTreeIndex::open(RecordStore& records, RecordId header_id) {
  RecordId first_leaf;
  try {
    first_leaf = decode_header(records.read(header_id));
  } catch (const IndexCorruption&) {
    throw;
  } catch (const std::exception& error) {
    throw IndexCorruption(std::string("could not read index header: ") + error.what());
  }
  (void)load_chain(records, first_leaf);
  return BPlusTreeIndex(records, header_id, first_leaf);
}

std::size_t BPlusTreeIndex::leaf_count() { return load_chain(records_, first_leaf_).size(); }

bool BPlusTreeIndex::insert(std::int64_t key, RecordId value) {
  const IndexEntry entry{key, value};
  auto leaves = load_chain(records_, first_leaf_);
  auto target = leaves.end() - 1;
  for (auto current = leaves.begin(); current != leaves.end(); ++current) {
    if (current->leaf.entries.empty() || !entry_less(current->leaf.entries.back(), entry)) {
      target = current;
      break;
    }
  }

  auto position = std::lower_bound(target->leaf.entries.begin(), target->leaf.entries.end(), entry,
                                   entry_less);
  if (position != target->leaf.entries.end() && *position == entry) return false;
  target->leaf.entries.insert(position, entry);
  if (target->leaf.entries.size() <= kBPlusLeafCapacity) {
    records_.replace(target->id, encode_leaf(target->leaf));
    return true;
  }

  const auto split = target->leaf.entries.size() / 2;
  Leaf right;
  right.next = target->leaf.next;
  right.entries.assign(target->leaf.entries.begin() + static_cast<std::ptrdiff_t>(split),
                       target->leaf.entries.end());
  target->leaf.entries.erase(target->leaf.entries.begin() + static_cast<std::ptrdiff_t>(split),
                             target->leaf.entries.end());
  const auto right_id = records_.insert(encode_leaf(right));
  target->leaf.next = right_id;
  records_.replace(target->id, encode_leaf(target->leaf));
  return true;
}

std::vector<RecordId> BPlusTreeIndex::find(std::int64_t key) {
  std::vector<RecordId> result;
  for (const auto& loaded : load_chain(records_, first_leaf_)) {
    for (const auto& entry : loaded.leaf.entries) {
      if (entry.key == key) result.push_back(entry.value);
    }
    if (!loaded.leaf.entries.empty() && loaded.leaf.entries.back().key > key) break;
  }
  return result;
}

std::vector<IndexEntry> BPlusTreeIndex::range(std::optional<std::int64_t> lower,
                                               std::optional<std::int64_t> upper) {
  if (lower && upper && *lower > *upper) {
    throw std::invalid_argument("index range lower bound exceeds upper bound");
  }
  std::vector<IndexEntry> result;
  for (const auto& loaded : load_chain(records_, first_leaf_)) {
    for (const auto& entry : loaded.leaf.entries) {
      if (lower && entry.key < *lower) continue;
      if (upper && entry.key > *upper) return result;
      result.push_back(entry);
    }
  }
  return result;
}

}  // namespace pageforge
