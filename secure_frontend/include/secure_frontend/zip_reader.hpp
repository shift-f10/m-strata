#pragma once

// A plugin's .zip, read from memory (plugin spec 2026-10-30 section 12) - code written here, no archive library. The
// end record and the central directory are read and every entry checked before any is extracted: no ZIP64, no
// encryption, no links, no name that is empty, absolute, has "." or "..", a drive, a backslash or a control character,
// none twice, at most kMaxZipEntries, each entry's local header agreeing with the directory, no two entries' bytes
// overlapping, entries stored or deflated, none claiming more than kMaxZipRatio times its compressed size, at most
// kMaxZipBytes in all. Extracting streams an entry into a sink (inflate.hpp) and checks its size and CRC-32 at the end.

#include "secure_frontend/inflate.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace secure_frontend {

inline constexpr std::size_t kMaxZipEntries = 256;
inline constexpr std::uint64_t kMaxZipRatio = 1000;                 // DEFLATE itself stays under about 1032
inline constexpr std::uint64_t kMaxZipBytes = std::uint64_t{1} << 30U;  // all entries' sizes together: 1 GiB

struct ZipEntry final {
  std::string name;  // '/'-separated, checked; a folder's ends with '/'
  std::uint16_t method{0};  // 0 stored, 8 deflated
  std::uint32_t crc32{0};
  std::uint64_t compressed{0};
  std::uint64_t size{0};
  std::size_t data{0};  // where its bytes start in the archive
};

struct ZipArchive final {
  std::vector<ZipEntry> entries;  // in the directory's order, folders included
};

// The archive's entries, or why it is refused (a sentence for the page).
[[nodiscard]] std::variant<ZipArchive, std::string> read_zip(std::span<const std::uint8_t> bytes);

// One entry's bytes into `sink`, inflated or copied: at most its size, its size and CRC-32 checked at the end. nullopt:
// done; else why not.
[[nodiscard]] std::optional<std::string> extract_zip_entry(std::span<const std::uint8_t> bytes, const ZipEntry& entry,
                                                           const InflateSink& sink);

// CRC-32 as zip computes it (IEEE 802.3, reflected, 0xEDB88320), streaming.
class Crc32 final {
 public:
  void update(std::span<const std::uint8_t> bytes) noexcept;
  [[nodiscard]] std::uint32_t value() const noexcept { return ~state_; }

 private:
  std::uint32_t state_{0xFFFFFFFFU};
};

}  // namespace secure_frontend
