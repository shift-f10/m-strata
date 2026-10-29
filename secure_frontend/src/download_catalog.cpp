#include "secure_frontend/download_catalog.hpp"

#include "secure_frontend/hf_source.hpp"
#include "secure_frontend/json.hpp"

#include <algorithm>
#include <array>
#include <set>
#include <span>

namespace secure_frontend {
namespace {

constexpr JsonLimits kCatalogLimits{.max_bytes = 4U << 20U, .max_depth = 8U, .max_container_entries = 65536U,
                                    .max_string_bytes = 4096U, .allow_fractions = false};

const JsonValue* member(const JsonValue& object, const std::string_view key) {
  const auto* members = object.if_object();
  if (members == nullptr) return nullptr;
  for (const auto& m : *members) {
    if (m.key == key) return &m.value;
  }
  return nullptr;
}

std::optional<std::string> text(const JsonValue& object, const std::string_view key) {
  const auto* value = member(object, key);
  if (value == nullptr || value->if_string() == nullptr) return std::nullopt;
  return *value->if_string();
}

bool lower_hex64(const std::string_view s) {
  return s.size() == 64 && std::all_of(s.begin(), s.end(), [](const char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool valid_id(const std::string_view id) {
  return !id.empty() && id.size() <= 64 && std::all_of(id.begin(), id.end(), [](const char c) {
           return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
         });
}

bool printable(const std::string_view s, const std::size_t max) {
  return s.size() <= max && std::all_of(s.begin(), s.end(), [](const char c) { return c >= 0x20 && c < 0x7f; });
}

std::optional<CatalogFile> read_file(const JsonValue& value) {
  CatalogFile file;
  const auto path = text(value, "path");
  const auto sha = text(value, "sha256");
  if (!path || !sha || !is_hf_gguf_path(*path) || !lower_hex64(*sha)) return std::nullopt;
  file.path = *path;
  file.sha256 = *sha;
  if (const auto* size = member(value, "size")) {
    if (const auto* bytes = size->if_integer(); bytes != nullptr && *bytes > 0) file.size = static_cast<std::uint64_t>(*bytes);
    else if (size->kind() != JsonKind::null) return std::nullopt;
  }
  return file;
}

// A side file of an entry (the image encoder, the MTP draft file): a pinned file, with its own repository and revision
// when it names them (empty: the entry's).
std::optional<CatalogFile> read_side_file(const JsonValue& value) {
  auto file = read_file(value);
  if (!file) return std::nullopt;
  if (const auto own_repo = text(value, "repo")) {
    if (!is_hf_repo(*own_repo)) return std::nullopt;
    file->repo = *own_repo;
  }
  if (const auto own_revision = text(value, "revision")) {
    if (!is_hf_commit(*own_revision) && *own_revision != "main") return std::nullopt;
    file->revision = *own_revision;
  }
  return file;
}

// strata | maya: upstream's and Maya's pins; m-strata: our own (tools/secure/catalog_pins.json); custom: a pasted link
constexpr std::array<std::string_view, 4> kSources{"strata", "maya", "m-strata", "custom"};
constexpr std::array<std::string_view, 7> kEngines{"qwen", "glm53", "deepseek", "minimax", "gemma4", "flint", "raon1"};

bool one_of(const std::span<const std::string_view> names, const std::string_view name) {
  return std::find(names.begin(), names.end(), name) != names.end();
}

std::optional<DownloadEntry> read_entry(const JsonValue& value) {
  DownloadEntry e;
  const auto id = text(value, "id");
  const auto name = text(value, "name");
  const auto source = text(value, "source");
  const auto engine = text(value, "engine");
  const auto repo = text(value, "repo");
  const auto revision = text(value, "revision");
  if (!id || !valid_id(*id) || !name || !printable(*name, 128) || name->empty() || !source || !one_of(kSources, *source) || !engine ||
      !one_of(kEngines, *engine) || !repo || !is_hf_repo(*repo) || !revision || (!is_hf_commit(*revision) && *revision != "main")) {
    return std::nullopt;
  }
  e.id = *id;
  e.name = *name;
  e.source = *source;
  e.engine = *engine;
  e.repo = *repo;
  e.revision = *revision;
  e.family = text(value, "family").value_or("");
  e.about = text(value, "about").value_or("");
  if (!printable(e.about, 1024) || !printable(e.family, 64)) return std::nullopt;
  e.prepare = text(value, "prepare").value_or("none");
  // maya_pack: a GLM-5.3, raon-1-mini, DeepSeek-V4.1 or MiniMax-M3.1 download packed as Maya's setup packs it
  // (strata-prepare download --maya)
  if (e.prepare != "iq_pack" && e.prepare != "none" && e.prepare != "maya_pack" && e.prepare != "unsupported") return std::nullopt;
  const auto* files = member(value, "files");
  if (files == nullptr || files->if_array() == nullptr || files->if_array()->empty()) return std::nullopt;
  for (const auto& item : *files->if_array()) {
    auto file = read_file(item);
    if (!file) return std::nullopt;
    // A pinned commit carries its sizes; only Maya's "main" entries read them at start.
    if (is_hf_commit(e.revision) && !file->size) return std::nullopt;
    e.files.push_back(std::move(*file));
  }
  if (const auto* args = member(value, "pack_args"); args != nullptr && args->if_array() != nullptr) {
    for (const auto& arg : *args->if_array()) {
      if (arg.if_string() == nullptr || !printable(*arg.if_string(), 64) || arg.if_string()->rfind("--", 0) != 0) return std::nullopt;
      e.pack_args.push_back(*arg.if_string());
    }
  }
  if (const auto profile = text(value, "expert_profile")) {
    if (profile->empty() || profile->find_first_of("/\\") != std::string::npos || !printable(*profile, 128)) return std::nullopt;
    e.expert_profile = *profile;
  }
  if (const auto* mmproj = member(value, "mmproj"); mmproj != nullptr && mmproj->kind() != JsonKind::null) {
    auto file = read_side_file(*mmproj);
    if (!file) return std::nullopt;
    e.mmproj = std::move(*file);
  }
  // Gemma 4's MTP draft file (owner 2026-10-28): its engine's only, always fetched with the model - so pinned to a
  // commit (its own or the entry's) with its size
  if (const auto* mtp = member(value, "mtp"); mtp != nullptr && mtp->kind() != JsonKind::null) {
    auto file = read_side_file(*mtp);
    if (!file || e.engine != "gemma4" || !file->size || !is_hf_commit(file->revision.empty() ? e.revision : file->revision)) {
      return std::nullopt;
    }
    // a file of its own: neither one of the model's files nor a shard (the manifest's weights are the model's shard set)
    const auto same = [&file](const CatalogFile& model) { return model.path == file->path; };
    if (std::any_of(e.files.begin(), e.files.end(), same) || gguf_shard_set(file->path).size() > 1U) return std::nullopt;
    e.mtp = std::move(*file);
  }
  return e;
}

}  // namespace

DownloadCatalog parse_download_catalog(const std::string_view text_in) {
  DownloadCatalog catalog;
  const auto parsed = parse_json(text_in, kCatalogLimits);
  const auto* root = std::get_if<JsonValue>(&parsed);
  if (root == nullptr) return catalog;
  const auto* version = member(*root, "schema_version");
  const auto* models = member(*root, "models");
  if (version == nullptr || version->if_integer() == nullptr || *version->if_integer() != 1 || models == nullptr ||
      models->if_array() == nullptr) {
    return catalog;
  }
  std::set<std::string> ids;
  for (const auto& item : *models->if_array()) {
    auto entry = read_entry(item);
    if (!entry || !ids.insert(entry->id).second) {
      ++catalog.dropped;
      continue;
    }
    catalog.entries.push_back(std::move(*entry));
  }
  return catalog;
}

}  // namespace secure_frontend
