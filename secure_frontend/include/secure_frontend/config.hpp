#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace secure_frontend {

inline constexpr std::string_view kDefaultHost{"127.0.0.1"};
inline constexpr std::uint16_t kDefaultPort{8080};

enum class IpFamily { v4, v6 };
enum class AddressScope { loopback, wildcard, remote };

// A listener address parsed once from text into bytes. Scope and canonical
// text are derived from the bytes, never from the spelling (NET-01..03).
// Built only by parse_listener_address.
class IpAddress final {
 public:
  [[nodiscard]] IpFamily family() const noexcept { return family_; }
  [[nodiscard]] const std::array<unsigned char, 16>& bytes() const noexcept { return bytes_; }
  [[nodiscard]] AddressScope scope() const noexcept { return scope_; }
  // inet_ntop text of the bytes: what is bound and logged. IPv4-mapped IPv6
  // input (::ffff:a.b.c.d) is canonicalized to its IPv4 form.
  [[nodiscard]] const std::string& canonical_text() const noexcept { return text_; }

 private:
  friend std::optional<IpAddress> parse_listener_address(std::string_view text);
  IpAddress() = default;
  IpFamily family_{IpFamily::v4};
  std::array<unsigned char, 16> bytes_{};
  AddressScope scope_{AddressScope::remote};
  std::string text_;
};

// Accepts an IPv4 or IPv6 literal, or exactly "localhost" (ASCII
// case-insensitive), which maps to 127.0.0.1 with no resolver call. Every
// other hostname is rejected: the listener never resolves names.
[[nodiscard]] std::optional<IpAddress> parse_listener_address(std::string_view text);

// models.control_remote_networks (owner 2026-10-27): one address ("172.16.1.50": all of its bits) or a subnet in CIDR
// form ("172.16.1.0/24", "fd00::/8"), IPv4 or IPv6. Host names, localhost included, are refused (never resolved), and
// so is a subnet with host bits set ("172.16.1.5/24": refused rather than widened).
struct IpNetwork final {
  IpFamily family{IpFamily::v4};
  std::array<unsigned char, 16> bytes{};
  unsigned prefix{32};
};
[[nodiscard]] std::optional<IpNetwork> parse_ip_network(std::string_view text);
// inet_ntop text, with "/prefix" unless the entry is one address: "172.16.1.0/24", "10.1.2.3".
[[nodiscard]] std::string ip_network_text(const IpNetwork& network);

// An accepted peer as the listener records it (HttpSocketServer::Accepted::peer_key): a family byte, 4 or 6 (0: not
// known), then the address; an IPv4-mapped IPv6 peer is folded to 4.
using PeerKey = std::array<unsigned char, 17>;
[[nodiscard]] bool network_contains(const IpNetwork& network, const PeerKey& peer) noexcept;

// Model control (model launcher D2): loopback clients always; another client only with models.control_remote and,
// when networks are listed (models.control_remote_networks), only from one of them.
[[nodiscard]] bool model_control_allowed(bool loopback_peer, const PeerKey& peer, bool control_remote,
                                         const std::vector<IpNetwork>& networks) noexcept;
// The page's model-file actions (this PC's folders, a card from a file, a card's folder, a delete; owner 2026-10-27):
// loopback clients always; another client only with models.control_remote AND a listed network it is in - never
// through control_remote alone.
[[nodiscard]] bool model_files_allowed(bool loopback_peer, const PeerKey& peer, bool control_remote,
                                       const std::vector<IpNetwork>& networks) noexcept;

struct ConfigInput final {
  std::optional<std::string> host;
  std::optional<std::uint32_t> port;
  std::optional<std::string> api_key;
  bool allow_remote{false};
  bool allow_wildcard_bind{false};
  // listener.loopback_without_key (owner decision 2026-10-21): the one keyless listener,
  // an explicit opt-in that a loopback address alone can use.
  bool allow_loopback_without_key{false};
};

struct Config final {
  std::string host;
  std::uint16_t port;
  bool has_api_key;
  bool is_loopback;
  bool allow_remote;
  bool allow_wildcard_bind;
  bool loopback_without_key;   // no key, by the opt-in, on a loopback address
};

enum class ConfigErrorCode {
  invalid_host,
  invalid_port,
  api_key_required,
  remote_not_enabled,
  wildcard_bind_not_enabled,
};

struct ConfigError final {
  ConfigErrorCode code;
  std::string message;
};

using ConfigValidation = std::variant<Config, ConfigError>;

[[nodiscard]] ConfigValidation validate_config(const ConfigInput& input);
[[nodiscard]] bool is_valid_host(std::string_view host);

}  // namespace secure_frontend
