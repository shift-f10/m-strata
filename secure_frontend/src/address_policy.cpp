#include "secure_frontend/address_policy.hpp"

#include <cstdio>

namespace secure_frontend {
namespace {

struct Range final {
  std::array<std::uint8_t, 16> prefix;
  unsigned bits;
};

constexpr Range v4(const std::uint8_t a, const std::uint8_t b, const std::uint8_t c, const std::uint8_t d,
                   const unsigned bits) noexcept {
  return Range{{a, b, c, d}, bits};
}

constexpr Range v6(const std::array<std::uint16_t, 8> g, const unsigned bits) noexcept {
  Range range{{}, bits};
  for (std::size_t i = 0; i < 8; ++i) {
    range.prefix[2 * i] = static_cast<std::uint8_t>(g[i] >> 8U);
    range.prefix[2 * i + 1] = static_cast<std::uint8_t>(g[i] & 0xFFU);
  }
  return range;
}

constexpr std::array<Range, 14> kDenied4{
    v4(0, 0, 0, 0, 8),     v4(10, 0, 0, 0, 8),    v4(100, 64, 0, 0, 10), v4(127, 0, 0, 0, 8),
    v4(169, 254, 0, 0, 16), v4(172, 16, 0, 0, 12), v4(192, 0, 0, 0, 24),  v4(192, 0, 2, 0, 24),
    v4(192, 168, 0, 0, 16), v4(198, 18, 0, 0, 15), v4(198, 51, 100, 0, 24), v4(203, 0, 113, 0, 24),
    v4(224, 0, 0, 0, 4),   v4(240, 0, 0, 0, 4)};

constexpr std::array<Range, 13> kDenied6{
    v6({0, 0, 0, 0, 0, 0, 0, 0}, 128),          v6({0, 0, 0, 0, 0, 0, 0, 1}, 128),
    v6({0, 0, 0, 0, 0, 0, 0, 0}, 96),           v6({0, 0, 0, 0, 0, 0xffff, 0, 0}, 96),
    v6({0x64, 0xff9b, 0, 0, 0, 0, 0, 0}, 96),   v6({0x64, 0xff9b, 1, 0, 0, 0, 0, 0}, 48),
    v6({0x100, 0, 0, 0, 0, 0, 0, 0}, 64),       v6({0x2001, 0, 0, 0, 0, 0, 0, 0}, 32),
    v6({0x2001, 0xdb8, 0, 0, 0, 0, 0, 0}, 32),  v6({0x2002, 0, 0, 0, 0, 0, 0, 0}, 16),
    v6({0xfc00, 0, 0, 0, 0, 0, 0, 0}, 7),       v6({0xfe80, 0, 0, 0, 0, 0, 0, 0}, 10),
    v6({0xff00, 0, 0, 0, 0, 0, 0, 0}, 8)};

bool in_range(const std::array<std::uint8_t, 16>& address, const Range& range) noexcept {
  const unsigned full = range.bits / 8U;
  for (unsigned i = 0; i < full; ++i) {
    if (address[i] != range.prefix[i]) return false;
  }
  const unsigned rest = range.bits % 8U;
  if (rest == 0) return true;
  const auto mask = static_cast<std::uint8_t>(0xFFU << (8U - rest));
  return (address[full] & mask) == (range.prefix[full] & mask);
}

}  // namespace

ResolvedAddress make_ipv4(const std::uint8_t a, const std::uint8_t b, const std::uint8_t c, const std::uint8_t d) noexcept {
  ResolvedAddress address;
  address.bytes[0] = a;
  address.bytes[1] = b;
  address.bytes[2] = c;
  address.bytes[3] = d;
  return address;
}

ResolvedAddress make_ipv6(const std::array<std::uint16_t, 8>& groups) noexcept {
  ResolvedAddress address;
  address.ipv6 = true;
  address.bytes = v6(groups, 128).prefix;
  return address;
}

bool is_denied_address(const ResolvedAddress& address) noexcept {
  if (!address.ipv6) {
    for (const auto& range : kDenied4) {
      if (in_range(address.bytes, range)) return true;
    }
    return false;
  }
  for (const auto& range : kDenied6) {
    if (in_range(address.bytes, range)) return true;
  }
  return false;
}

AddressVerdict choose_pinned_address(const std::span<const ResolvedAddress> answers, const bool allow_private) noexcept {
  if (answers.empty()) return {AddressVerdictKind::no_usable_address, {}};
  if (!allow_private) {
    for (const auto& answer : answers) {
      if (is_denied_address(answer)) return {AddressVerdictKind::denied, {}};
    }
  }
  for (const auto& answer : answers) {
    if (!answer.ipv6) return {AddressVerdictKind::pinned, answer};
  }
  if (kIpv6ResolvePinVerified) return {AddressVerdictKind::pinned, answers.front()};
  return {AddressVerdictKind::no_usable_address, {}};
}

std::string format_address(const ResolvedAddress& address) {
  char buffer[48] = {};
  if (!address.ipv6) {
    std::snprintf(buffer, sizeof buffer, "%u.%u.%u.%u", address.bytes[0], address.bytes[1], address.bytes[2],
                  address.bytes[3]);
    return buffer;
  }
  std::string text;
  for (std::size_t i = 0; i < 8; ++i) {
    const unsigned group = (static_cast<unsigned>(address.bytes[2 * i]) << 8U) | address.bytes[2 * i + 1];
    std::snprintf(buffer, sizeof buffer, "%x", group);
    if (i != 0) text.push_back(':');
    text += buffer;
  }
  return text;
}

std::string resolve_argument(const std::string_view host, const ResolvedAddress& address) {
  const std::string formatted = format_address(address);
  return std::string{host} + ":443:" + (address.ipv6 ? "[" + formatted + "]" : formatted);
}

}  // namespace secure_frontend
