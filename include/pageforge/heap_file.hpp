#pragma once

#include <filesystem>
#include <fstream>

#include "pageforge/page.hpp"

namespace pageforge {

class HeapFile {
 public:
  static HeapFile create(const std::filesystem::path& path);
  static HeapFile open(const std::filesystem::path& path);

  HeapFile(const HeapFile&) = delete;
  HeapFile& operator=(const HeapFile&) = delete;
  HeapFile(HeapFile&&) noexcept = default;
  HeapFile& operator=(HeapFile&&) noexcept = default;
  ~HeapFile();

  [[nodiscard]] std::uint32_t page_count() const noexcept { return page_count_; }
  SlottedPage allocate_page();
  [[nodiscard]] SlottedPage read_page(PageId page_id);
  void write_page(const SlottedPage& page);
  void sync();

 private:
  HeapFile() = default;
  void read_header();
  void write_header();
  [[nodiscard]] std::streamoff page_offset(PageId page_id) const;

  std::filesystem::path path_;
  std::fstream stream_;
  std::uint32_t page_count_ = 0;
};

}  // namespace pageforge
