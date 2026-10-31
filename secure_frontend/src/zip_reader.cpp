// A plugin's .zip read from memory (plugin spec 2026-10-30 section 12), from PKWARE's APPNOTE (4.3: local file headers,
// the central directory, the end of central directory record). Three passes, nothing extracted before all three hold:
// the directory's records and their names; no two entries' bytes overlapping (from the directory alone); each local
// header agreeing with its record.
#include "secure_frontend/zip_reader.hpp"

#include <algorithm>
#include <array>
#include <set>
#include <string_view>
#include <utility>

namespace secure_frontend {
namespace {

constexpr std::uint32_t kEndSignature = 0x06054B50U;
constexpr std::uint32_t kCentralSignature = 0x02014B50U;
constexpr std::uint32_t kLocalSignature = 0x04034B50U;
constexpr std::size_t kEndSize = 22U;
constexpr std::size_t kCentralSize = 46U;
constexpr std::size_t kLocalSize = 30U;
constexpr std::size_t kMaxName = 255U;
constexpr std::size_t kChunk = 65536U;

constexpr std::array<std::uint32_t, 256> kCrcTable = [] {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t n = 0; n < 256U; ++n) {
    std::uint32_t c = n;
    for (int k = 0; k < 8; ++k) c = (c & 1U) != 0U ? 0xEDB88320U ^ (c >> 1U) : c >> 1U;
    table[n] = c;
  }
  return table;
}();

std::uint16_t u16(const std::span<const std::uint8_t> b, const std::size_t at) {
  return static_cast<std::uint16_t>(b[at] | (b[at + 1U] << 8U));
}
std::uint32_t u32(const std::span<const std::uint8_t> b, const std::size_t at) {
  return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1U]) << 8U) |
         (static_cast<std::uint32_t>(b[at + 2U]) << 16U) | (static_cast<std::uint32_t>(b[at + 3U]) << 24U);
}

const std::string kNotZip = "this is not a zip archive (no end record)";
const std::string kDamaged = "this is not a zip archive (its directory is damaged)";
const std::string kZip64 = "ZIP64 archives are not taken";

// A plain relative path: not empty, not absolute, no "." or ".." or empty part, no drive, backslash or control character.
// A folder's name ends with '/'.
bool plain_name(const std::string_view name) {
  if (name.empty() || name.size() > kMaxName || name.front() == '/') return false;
  for (const char c : name) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20U || u == 0x7FU || c == '\\' || c == ':') return false;
  }
  const std::string_view body = name.back() == '/' ? name.substr(0, name.size() - 1U) : name;
  std::size_t start = 0;
  while (start <= body.size()) {
    const auto slash = body.find('/', start);
    const auto part = body.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
    if (part.empty() || part == "." || part == "..") return false;
    if (slash == std::string_view::npos) break;
    start = slash + 1U;
  }
  return true;
}

// The extra field: well formed (each block within it), and no ZIP64 block (0x0001). nullopt: fine.
std::optional<std::string> extra_problem(const std::span<const std::uint8_t> extra) {
  std::size_t at = 0;
  while (at < extra.size()) {
    if (extra.size() - at < 4U) return kDamaged;
    const std::uint16_t id = u16(extra, at);
    const std::uint16_t size = u16(extra, at + 2U);
    if (id == 0x0001U) return kZip64;
    if (extra.size() - at - 4U < size) return kDamaged;
    at += 4U + size;
  }
  return std::nullopt;
}

}  // namespace

void Crc32::update(const std::span<const std::uint8_t> bytes) noexcept {
  for (const auto b : bytes) state_ = kCrcTable[(state_ ^ b) & 0xFFU] ^ (state_ >> 8U);
}

