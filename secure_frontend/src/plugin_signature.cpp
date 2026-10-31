#include "secure_frontend/plugin_signature.hpp"

#include "ed25519_verify.hpp"
#include "release_manifest.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>

namespace secure_frontend {
namespace {

constexpr std::string_view kStatementSchema = "m-strata.plugin-signature/1";

bool lower_hex(const std::string_view text, const std::size_t size) noexcept {
  return text.size() == size && std::all_of(text.begin(), text.end(), [](const char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::array<std::uint8_t, 32> key_bytes(const std::string_view hex) noexcept {
  std::array<std::uint8_t, 32> out{};
  const auto nibble = [](const char c) { return static_cast<std::uint8_t>(c <= '9' ? c - '0' : c - 'a' + 10); };
  for (std::size_t i = 0; i < out.size(); ++i) out[i] = static_cast<std::uint8_t>((nibble(hex[2 * i]) << 4U) | nibble(hex[2 * i + 1]));
  return out;
}

// The statement's fields, strictly: its lines in order, each as the signer writes it.
struct Statement final {
  std::string key;
  std::string id;
  std::string platform;
  std::string manifest;
  std::map<std::string, std::string> files;
};

std::optional<Statement> read_statement(std::string_view text) {
  if (text.empty() || text.back() != '\n') return std::nullopt;
  std::vector<std::string_view> lines;
  for (std::size_t start = 0; start < text.size();) {
    const auto end = text.find('\n', start);
    lines.push_back(text.substr(start, end - start));
    start = end + 1U;
  }
  const auto field = [&lines](const std::size_t at, const std::string_view name) -> std::optional<std::string_view> {
    if (at >= lines.size() || lines[at].size() <= name.size() + 1U || lines[at].substr(0, name.size()) != name || lines[at][name.size()] != ' ') {
      return std::nullopt;
    }
    return lines[at].substr(name.size() + 1U);
  };
  if (lines.empty() || lines[0] != kStatementSchema) return std::nullopt;
  const auto key = field(1, "key");
  const auto id = field(2, "id");
  const auto platform = field(3, "platform");
  const auto manifest = field(4, "manifest");
  if (!key || !valid_signing_key(*key) || !id || id->size() > 64U || !platform || (*platform != "windows" && *platform != "linux") ||
      !manifest || !lower_hex(*manifest, 64U)) {
    return std::nullopt;
  }
  Statement statement{.key = std::string{*key}, .id = std::string{*id}, .platform = std::string{*platform}, .manifest = std::string{*manifest}, .files = {}};
  std::string previous;
  for (std::size_t at = 5; at < lines.size(); ++at) {
    const auto rest = field(at, "file");
    const auto space = rest ? rest->rfind(' ') : std::string_view::npos;
    if (space == std::string_view::npos || space == 0U) return std::nullopt;
    const std::string name{rest->substr(0, space)};
    const std::string_view sum = rest->substr(space + 1U);
    if (!lower_hex(sum, 64U) || name.size() > 128U || (!previous.empty() && name <= previous)) return std::nullopt;  // sorted, once each
    statement.files[name] = std::string{sum};
    previous = name;
  }
  return statement;
}

}  // namespace

bool valid_signing_key(const std::string_view text) noexcept { return lower_hex(text, 64U); }

std::string_view plugin_signature_platform() noexcept {
#if defined(_WIN32)
  return "windows";
#else
  return "linux";
#endif
}

std::string plugin_signature_statement(const std::string_view key, const std::string_view id, const std::string_view platform,
                                       const std::string_view manifest_sha256, const std::map<std::string, std::string>& files) {
  std::string out = std::string{kStatementSchema} + "\nkey " + std::string{key} + "\nid " + std::string{id} + "\nplatform " +
                    std::string{platform} + "\nmanifest " + std::string{manifest_sha256} + "\n";
  for (const auto& [name, sum] : files) out += "file " + name + " " + sum + "\n";  // (a std::map: sorted by name)
  return out;
}

std::variant<TrustedStatement, std::string> read_trusted_statement(const std::string_view envelope, const std::string_view id,
                                                                  const std::vector<TrustedKey>& keys, std::string* const named_key) {
  if (envelope.size() > kMaxPluginSignatureBytes) return std::string{"its plugin.sig is larger than 16 KiB"};
  std::string_view signed_bytes;
  std::array<std::uint8_t, 64> signature{};
  if (split_signed_envelope(envelope, &signed_bytes, &signature).has_value()) {
    return std::string{"its plugin.sig is not a signature this server reads"};
  }
  auto read = read_statement(signed_bytes);
  if (!read) return std::string{"its plugin.sig's statement is not one this server reads"};
  Statement& statement = *read;
  if (named_key != nullptr) *named_key = statement.key;
  if (!ed25519_verify(key_bytes(statement.key), signed_bytes, signature)) {
    return std::string{"its signature does not verify under the key it names"};
  }
  const auto trusted = std::find_if(keys.begin(), keys.end(), [&statement](const TrustedKey& key) { return key.key == statement.key; });
  if (trusted == keys.end()) return std::string{"it is signed with a key this server does not trust"};
  if (statement.id != id) return "its signature is for another plugin (" + statement.id + ")";
  if (statement.platform != plugin_signature_platform()) return "its signature is for another platform (" + statement.platform + ")";
  return TrustedStatement{.key = std::move(statement.key),
                          .signer = trusted->name,
                          .manifest_sha256 = std::move(statement.manifest),
                          .files = std::move(statement.files)};
}

PluginSignature check_plugin_signature(const std::string_view envelope, const std::string_view id, const std::string_view manifest_sha256,
                                       const std::map<std::string, std::string>& files, const std::vector<TrustedKey>& keys) {
  PluginSignature checked;
  auto read = read_trusted_statement(envelope, id, keys, &checked.key);
  if (auto* const problem = std::get_if<std::string>(&read)) {
    checked.problem = std::move(*problem);
  } else if (const auto& statement = std::get<TrustedStatement>(read);
             statement.manifest_sha256 != manifest_sha256 || statement.files != files) {
    checked.problem = "its manifest or a file changed since it was signed";
  } else {
    checked.signer = statement.signer;
  }
  return checked;
}

}  // namespace secure_frontend
