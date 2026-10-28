#include "secure_frontend/config.hpp"
#include "support/test_support.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

const secure_frontend::Config* config_from(const secure_frontend::ConfigValidation& result) {
  return std::get_if<secure_frontend::Config>(&result);
}

const secure_frontend::ConfigError* error_from(const secure_frontend::ConfigValidation& result) {
  return std::get_if<secure_frontend::ConfigError>(&result);
}

void defaults_to_loopback() {
  const auto result = secure_frontend::validate_config(
      {.host = std::nullopt, .port = std::nullopt, .api_key = std::string{"test-key"}});
  const auto* config = config_from(result);
  expect(config != nullptr, "default configuration should be accepted");
  if (config != nullptr) {
    expect(config->host == "127.0.0.1", "default host should be IPv4 loopback");
    expect(config->port == 8080, "default port should be 8080");
    expect(config->is_loopback, "default host should be loopback");
    expect(config->has_api_key, "validated configuration should record API key presence");
    expect(!config->allow_remote, "remote listening should default to disabled");
  }
}

void requires_key_for_loopback() {
  const auto result = secure_frontend::validate_config(
      {.host = std::string{"::1"}, .port = 9000, .api_key = std::nullopt});
  const auto* error = error_from(result);
  expect(error != nullptr, "loopback bind without a key should be rejected");
  if (error != nullptr) {
    expect(error->code == secure_frontend::ConfigErrorCode::api_key_required,
           "loopback bind should report the API key error");
  }
}

void requires_explicit_opt_in_for_remote_host() {
  const auto result = secure_frontend::validate_config(
      {.host = std::string{"192.0.2.10"}, .port = 8080, .api_key = std::string{"test-key"}});
  const auto* error = error_from(result);
  expect(error != nullptr, "remote bind without an explicit opt-in should be rejected");
  if (error != nullptr) {
    expect(error->code == secure_frontend::ConfigErrorCode::remote_not_enabled,
           "remote bind should report the explicit opt-in error");
  }
}

void requires_key_after_remote_opt_in() {
  const auto result = secure_frontend::validate_config(
      {.host = std::string{"192.0.2.10"}, .port = 8080, .api_key = std::nullopt, .allow_remote = true});
  const auto* error = error_from(result);
  expect(error != nullptr && error->code == secure_frontend::ConfigErrorCode::api_key_required,
         "remote opt-in must still require an API key");
}

void accepts_remote_host_with_opt_in_and_key() {
  const auto result = secure_frontend::validate_config(
      {.host = std::string{"192.0.2.10"}, .port = 65535, .api_key = std::string{"test-key"},
       .allow_remote = true});
  const auto* config = config_from(result);
  expect(config != nullptr, "remote bind with a nonempty key should be accepted");
  if (config != nullptr) {
    expect(!config->is_loopback, "remote IPv4 should not be classified as loopback");
    expect(config->allow_remote, "validated remote configuration should retain opt-in");
  }
}

void requires_separate_opt_in_for_wildcard_host() {
  const auto rejected = secure_frontend::validate_config(
      {.host = std::string{"0.0.0.0"}, .port = 8080, .api_key = std::string{"test-key"},
       .allow_remote = true});
  const auto* error = error_from(rejected);
  expect(error != nullptr && error->code == secure_frontend::ConfigErrorCode::wildcard_bind_not_enabled,
         "wildcard bind must not share the regular remote opt-in");

  const auto accepted = secure_frontend::validate_config(
      {.host = std::string{"::"}, .port = 8080, .api_key = std::string{"test-key"},
       .allow_remote = true, .allow_wildcard_bind = true});
  const auto* config = config_from(accepted);
  expect(config != nullptr && config->allow_wildcard_bind,
         "wildcard bind should require and retain its dedicated opt-in");
}

void rejects_invalid_ports() {
  for (const auto port : {0U, 65536U, 70000U}) {
    const auto result = secure_frontend::validate_config(
        {.host = std::nullopt, .port = port, .api_key = std::string{"test-key"}});
    const auto* error = error_from(result);
    expect(error != nullptr && error->code == secure_frontend::ConfigErrorCode::invalid_port,
           "out-of-range TCP port should be rejected");
  }
}