std::variant<ZipArchive, std::string> read_zip(const std::span<const std::uint8_t> bytes) {
  if (bytes.size() < kEndSize) return kNotZip;
  // the end record: the last signature within the last 64 KiB and 22 bytes whose comment reaches exactly to the end
  std::size_t end = bytes.size();
  const std::size_t lowest = bytes.size() > kEndSize + 65535U ? bytes.size() - kEndSize - 65535U : 0U;
  for (std::size_t at = bytes.size() - kEndSize + 1U; at-- > lowest;) {
    if (u32(bytes, at) == kEndSignature && at + kEndSize + u16(bytes, at + 20U) == bytes.size()) {
      end = at;
      break;
    }
  }
  if (end == bytes.size()) return kNotZip;
  const std::uint16_t disk = u16(bytes, end + 4U);
  const std::uint16_t directory_disk = u16(bytes, end + 6U);
  const std::uint16_t on_disk = u16(bytes, end + 8U);
  const std::uint16_t total = u16(bytes, end + 10U);
  const std::uint32_t directory_size = u32(bytes, end + 12U);
  const std::uint32_t directory = u32(bytes, end + 16U);
  if (on_disk == 0xFFFFU || total == 0xFFFFU || directory_size == 0xFFFFFFFFU || directory == 0xFFFFFFFFU) return kZip64;
  if (disk != 0U || directory_disk != 0U || on_disk != total) return std::string{"split archives are not taken"};
  if (total > kMaxZipEntries) return "the archive has more than " + std::to_string(kMaxZipEntries) + " entries";
  if (directory > end || directory_size > end - directory) return kDamaged;

  // 1. the directory's records
  struct Record final {
    ZipEntry entry;
    std::size_t local{0};
  };
  std::vector<Record> records;
  std::set<std::string, std::less<>> names;
  std::uint64_t all = 0;
  const std::size_t directory_end = directory + directory_size;
  std::size_t at = directory;
  for (std::uint16_t i = 0; i < total; ++i) {
    if (directory_end - at < kCentralSize || u32(bytes, at) != kCentralSignature) return kDamaged;
    const std::uint16_t made_by = u16(bytes, at + 4U);
    const std::uint16_t flags = u16(bytes, at + 8U);
    const std::uint16_t method = u16(bytes, at + 10U);
    const std::uint32_t crc = u32(bytes, at + 16U);
    const std::uint32_t compressed = u32(bytes, at + 20U);
    const std::uint32_t size = u32(bytes, at + 24U);
    const std::size_t name_length = u16(bytes, at + 28U);
    const std::size_t extra_length = u16(bytes, at + 30U);
    const std::size_t comment_length = u16(bytes, at + 32U);
    const std::uint16_t start_disk = u16(bytes, at + 34U);
    const std::uint32_t external = u32(bytes, at + 38U);
    const std::uint32_t local = u32(bytes, at + 42U);
    if (directory_end - at - kCentralSize < name_length + extra_length + comment_length) return kDamaged;
    const std::string name(reinterpret_cast<const char*>(bytes.data() + at + kCentralSize), name_length);
    if ((flags & 0x0041U) != 0U) return std::string{"encrypted entries are not taken"};
    if (compressed == 0xFFFFFFFFU || size == 0xFFFFFFFFU || local == 0xFFFFFFFFU || start_disk == 0xFFFFU) return kZip64;
    if (auto problem = extra_problem(bytes.subspan(at + kCentralSize + name_length, extra_length))) return *problem;
    if (method != 0U && method != 8U) {
      return std::string{"an entry uses a compression this server does not read (only stored and deflated)"};
    }
    if (!plain_name(name)) {
      return std::string{"an entry's name is not a plain relative path (it is empty, absolute, or has \".\", \"..\", a drive, a "
                         "backslash or a control character)"};
    }
    // a link, as Unix tools record one (the host in the high byte of "made by", the file's mode in the high 16 bits)
    if ((made_by >> 8U) == 3U && ((external >> 16U) & 0170000U) == 0120000U) return std::string{"the archive has a link, which is not taken"};
    if (!names.insert(name).second) return std::string{"the archive names an entry twice"};
    if (name.back() == '/' && size != 0U) return kDamaged;
    if (method == 0U && compressed != size) return kDamaged;
    if (method == 8U && size > std::uint64_t{compressed} * kMaxZipRatio) {
      return "an entry claims to inflate to more than " + std::to_string(kMaxZipRatio) + " times its compressed size";
    }
    all += size;
    if (all > kMaxZipBytes) return std::string{"the archive's files come to more than 1 GiB"};
    records.push_back(Record{ZipEntry{.name = name, .method = method, .crc32 = crc, .compressed = compressed, .size = size, .data = 0U}, local});
    at += kCentralSize + name_length + extra_length + comment_length;
  }

  // 2. no two entries' bytes overlap: from the directory alone (a header and its name, then its bytes), each before the next
  std::vector<std::pair<std::size_t, std::size_t>> spans;
  for (const auto& r : records) {
    spans.emplace_back(r.local, r.local + kLocalSize + r.entry.name.size() + static_cast<std::size_t>(r.entry.compressed));
  }
  std::sort(spans.begin(), spans.end());
  for (std::size_t i = 1; i < spans.size(); ++i) {
    if (spans[i].first < spans[i - 1U].second) return std::string{"two entries overlap in the archive"};
  }

  // 3. each local header where the directory says, agreeing with it, its bytes before the directory
  ZipArchive archive;
  for (auto& r : records) {
    if (r.local > directory || directory - r.local < kLocalSize || u32(bytes, r.local) != kLocalSignature) {
      return std::string{"an entry's local header is not where the directory says"};
    }
    const std::uint16_t flags = u16(bytes, r.local + 6U);
    const std::uint16_t method = u16(bytes, r.local + 8U);
    const std::size_t name_length = u16(bytes, r.local + 26U);
    const std::size_t extra_length = u16(bytes, r.local + 28U);
    if (directory - r.local - kLocalSize < name_length + extra_length) return kDamaged;
    const std::string_view local_name(reinterpret_cast<const char*>(bytes.data() + r.local + kLocalSize), name_length);
    if (local_name != r.entry.name || method != r.entry.method || (flags & 0x0041U) != 0U) {
      return std::string{"an entry's local header disagrees with the directory"};
    }
    if (auto problem = extra_problem(bytes.subspan(r.local + kLocalSize + name_length, extra_length))) return *problem;
    r.entry.data = r.local + kLocalSize + name_length + extra_length;
    if (r.entry.data > directory || directory - r.entry.data < r.entry.compressed) {
      return std::string{"an entry's bytes run past the archive's directory"};
    }
    archive.entries.push_back(std::move(r.entry));
  }
  return archive;
}

