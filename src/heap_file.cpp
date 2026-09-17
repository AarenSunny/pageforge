#include "pageforge/heap_file.hpp"

#include <array>
#include <cstring>
#include <stdexcept>

namespace pageforge {
namespace {

using Header = std::array<std::byte, kPageSize>;
constexpr std::array<char, 8> kMagic{'P', 'F', 'D', 'B', '0', '0', '0', '1'};
constexpr std::size_t kPageSizeOffset = 8;
constexpr std::size_t kPageCountOffset = 12;
constexpr std::size_t kChecksumOffset = 16;

std::uint32_t read_u32(const Header& bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(std::to_integer<unsigned>(bytes[offset + index])) << (index * 8U);
  }
  return value;
}

void write_u32(Header& bytes, std::size_t offset, std::uint32_t value) {
  for (std::size_t index = 0; index < 4; ++index) {
    bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
  }
}

std::uint32_t checksum(const Header& bytes) {
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

void require_stream(const std::fstream& stream, const char* message) {
  if (!stream) throw std::runtime_error(message);
}

}  // namespace

HeapFile HeapFile::create(const std::filesystem::path& path) {
  HeapFile file;
  file.path_ = path;
  file.stream_.open(path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
  require_stream(file.stream_, "could not create heap file");
  file.write_header();
  file.sync();
  return file;
}

HeapFile HeapFile::open(const std::filesystem::path& path) {
  HeapFile file;
  file.path_ = path;
  file.stream_.open(path, std::ios::binary | std::ios::in | std::ios::out);
  require_stream(file.stream_, "could not open heap file");
  file.read_header();
  return file;
}

HeapFile::~HeapFile() {
  if (stream_.is_open()) stream_.close();
}

SlottedPage HeapFile::allocate_page() {
  auto page = SlottedPage::initialize(page_count_);
  stream_.clear();
  stream_.seekp(page_offset(page_count_));
  stream_.write(reinterpret_cast<const char*>(page.bytes().data()), page.bytes().size());
  require_stream(stream_, "could not allocate heap page");
  ++page_count_;
  write_header();
  return page;
}

SlottedPage HeapFile::read_page(PageId page_id) {
  if (page_id >= page_count_) throw std::out_of_range("page id is outside the heap file");
  SlottedPage::Bytes bytes{};
  stream_.clear();
  stream_.seekg(page_offset(page_id));
  stream_.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
  require_stream(stream_, "could not read complete heap page");
  auto page = SlottedPage::from_bytes(bytes);
  if (page.page_id() != page_id) throw PageCorruption("page id does not match its heap position");
  return page;
}

void HeapFile::write_page(const SlottedPage& page) {
  if (page.page_id() >= page_count_) throw std::out_of_range("page id is outside the heap file");
  page.validate();
  stream_.clear();
  stream_.seekp(page_offset(page.page_id()));
  stream_.write(reinterpret_cast<const char*>(page.bytes().data()), page.bytes().size());
  require_stream(stream_, "could not write heap page");
}

void HeapFile::sync() {
  stream_.flush();
  require_stream(stream_, "could not flush heap file");
}

void HeapFile::read_header() {
  Header header{};
  stream_.seekg(0);
  stream_.read(reinterpret_cast<char*>(header.data()), header.size());
  require_stream(stream_, "could not read heap header");
  for (std::size_t index = 0; index < kMagic.size(); ++index) {
    if (std::to_integer<unsigned char>(header[index]) != static_cast<unsigned char>(kMagic[index])) {
      throw PageCorruption("invalid heap-file magic or version");
    }
  }
  if (read_u32(header, kPageSizeOffset) != kPageSize) throw PageCorruption("incompatible heap page size");
  if (read_u32(header, kChecksumOffset) != checksum(header)) throw PageCorruption("heap header checksum mismatch");
  page_count_ = read_u32(header, kPageCountOffset);

  const auto expected_size = static_cast<std::uintmax_t>(page_offset(page_count_));
  if (std::filesystem::file_size(path_) < expected_size) throw PageCorruption("heap file is truncated");
}

void HeapFile::write_header() {
  Header header{};
  for (std::size_t index = 0; index < kMagic.size(); ++index) header[index] = static_cast<std::byte>(kMagic[index]);
  write_u32(header, kPageSizeOffset, static_cast<std::uint32_t>(kPageSize));
  write_u32(header, kPageCountOffset, page_count_);
  write_u32(header, kChecksumOffset, checksum(header));
  stream_.clear();
  stream_.seekp(0);
  stream_.write(reinterpret_cast<const char*>(header.data()), header.size());
  require_stream(stream_, "could not write heap header");
}

std::streamoff HeapFile::page_offset(PageId page_id) const {
  return static_cast<std::streamoff>(kPageSize) * (static_cast<std::streamoff>(page_id) + 1);
}

}  // namespace pageforge
