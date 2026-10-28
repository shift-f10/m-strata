#include "secure_frontend/model_index_model.hpp"

#include <algorithm>
#include <tuple>

namespace secure_frontend {
namespace {

void quoted(std::string& out, const std::string_view text) {
  out.push_back('"');
  for (const char c : text) {
    if (c == '"' || c == '\\') out.push_back('\\');
    out.push_back(c);
  }
  out.push_back('"');
}

void member(std::string& out, const std::string_view key, const std::string_view value, const bool comma = true) {
  quoted(out, key);
  out.push_back(':');
  quoted(out, value);
  if (comma) out.push_back(',');
}

}  // namespace

std::string_view origin_name(const ModelOrigin origin) noexcept {
  return origin == ModelOrigin::catalog ? "catalog" : "third_party";
}

std::string render_model_index(const ModelIndex& index) {
  auto models = index.models;
  std::sort(models.begin(), models.end(), [](const IndexEntry& a, const IndexEntry& b) {
    return std::tie(a.name, a.tag) < std::tie(b.name, b.tag);
  });
  auto retired = index.retired;
  std::sort(retired.begin(), retired.end(), [](const RetiredEntry& a, const RetiredEntry& b) {
    return std::tie(a.name, a.tag, a.dir) < std::tie(b.name, b.tag, b.dir);
  });
  std::string out = "{\"format\":\"strata-model-index\",\"schema_version\":1,\"models\":[";
  for (std::size_t i = 0; i < models.size(); ++i) {
    const auto& e = models[i];
    if (i != 0) out.push_back(',');
    out.push_back('{');
    member(out, "name", e.name);
    member(out, "tag", e.tag);
    member(out, "version", e.version);
    out += "\"sequence\":" + std::to_string(e.sequence) + ",";
    member(out, "dir", e.dir);
    member(out, "manifest_sha256", e.manifest_sha256);
    member(out, "origin", origin_name(e.origin), false);
    out.push_back('}');
  }
  out += "],\"retired\":[";
  for (std::size_t i = 0; i < retired.size(); ++i) {
    const auto& e = retired[i];
    if (i != 0) out.push_back(',');
    out.push_back('{');
    member(out, "name", e.name);
    member(out, "tag", e.tag);
    member(out, "dir", e.dir);
    member(out, "manifest_sha256", e.manifest_sha256, false);
    out.push_back('}');
  }
  out += "]}\n";
  return out;
}

const IndexEntry* find_model(const ModelIndex& index, const std::string_view name, const std::string_view tag) noexcept {
  for (const auto& e : index.models) {
    if (e.name == name && e.tag == tag) return &e;
  }
  return nullptr;
}

ModelIndex with_installed(ModelIndex index, IndexEntry entry) {
  // A directory is never live and retired at once (e.g. a downgrade adopting a retired version again).
  std::erase_if(index.retired,
                [&](const RetiredEntry& r) { return r.name == entry.name && r.tag == entry.tag && r.dir == entry.dir; });
  for (auto& e : index.models) {
    if (e.name != entry.name || e.tag != entry.tag) continue;
    const bool already_retired = std::any_of(index.retired.begin(), index.retired.end(), [&](const RetiredEntry& r) {
      return r.name == e.name && r.tag == e.tag && r.dir == e.dir;
    });
    if (e.dir != entry.dir && !already_retired) {
      index.retired.push_back(RetiredEntry{e.name, e.tag, e.dir, e.manifest_sha256});
    }
    e = std::move(entry);
    return index;
  }
  index.models.push_back(std::move(entry));
  return index;
}

ModelIndex without_model(ModelIndex index, const std::string_view name, const std::string_view tag) {
  std::erase_if(index.models, [&](const IndexEntry& e) { return e.name == name && e.tag == tag; });
  return index;
}

ModelIndex without_retired(ModelIndex index, const std::string_view name, const std::string_view tag,
                           const std::string_view dir) {
  std::erase_if(index.retired, [&](const RetiredEntry& e) { return e.name == name && e.tag == tag && e.dir == dir; });
  return index;
}

std::string version_directory_name(const std::uint32_t sequence, const std::string_view manifest_sha256) {
  return "v" + std::to_string(sequence) + "-" + std::string{manifest_sha256.substr(0, 12)};
}

std::string staging_key(const std::string_view manifest_sha256) { return std::string{manifest_sha256.substr(0, 32)}; }

bool is_staging_key_name(const std::string_view name) noexcept {
  return name.size() == 32 &&
         std::all_of(name.begin(), name.end(), [](const char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

}  // namespace secure_frontend
