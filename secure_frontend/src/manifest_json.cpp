#include "secure_frontend/manifest_json.hpp"

#include "secure_frontend/acquire_url.hpp"
#include "secure_frontend/json.hpp"
#include "secure_frontend/sha256.hpp"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <utility>
#include <vector>

namespace secure_frontend {
namespace {

using Members = std::vector<JsonMember>;

std::string join(const std::string& path, const std::string_view key) {
  return path.empty() ? std::string{key} : path + "." + std::string{key};
}

bool printable(const std::string_view text) {
  return std::all_of(text.begin(), text.end(), [](const char c) { return c >= 0x20 && c <= 0x7E; });
}

class Reader final {
 public:
  std::optional<ManifestIssue> issue;

  void fail(std::string key, const std::string_view why, const AcquireReason reason = AcquireReason::manifest_invalid) {
    if (!issue) issue = ManifestIssue{reason, std::move(key), why};
  }

  static const JsonValue* find(const Members& members, const std::string_view key) {
    for (const auto& member : members) {
      if (member.key == key) return &member.value;
    }
    return nullptr;
  }

  void only(const Members& members, const std::initializer_list<std::string_view> allowed, const std::string& path) {
    for (const auto& member : members) {
      if (std::find(allowed.begin(), allowed.end(), member.key) == allowed.end()) {
        fail(join(path, printable(member.key) ? member.key : std::string{"?"}), "unknown key");
        return;
      }
    }
  }

  const Members* object(const JsonValue* value, const std::string& key, const bool required = true) {
    if (value == nullptr) {
      if (required) fail(key, "is required");
      return nullptr;
    }
    const auto* members = value->if_object();
    if (members == nullptr) fail(key, "must be an object");
    return members;
  }

  std::optional<std::string> text(const Members& members, const std::string_view k, const std::string& path,
                                  const std::size_t max, const bool required = true) {
    const std::string key = join(path, k);
    const auto* value = find(members, k);
    if (value == nullptr) {
      if (required) fail(key, "is required");
      return std::nullopt;
    }
    return string_value(*value, key, max);
  }

  std::optional<std::string> string_value(const JsonValue& value, const std::string& key, const std::size_t max) {
    const auto* s = value.if_string();
    if (s == nullptr) {
      fail(key, "must be a string");
      return std::nullopt;
    }
    if (s->size() > max || !printable(*s)) {
      fail(key, "must be printable ASCII within its length limit");
      return std::nullopt;
    }
    return *s;
  }

  std::int64_t integer(const Members& members, const std::string_view k, const std::string& path) {
    const std::string key = join(path, k);
    const auto* value = find(members, k);
    const auto* number = value != nullptr ? value->if_integer() : nullptr;
    if (number == nullptr) {
      fail(key, value == nullptr ? "is required" : "must be an integer");
      return 0;
    }
    return *number;
  }

  bool boolean(const Members& members, const std::string_view k, const std::string& path) {
    const std::string key = join(path, k);
    const auto* value = find(members, k);
    const auto* flag = value != nullptr ? value->if_boolean() : nullptr;
    if (flag == nullptr) {
      fail(key, value == nullptr ? "is required" : "must be true or false");
      return false;
    }
    return *flag;
  }

