#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace secure_frontend {

// One getaddrinfo answer. IPv4 occupies bytes[0..3].
struct ResolvedAddress final {
  bool ipv6{false};
  std::array<std::uint8_t, 16> bytes{};
};

[[nodiscard]] ResolvedAddress make_ipv4(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d) noexcept;
[[nodiscard]] ResolvedAddress make_ipv6(const std::array<std::uint16_t, 8>& groups) noexcept;

// Spec §3.5 denied ranges (used unless pull.allow_private_addresses).
[[nodiscard]] bool is_denied_address(const ResolvedAddress& address) noexcept;

// Spec §3.5: until `--resolve host:443:[addr]` is verified on every curl at or
// above the D3 floor, an IPv6-only hop fails closed with address_not_allowed.
inline constexpr bool kIpv6ResolvePinVerified = false;

enum class AddressVerdictKind { pinned, denied, no_usable_address };
struct AddressVerdict final {
  AddressVerdictKind kind;
  ResolvedAddress pinned;
};

// Denied if ANY answer is in a denied range (unless allow_private); otherwise
// pins the first IPv4 answer (IPv6 only once kIpv6ResolvePinVerified).
// An IPv6-only hop yields no_usable_address; Task 21 maps that to
// address_not_allowed (spec §3.5).
[[nodiscard]] AddressVerdict choose_pinned_address(std::span<const ResolvedAddress> answers, bool allow_private) noexcept;

// Dotted IPv4, or eight lowercase hex groups without compression. For curl's
// argv only: never printed, logged or stored (spec §8.1).
[[nodiscard]] std::string format_address(const ResolvedAddress& address);
[[nodiscard]] std::string resolve_argument(std::string_view host, const ResolvedAddress& address);

}  // namespace secure_frontend
