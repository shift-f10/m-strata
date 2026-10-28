#include "secure_frontend/config.hpp"

#include <algorithm>
#include <cstring>
#include <string_view>

#ifdef _WIN32
#include "secure_frontend/win32_prelude.hpp"
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

namespace secure_frontend {
namespace {

[[nodiscard]] bool is_blank(const std::string_view value) {
  return std::all_of(value.begin(), value.end(), [](const unsigned char character) {
    return character == ' ' || character == '\t' || character == '\n' || character == '\r' || character == '\f' ||
           character == '\v';
  });
}

[[nodiscard]] bool is_localhost(const std::string_view text) {
  constexpr std::string_view kLocalhost{"localhost"};
  if (text.size() != kLocalhost.size()) return false;
  for (std::size_t index = 0; index < text.size(); ++index) {
    const char c = text[index];
    const char lowered = c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
    if (lowered != kLocalhost[index]) return false;
  }
  return true;
}

}  // namespace

std::optional<IpAddress> parse_listener_address(const std::string_view text) {
  if (text.empty() || text.size() > 64U || text.find('\0') != std::string_view::npos) return std::nullopt;
  const std::string candidate = is_localhost(text) ? std::string{"127.0.0.1"} : std::string{text};
  IpAddress address;
  in_addr v4{};
  in6_addr v6{};
  if (inet_pton(AF_INET, candidate.c_str(), &v4) == 1) {
    address.family_ = IpFamily::v4;
    std::memcpy(address.bytes_.data(), &v4, 4U);
  } else if (inet_pton(AF_INET6, candidate.c_str(), &v6) == 1) {
    unsigned char raw[16];
    std::memcpy(raw, &v6, 16U);
    const bool v4_mapped = std::all_of(raw, raw + 10, [](const unsigned char b) { return b == 0U; }) &&
                           raw[10] == 0xffU && raw[11] == 0xffU;
    if (v4_mapped) {
      address.family_ = IpFamily::v4;
      std::memcpy(address.bytes_.data(), raw + 12, 4U);
    } else {
      address.family_ = IpFamily::v6;
      std::memcpy(address.bytes_.data(), raw, 16U);
    }
  } else {
    return std::nullopt;
  }
  const auto& b = address.bytes_;
  if (address.family_ == IpFamily::v4) {
    const bool unspecified = b[0] == 0U && b[1] == 0U && b[2] == 0U && b[3] == 0U;
    address.scope_ = b[0] == 127U ? AddressScope::loopback : unspecified ? AddressScope::wildcard : AddressScope::remote;
  } else {
    const bool leading_zero = std::all_of(b.begin(), b.begin() + 15, [](const unsigned char x) { return x == 0U; });
    address.scope_ = leading_zero && b[15] == 0U   ? AddressScope::wildcard   // IN6_IS_ADDR_UNSPECIFIED
                     : leading_zero && b[15] == 1U ? AddressScope::loopback   // IN6_IS_ADDR_LOOPBACK
                                                   : AddressScope::remote;
  }
  char buffer[INET6_ADDRSTRLEN] = {};
  const int family = address.family_ == IpFamily::v4 ? AF_INET : AF_INET6;
  if (inet_ntop(family, address.bytes_.data(), buffer, sizeof(buffer)) == nullptr) return std::nullopt;
  address.text_ = buffer;
  return address;
}

bool is_valid_host(const std::string_view host) { return parse_listener_address(host).has_value(); }

std::optional<IpNetwork> parse_ip_network(const std::string_view text) {
  const auto slash = text.find('/');
  const std::string_view address_text = text.substr(0, slash);
  if (is_localhost(address_text)) return std::nullopt;   // an address, never a name
  const auto address = parse_listener_address(address_text);
  if (!address) return std::nullopt;
  IpNetwork network{address->family(), address->bytes(), address->family() == IpFamily::v4 ? 32U : 128U};
  if (slash != std::string_view::npos) {
    const std::string_view prefix = text.substr(slash + 1U);
    // 1-3 plain digits, no sign, space or leading zero
    if (prefix.empty() || prefix.size() > 3U || (prefix.size() > 1U && prefix.front() == '0') ||
        !std::all_of(prefix.begin(), prefix.end(), [](const char c) { return c >= '0' && c <= '9'; })) {
      return std::nullopt;
    }
    unsigned value = 0;
    for (const char c : prefix) value = value * 10U + static_cast<unsigned>(c - '0');
    if (value > network.prefix) return std::nullopt;
    network.prefix = value;
  }
  // host bits set: refused, never widened to the subnet
  for (unsigned bit = network.prefix; bit < (network.family == IpFamily::v4 ? 32U : 128U); ++bit) {
    if ((network.bytes[bit / 8U] >> (7U - bit % 8U) & 1U) != 0U) return std::nullopt;
  }
  return network;
}

std::string ip_network_text(const IpNetwork& network) {
  char buffer[INET6_ADDRSTRLEN] = {};
  const bool v4 = network.family == IpFamily::v4;
  if (inet_ntop(v4 ? AF_INET : AF_INET6, network.bytes.data(), buffer, sizeof(buffer)) == nullptr) return {};
  std::string text{buffer};
  if (network.prefix != (v4 ? 32U : 128U)) text += "/" + std::to_string(network.prefix);
  return text;
}

bool network_contains(const IpNetwork& network, const PeerKey& peer) noexcept {
  const unsigned char family = network.family == IpFamily::v4 ? 4U : 6U;
  if (peer[0] != family) return false;
  for (unsigned bit = 0; bit < network.prefix; ++bit) {
    const unsigned mask = 1U << (7U - bit % 8U);
    if ((network.bytes[bit / 8U] & mask) != (peer[1U + bit / 8U] & mask)) return false;
  }
  return true;
}

bool model_control_allowed(const bool loopback_peer, const PeerKey& peer, const bool control_remote,
                           const std::vector<IpNetwork>& networks) noexcept {
  if (loopback_peer) return true;
  if (!control_remote) return false;
  return networks.empty() ||
         std::any_of(networks.begin(), networks.end(), [&peer](const IpNetwork& network) { return network_contains(network, peer); });
}

bool model_files_allowed(const bool loopback_peer, const PeerKey& peer, const bool control_remote,
                         const std::vector<IpNetwork>& networks) noexcept {
  if (loopback_peer) return true;
  return control_remote && !networks.empty() && model_control_allowed(false, peer, control_remote, networks);
}

ConfigValidation validate_config(const ConfigInput& input) {
  const std::string host_text = input.host.value_or(std::string{kDefaultHost});
  const std::uint32_t requested_port = input.port.value_or(kDefaultPort);

  const auto address = parse_listener_address(host_text);
  if (!address.has_value()) {
    return ConfigError{ConfigErrorCode::invalid_host, "host must be an IP literal or localhost"};
  }
  if (requested_port == 0 || requested_port > 65535) {
    return ConfigError{ConfigErrorCode::invalid_port, "port must be in the TCP range 1-65535"};
  }
  const bool loopback = address->scope() == AddressScope::loopback;
  if (!loopback && !input.allow_remote) {
    return ConfigError{ConfigErrorCode::remote_not_enabled,
                       "a non-loopback bind requires explicit remote-listening opt-in"};
  }
  if (address->scope() == AddressScope::wildcard && !input.allow_wildcard_bind) {
    return ConfigError{ConfigErrorCode::wildcard_bind_not_enabled,
                       "a wildcard bind requires explicit wildcard-listening opt-in"};
  }
  const bool has_api_key = input.api_key.has_value() && !input.api_key->empty() && !is_blank(*input.api_key);
  // listener.loopback_without_key (owner decision 2026-10-21): the one keyless listener needs
  // the explicit opt-in AND a loopback address; a remote or wildcard bind always needs the key.
  const bool keyless = !has_api_key && input.allow_loopback_without_key && loopback;
  if (!has_api_key && !keyless) {
    return ConfigError{ConfigErrorCode::api_key_required,
                       "a nonempty API key is required for every listener but an opted-in loopback one"};
  }
  // The authentication layer, not the long-lived validated configuration,
  // owns the secret. Only evidence that validation saw a non-blank key is kept.
  return Config{address->canonical_text(), static_cast<std::uint16_t>(requested_port), has_api_key, loopback,
                input.allow_remote, input.allow_wildcard_bind, keyless};
}

}  // namespace secure_frontend
