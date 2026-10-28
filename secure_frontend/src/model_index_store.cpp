#include "secure_frontend/model_index_store.hpp"

#include "secure_frontend/json.hpp"
#include "secure_frontend/sha256.hpp"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

namespace secure_frontend {

std::variant<ModelRootScan, PathError> scan_model_root(const CanonicalRoot& root) {
  ModelRootScan scan;
  std::vector<std::optional<RelativePath>> pending{std::nullopt};
  while (!pending.empty()) {
    const auto directory = std::move(pending.back());
    pending.pop_back();
    auto listed = list_directory(root, directory);
    if (auto* failure = std::get_if<PathError>(&listed)) return *failure;
    for (const auto& entry : std::get<std::vector<DirectoryEntry>>(listed)) {
      const std::string text = directory ? directory->generic() + "/" + entry.name : entry.name;
      auto relative = parse_relative_path(text);
      if (!std::holds_alternative<RelativePath>(relative)) {  // not P1-portable: not ours
        ++scan.skipped;
        continue;
      }
      if (entry.type == EntryType::directory) pending.emplace_back(std::get<RelativePath>(std::move(relative)));
      else if (entry.type == EntryType::regular_file) scan.files.push_back(ModelRootFile{text, entry.size});
      else ++scan.skipped;  // link, junction, device, FIFO: never followed
    }
  }
  std::sort(scan.files.begin(), scan.files.end(), [](const ModelRootFile& a, const ModelRootFile& b) { return a.path < b.path; });
  return scan;
}

std::variant<std::vector<ModelRootFile>, PathError> scan_model_root_files(const CanonicalRoot& root) {
  auto scanned = scan_model_root(root);
  if (auto* failure = std::get_if<PathError>(&scanned)) return *failure;
  return std::get<ModelRootScan>(std::move(scanned)).files;
}

std::variant<IndexEntry, AcquireError> find_installed(const ModelIndex& index, const ModelReference& reference) {
  const IndexEntry* match = nullptr;
  for (const auto& entry : index.models) {
    if (entry.name != reference.name || (reference.tag && entry.tag != *reference.tag)) continue;
    if (match != nullptr) return AcquireError{AcquireReason::usage_error, "several tags installed; name one as <name:tag>"};
    match = &entry;
  }
  if (match == nullptr) {
    return AcquireError{AcquireReason::model_unknown, reference.tag ? model_ref_text(reference.name, *reference.tag) : reference.name};
  }
  return *match;
}

namespace {

// §4.5: index.json is compact JSON of fixed shape: top object, models array, entry object.
constexpr JsonLimits kIndexJsonLimits{kIndexMaxBytes, 4, 65536, 1024};
constexpr std::uint32_t kMaxSequence = 0x7FFFFFFFU;

AcquireError inconsistent(const std::string& detail) { return AcquireError{AcquireReason::index_inconsistent, detail}; }

const JsonValue* member(const std::vector<JsonMember>& members, const std::string_view key) {
  for (const auto& m : members) {
    if (m.key == key) return &m.value;
  }
  return nullptr;
}

bool exact_keys(const std::vector<JsonMember>& members, const std::initializer_list<std::string_view> keys) {
  if (members.size() != keys.size()) return false;
  for (const auto key : keys) {
    if (member(members, key) == nullptr) return false;
  }
  return true;
}

std::optional<std::string> string_of(const std::vector<JsonMember>& members, const std::string_view key) {
  const auto* value = member(members, key);
  const auto* s = value != nullptr ? value->if_string() : nullptr;
  return s != nullptr ? std::optional<std::string>{*s} : std::nullopt;
}

// dir == v<sequence>-<first 12 hex of digest>, sequence 1..2^31-1 in canonical decimal.
bool is_version_dir_for(const std::string_view dir, const std::string_view digest) {
  if (!dir.starts_with('v')) return false;
  const auto dash = dir.find('-');
  if (dash == std::string_view::npos || dash < 2U || dash > 11U || dir[1] == '0') return false;
  std::uint64_t sequence = 0;
  for (const char c : dir.substr(1, dash - 1U)) {
    if (c < '0' || c > '9') return false;
    sequence = sequence * 10U + static_cast<std::uint64_t>(c - '0');
  }
  return sequence <= kMaxSequence && dir == version_directory_name(static_cast<std::uint32_t>(sequence), digest);
}

bool valid_entry(const IndexEntry& e) {
  return is_valid_model_name(e.name) && is_valid_model_name(e.tag) && is_version_label(e.version) && e.sequence >= 1U &&
         e.sequence <= kMaxSequence && is_lower_hex_digest(e.manifest_sha256) &&
         e.dir == version_directory_name(e.sequence, e.manifest_sha256);
}

}  // namespace

RelativePath version_dir_path(const std::string_view name, const std::string_view tag, const std::string_view dir) {
  auto parsed = parse_relative_path(std::string{name} + "/" + std::string{tag} + "/" + std::string{dir});
  if (auto* path = std::get_if<RelativePath>(&parsed)) return std::move(*path);
  throw std::logic_error("index names are validated before use");
}

std::variant<ModelIndex, AcquireError> parse_model_index(const std::string_view bytes) {
  auto parsed = parse_json(bytes, kIndexJsonLimits);
  if (std::holds_alternative<JsonError>(parsed)) return inconsistent("index.json is not valid strict JSON");
  const auto* top = std::get<JsonValue>(parsed).if_object();
  if (top == nullptr || !exact_keys(*top, {"format", "schema_version", "models", "retired"}) ||
      string_of(*top, "format") != "strata-model-index") {
    return inconsistent("index.json has the wrong shape");
  }
  const auto* version = member(*top, "schema_version")->if_integer();
  const auto* models = member(*top, "models")->if_array();
  const auto* retired = member(*top, "retired")->if_array();
  if (version == nullptr || *version != 1 || models == nullptr || retired == nullptr) return inconsistent("index.json has the wrong shape");
  ModelIndex index;
  std::set<std::pair<std::string, std::string>> seen;
  for (const auto& item : *models) {
    const auto* e = item.if_object();
    if (e == nullptr || !exact_keys(*e, {"name", "tag", "version", "sequence", "dir", "manifest_sha256", "origin"})) {
      return inconsistent("index.json model entry has the wrong keys");
    }
    const auto* sequence = member(*e, "sequence")->if_integer();
    IndexEntry entry{string_of(*e, "name").value_or(""), string_of(*e, "tag").value_or(""), string_of(*e, "version").value_or(""),
                     sequence != nullptr && *sequence >= 1 && *sequence <= kMaxSequence ? static_cast<std::uint32_t>(*sequence) : 0U,
                     string_of(*e, "dir").value_or(""), string_of(*e, "manifest_sha256").value_or(""), ModelOrigin::catalog};
    const auto origin = string_of(*e, "origin").value_or("");
    if (origin == "third_party") {
      entry.origin = ModelOrigin::third_party;
    } else if (origin != "catalog") {
      return inconsistent("index.json origin");
    }
    if (!valid_entry(entry) || !seen.insert({entry.name, entry.tag}).second) {
      return inconsistent("index.json model entry " + (is_valid_model_name(entry.name) ? entry.name : std::string{"?"}));
    }
    index.models.push_back(std::move(entry));
  }
  for (const auto& item : *retired) {
    const auto* e = item.if_object();
    if (e == nullptr || !exact_keys(*e, {"name", "tag", "dir", "manifest_sha256"})) return inconsistent("index.json retired entry");
    RetiredEntry entry{string_of(*e, "name").value_or(""), string_of(*e, "tag").value_or(""), string_of(*e, "dir").value_or(""),
                       string_of(*e, "manifest_sha256").value_or("")};
    if (!is_valid_model_name(entry.name) || !is_valid_model_name(entry.tag) || !is_lower_hex_digest(entry.manifest_sha256) ||
        !is_version_dir_for(entry.dir, entry.manifest_sha256)) {
      return inconsistent("index.json retired entry");
    }
    index.retired.push_back(std::move(entry));
  }
  return index;
}

std::variant<ModelIndex, AcquireError> read_model_index(const CanonicalRoot& models) {
  auto opened = open_beneath(models, std::get<RelativePath>(parse_relative_path(kIndexFileName)),
                             OpenOptions{OpenKind::regular_file, FileTrust::owner_only, true});
  if (const auto* failure = std::get_if<PathError>(&opened)) {
    if (failure->code == PathErrorCode::not_found) return ModelIndex{};
    return inconsistent("index.json cannot be opened safely");
  }
  auto bytes = std::get<OpenedFile>(opened).read_all(kIndexMaxBytes);
  if (std::holds_alternative<PathError>(bytes)) return inconsistent("index.json is larger than 4 MiB or unreadable");
  return parse_model_index(std::get<std::string>(bytes));
}

std::variant<InstalledModel, AcquireError> load_installed(const CanonicalRoot& models, const IndexEntry& entry) {
  if (!valid_entry(entry)) return inconsistent("index entry is not valid");
  const std::string ref = model_ref_text(entry.name, entry.tag);
  const auto dir = version_dir_path(entry.name, entry.tag, entry.dir);
  auto manifest_path = parse_relative_path(dir.generic() + "/" + std::string{kVersionManifestName});
  auto opened = open_beneath(models, std::get<RelativePath>(manifest_path), OpenOptions{OpenKind::regular_file, FileTrust::any, true});
  if (std::holds_alternative<PathError>(opened)) return inconsistent(ref + ": manifest.json missing");
  auto bytes = std::get<OpenedFile>(opened).read_all(kManifestMaxBytes);
  if (std::holds_alternative<PathError>(bytes)) return inconsistent(ref + ": manifest.json unreadable");
  const auto& text = std::get<std::string>(bytes);
  if (to_hex(sha256(text)) != entry.manifest_sha256) return inconsistent(ref + ": manifest.json does not match the index");
  ManifestRules rules;
  rules.max_total_bytes = ~std::uint64_t{0};  // already accepted; budgets apply to new transfers only
  auto parsed = parse_manifest(text, rules);
  if (std::holds_alternative<ManifestIssue>(parsed)) return inconsistent(ref + ": manifest.json invalid");
  auto& manifest = std::get<ParsedManifest>(parsed);
  if (manifest.manifest.name != entry.name || manifest.manifest.tag != entry.tag || manifest.manifest.version != entry.version ||
      manifest.manifest.sequence != entry.sequence) {
    return inconsistent(ref + ": manifest.json names another model");
  }
  return InstalledModel{entry, std::move(manifest), dir};
}

}  // namespace secure_frontend
