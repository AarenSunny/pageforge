#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "pageforge/record_store.hpp"

namespace pageforge {

inline constexpr std::size_t kBPlusLeafCapacity = 128;
inline constexpr std::size_t kBPlusInternalCapacity = 128;

struct IndexEntry {
  std::int64_t key;
  RecordId value;

  bool operator==(const IndexEntry&) const = default;
};

class IndexCorruption final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class BPlusTreeIndex {
 public:
  [[nodiscard]] static BPlusTreeIndex create(RecordStore& records);
  [[nodiscard]] static BPlusTreeIndex open(RecordStore& records, RecordId header_id);

  BPlusTreeIndex(const BPlusTreeIndex&) = delete;
  BPlusTreeIndex& operator=(const BPlusTreeIndex&) = delete;
  BPlusTreeIndex(BPlusTreeIndex&&) noexcept = default;

  [[nodiscard]] RecordId header_id() const noexcept { return header_id_; }
  [[nodiscard]] RecordId root_id() const noexcept { return root_; }
  [[nodiscard]] std::size_t height() const noexcept { return height_; }
  [[nodiscard]] std::size_t leaf_count();
  bool insert(std::int64_t key, RecordId value);
  bool erase(std::int64_t key, RecordId value);
  [[nodiscard]] std::vector<RecordId> find(std::int64_t key);
  [[nodiscard]] std::vector<IndexEntry> range(std::optional<std::int64_t> lower,
                                              std::optional<std::int64_t> upper);

 private:
  BPlusTreeIndex(RecordStore& records, RecordId header_id, RecordId root, RecordId first_leaf,
                 std::size_t height)
      : records_(records), header_id_(header_id), root_(root), first_leaf_(first_leaf),
        height_(height) {}

  RecordStore& records_;
  RecordId header_id_;
  RecordId root_;
  RecordId first_leaf_;
  std::size_t height_;
};

}  // namespace pageforge
