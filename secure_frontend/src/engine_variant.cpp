#include "secure_frontend/engine_variant.hpp"
#include "secure_frontend/json.hpp"
#include "secure_frontend/sha256.hpp"
#include <array>
#include <algorithm>
#include <vector>

namespace secure_frontend {
namespace {
constexpr std::size_t kManifestLimit = 256U * 1024U;
const JsonValue* member(const JsonValue& value, const std::string_view key) {
  const auto* object = value.if_object();
  if (object) for (const auto& entry : *object) if (entry.key == key) return &entry.value;
  return nullptr;
}
bool text_is(const JsonValue& value, const std::string_view key, const std::string_view expected) {
  const auto* field = member(value, key);
  return field && field->if_string() && *field->if_string() == expected;
}
bool same_path(const std::filesystem::path& a, const std::filesystem::path& b) {
  return is_within(a, b) && is_within(b, a);
}
std::string failure() {
  return "CUDA fast math is unavailable: the installed variant is missing, untrusted, incomplete or does not match its manifest.";
}
bool verify_file(const CanonicalRoot& root, const RelativePath& path, const std::int64_t expected_bytes,
                 const std::string& expected_digest) {
  auto result = open_beneath(root, path, {.kind = OpenKind::regular_file, .trust = FileTrust::not_shared_writable,
                                        .reject_hard_links = true});
  auto* file = std::get_if<OpenedFile>(&result);
  if (!file || file->size() != static_cast<std::uint64_t>(expected_bytes)) return false;
  Sha256 digest;
  std::array<char, 64U * 1024U> buffer{};
  while (true) {
    const auto read = file->read_some(buffer.data(), buffer.size());
    const auto* count = std::get_if<std::size_t>(&read);
    if (!count) return false;
    if (*count == 0U) break;
    if (*count > static_cast<std::uint64_t>(expected_bytes) - digest.bytes_hashed()) return false;
    digest.update(std::string_view{buffer.data(), *count});
  }
  return digest.bytes_hashed() == static_cast<std::uint64_t>(expected_bytes) && to_hex(digest.finish()) == expected_digest;
}
}  // namespace

std::variant<CanonicalFile, std::string> select_engine_variant(const CanonicalFile& accurate, const bool fast_math) {
  if (!fast_math) return accurate;
  auto root_result = open_root(accurate.path().parent_path());
  const auto* root = std::get_if<CanonicalRoot>(&root_result);
  if (!root) return failure();
  const auto manifest_path = parse_relative_path("variants.json");
  auto manifest_file = open_beneath(*root, std::get<RelativePath>(manifest_path),
      {.kind = OpenKind::regular_file, .trust = FileTrust::not_shared_writable, .reject_hard_links = true});
  const auto* opened = std::get_if<OpenedFile>(&manifest_file);
  if (!opened) return failure();
  const auto contents = opened->read_all(kManifestLimit);
  const auto* text = std::get_if<std::string>(&contents);
  if (!text) return failure();
  const auto parsed = parse_json(*text, {.max_bytes = kManifestLimit, .max_depth = 6U,
      .max_container_entries = 4096U, .max_string_bytes = kRelativePathMaxBytes});
  const auto* document = std::get_if<JsonValue>(&parsed);
  if (!document || !document->if_object() || document->if_object()->size() != 2U) return failure();
  const auto* version = member(*document, "schema_version");
  const auto* variants = member(*document, "variants");
  if (!version || !version->if_integer() || *version->if_integer() != 1 || !variants || !variants->if_array() ||
      variants->if_array()->size() != 2U) return failure();
  const std::string basename = path_to_utf8(accurate.path().filename());
  const JsonValue* selected = nullptr;
  bool have_accurate = false;
  for (const auto& entry : *variants->if_array()) {
    if (!entry.if_object() || entry.if_object()->size() != 5U || !text_is(entry, "executable", basename)) return failure();
    const auto* mode = member(entry, "fast_math");
    const auto* files = member(entry, "files");
    if (!mode || !mode->if_boolean() || !files || !files->if_array() || files->if_array()->empty()) return failure();
    if (text_is(entry, "name", "accurate") && text_is(entry, "directory", ".") && !*mode->if_boolean()) {
      if (have_accurate) return failure();
      have_accurate = true;
    } else if (text_is(entry, "name", "fast-math") && text_is(entry, "directory", "fast-math") && *mode->if_boolean()) {
      if (selected) return failure();
      selected = &entry;
    } else return failure();
  }
  if (!selected || !have_accurate) return failure();
  const auto expected_executable = root->path() / "fast-math" / accurate.path().filename();
  bool have_executable = false;
  std::vector<std::filesystem::path> seen;
  // Validate every selected runtime sidecar, including CUDA libraries. File paths
  // are relative to the variant; walking from the installation root refuses a
  // symlink/junction at the variant directory as well as at every lower component.
  for (const auto& record : *member(*selected, "files")->if_array()) {
    if (!record.if_object() || record.if_object()->size() != 3U) return failure();
    const auto* path = member(record, "path");
    const auto* bytes = member(record, "bytes");
    const auto* digest = member(record, "sha256");
    if (!path || !path->if_string() || !bytes || !bytes->if_integer() || *bytes->if_integer() < 0 ||
        !digest || !digest->if_string() || !is_lower_hex_digest(*digest->if_string())) return failure();
    const auto relative_result = parse_relative_path(*path->if_string());
    const auto* relative = std::get_if<RelativePath>(&relative_result);
    if (!relative) return failure();
    const auto nested_result = parse_relative_path("fast-math/" + relative->generic());
    const auto* nested = std::get_if<RelativePath>(&nested_result);
    if (!nested) return failure();
    const auto absolute = root->join(*nested);
    if (std::any_of(seen.begin(), seen.end(), [&](const auto& old) { return same_path(old, absolute); })) return failure();
    seen.push_back(absolute);
    if (same_path(absolute, expected_executable)) have_executable = true;
    if (!verify_file(*root, *nested, *bytes->if_integer(), *digest->if_string())) return failure();
  }
  if (!have_executable) return failure();
  auto executable = open_executable(expected_executable);
  const auto* checked = std::get_if<CanonicalFile>(&executable);
  if (!checked || !same_path(checked->path(), expected_executable)) return failure();
  return *checked;
}
}  // namespace secure_frontend
