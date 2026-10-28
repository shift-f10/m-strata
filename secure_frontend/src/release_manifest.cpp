// SP7 M2 Task 2 (spec D-2/D-3): verify the signed release envelope.
//
// Gate order (R-T2-a): framing -> strict base64 -> JSON -> schema -> Ed25519.
// Structure problems reject before any crypto runs; a well-formed but wrongly
// signed envelope reaches the equation and rejects with signature_invalid.
#include "release_manifest.hpp"

#include "ed25519_verify.hpp"
#include "secure_frontend/json.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace secure_frontend {
namespace {

constexpr std::string_view kFormat = "strata-release-v1";
constexpr std::size_t kMaxManifestBytes = std::size_t{1} << 20U;
constexpr std::size_t kMaxComponents = 16;
constexpr std::size_t kSignatureB64Chars = 88;  // 64 bytes, canonical padding

bool is_b64_char(const char c) noexcept {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/';
}

int b64_value(const char c) noexcept {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  return c == '+' ? 62 : 63;
}

bool is_printable_ascii(const std::string_view s) noexcept {
  for (const char c : s) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x21U || u > 0x7EU) return false;
  }
  return true;
}

bool is_lower_hex64(const std::string_view s) noexcept {
  if (s.size() != 64) return false;
  for (const char c : s) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

// Split `envelope` at the single framing marker and strictly decode the
// signature token (R-T2-c). `manifest` and `signature` are set on success.
std::optional<ReleaseIssue> split_envelope(std::string_view envelope, std::string_view* manifest,
                                           std::array<std::uint8_t, 64>* signature) noexcept {
  const std::size_t first = envelope.find(kReleaseSignatureFraming);
  if (first == std::string_view::npos) return ReleaseIssue{ReleaseReject::framing, "framing line missing"};
  if (envelope.find(kReleaseSignatureFraming, first + 1) != std::string_view::npos) {
    return ReleaseIssue{ReleaseReject::framing, "framing line duplicated"};
  }
  *manifest = envelope.substr(0, first);
  const std::string_view rest = envelope.substr(first + kReleaseSignatureFraming.size());

  // Token scan: base64 alphabet run, then canonical "==" padding for 64
  // bytes (86 alphabet chars + "=="), then at most one '\n' to the end.
  std::size_t token = 0;
  while (token < rest.size() && is_b64_char(rest[token])) ++token;
  if (token == 0) return ReleaseIssue{ReleaseReject::signature_base64, "signature alphabet violated"};
  std::size_t end = token;
  if (end < rest.size() && rest[end] == '=') {
    std::size_t pad = 0;
    while (pad < rest.size() - end && rest[end + pad] == '=') ++pad;
    if (pad > 2) return ReleaseIssue{ReleaseReject::signature_base64, "signature padding invalid"};
    const std::size_t after_pad = end + pad;
    if (after_pad < rest.size() && rest[after_pad] != '\n') {
      return ReleaseIssue{ReleaseReject::framing, "junk after the signature line"};
    }
    if (pad != 2 || token != kSignatureB64Chars - 2) {
      return ReleaseIssue{ReleaseReject::signature_base64, "signature length/padding invalid"};
    }
    end = after_pad;
  } else {
    return ReleaseIssue{ReleaseReject::signature_base64, "signature length/padding invalid"};
  }
  const std::string_view tail = rest.substr(end);
  if (!tail.empty() && !(tail.size() == 1 && tail[0] == '\n')) {
    return ReleaseIssue{ReleaseReject::framing, "junk after the signature line"};
  }
  // Canonical trailing bits: the last group is one data byte plus "==" and
  // the four unused low bits of its second character must be zero.
  if ((b64_value(rest[kSignatureB64Chars - 3]) & 0x0F) != 0) {
    return ReleaseIssue{ReleaseReject::signature_base64, "signature trailing bits nonzero"};
  }
  for (std::size_t group = 0; group < 21; ++group) {  // 21 full groups = 63 bytes
    const int c0 = b64_value(rest[4 * group]);
    const int c1 = b64_value(rest[4 * group + 1]);
    const int c2 = b64_value(rest[4 * group + 2]);
    const int c3 = b64_value(rest[4 * group + 3]);
    (*signature)[3 * group] = static_cast<std::uint8_t>((c0 << 2) | (c1 >> 4));
    (*signature)[3 * group + 1] = static_cast<std::uint8_t>((c1 << 4) | (c2 >> 2));
    (*signature)[3 * group + 2] = static_cast<std::uint8_t>((c2 << 6) | c3);
  }
  (*signature)[63] = static_cast<std::uint8_t>((b64_value(rest[84]) << 2) | (b64_value(rest[85]) >> 4));
  return std::nullopt;
}

const JsonValue* member_of(const JsonValue& object, const std::string_view key) noexcept {
  const auto* members = object.if_object();
  if (members == nullptr) return nullptr;
  for (const auto& m : *members) {
    if (m.key == key) return &m.value;
  }
  return nullptr;
}

// Schema validation; fills `out` field by field. detail names the offending
// key path (fixed text), never the input.
std::optional<ReleaseIssue> check_schema(const JsonValue& root, ReleaseManifest* out) {
  const auto* members = root.if_object();
  if (members == nullptr) return ReleaseIssue{ReleaseReject::schema, "top level is not an object"};
  std::size_t seen = 0;
  for (const auto& m : *members) {
    if (m.key == "format" || m.key == "version" || m.key == "channel" || m.key == "components") ++seen;
  }
  if (seen != members->size()) return ReleaseIssue{ReleaseReject::schema, "unknown top-level key"};
  if (members->size() != 4) return ReleaseIssue{ReleaseReject::schema, "top-level key missing"};

  const auto* format = member_of(root, "format");
  if (format == nullptr || format->if_string() == nullptr || *format->if_string() != kFormat) {
    return ReleaseIssue{ReleaseReject::schema, "format must be strata-release-v1"};
  }
  const auto* version = member_of(root, "version");
  if (version == nullptr || version->if_string() == nullptr || version->if_string()->empty() ||
      version->if_string()->size() > 64 || !is_printable_ascii(*version->if_string())) {
    return ReleaseIssue{ReleaseReject::schema, "version invalid"};
  }
  out->version = *version->if_string();
  const auto* channel = member_of(root, "channel");
  if (channel == nullptr || channel->if_string() == nullptr || channel->if_string()->empty() ||
      channel->if_string()->size() > 32 || !is_printable_ascii(*channel->if_string())) {
    return ReleaseIssue{ReleaseReject::schema, "channel invalid"};
  }
  out->channel = *channel->if_string();

  const auto* components = member_of(root, "components");
  const auto* list = components == nullptr ? nullptr : components->if_array();
  if (list == nullptr || list->empty() || list->size() > kMaxComponents) {
    return ReleaseIssue{ReleaseReject::schema, "components must be a 1..16 array"};
  }
  for (std::size_t i = 0; i < list->size(); ++i) {
    const JsonValue& row = (*list)[i];
    const auto* row_members = row.if_object();
    if (row_members == nullptr || row_members->size() != 5) {
      return ReleaseIssue{ReleaseReject::schema, "component fields invalid"};
    }
    std::size_t row_seen = 0;
    for (const auto& m : *row_members) {
      if (m.key == "name" || m.key == "file" || m.key == "size" || m.key == "sha256" || m.key == "notes_url") ++row_seen;
    }
    if (row_seen != 5) return ReleaseIssue{ReleaseReject::schema, "component fields invalid"};

    ReleaseComponent component;
    const auto* name = member_of(row, "name");
    if (name->if_string() == nullptr || !is_printable_ascii(*name->if_string())) {
      return ReleaseIssue{ReleaseReject::schema, "component name invalid"};
    }
    component.name = *name->if_string();
    if (component.name != "frontend" && component.name != "engine" && component.name != "cuda-runtime") {
      return ReleaseIssue{ReleaseReject::schema, "component name outside the closed set"};
    }
    for (const auto& prior : out->components) {
      if (prior.name == component.name) return ReleaseIssue{ReleaseReject::schema, "duplicate component name"};
    }
    const auto* file = member_of(row, "file");
    if (file->if_string() == nullptr || file->if_string()->empty() || file->if_string()->size() > 128 ||
        !is_printable_ascii(*file->if_string())) {
      return ReleaseIssue{ReleaseReject::schema, "component file invalid"};
    }
    component.file = *file->if_string();
    if (component.file.find('/') != std::string::npos || component.file.find('\\') != std::string::npos ||
        component.file.find(':') != std::string::npos || component.file.front() == '-' ||
        component.file == "." || component.file == "..") {
      return ReleaseIssue{ReleaseReject::schema, "component file is not a plain filename"};
    }
    const auto* size = member_of(row, "size");
    const std::int64_t* size_value = size->if_integer();
    if (size_value == nullptr || *size_value < 0) {
      return ReleaseIssue{ReleaseReject::schema, "component size invalid"};
    }
    component.size = static_cast<std::uint64_t>(*size_value);
    const auto* sha = member_of(row, "sha256");
    if (sha->if_string() == nullptr || !is_lower_hex64(*sha->if_string())) {
      return ReleaseIssue{ReleaseReject::schema, "component sha256 invalid"};
    }
    component.sha256 = *sha->if_string();
    const auto* url = member_of(row, "notes_url");
    if (url->if_string() == nullptr || url->if_string()->empty() || url->if_string()->size() > 512 ||
        !is_printable_ascii(*url->if_string())) {
      return ReleaseIssue{ReleaseReject::schema, "component notes_url invalid"};
    }
    component.notes_url = *url->if_string();
    if (component.notes_url.rfind("https://", 0) != 0) {
      return ReleaseIssue{ReleaseReject::schema, "component notes_url must be https"};
    }
    out->components.push_back(std::move(component));
  }
  return std::nullopt;
}

}  // namespace