void rejects_invalid_hosts_and_blank_remote_keys() {
  for (const auto host : {"", "0.0.0.0/0", "bad host", "[::1]", "example..com", "-bad.example"}) {
    const auto result = secure_frontend::validate_config(
        {.host = std::string{host}, .port = std::nullopt, .api_key = std::string{"test-key"}});
    const auto* error = error_from(result);
    expect(error != nullptr && error->code == secure_frontend::ConfigErrorCode::invalid_host,
           "invalid host should be rejected: " + std::string{host});
  }

  const auto result = secure_frontend::validate_config(
      {.host = std::string{"192.0.2.10"}, .port = std::nullopt, .api_key = std::string{" \t\n"},
       .allow_remote = true});
  const auto* error = error_from(result);
  expect(error != nullptr && error->code == secure_frontend::ConfigErrorCode::api_key_required,
         "blank API key must not permit remote binding");
}

// NET-03 / defect (a): every spelling of the unspecified address is a wildcard bind.
const char* const kWildcardSpellings[] = {"0.0.0.0", "::", "::0", "0::0", "0:0:0:0:0:0:0:0", "::0.0.0.0",
                                          "::ffff:0.0.0.0", "0000:0000:0000:0000:0000:0000:0000:0000"};

void net03_wildcard_spellings_need_wildcard_opt_in() {
  for (const char* host : kWildcardSpellings) {
    const auto without = secure_frontend::validate_config(
        {.host = std::string{host}, .port = 8080, .api_key = std::string{"k"}});
    const auto* error = error_from(without);
    expect(error != nullptr && error->code == secure_frontend::ConfigErrorCode::remote_not_enabled,
           std::string{"wildcard without any opt-in must be rejected: "} + host);
    const auto with_both = secure_frontend::validate_config(
        {.host = std::string{host}, .port = 8080, .api_key = std::string{"k"}, .allow_remote = true,
         .allow_wildcard_bind = true});
    const auto* config = config_from(with_both);
    expect(config != nullptr && !config->is_loopback, std::string{"wildcard with both opt-ins is accepted: "} + host);
    if (config != nullptr) {
      expect(config->host == "0.0.0.0" || config->host == "::",
             std::string{"a wildcard binds its canonical text, got "} + config->host);
    }
  }
}

void net03_remote_opt_in_alone_rejects_wildcard() {
  for (const char* host : kWildcardSpellings) {
    const auto result = secure_frontend::validate_config(
        {.host = std::string{host}, .port = 8080, .api_key = std::string{"k"}, .allow_remote = true});
    const auto* error = error_from(result);
    expect(error != nullptr && error->code == secure_frontend::ConfigErrorCode::wildcard_bind_not_enabled,
           std::string{"--allow-remote alone must not allow a wildcard bind: "} + host);
  }
}

void net01_loopback_spellings_classified_from_bytes() {
  const std::pair<const char*, const char*> cases[] = {
      {"127.0.0.1", "127.0.0.1"}, {"127.1.2.3", "127.1.2.3"}, {"::1", "::1"},
      {"0:0:0:0:0:0:0:1", "::1"}, {"0000::0001", "::1"}, {"::ffff:127.0.0.1", "127.0.0.1"}};
  for (const auto& [host, canonical] : cases) {
    const auto result = secure_frontend::validate_config(
        {.host = std::string{host}, .port = 8080, .api_key = std::string{"k"}});
    const auto* config = config_from(result);
    expect(config != nullptr && config->is_loopback, std::string{"loopback spelling must need no opt-in: "} + host);
    if (config != nullptr) expect(config->host == canonical, std::string{"canonical loopback text for "} + host);
  }
}

void net02_hostname_rejected_before_bind() {
  for (const char* host : {"example.com", "service.internal", "my-pc", "localhost.", "localhost.localdomain",
                           "ip6-localhost", "LOCALHOST.example"}) {
    const auto result = secure_frontend::validate_config(
        {.host = std::string{host}, .port = 8080, .api_key = std::string{"k"}, .allow_remote = true});
    const auto* error = error_from(result);
    expect(error != nullptr && error->code == secure_frontend::ConfigErrorCode::invalid_host,
           std::string{"hostnames are rejected at validation, not at bind(): "} + host);
  }
}

void net01_localhost_maps_to_ipv4_loopback() {
  for (const char* host : {"localhost", "LocalHost", "LOCALHOST"}) {
    const auto result = secure_frontend::validate_config(
        {.host = std::string{host}, .port = 8080, .api_key = std::string{"k"}});
    const auto* config = config_from(result);
    expect(config != nullptr && config->is_loopback && config->host == "127.0.0.1",
           std::string{"localhost maps to 127.0.0.1 without a resolver: "} + host);
  }
}

