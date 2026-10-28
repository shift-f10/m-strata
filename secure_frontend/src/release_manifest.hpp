#pragma once
// SP7 M2 Task 2 (spec D-2/D-3): the signed release envelope.
//
// Envelope = manifest bytes + "\n--STRATA-SIGNATURE-V1\n" + base64 Ed25519
// signature over the manifest bytes (minisign-style framing, ours). The
// manifest JSON follows the strata-release-v1 schema: format, version,
// channel, components[] with {name, file, size, sha256, notes_url}; names
// come from the closed set frontend | engine | cuda-runtime.
//
// Trust comes from the build-time pinned key only (spec D-2): the compile
// definition STRATA_RELEASE_PUBKEY_HEX (a lowercase 64-hex public key) is
// read once; empty or malformed means "not pinned" and every pinned-path
// verification refuses with `no release key pinned` (the exact refusal
// line; tests pin it). `update.source` stays untrusted.
//
// Gate order (R-T2-a): framing -> base64 -> JSON -> schema -> Ed25519.
// Structure/JSON/schema problems reject malformed input before any
// crypto runs; a well-formed but wrongly signed envelope rejects with
// signature_invalid.
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace secure_frontend {

inline constexpr std::string_view kReleaseSignatureFraming = "\n--STRATA-SIGNATURE-V1\n";

enum class ReleaseReject {
  no_pinned_key,   // STRATA_RELEASE_PUBKEY_HEX empty or malformed (pinned path only)
  framing,         // marker missing/duplicated, or junk after the signature line
  signature_base64,  // signature token not strict canonical base64 of 64 bytes
  json,            // manifest bytes are not strictly parseable JSON
  schema,          // JSON parsed but violates the strata-release-v1 schema
  signature_invalid,  // everything parsed; the Ed25519 equation does not hold
};

struct ReleaseIssue final {
  ReleaseReject reject;
  std::string_view detail;  // fixed text naming the offending key/path; never input text
};

struct ReleaseComponent final {
  std::string name;  // frontend | engine | cuda-runtime
  std::string file;  // single filename, no path separators
  std::uint64_t size{0};
  std::string sha256;  // lowercase 64-hex
  std::string notes_url;
};

struct ReleaseManifest final {
  std::string version;
  std::string channel;
  std::vector<ReleaseComponent> components;
  std::string manifest_bytes;  // exactly the signed prefix, verbatim
};

using ReleaseVerifyResult = std::variant<ReleaseManifest, ReleaseIssue>;

// Verify `envelope` against an explicit key. Key material arrives pinned in
// production; tests use this entry point to pin wrong/right keys explicitly.
[[nodiscard]] ReleaseVerifyResult verify_release_manifest(std::string_view envelope,
                                                          const std::array<std::uint8_t, 32>& public_key) noexcept;

// The build-time pinned key; nullopt when STRATA_RELEASE_PUBKEY_HEX is unset,
// empty or malformed (malformed is treated exactly like unset: refuse).
[[nodiscard]] std::optional<std::array<std::uint8_t, 32>> pinned_release_public_key() noexcept;

// The production path: pinned key + verify. Refuses with no_pinned_key when
// nothing is pinned.
[[nodiscard]] ReleaseVerifyResult verify_release_manifest_pinned(std::string_view envelope) noexcept;

// Stable human text for one issue; the no_pinned_key line is exactly
// "no release key pinned" (update prints it verbatim).
[[nodiscard]] std::string describe_release_issue(const ReleaseIssue& issue);

}  // namespace secure_frontend
