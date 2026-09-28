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
constexpr std::array<std::byte, 4> kInternalMagic{
    std::byte{'P'}, std::byte{'F'}, std::byte{'I'}, std::byte{'N'}};
constexpr std::array<std::byte, 4> kLeafMagic{
    std::byte{'P'}, std::byte{'F'}, std::byte{'I'}, std::byte{'L'}};
constexpr std::uint16_t kFormatVersion = 2;
constexpr std::size_t kHeaderBytes = 24;
constexpr std::size_t kNodeHeaderBytes = 16;
constexpr std::size_t kLeafEntryBytes = 14;
constexpr std::size_t kInternalEntryBytes = 20;
constexpr std::size_t kLeafBytes = kNodeHeaderBytes + kBPlusLeafCapacity * kLeafEntryBytes;
constexpr std::size_t kInternalBytes =
    kNodeHeaderBytes + kBPlusInternalCapacity * kInternalEntryBytes;
constexpr PageId kNoPage = std::numeric_limits<PageId>::max();
constexpr SlotId kNoSlot = std::numeric_limits<SlotId>::max();

struct Header {
  std::size_t height;
  RecordId root;
  RecordId first_leaf;
};

struct Leaf {
  std::optional<RecordId> next;
  std::vector<IndexEntry> entries;
};

struct LoadedLeaf {
  RecordId id;
  Leaf leaf;
};

struct Child {
  IndexEntry high;
  RecordId id;
};

struct Internal {
  std::vector<Child> children;
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

std::vector<std::byte> read_record(RecordStore& records, RecordId id, std::string_view kind) {
  try {
    return records.read(id);
  } catch (const std::exception& error) {
    throw IndexCorruption("could not read index " + std::string(kind) + ": " + error.what());
  }
}

std::vector<std::byte> encode_header(const Header& header) {
  if (header.height != 1 && header.height != 2) {
    throw std::invalid_argument("index height must be one or two");
  }
  std::vector<std::byte> output(kHeaderBytes);
  std::copy(kHeaderMagic.begin(), kHeaderMagic.end(), output.begin());
  write_u16(output, 4, kFormatVersion);
  write_u16(output, 6, static_cast<std::uint16_t>(header.height));
  write_u32(output, 8, header.root.page_id);
  write_u16(output, 12, header.root.slot_id);
  write_u32(output, 16, header.first_leaf.page_id);
  write_u16(output, 20, header.first_leaf.slot_id);
  return output;
}

Header decode_header(std::span<const std::byte> input) {
  if (input.size() != kHeaderBytes || !has_magic(input, kHeaderMagic)) {
    throw IndexCorruption("invalid index header record");
  }
  if (read_u16(input, 4) != kFormatVersion) {
    throw IndexCorruption("unsupported index header version");
  }
  const auto height = read_u16(input, 6);
  if (height != 1 && height != 2) throw IndexCorruption("unsupported index height");
  if (read_u16(input, 14) != 0 || read_u16(input, 22) != 0) {
    throw IndexCorruption("index header has nonzero reserved fields");
  }
  return {height, {read_u32(input, 8), read_u16(input, 12)},
          {read_u32(input, 16), read_u16(input, 20)}};
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
    const auto offset = kNodeHeaderBytes + index * kLeafEntryBytes;
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
    const auto offset = kNodeHeaderBytes + index * kLeafEntryBytes;
    leaf.entries.push_back(
        {std::bit_cast<std::int64_t>(read_u64(input, offset)),
         {read_u32(input, offset + 8), read_u16(input, offset + 12)}});
    if (index != 0 && !entry_less(leaf.entries[index - 1], leaf.entries[index])) {
      throw IndexCorruption("index leaf entries are not strictly ordered");
    }
  }
  const auto used = kNodeHeaderBytes + static_cast<std::size_t>(count) * kLeafEntryBytes;
  for (std::size_t offset = used; offset < input.size(); ++offset) {
    if (input[offset] != std::byte{0}) throw IndexCorruption("index leaf padding is not zero");
  }
  return leaf;
}

Leaf load_leaf(RecordStore& records, RecordId id) {
  return decode_leaf(read_record(records, id, "leaf"));
}

