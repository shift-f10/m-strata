#pragma once

// A .zip of stored entries as any archiver writes one - each local header and its bytes, the central directory, the end
// record (PKWARE's APPNOTE 4.3) - for the tests of adding a plugin from a zip (plugin spec 2026-10-30 section 12). The
// zip reader's own tests cover deflated entries and hostile archives.

#include "secure_frontend/zip_reader.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace secure_frontend_test {

struct ZipItem final {
  std::string name;   // a folder's ends with '/'
  std::string bytes;
};

inline void put16(std::string& out, const std::size_t value) {
  out += static_cast<char>(value & 0xFFU);
  out += static_cast<char>((value >> 8U) & 0xFFU);
}
inline void put32(std::string& out, const std::size_t value) {
  put16(out, value & 0xFFFFU);
  put16(out, (value >> 16U) & 0xFFFFU);
}

inline std::string make_stored_zip(const std::vector<ZipItem>& items) {
  std::string out;
  std::string directory;
  for (const auto& item : items) {
    secure_frontend::Crc32 crc;
    crc.update({reinterpret_cast<const std::uint8_t*>(item.bytes.data()), item.bytes.size()});
    const std::size_t offset = out.size();
    put32(out, 0x04034B50U);
    put16(out, 20U);    // version needed
    put16(out, 0U);     // flags
    put16(out, 0U);     // stored
    put16(out, 0U);     // time
    put16(out, 0x21U);  // 1980-01-01
    put32(out, crc.value());
    put32(out, item.bytes.size());
    put32(out, item.bytes.size());
    put16(out, item.name.size());
    put16(out, 0U);
    out += item.name;
    out += item.bytes;
    put32(directory, 0x02014B50U);
    put16(directory, 20U);  // made by: MS-DOS, 2.0
    put16(directory, 20U);
    put16(directory, 0U);
    put16(directory, 0U);
    put16(directory, 0U);
    put16(directory, 0x21U);
    put32(directory, crc.value());
    put32(directory, item.bytes.size());
    put32(directory, item.bytes.size());
    put16(directory, item.name.size());
    put16(directory, 0U);  // extra
    put16(directory, 0U);  // comment
    put16(directory, 0U);  // disk
    put16(directory, 0U);  // internal attributes
    put32(directory, 0U);  // external attributes
    put32(directory, offset);
    directory += item.name;
  }
  const std::size_t at = out.size();
  out += directory;
  put32(out, 0x06054B50U);
  put16(out, 0U);
  put16(out, 0U);
  put16(out, items.size());
  put16(out, items.size());
  put32(out, directory.size());
  put32(out, at);
  put16(out, 0U);
  return out;
}

}  // namespace secure_frontend_test
