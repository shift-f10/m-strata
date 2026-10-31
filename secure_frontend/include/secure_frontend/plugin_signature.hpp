#pragma once

// Trust by key (docs/superpowers/specs/2026-10-30-engine-family-plugins-design.md, section 11; the owner's decision 8):
// plugins/<id>/plugin.sig, the release envelope's form (release_manifest.hpp) over a statement of the folder's SHA-256s,
//
//   m-strata.plugin-signature/1
//   key <64 hex>
//   id <id>
//   platform <windows | linux>
//   manifest <plugin.json's SHA-256>
//   file <name> <SHA-256>            (each of this platform's files, sorted by name)
//
// checked against the keys this server trusts: the install's own (engine/build.json's signing_key) and the publisher
// keys the owner added (plugins.json). scripts/plugin_signing.py signs; this side only verifies (secure_frontend_ed25519).

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace secure_frontend {

inline constexpr std::string_view kPluginSignatureFile = "plugin.sig";
inline constexpr std::size_t kMaxPluginSignatureBytes = 16U * 1024U;
// The refusal of a signature that verifies under a key this server does not trust (the page's add names the key).
inline constexpr std::string_view kUntrustedKeyProblem = "it is signed with a key this server does not trust";

struct TrustedKey final {
  std::string key;      // the public key, 64 lowercase hex
  std::string name;     // the install's own: "this install's key"; else the name the owner gave it
  bool install{false};  // the install's own key (engine/build.json), not one the owner added
};

// A plugin folder's signature, checked.
struct PluginSignature final {
  std::string key;      // the key its statement names (64 hex); empty when the file is not a signature at all
  std::string signer;   // the trusted key's name, when it verifies under a key this server trusts and is this folder's
  std::string problem;  // why it is not taken (empty: signer is set)
};

[[nodiscard]] bool valid_signing_key(std::string_view text) noexcept;  // 64 lowercase hex
[[nodiscard]] std::string_view plugin_signature_platform() noexcept;   // "windows" or "linux"

// The statement a signature over this folder covers: what scripts/plugin_signing.py's statement() writes.
[[nodiscard]] std::string plugin_signature_statement(std::string_view key, std::string_view id, std::string_view platform,
                                                     std::string_view manifest_sha256, const std::map<std::string, std::string>& files);

// What a trusted signature says, before the files are looked at (spec section 12: a plugin added from the page is
// trusted before anything is copied or unpacked): `envelope` verified under a key this server trusts, for plugin `id`
// on this platform - the manifest's and each file's SHA-256 the files must then have. Else why not (check's words).
// `named_key`, when given, gets the key the statement names once it is read, trusted or not (a refusal can name it).
struct TrustedStatement final {
  std::string key;      // 64 hex
  std::string signer;   // the trusted key's name
  std::string manifest_sha256;
  std::map<std::string, std::string> files;  // this platform's files: name -> SHA-256
};
[[nodiscard]] std::variant<TrustedStatement, std::string> read_trusted_statement(std::string_view envelope, std::string_view id,
                                                                                const std::vector<TrustedKey>& keys,
                                                                                std::string* named_key = nullptr);

// `envelope` (plugin.sig's bytes) checked for plugin `id` whose manifest and files hash as given now, on this platform.
[[nodiscard]] PluginSignature check_plugin_signature(std::string_view envelope, std::string_view id, std::string_view manifest_sha256,
                                                     const std::map<std::string, std::string>& files, const std::vector<TrustedKey>& keys);

}  // namespace secure_frontend