std::vector<std::byte> encode_internal(const Internal& internal) {
  if (internal.children.size() < 2 || internal.children.size() > kBPlusInternalCapacity) {
    throw std::invalid_argument("index internal child count is outside its capacity");
  }
  for (std::size_t index = 1; index < internal.children.size(); ++index) {
    if (!entry_less(internal.children[index - 1].high, internal.children[index].high)) {
      throw std::invalid_argument("index internal separators must be strictly ordered");
    }
  }
  std::vector<std::byte> output(kInternalBytes);
  std::copy(kInternalMagic.begin(), kInternalMagic.end(), output.begin());
  write_u16(output, 4, kFormatVersion);
  write_u16(output, 8, static_cast<std::uint16_t>(internal.children.size()));
  for (std::size_t index = 0; index < internal.children.size(); ++index) {
    const auto offset = kNodeHeaderBytes + index * kInternalEntryBytes;
    write_u64(output, offset, std::bit_cast<std::uint64_t>(internal.children[index].high.key));
    write_u32(output, offset + 8, internal.children[index].high.value.page_id);
    write_u16(output, offset + 12, internal.children[index].high.value.slot_id);
    write_u32(output, offset + 14, internal.children[index].id.page_id);
    write_u16(output, offset + 18, internal.children[index].id.slot_id);
  }
  return output;
}

Internal decode_internal(std::span<const std::byte> input) {
  if (input.size() != kInternalBytes || !has_magic(input, kInternalMagic)) {
    throw IndexCorruption("invalid index internal record");
  }
  if (read_u16(input, 4) != kFormatVersion) {
    throw IndexCorruption("unsupported index internal version");
  }
  if (read_u16(input, 6) != 0 || read_u16(input, 10) != 0 || read_u32(input, 12) != 0) {
    throw IndexCorruption("index internal node has nonzero reserved fields");
  }
  const auto count = read_u16(input, 8);
  if (count < 2 || count > kBPlusInternalCapacity) {
    throw IndexCorruption("index internal child count is outside its capacity");
  }
  Internal internal;
  internal.children.reserve(count);
  std::unordered_set<std::uint64_t> child_ids;
  for (std::size_t index = 0; index < count; ++index) {
    const auto offset = kNodeHeaderBytes + index * kInternalEntryBytes;
    internal.children.push_back(
        {{std::bit_cast<std::int64_t>(read_u64(input, offset)),
          {read_u32(input, offset + 8), read_u16(input, offset + 12)}},
         {read_u32(input, offset + 14), read_u16(input, offset + 18)}});
    if (!child_ids.insert(record_key(internal.children.back().id)).second) {
      throw IndexCorruption("index internal node contains a duplicate child pointer");
    }
    if (index != 0 &&
        !entry_less(internal.children[index - 1].high, internal.children[index].high)) {
      throw IndexCorruption("index internal separators are not strictly ordered");
    }
  }
  const auto used = kNodeHeaderBytes + static_cast<std::size_t>(count) * kInternalEntryBytes;
  for (std::size_t offset = used; offset < input.size(); ++offset) {
    if (input[offset] != std::byte{0}) {
      throw IndexCorruption("index internal padding is not zero");
    }
  }
  return internal;
}

Internal load_internal(RecordStore& records, RecordId id) {
  return decode_internal(read_record(records, id, "internal node"));
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
    auto leaf = load_leaf(records, *current);
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

std::vector<LoadedLeaf> validate_tree(RecordStore& records, const Header& header) {
  auto leaves = load_chain(records, header.first_leaf);
  if (header.height == 1) {
    if (header.root != header.first_leaf || leaves.size() != 1) {
      throw IndexCorruption("height-one index must contain exactly its root leaf");
    }
    return leaves;
  }

  const auto internal = load_internal(records, header.root);
  if (internal.children.size() != leaves.size()) {
    throw IndexCorruption("internal root child count does not match the leaf chain");
  }
  for (std::size_t index = 0; index < leaves.size(); ++index) {
    if (leaves[index].leaf.entries.empty() || internal.children[index].id != leaves[index].id ||
        internal.children[index].high != leaves[index].leaf.entries.back()) {
      throw IndexCorruption("internal root separators do not match leaf boundaries");
    }
  }
  return leaves;
}

std::size_t route_entry(const Internal& internal, const IndexEntry& entry) {
  const auto position = std::lower_bound(
      internal.children.begin(), internal.children.end(), entry,
      [](const Child& child, const IndexEntry& sought) { return entry_less(child.high, sought); });
  return position == internal.children.end()
             ? internal.children.size() - 1
             : static_cast<std::size_t>(position - internal.children.begin());
}

std::size_t route_key(const Internal& internal, std::int64_t key) {
  const auto position = std::lower_bound(
      internal.children.begin(), internal.children.end(), key,
      [](const Child& child, std::int64_t sought) { return child.high.key < sought; });
  return position == internal.children.end()
             ? internal.children.size() - 1
             : static_cast<std::size_t>(position - internal.children.begin());
}

template <typename Visitor>
void visit_from(RecordStore& records, RecordId start, Visitor visitor) {
  std::unordered_set<std::uint64_t> visited;
  auto current = std::optional<RecordId>{start};
  while (current) {
    if (!visited.insert(record_key(*current)).second) {
      throw IndexCorruption("index leaf chain contains a cycle");
    }
    auto leaf = load_leaf(records, *current);
    if (!visitor(leaf)) return;
    current = leaf.next;
  }
}

}  // namespace

