#include "secure_frontend/hf_source.hpp"

#include "secure_frontend/acquire_manifest.hpp"
#include "secure_frontend/json.hpp"

#include <algorithm>
#include <cstdio>

namespace secure_frontend {
namespace {

constexpr std::string_view kLinkPrefix = "https://huggingface.co/";
constexpr std::string_view kDownloadQuery = "?download=true";

bool name_char(const char c) noexcept {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

// One name: 1..max of [A-Za-z0-9._-], not starting with '.' (no "." or "..", no hidden names).
bool name_ok(const std::string_view s, const std::size_t max, const bool dash_first_ok) noexcept {
  if (s.empty() || s.size() > max || s.front() == '.' || (!dash_first_ok && s.front() == '-')) return false;
  return std::all_of(s.begin(), s.end(), name_char);
}

bool ends_with_gguf(const std::string_view path) noexcept {
  constexpr std::string_view kExtension = ".gguf";
  if (path.size() <= kExtension.size()) return false;
  const std::string_view tail = path.substr(path.size() - kExtension.size());
  for (std::size_t i = 0; i < kExtension.size(); ++i) {
    const char c = tail[i] >= 'A' && tail[i] <= 'Z' ? static_cast<char>(tail[i] - 'A' + 'a') : tail[i];
    if (c != kExtension[i]) return false;
  }
  return true;
}

std::vector<std::string_view> split(const std::string_view text, const char separator) {
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  for (;;) {
    const auto at = text.find(separator, start);
    parts.push_back(text.substr(start, at == std::string_view::npos ? std::string_view::npos : at - start));
    if (at == std::string_view::npos) return parts;
    start = at + 1U;
  }
}

bool lower_hex(const std::string_view text, const std::size_t size) noexcept {
  return text.size() == size &&
         std::all_of(text.begin(), text.end(), [](const char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool digits(const std::string_view text) noexcept {
  return !text.empty() && std::all_of(text.begin(), text.end(), [](const char c) { return c >= '0' && c <= '9'; });
}

}  // namespace

bool is_hf_repo(const std::string_view repo) noexcept {
  const auto slash = repo.find('/');
  if (slash == std::string_view::npos || repo.find('/', slash + 1U) != std::string_view::npos) return false;
  return name_ok(repo.substr(0, slash), 96, false) && name_ok(repo.substr(slash + 1U), 96, false);
}

bool is_hf_commit(const std::string_view revision) noexcept { return lower_hex(revision, 40); }

bool is_hf_revision(const std::string_view revision) noexcept { return name_ok(revision, 96, true); }

bool is_hf_gguf_path(const std::string_view path) noexcept {
  if (path.empty() || path.size() > 512 || !ends_with_gguf(path)) return false;
  for (const auto segment : split(path, '/')) {
    if (!name_ok(segment, 255, true)) return false;
  }
  return true;
}

std::variant<HfFileLink, std::string_view> parse_hf_link(const std::string_view text) {
  if (text.substr(0, kLinkPrefix.size()) != kLinkPrefix) return std::string_view{"Only https://huggingface.co links are accepted."};
  std::string_view rest = text.substr(kLinkPrefix.size());
  if (rest.size() >= kDownloadQuery.size() && rest.substr(rest.size() - kDownloadQuery.size()) == kDownloadQuery) {
    rest.remove_suffix(kDownloadQuery.size());
  }
  if (rest.find_first_of("?#") != std::string_view::npos) {
    return std::string_view{"The link may not carry a query or a fragment (only ?download=true)."};
  }
  const auto parts = split(rest, '/');
  if (!parts.empty() && (parts[0] == "datasets" || parts[0] == "spaces")) {
    return std::string_view{"Only model repositories can be downloaded."};
  }
  if (parts.size() < 5U || (parts[2] != "resolve" && parts[2] != "blob")) {
    return std::string_view{"Paste a link to one file (.../resolve/<revision>/<file>.gguf or .../blob/...), not a page or a folder."};
  }
  HfFileLink link;
  link.repo = std::string{parts[0]} + "/" + std::string{parts[1]};
  link.revision = std::string{parts[3]};
  // The path is everything after the fourth '/': owner, name, resolve|blob, revision.
  const std::size_t path_start = parts[0].size() + parts[1].size() + parts[2].size() + parts[3].size() + 4U;
  link.path = std::string{rest.substr(std::min(path_start, rest.size()))};
  if (!is_hf_repo(link.repo)) return std::string_view{"The link does not name a Hugging Face repository (owner/name)."};
  if (!is_hf_revision(link.revision)) return std::string_view{"The link's revision is not a branch, tag or commit name."};
  if (!is_hf_gguf_path(link.path)) return std::string_view{"Only .gguf files can be downloaded (a file name, no ../ or odd characters)."};
  return link;
}

std::string hf_file_url(const std::string_view repo, const std::string_view commit, const std::string_view path) {
  return std::string{kLinkPrefix} + std::string{repo} + "/resolve/" + std::string{commit} + "/" + std::string{path};
}

std::string hf_revision_api_url(const std::string_view repo, const std::string_view revision) {
  return std::string{kLinkPrefix} + "api/models/" + std::string{repo} + "/revision/" + std::string{revision};
}

std::string hf_tree_api_url(const std::string_view repo, const std::string_view commit, const std::string_view folder) {
  std::string url = std::string{kLinkPrefix} + "api/models/" + std::string{repo} + "/tree/" + std::string{commit};
  if (!folder.empty()) url += "/" + std::string{folder};
  return url;
}

std::vector<std::string> gguf_shard_set(const std::string_view path) {
  // <stem>-NNNNN-of-MMMMM.gguf (five digits each, as llama.cpp splits)
  constexpr std::size_t kTail = std::string_view{"-00001-of-00002.gguf"}.size();
  if (path.size() <= kTail || !ends_with_gguf(path)) return {std::string{path}};
  const std::string_view tail = path.substr(path.size() - kTail);
  if (tail[0] != '-' || !digits(tail.substr(1, 5)) || tail.substr(6, 4) != "-of-" || !digits(tail.substr(10, 5))) {
    return {std::string{path}};
  }
  const int count = std::stoi(std::string{tail.substr(10, 5)});
  const int number = std::stoi(std::string{tail.substr(1, 5)});
  if (count < 1 || number < 1 || number > count) return {std::string{path}};
  const std::string head{path.substr(0, path.size() - kTail)};
  const std::string extension{path.substr(path.size() - 5U)};
  std::vector<std::string> names;
  for (int k = 1; k <= count; ++k) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "-%05d-of-%05d", k, count);
    names.push_back(head + buffer + extension);
  }
  return names;
}

std::variant<std::string, std::string_view> hf_manifest_json(const HfDownload& d, const std::string_view version) {
  if (d.profile != "gguf" && d.profile != "glm53") return std::string_view{"the profile must be gguf or glm53"};
  if (!is_valid_model_name(d.name) || !is_valid_model_name(d.tag) || d.tag == "default") {
    return std::string_view{"the name and tag must be model names"};
  }
  if (!is_version_label(version)) return std::string_view{"the version must be YYYY.MM.DD.N"};
  if (!is_hf_repo(d.repo)) return std::string_view{"the repository must be owner/name"};
  if (!is_hf_commit(d.commit)) return std::string_view{"the revision must be resolved to a commit"};
  if (d.files.empty()) return std::string_view{"a download lists at least one file"};
  std::size_t mmproj = 0;
  for (const auto& f : d.files) {
    if (!is_hf_gguf_path(f.path)) return std::string_view{"every file must be a .gguf"};
    if (f.size == 0) return std::string_view{"every file has a size"};
    if (!lower_hex(f.sha256, 64)) return std::string_view{"every file has a SHA-256"};
    if (!f.repo.empty() && !is_hf_repo(f.repo)) return std::string_view{"a file's repository must be owner/name"};
    if (!f.commit.empty() && !is_hf_commit(f.commit)) return std::string_view{"a file's revision must be resolved to a commit"};
    if (f.mmproj) ++mmproj;
  }
  if (mmproj > 1U) return std::string_view{"at most one mmproj file"};
  const auto quoted = [](const std::string_view text) {
    std::string out;
    append_json_string(&out, text);
    return out;
  };
  std::string json = "{\"format\":\"strata-model-manifest\",\"schema_version\":1,\"name\":" + quoted(d.name) +
                     ",\"tag\":" + quoted(d.tag) + ",\"version\":" + quoted(version) + ",\"sequence\":1,\"profile\":" +
                     quoted(d.profile) + ",\"engine\":{\"family\":" + quoted(d.profile == "gguf" ? "none" : "glm") +
                     ",\"min_version\":\"0.0.0\"},\"license\":{\"id\":" + quoted(d.license_id) + ",\"url\":" +
                     quoted(d.license_url.empty() ? hf_file_url(d.repo, d.commit, "").substr(0, kLinkPrefix.size() + d.repo.size())
                                                  : d.license_url) +
                     ",\"bundle_allowed\":false,\"notice\":\"Downloaded from Hugging Face: read the model card at the license URL "
                     "before use.\"},\"hosts\":[\"huggingface.co\"],\"redirect_hosts\":[";
  for (std::size_t i = 0; i < kHfRedirectPatterns.size(); ++i) json += (i != 0U ? "," : "") + quoted(kHfRedirectPatterns[i]);
  json += "],\"files\":[";
  std::optional<std::string> first_shard;
  for (std::size_t i = 0; i < d.files.size(); ++i) {
    const auto& f = d.files[i];
    if (i != 0U) json += ',';
    json += "{\"path\":" + quoted(f.path) + ",\"role\":" + quoted(f.mmproj ? "mmproj" : "weights") +
            ",\"size\":" + std::to_string(f.size) + ",\"sha256\":" + quoted(f.sha256) + ",\"sources\":[" +
            quoted(hf_file_url(f.repo.empty() ? d.repo : f.repo, f.commit.empty() ? d.commit : f.commit, f.path)) + "]}";
    if (!f.mmproj && !first_shard.has_value()) {
      const auto shards = gguf_shard_set(f.path);
      first_shard = shards.front();
    }
  }
  json += "]";
  if (d.profile == "glm53") {
    if (!first_shard.has_value()) return std::string_view{"a glm53 download lists its weights"};
    json += ",\"launch\":{\"native\":" + quoted(*first_shard) + "}";
  }
  json += "}\n";
  return json;
}

}  // namespace secure_frontend