ReleaseVerifyResult verify_release_manifest(std::string_view envelope,
                                            const std::array<std::uint8_t, 32>& public_key) noexcept {
  std::string_view manifest;
  std::array<std::uint8_t, 64> signature{};
  if (const auto framing = split_envelope(envelope, &manifest, &signature); framing.has_value()) {
    return *framing;
  }
  if (manifest.size() > kMaxManifestBytes) {
    return ReleaseIssue{ReleaseReject::json, "manifest too large"};
  }
  const JsonLimits limits{kMaxManifestBytes, 6, 1024, 4096, false};
  const JsonParseResult parsed = parse_json(manifest, limits);
  const auto* root = std::get_if<JsonValue>(&parsed);
  if (root == nullptr) {
    return ReleaseIssue{ReleaseReject::json, describe_json_error(std::get<JsonError>(parsed).code)};
  }
  ReleaseManifest result;
  if (const auto schema = check_schema(*root, &result); schema.has_value()) {
    return *schema;
  }
  if (!ed25519_verify(public_key, manifest, signature)) {
    return ReleaseIssue{ReleaseReject::signature_invalid, "ed25519 equation does not hold"};
  }
  result.manifest_bytes = std::string(manifest);
  return result;
}

std::optional<std::array<std::uint8_t, 32>> pinned_release_public_key() noexcept {
  static const std::optional<std::array<std::uint8_t, 32>> pinned = []() -> std::optional<std::array<std::uint8_t, 32>> {
#ifndef STRATA_RELEASE_PUBKEY_HEX
    return std::nullopt;
#else
    const std::string_view hex{STRATA_RELEASE_PUBKEY_HEX};
    if (!is_lower_hex64(hex)) return std::nullopt;
    std::array<std::uint8_t, 32> key{};
    for (std::size_t i = 0; i < 32; ++i) {
      const auto hi = static_cast<std::uint8_t>(hex[2 * i] <= '9' ? hex[2 * i] - '0' : hex[2 * i] - 'a' + 10);
      const auto lo = static_cast<std::uint8_t>(hex[2 * i + 1] <= '9' ? hex[2 * i + 1] - '0' : hex[2 * i + 1] - 'a' + 10);
      key[i] = static_cast<std::uint8_t>((hi << 4U) | lo);
    }
    return key;
#endif
  }();
  return pinned;
}

ReleaseVerifyResult verify_release_manifest_pinned(std::string_view envelope) noexcept {
  const auto key = pinned_release_public_key();
  if (!key.has_value()) {
    return ReleaseIssue{ReleaseReject::no_pinned_key, "no release key pinned"};
  }
  return verify_release_manifest(envelope, *key);
}

std::string describe_release_issue(const ReleaseIssue& issue) {
  switch (issue.reject) {
    case ReleaseReject::no_pinned_key: return "no release key pinned";
    case ReleaseReject::framing: return std::string{"release envelope framing invalid: "} + std::string(issue.detail);
    case ReleaseReject::signature_base64:
      return std::string{"release signature not canonical base64: "} + std::string(issue.detail);
    case ReleaseReject::json: return std::string{"release manifest JSON invalid: "} + std::string(issue.detail);
    case ReleaseReject::schema: return std::string{"release manifest schema invalid: "} + std::string(issue.detail);
    case ReleaseReject::signature_invalid: return "release signature invalid";
  }
  return "release envelope invalid";
}

}  // namespace secure_frontend