// listener.loopback_without_key (owner decision 2026-10-21): the one keyless listener needs
// the explicit opt-in AND a loopback address; a remote or wildcard bind always needs the key.
void keyless_listener_needs_opt_in_and_loopback() {
  for (const char* host : {"127.0.0.1", "::1", "localhost", "127.0.0.2"}) {
    const auto result = secure_frontend::validate_config(
        {.host = std::string{host}, .port = 8080, .api_key = std::nullopt, .allow_loopback_without_key = true});
    const auto* config = config_from(result);
    expect(config != nullptr && config->is_loopback && !config->has_api_key && config->loopback_without_key,
           std::string{"a loopback bind with the opt-in runs keyless: "} + host);
  }
  const auto keyed = secure_frontend::validate_config(
      {.host = std::string{"127.0.0.1"}, .port = 8080, .api_key = std::string{"k"}, .allow_loopback_without_key = true});
  expect(config_from(keyed) != nullptr && config_from(keyed)->has_api_key && !config_from(keyed)->loopback_without_key,
         "a key, when there is one, is still required (the opt-in never drops a configured key)");
  const auto remote = secure_frontend::validate_config({.host = std::string{"192.0.2.10"}, .port = 8080, .api_key = std::nullopt,
                                                        .allow_remote = true, .allow_loopback_without_key = true});
  expect(error_from(remote) != nullptr && error_from(remote)->code == secure_frontend::ConfigErrorCode::api_key_required,
         "a remote bind never runs keyless, opt-in or not");
  for (const char* wildcard : {"0.0.0.0", "::"}) {
    const auto open = secure_frontend::validate_config({.host = std::string{wildcard}, .port = 8080, .api_key = std::nullopt,
                                                        .allow_remote = true, .allow_wildcard_bind = true,
                                                        .allow_loopback_without_key = true});
    expect(error_from(open) != nullptr && error_from(open)->code == secure_frontend::ConfigErrorCode::api_key_required,
           std::string{"a wildcard bind never runs keyless: "} + wildcard);
  }
  const auto no_opt_in = secure_frontend::validate_config({.host = std::string{"127.0.0.1"}, .port = 8080, .api_key = std::nullopt});
  expect(error_from(no_opt_in) != nullptr && error_from(no_opt_in)->code == secure_frontend::ConfigErrorCode::api_key_required,
         "without the opt-in a loopback bind still needs the key");
}

// The accepted socket's peer as the listener records it: a family byte, then the address.
secure_frontend::PeerKey peer(const char* text) {
  secure_frontend::PeerKey key{};
  const auto address = secure_frontend::parse_listener_address(text);
  if (!address) return key;
  const bool v4 = address->family() == secure_frontend::IpFamily::v4;
  key[0] = v4 ? 4U : 6U;
  std::memcpy(key.data() + 1, address->bytes().data(), v4 ? 4U : 16U);
  return key;
}

// models.control_remote_networks (owner 2026-10-27: "add 172.16.1.0/24 and 172.16.2.0/24 to the control_remote",
// "it should allow hosts list or subnets"): one address or a CIDR subnet, IPv4 or IPv6; names are never resolved.
void control_networks_take_hosts_and_subnets() {
  using secure_frontend::parse_ip_network;
  for (const char* good : {"172.16.1.0/24", "172.16.2.0/24", "172.16.1.50", "172.16.1.50/32", "10.0.0.0/8", "0.0.0.0/0",
                           "fd00::/8", "2001:db8::1", "2001:db8::/32"}) {
    expect(parse_ip_network(good).has_value(), std::string{"accepted: "} + good);
  }
  for (const char* bad : {"", "172.16.1.5/24", "172.16.1.0/33", "172.16.1.0/", "/24", "172.16.1.0/24/8", "172.16.1.0/024",
                          "172.16.1.0/ 24", "172.16.1.0/+8", "172.16.1.0/-1", "myhost", "myhost/24", "localhost", "localhost/8",
                          "fd00::1/8", "fd00::/129", "172.16.1.0 /24"}) {
    expect(!parse_ip_network(bad).has_value(), std::string{"refused: "} + bad);
  }
  const auto lan1 = parse_ip_network("172.16.1.0/24");
  const auto host = parse_ip_network("172.16.2.50");
  const auto v6 = parse_ip_network("fd00::/8");
  const auto all = parse_ip_network("0.0.0.0/0");
  if (!lan1 || !host || !v6 || !all) return;
  using secure_frontend::network_contains;
  expect(network_contains(*lan1, peer("172.16.1.7")) && network_contains(*lan1, peer("172.16.1.255")), "a /24 holds its addresses");
  expect(!network_contains(*lan1, peer("172.16.2.7")) && !network_contains(*lan1, peer("192.169.1.7")), "and no others");
  expect(network_contains(*host, peer("172.16.2.50")) && !network_contains(*host, peer("172.16.2.51")), "a host is that address only");
  expect(network_contains(*v6, peer("fd12::1")) && !network_contains(*v6, peer("fe80::1")), "an IPv6 subnet");
  expect(!network_contains(*lan1, peer("fd12::1")) && !network_contains(*v6, peer("172.16.1.7")), "families never mix");
  expect(network_contains(*all, peer("8.8.8.8")) && !network_contains(*all, secure_frontend::PeerKey{}), "/0 holds every IPv4 peer, not an unknown one");
}