BPlusTreeIndex BPlusTreeIndex::create(RecordStore& records) {
  const auto leaf_id = records.insert(encode_leaf({}));
  const Header header{1, leaf_id, leaf_id};
  const auto header_id = records.insert(encode_header(header));
  return BPlusTreeIndex(records, header_id, leaf_id, leaf_id, 1);
}

BPlusTreeIndex BPlusTreeIndex::open(RecordStore& records, RecordId header_id) {
  Header header;
  try {
    header = decode_header(read_record(records, header_id, "header"));
    (void)validate_tree(records, header);
  } catch (const IndexCorruption&) {
    throw;
  } catch (const std::exception& error) {
    throw IndexCorruption(std::string("could not open index: ") + error.what());
  }
  return BPlusTreeIndex(records, header_id, header.root, header.first_leaf, header.height);
}

std::size_t BPlusTreeIndex::leaf_count() {
  return validate_tree(records_, {height_, root_, first_leaf_}).size();
}

bool BPlusTreeIndex::insert(std::int64_t key, RecordId value) {
  const IndexEntry entry{key, value};
  std::optional<Internal> internal;
  std::size_t child_index = 0;
  RecordId target_id = root_;
  if (height_ == 2) {
    internal = load_internal(records_, root_);
    child_index = route_entry(*internal, entry);
    target_id = internal->children[child_index].id;
  }
  auto leaf = load_leaf(records_, target_id);
  auto position = std::lower_bound(leaf.entries.begin(), leaf.entries.end(), entry, entry_less);
  if (position != leaf.entries.end() && *position == entry) return false;
  leaf.entries.insert(position, entry);

  if (leaf.entries.size() <= kBPlusLeafCapacity) {
    records_.replace(target_id, encode_leaf(leaf));
    if (internal) {
      internal->children[child_index].high = leaf.entries.back();
      records_.replace(root_, encode_internal(*internal));
    }
    return true;
  }

  if (internal && internal->children.size() == kBPlusInternalCapacity) {
    throw PageFull("B+ tree root is full; height-three routing is not implemented");
  }
  const auto split = leaf.entries.size() / 2;
  Leaf right;
  right.next = leaf.next;
  right.entries.assign(leaf.entries.begin() + static_cast<std::ptrdiff_t>(split),
                       leaf.entries.end());
  leaf.entries.erase(leaf.entries.begin() + static_cast<std::ptrdiff_t>(split),
                     leaf.entries.end());
  const auto right_id = records_.insert(encode_leaf(right));
  leaf.next = right_id;
  records_.replace(target_id, encode_leaf(leaf));

  if (!internal) {
    Internal new_root{{{leaf.entries.back(), target_id}, {right.entries.back(), right_id}}};
    const auto root_id = records_.insert(encode_internal(new_root));
    records_.replace(header_id_, encode_header({2, root_id, first_leaf_}));
    root_ = root_id;
    height_ = 2;
  } else {
    internal->children[child_index].high = leaf.entries.back();
    internal->children.insert(internal->children.begin() +
                                  static_cast<std::ptrdiff_t>(child_index + 1),
                              {right.entries.back(), right_id});
    records_.replace(root_, encode_internal(*internal));
  }
  return true;
}

std::vector<RecordId> BPlusTreeIndex::find(std::int64_t key) {
  RecordId start = root_;
  if (height_ == 2) {
    const auto internal = load_internal(records_, root_);
    start = internal.children[route_key(internal, key)].id;
  }
  std::vector<RecordId> result;
  visit_from(records_, start, [&](const Leaf& leaf) {
    for (const auto& entry : leaf.entries) {
      if (entry.key == key) result.push_back(entry.value);
    }
    return leaf.entries.empty() || leaf.entries.back().key <= key;
  });
  return result;
}

std::vector<IndexEntry> BPlusTreeIndex::range(std::optional<std::int64_t> lower,
                                               std::optional<std::int64_t> upper) {
  if (lower && upper && *lower > *upper) {
    throw std::invalid_argument("index range lower bound exceeds upper bound");
  }
  RecordId start = first_leaf_;
  if (lower && height_ == 2) {
    const auto internal = load_internal(records_, root_);
    start = internal.children[route_key(internal, *lower)].id;
  }
  std::vector<IndexEntry> result;
  visit_from(records_, start, [&](const Leaf& leaf) {
    for (const auto& entry : leaf.entries) {
      if (lower && entry.key < *lower) continue;
      if (upper && entry.key > *upper) return false;
      result.push_back(entry);
    }
    return true;
  });
  return result;
}

}  // namespace pageforge
