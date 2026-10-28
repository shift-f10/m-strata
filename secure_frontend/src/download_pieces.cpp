#include "secure_frontend/download_pieces.hpp"

#include <algorithm>
#include <array>

namespace secure_frontend {
namespace {

// The sidecar line (owner-only), fixed grammar, one line:
//   STRATA-CHUNKS 1 total=<dec> piece=<dec> etag=<hex> bits=<hex>
// etag is the hex of the server's raw ETag bytes (which may contain quotes); bits is the hex of
// ceil(count/8) bytes, bit i (LSB first within a byte) = piece i done. Anything else: parse fails.
constexpr std::string_view kMagic = "STRATA-CHUNKS 1 ";

std::string to_hex_bytes(const std::string_view bytes) {
  static constexpr std::array<char, 16> kHex{'0', '1', '2', '3', '4', '5', '6', '7',
                                             '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  std::string out;
  out.reserve(bytes.size() * 2U);
  for (const unsigned char c : bytes) {
    out.push_back(kHex[c >> 4U]);
    out.push_back(kHex[c & 0x0FU]);
  }
  return out;
}

std::optional<int> hex_digit(const char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return std::nullopt;
}

std::optional<std::string> from_hex_bytes(const std::string_view hex) {
  if (hex.size() % 2U != 0U) return std::nullopt;
  std::string out;
  out.reserve(hex.size() / 2U);
  for (std::size_t i = 0; i < hex.size(); i += 2U) {
    const auto hi = hex_digit(hex[i]);
    const auto lo = hex_digit(hex[i + 1U]);
    if (!hi || !lo) return std::nullopt;
    out.push_back(static_cast<char>((*hi << 4U) | *lo));
  }
  return out;
}

std::optional<std::uint64_t> parse_decimal(const std::string_view text) {
  if (text.empty() || text.size() > 20U) return std::nullopt;
  std::uint64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return std::nullopt;
    const auto digit = static_cast<std::uint64_t>(c - '0');
    if (value > (~std::uint64_t{0} - digit) / 10U) return std::nullopt;  // overflow
    value = value * 10U + digit;
  }
  return value;
}

// value of "key=..." within `field`, or nullopt when the key does not match.
std::optional<std::string_view> field_value(const std::string_view field, const std::string_view key) {
  if (field.size() < key.size() + 1U || field.substr(0, key.size()) != key || field[key.size()] != '=') return std::nullopt;
  return field.substr(key.size() + 1U);
}

}  // namespace

std::uint64_t piece_count(const std::uint64_t total, const std::uint64_t piece_bytes) noexcept {
  if (piece_bytes == 0U || total == 0U) return 0U;
  return (total + piece_bytes - 1U) / piece_bytes;
}

PieceMap piece_map_new(const std::uint64_t total, std::string etag, const std::uint64_t piece_bytes) {
  PieceMap map;
  map.total = total;
  map.piece_bytes = piece_bytes == 0U ? kDownloadPieceBytes : piece_bytes;
  map.etag = std::move(etag);
  map.done.assign(static_cast<std::size_t>(piece_count(total, map.piece_bytes)), false);
  return map;
}

PieceSpan piece_span(const PieceMap& map, const std::uint64_t index) noexcept {
  if (map.piece_bytes == 0U || index >= piece_count(map.total, map.piece_bytes)) return PieceSpan{0, 0};
  const std::uint64_t offset = index * map.piece_bytes;
  const std::uint64_t length = std::min(map.piece_bytes, map.total - offset);
  return PieceSpan{offset, length};
}

bool all_done(const PieceMap& map) noexcept {
  for (const bool bit : map.done) {
    if (!bit) return false;
  }
  return true;
}

std::uint64_t done_bytes(const PieceMap& map) noexcept {
  std::uint64_t total = 0;
  for (std::uint64_t i = 0; i < map.done.size(); ++i) {
    if (map.done[static_cast<std::size_t>(i)]) total += piece_span(map, i).length;
  }
  return total;
}

std::string serialize_piece_map(const PieceMap& map) {
  std::string bits(static_cast<std::size_t>((map.done.size() + 7U) / 8U), '\0');
  for (std::size_t i = 0; i < map.done.size(); ++i) {
    if (map.done[i]) bits[i / 8U] = static_cast<char>(static_cast<unsigned char>(bits[i / 8U]) | (1U << (i % 8U)));
  }
  return std::string{kMagic} + "total=" + std::to_string(map.total) + " piece=" + std::to_string(map.piece_bytes) +
         " etag=" + to_hex_bytes(map.etag) + " bits=" + to_hex_bytes(bits) + "\n";
}

std::optional<PieceMap> parse_piece_map(std::string_view text) {
  if (!text.empty() && text.back() == '\n') text.remove_suffix(1);
  if (text.size() < kMagic.size() || text.substr(0, kMagic.size()) != kMagic) return std::nullopt;
  std::string_view rest = text.substr(kMagic.size());
  std::optional<std::uint64_t> total;
  std::optional<std::uint64_t> piece;
  std::optional<std::string> etag;
  std::optional<std::string> bits;
  while (!rest.empty()) {
    const auto space = rest.find(' ');
    const std::string_view field = rest.substr(0, space);
    rest = space == std::string_view::npos ? std::string_view{} : rest.substr(space + 1U);
    if (const auto v = field_value(field, "total")) total = parse_decimal(*v);
    else if (const auto v2 = field_value(field, "piece")) piece = parse_decimal(*v2);
    else if (const auto v3 = field_value(field, "etag")) etag = from_hex_bytes(*v3);
    else if (const auto v4 = field_value(field, "bits")) bits = from_hex_bytes(*v4);
    else return std::nullopt;  // an unknown field
  }
  if (!total || !piece || !etag || !bits || *piece == 0U) return std::nullopt;
  const std::uint64_t count = piece_count(*total, *piece);
  if (bits->size() != static_cast<std::size_t>((count + 7U) / 8U)) return std::nullopt;
  PieceMap map;
  map.total = *total;
  map.piece_bytes = *piece;
  map.etag = std::move(*etag);
  map.done.assign(static_cast<std::size_t>(count), false);
  for (std::uint64_t i = 0; i < count; ++i) {
    if ((static_cast<unsigned char>((*bits)[static_cast<std::size_t>(i / 8U)]) >> (i % 8U)) & 1U) {
      map.done[static_cast<std::size_t>(i)] = true;
    }
  }
  return map;
}

}  // namespace secure_frontend