  std::vector<std::string> texts(const Members& members, const std::string_view k, const std::string& path,
                                 const std::size_t max_items, const std::size_t max_bytes, const bool required = true) {
    const std::string key = join(path, k);
    const auto* value = find(members, k);
    std::vector<std::string> out;
    if (value == nullptr) {
      if (required) fail(key, "is required");
      return out;
    }
    const auto* items = value->if_array();
    if (items == nullptr) {
      fail(key, "must be an array of strings");
      return out;
    }
    if (items->size() > max_items) {
      fail(key, "has too many entries");
      return out;
    }
    for (std::size_t i = 0; i < items->size(); ++i) {
      if (auto s = string_value((*items)[i], key + "[" + std::to_string(i) + "]", max_bytes)) out.push_back(std::move(*s));
    }
    return out;
  }
};

ManifestLicense read_license(Reader& r, const Members& members, const std::string& path) {
  r.only(members, {"id", "url", "bundle_allowed", "notice"}, path);
  ManifestLicense license;
  license.id = r.text(members, "id", path, 128).value_or("");
  license.url = r.text(members, "url", path, kManifestMaxStringBytes).value_or("");
  license.bundle_allowed = r.boolean(members, "bundle_allowed", path);
  license.notice = r.text(members, "notice", path, kManifestMaxNoticeBytes, false).value_or("");
  return license;
}

}  // namespace

ManifestParseResult parse_manifest(const std::string_view bytes, const ManifestRules& rules) {
  if (bytes.size() > kManifestMaxBytes) return ManifestIssue{AcquireReason::manifest_invalid, "manifest", "is larger than 1 MiB"};
  auto parsed = parse_json(bytes, JsonLimits{kManifestMaxBytes, kManifestMaxDepth, kManifestMaxFiles, kMaxSourceUrlBytes});
  if (const auto* error = std::get_if<JsonError>(&parsed)) {
    return ManifestIssue{AcquireReason::manifest_invalid, "manifest", describe_json_error(error->code)};
  }
  const JsonValue& root = std::get<JsonValue>(parsed);
  Reader r;
  const auto* top = r.object(&root, "manifest");
  if (top == nullptr) return *r.issue;
  r.only(*top, {"format", "schema_version", "name", "tag", "version", "sequence", "profile", "engine", "license", "hosts",
                "redirect_hosts", "files", "launch"}, "");
  if (r.text(*top, "format", "", 64).value_or("") != "strata-model-manifest") r.fail("format", "must be strata-model-manifest");
  if (r.issue) return *r.issue;
  if (r.integer(*top, "schema_version", "") != 1) {
    if (!r.issue) r.fail("schema_version", "is not supported by this release", AcquireReason::manifest_unsupported_version);
    return *r.issue;
  }
  AcquireManifest m;
  m.name = ascii_lower(r.text(*top, "name", "", 128).value_or(""));
  m.tag = ascii_lower(r.text(*top, "tag", "", 128).value_or(""));
  m.version = r.text(*top, "version", "", 32).value_or("");
  const auto sequence = r.integer(*top, "sequence", "");
  m.sequence = sequence >= 1 && sequence <= 0x7FFFFFFF ? static_cast<std::uint32_t>(sequence) : 0U;
  const auto profile = r.text(*top, "profile", "", 16).value_or("");
  if (profile == "qwen") m.profile = ManifestProfile::qwen;
  else if (profile == "glm53") m.profile = ManifestProfile::glm53;
  else if (profile == "gemma4") m.profile = ManifestProfile::gemma4;
  else if (profile == "deepseek") m.profile = ManifestProfile::deepseek;  // unified-engine design §2.5
  else if (profile == "raon1") m.profile = ManifestProfile::raon1;
  else if (profile == "raw") m.profile = ManifestProfile::raw;
  else if (profile == "gguf") m.profile = ManifestProfile::gguf;  // model downloads (owner, 2026-10-23)
  else if (profile == "mtp") m.profile = ManifestProfile::mtp;    // Qwen3.8's MTP draft layer (owner, 2026-10-29)
  else r.fail("profile", "must be qwen, glm53, gemma4, deepseek, raon1, gguf or mtp");
  if (const auto* engine = r.object(Reader::find(*top, "engine"), "engine")) {
    r.only(*engine, {"family", "min_version"}, "engine");
    const auto family = r.text(*engine, "family", "engine", 16).value_or("");
    if (family == "strata") m.family = ManifestFamily::strata;
    else if (family == "glm") m.family = ManifestFamily::glm;
    else if (family == "gemma4") m.family = ManifestFamily::gemma4;
    else if (family == "unified") m.family = ManifestFamily::unified;  // unified-engine design §2.5
    else if (family == "none") m.family = ManifestFamily::none;
    else r.fail("engine.family", "must be strata, glm, gemma4, unified or none");
    const auto version = parse_engine_version(r.text(*engine, "min_version", "engine", 32).value_or(""));
    if (!version) r.fail("engine.min_version", "must be MAJOR.MINOR.PATCH[.N]");
    else m.min_version = *version;
  }
  if (const auto* license = r.object(Reader::find(*top, "license"), "license")) m.license = read_license(r, *license, "license");
  m.hosts = r.texts(*top, "hosts", "", kManifestMaxHosts, 253);
  m.redirect_hosts = r.texts(*top, "redirect_hosts", "", kManifestMaxHosts, 255, false);
  const auto* files = Reader::find(*top, "files");
  const auto* file_items = files != nullptr ? files->if_array() : nullptr;
  if (file_items == nullptr) r.fail("files", "must be an array");
  for (std::size_t i = 0; file_items != nullptr && i < file_items->size() && !r.issue; ++i) {
    const std::string at = "files[" + std::to_string(i) + "]";
    const auto* f = r.object(&(*file_items)[i], at);
    if (f == nullptr) break;
    r.only(*f, {"path", "role", "size", "sha256", "sources", "license", "source_offset"}, at);
    ManifestFile file;
    file.path = r.text(*f, "path", at, kManifestMaxStringBytes).value_or("");
    const auto role = r.text(*f, "role", at, 16).value_or("");
    if (role == "weights") file.role = FileRole::weights;
    else if (role == "pack") file.role = FileRole::pack;
    else if (role == "tokenizer") file.role = FileRole::tokenizer;
    else if (role == "mmproj") file.role = FileRole::mmproj;  // SP5 M1 Task 11 (D-9, Ruling T11-bb)
    else r.fail(at + ".role", role == "vision" || role == "draft" ? "is reserved and rejected in schema 1"
                                                                  : "must be weights, pack, tokenizer or mmproj");
    const auto size = r.integer(*f, "size", at);
    file.size = size > 0 ? static_cast<std::uint64_t>(size) : 0U;
    file.sha256 = r.text(*f, "sha256", at, 64).value_or("");
    file.sources = r.texts(*f, "sources", at, kManifestMaxSources, kMaxSourceUrlBytes);
    // owner 2026-10-29: a window of its source (a tensor of a checkpoint shard: Qwen3.8's MTP draft layer)
    if (const auto* offset = Reader::find(*f, "source_offset")) {
      const auto* number = offset->if_integer();
      if (number == nullptr || *number < 0) r.fail(at + ".source_offset", "must be a non-negative integer");
      else file.source_offset = static_cast<std::uint64_t>(*number);
    }
    if (const auto* license = r.object(Reader::find(*f, "license"), at + ".license", false)) {
      file.license = read_license(r, *license, at + ".license");
    }
    m.files.push_back(std::move(file));
  }
  if (const auto* launch = r.object(Reader::find(*top, "launch"), "launch", false)) {
    r.only(*launch, {"native", "ple_gguf", "pack", "tokenizer_pack", "mmproj"}, "launch");
    m.launch.native = r.text(*launch, "native", "launch", kManifestMaxStringBytes, false);
    m.launch.ple_gguf = r.text(*launch, "ple_gguf", "launch", kManifestMaxStringBytes, false);
    m.launch.pack = r.text(*launch, "pack", "launch", kManifestMaxStringBytes, false);
    m.launch.tokenizer_pack = r.text(*launch, "tokenizer_pack", "launch", kManifestMaxStringBytes, false);
    // SP5 M1 Task 11 (D-9): the optional vision encoder reference; the profile rules in
    // validate_manifest require it to name a listed mmproj file.
    m.launch.mmproj = r.text(*launch, "mmproj", "launch", kManifestMaxStringBytes, false);
  }
  if (r.issue) return *r.issue;
  // validate_manifest() owns every struct-level rule, including the P1 path grammar.
  if (const auto issue = validate_manifest(m, rules)) return *issue;
  return ParsedManifest{std::move(m), std::string{bytes}, to_hex(sha256(bytes))};
}

}  // namespace secure_frontend