std::optional<std::string> extract_zip_entry(const std::span<const std::uint8_t> bytes, const ZipEntry& entry, const InflateSink& sink) {
  if (entry.data > bytes.size() || bytes.size() - entry.data < entry.compressed) return std::string{"the entry's bytes are not in the archive"};
  const auto input = bytes.subspan(entry.data, static_cast<std::size_t>(entry.compressed));
  Crc32 crc;
  std::uint64_t written = 0;
  const auto pass = [&](const std::span<const std::uint8_t> chunk) {
    crc.update(chunk);
    written += chunk.size();
    return sink(chunk);
  };
  if (entry.method == 0U) {
    for (std::size_t at = 0; at < input.size(); at += kChunk) {
      if (!pass(input.subspan(at, std::min(kChunk, input.size() - at)))) return std::string{"the entry's bytes were not taken"};
    }
  } else {
    const auto result = inflate_raw(input, entry.size, pass);
    if (const auto* error = std::get_if<InflateError>(&result)) {
      if (*error == InflateError::sink_refused) return std::string{"the entry's bytes were not taken"};
      if (*error == InflateError::too_large) return std::string{"the entry inflates to more than the archive says"};
      return "the entry does not inflate: " + std::string{inflate_error_text(*error)};
    }
  }
  if (written != entry.size) return std::string{"the entry's size is not the one the archive gives"};
  if (crc.value() != entry.crc32) return std::string{"the entry's CRC-32 is not the one the archive gives"};
  return std::nullopt;
}

}  // namespace secure_frontend