// Model control (D2): loopback always; another client only with models.control_remote and, when networks are listed,
// only from one of them.
void model_control_follows_the_networks() {
  using secure_frontend::model_control_allowed;
  const std::vector<secure_frontend::IpNetwork> networks{*secure_frontend::parse_ip_network("172.16.1.0/24"),
                                                         *secure_frontend::parse_ip_network("172.16.2.0/24"),
                                                         *secure_frontend::parse_ip_network("10.1.2.3")};
  expect(model_control_allowed(true, peer("127.0.0.1"), false, networks), "loopback, always");
  expect(!model_control_allowed(false, peer("172.16.1.7"), false, networks), "without control_remote no other client");
  expect(model_control_allowed(false, peer("172.16.0.9"), true, {}), "control_remote with no list: any client");
  expect(model_control_allowed(false, peer("172.16.1.7"), true, networks) && model_control_allowed(false, peer("172.16.2.200"), true, networks) &&
             model_control_allowed(false, peer("10.1.2.3"), true, networks),
         "a client in a listed subnet or the listed host");
  expect(!model_control_allowed(false, peer("172.16.3.7"), true, networks) && !model_control_allowed(false, peer("10.1.2.4"), true, networks),
         "a client outside them");
  expect(!model_control_allowed(false, secure_frontend::PeerKey{}, true, networks), "an unknown peer, with a list set");
}

// The page's model-file actions (folders, add, change folder, delete; owner 2026-10-27 "Allow all from subnets"): this
// PC always; another machine only from a listed host or subnet - never through control_remote alone.
void model_files_need_a_listed_network() {
  using secure_frontend::model_files_allowed;
  const std::vector<secure_frontend::IpNetwork> networks{*secure_frontend::parse_ip_network("172.16.1.0/24"),
                                                         *secure_frontend::parse_ip_network("172.16.2.0/24")};
  expect(model_files_allowed(true, peer("127.0.0.1"), false, {}), "loopback, always");
  expect(model_files_allowed(false, peer("172.16.1.7"), true, networks) && model_files_allowed(false, peer("172.16.2.9"), true, networks),
         "a client in a listed subnet");
  expect(!model_files_allowed(false, peer("172.16.3.7"), true, networks), "a client outside them");
  expect(!model_files_allowed(false, peer("172.16.1.7"), true, {}), "control_remote with no list: this PC only");
  expect(!model_files_allowed(false, peer("172.16.1.7"), false, networks), "without control_remote: this PC only");
  expect(!model_files_allowed(false, secure_frontend::PeerKey{}, true, networks), "an unknown peer");
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"defaults_to_loopback", defaults_to_loopback},
      {"requires_key_for_loopback", requires_key_for_loopback},
      {"requires_explicit_opt_in_for_remote_host", requires_explicit_opt_in_for_remote_host},
      {"requires_key_after_remote_opt_in", requires_key_after_remote_opt_in},
      {"accepts_remote_host_with_opt_in_and_key", accepts_remote_host_with_opt_in_and_key},
      {"requires_separate_opt_in_for_wildcard_host", requires_separate_opt_in_for_wildcard_host},
      {"rejects_invalid_ports", rejects_invalid_ports},
      {"rejects_invalid_hosts_and_blank_remote_keys", rejects_invalid_hosts_and_blank_remote_keys},
      {"net03_wildcard_spellings_need_wildcard_opt_in", net03_wildcard_spellings_need_wildcard_opt_in},
      {"net03_remote_opt_in_alone_rejects_wildcard", net03_remote_opt_in_alone_rejects_wildcard},
      {"net01_loopback_spellings_classified_from_bytes", net01_loopback_spellings_classified_from_bytes},
      {"net02_hostname_rejected_before_bind", net02_hostname_rejected_before_bind},
      {"net01_localhost_maps_to_ipv4_loopback", net01_localhost_maps_to_ipv4_loopback},
      {"keyless_listener_needs_opt_in_and_loopback", keyless_listener_needs_opt_in_and_loopback},
      {"control_networks_take_hosts_and_subnets", control_networks_take_hosts_and_subnets},
      {"model_control_follows_the_networks", model_control_follows_the_networks},
      {"model_files_need_a_listed_network", model_files_need_a_listed_network},
  }, failures);
}
