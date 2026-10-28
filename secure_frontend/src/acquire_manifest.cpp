#include "secure_frontend/acquire_manifest.hpp"

#include "secure_frontend/acquire_url.hpp"
#include "secure_frontend/bundle_text.hpp"
#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/model_index_model.hpp"
#include "secure_frontend/model_inventory.hpp"
#include "secure_frontend/sha256.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <span>
#include <variant>

namespace secure_frontend {
namespace {

bool printable(const std::string_view text) noexcept {
  return std::all_of(text.begin(), text.end(), [](const char c) { return c >= 0x20 && c <= 0x7E; });
}

}  // namespace

bool is_version_label(const std::string_view text) noexcept {
  // YYYY.MM.DD.N, N 1..999
  if (text.size() < 12 || text.size() > 14 || text[4] != '.' || text[7] != '.' || text[10] != '.') return false;
  const auto digits = [&](const std::size_t from, const std::size_t count) {
    return std::all_of(text.begin() + static_cast<std::ptrdiff_t>(from),
                       text.begin() + static_cast<std::ptrdiff_t>(from + count),
                       [](const char c) { return c >= '0' && c <= '9'; });
  };
  if (!digits(0, 4) || !digits(5, 2) || !digits(8, 2) || !digits(11, text.size() - 11)) return false;
  const int month = (text[5] - '0') * 10 + (text[6] - '0');
  const int day = (text[8] - '0') * 10 + (text[9] - '0');
  if (month < 1 || month > 12 || day < 1 || day > 31) return false;
  if (text[11] == '0') return false;
  return true;  // 1..3 digits without a leading zero: 1..999
}

namespace {

bool is_license_id(const std::string_view id) noexcept {
  const auto body = id.starts_with("LicenseRef-") ? id.substr(11) : id;
  if (body.empty() || id.size() > 128) return false;
  const bool ref = body.size() != id.size();
  return std::all_of(body.begin(), body.end(), [ref](const char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' ||
           (!ref && c == '+');
  });
}

std::optional<std::string_view> license_problem(const ManifestLicense& license, std::string& key, const std::string& prefix) {
  if (!is_license_id(license.id)) {
    key = prefix + ".id";
    return "must be an SPDX id or LicenseRef-<name>";
  }
  if (!license.url.starts_with("https://") || license.url.size() > kManifestMaxStringBytes || !printable(license.url)) {
    key = prefix + ".url";
    return "must be an https:// URL of printable ASCII";
  }
  if (license.notice.size() > kManifestMaxNoticeBytes || !printable(license.notice)) {
    key = prefix + ".notice";
    return "must be at most 512 bytes of printable ASCII";
  }
  return std::nullopt;
}

std::optional<std::string_view> path_problem(const std::string_view path) {
  if (path.empty() || path.size() > kManifestMaxStringBytes) return "must be 1-1024 bytes";
  if (!printable(path)) return "must be printable ASCII";
  if (path.find('\\') != std::string_view::npos || path.find(':') != std::string_view::npos) {
    return "must use '/' separators and no ':'";
  }
  std::size_t start = 0;
  bool first = true;
  while (true) {
    const auto slash = path.find('/', start);
    const auto component = path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
    if (component.empty() || component.front() == '.') return "components must be non-empty and not start with '.'";
    const auto lowered = ascii_lower(component);
    if (lowered.ends_with(kPartSuffix)) return "components must not end in .part";
    if (first && std::find(kReservedTopLevelNames.begin(), kReservedTopLevelNames.end(), lowered) !=
                     kReservedTopLevelNames.end()) {
      return "is a reserved top-level name";
    }
    first = false;
    if (slash == std::string_view::npos) break;
    start = slash + 1;
  }
  // SP1's P1 grammar is the one portable-path rule (device names, trailing dot or
  // space, 8.3 aliases, < > " | ? *, 255-byte components). Applying it here means
  // every caller of validate_manifest, and so parse_manifest, gets it.
  const auto relative = parse_relative_path(path);
  const auto* parsed = std::get_if<RelativePath>(&relative);
  if (parsed == nullptr || parsed->generic() != path) return "must be a portable relative path (P1)";
  return std::nullopt;
}

// "<dir>/<prefix>-NNNNN-of-MMMMM.gguf" → (set key, N, M).
struct ShardName final {
  std::string key;
  unsigned number;
  unsigned count;
};
std::optional<ShardName> shard_name(const std::string_view path) {
  const auto slash = path.rfind('/');
  const auto name = path.substr(slash == std::string_view::npos ? 0 : slash + 1);
  constexpr std::size_t kSuffix = 20;  // "-00001-of-00002.gguf"
  if (name.size() <= kSuffix || !name.ends_with(".gguf")) return std::nullopt;
  const auto tail = name.substr(name.size() - kSuffix);
  const auto is_d = [](const char c) { return c >= '0' && c <= '9'; };
  if (tail[0] != '-' || tail.substr(6, 4) != "-of-") return std::nullopt;
  for (const std::size_t i : {1U, 2U, 3U, 4U, 5U, 10U, 11U, 12U, 13U, 14U}) {
    if (!is_d(tail[i])) return std::nullopt;
  }
  const auto number = static_cast<unsigned>(std::stoul(std::string{tail.substr(1, 5)}));
  const auto count = static_cast<unsigned>(std::stoul(std::string{tail.substr(10, 5)}));
  const auto dir = slash == std::string_view::npos ? std::string_view{} : path.substr(0, slash);
  return ShardName{std::string{dir} + "/" + std::string{name.substr(0, name.size() - kSuffix)} + "/" + std::to_string(count),
                   number, count};
}

const ManifestFile* find_file(const AcquireManifest& m, const std::string_view path) {
  for (const auto& f : m.files) {
    if (f.path == path) return &f;
  }
  return nullptr;
}

bool is_directory_prefix(const AcquireManifest& m, const std::string_view dir) {
  if (dir.empty() || find_file(m, dir) != nullptr) return false;
  const std::string prefix = std::string{dir} + "/";
  return std::any_of(m.files.begin(), m.files.end(), [&](const ManifestFile& f) { return f.path.starts_with(prefix); });
}

}  // namespace

std::string ascii_lower(const std::string_view text) {
  std::string out{text};
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

// SP1's validate_model_id is the one model-ID grammar (and, through
// is_windows_reserved_component, the one device-name list); a manifest name
// is additionally lowercase and never "index.json".
bool is_valid_model_name(const std::string_view name) noexcept {
  if (std::any_of(name.begin(), name.end(), [](const char c) { return c >= 'A' && c <= 'Z'; })) return false;
  return name != "index.json" && std::holds_alternative<std::string>(validate_model_id(name));
}

std::optional<ModelReference> parse_model_reference(const std::string_view text) {
  const auto lowered = ascii_lower(text);
  const auto colon = lowered.find(':');
  ModelReference reference{lowered.substr(0, colon), std::nullopt};
  if (colon != std::string::npos) reference.tag = lowered.substr(colon + 1);
  if (!is_valid_model_name(reference.name)) return std::nullopt;
  if (reference.tag && !is_valid_model_name(*reference.tag)) return std::nullopt;
  return reference;
}

std::string model_ref_text(const std::string_view name, const std::string_view tag) {
  return std::string{name} + ":" + std::string{tag};
}

std::uint64_t total_bytes(const AcquireManifest& manifest) noexcept {
  std::uint64_t total = 0;
  for (const auto& f : manifest.files) total += f.size;
  return total;
}

const ManifestLicense& effective_license(const AcquireManifest& manifest, const std::size_t file_index) noexcept {
  const auto& f = manifest.files[file_index];
  return f.license ? *f.license : manifest.license;
}

bool bundle_allowed(const AcquireManifest& manifest) noexcept {
  for (std::size_t i = 0; i < manifest.files.size(); ++i) {
    if (!effective_license(manifest, i).bundle_allowed) return false;
  }
  return true;
}

bool has_sourceless_files(const AcquireManifest& manifest) noexcept {
  return std::any_of(manifest.files.begin(), manifest.files.end(), [](const ManifestFile& f) { return f.sources.empty(); });
}

std::string_view profile_name(const ManifestProfile profile) noexcept {
  switch (profile) {
    case ManifestProfile::qwen: return "qwen";
    case ManifestProfile::glm53: return "glm53";
    case ManifestProfile::gemma4: return "gemma4";
    case ManifestProfile::raw: return "raw";
    case ManifestProfile::gguf: return "gguf";
    case ManifestProfile::deepseek: return "deepseek";
    case ManifestProfile::raon1: return "raon1";
  }
  return "unknown";
}

std::string_view family_name(const ManifestFamily family) noexcept {
  switch (family) {
    case ManifestFamily::strata: return "strata";
    case ManifestFamily::glm: return "glm";
    case ManifestFamily::gemma4: return "gemma4";
    case ManifestFamily::none: return "none";
    case ManifestFamily::unified: return "unified";
  }
  return "unknown";
}

std::string_view role_name(const FileRole role) noexcept {
  switch (role) {
    case FileRole::weights: return "weights";
    case FileRole::pack: return "pack";
    case FileRole::tokenizer: return "tokenizer";
    case FileRole::mmproj: return "mmproj";
  }
  return "unknown";
}

namespace {

// Controller ruling A: the store and bundles address a file under a prefix, and
// SP1's P1 grammar caps the WHOLE relative path, so the longest stored form must
// fit or a later join would fail. Lengths only: any digest gives the real widths.
// The one family each profile runs on (spec §6.1), an exhaustive switch.
ManifestFamily family_for(const ManifestProfile profile) {
  switch (profile) {
    case ManifestProfile::qwen: return ManifestFamily::strata;
    case ManifestProfile::glm53: return ManifestFamily::glm;
    case ManifestProfile::gemma4: return ManifestFamily::gemma4;
    case ManifestProfile::raw: return ManifestFamily::none;
    case ManifestProfile::gguf: return ManifestFamily::none;
    case ManifestProfile::deepseek: return ManifestFamily::unified;  // unified-engine design §2.5
    case ManifestProfile::raon1: return ManifestFamily::unified;
  }
  return ManifestFamily::none;  // unreachable: every value is a case above
}

std::size_t stored_path_overhead(const AcquireManifest& m) {
  const std::string digest = to_hex(sha256(std::string_view{}));
  // <name>/<tag>/v<sequence>-<12 hex>/<path>
  const std::size_t version = m.name.size() + 1U + m.tag.size() + 1U + version_directory_name(m.sequence, digest).size() + 1U;
  // .staging/<32 hex>/<path>.part
  const std::size_t staging = kStagingDirName.size() + 1U + staging_key(digest).size() + 1U + kPartSuffix.size();
  // files/<path> (bundle)
  const std::size_t bundle = kBundleFilesDir.size() + 1U;
  return std::max({version, staging, bundle});
}

}  // namespace

std::optional<ManifestIssue> validate_manifest(const AcquireManifest& m, const ManifestRules& rules) {
  const auto bad = [](std::string key, const std::string_view why) {
    return ManifestIssue{AcquireReason::manifest_invalid, std::move(key), why};
  };
  if (!is_valid_model_name(m.name)) return bad("name", "must match [a-z0-9][a-z0-9._-]{0,127}, not a device name or index.json");
  if (!is_valid_model_name(m.tag) || m.tag == "default") return bad("tag", "must be a model-id; 'default' is reserved");
  if (!is_version_label(m.version)) return bad("version", "must be YYYY.MM.DD.N with N 1..999");
  if (m.sequence < 1 || m.sequence > 0x7FFFFFFFU) return bad("sequence", "must be 1..2147483647");
  if (m.profile == ManifestProfile::raw && !rules.allow_raw_profile) {
    return bad("profile", "raw is available only in SECURE_FRONTEND_TEST_HOOKS builds");
  }
  if (m.family != family_for(m.profile)) {
    return bad("engine.family",
               "must be strata for qwen, glm for glm53, gemma4 for gemma4, unified for deepseek and raon1, "
               "none for raw and gguf");
  }
  if ((m.profile == ManifestProfile::raw || m.profile == ManifestProfile::gguf) &&
      compare_versions(m.min_version, EngineVersion{}) != 0) {
    return bad("engine.min_version", "must be 0.0.0 for raw and gguf");
  }
  std::string key;
  if (const auto why = license_problem(m.license, key, "license")) return bad(key, *why);
  if (m.hosts.empty() || m.hosts.size() > kManifestMaxHosts) return bad("hosts", "must list 1..16 hosts");
  for (std::size_t i = 0; i < m.hosts.size(); ++i) {
    if (!is_valid_dns_name(m.hosts[i])) return bad("hosts[" + std::to_string(i) + "]", "must be a lowercase DNS name");
  }
  if (m.redirect_hosts.size() > kManifestMaxHosts) return bad("redirect_hosts", "must list at most 16 entries");
  for (std::size_t i = 0; i < m.redirect_hosts.size(); ++i) {
    if (!is_valid_redirect_pattern(m.redirect_hosts[i])) {
      return bad("redirect_hosts[" + std::to_string(i) + "]", "must be a DNS name or *.<name with two labels>");
    }
  }
  if (m.files.empty() || m.files.size() > kManifestMaxFiles) return bad("files", "must list 1..4096 files");
  std::set<std::string> folded_files;
  std::map<std::string, std::string> directory_spellings;  // case-folded directory prefix -> its exact spelling
  const std::size_t overhead = stored_path_overhead(m);
  for (std::size_t i = 0; i < m.files.size(); ++i) {
    const auto& f = m.files[i];
    const std::string at = "files[" + std::to_string(i) + "]";
    if (const auto why = path_problem(f.path)) return bad(at + ".path", *why);
    if (!folded_files.insert(ascii_lower(f.path)).second) return bad(at + ".path", "must be unique after case folding");
    // Review Focus 2: every directory prefix has one exact spelling ("Pack/x" beside "pack/y"
    // is one directory on Windows and would fail only at publish there).
    for (auto slash = f.path.find('/'); slash != std::string::npos; slash = f.path.find('/', slash + 1)) {
      const std::string prefix = f.path.substr(0, slash);
      const auto [it, inserted] = directory_spellings.emplace(ascii_lower(prefix), prefix);
      if (!inserted && it->second != prefix) return bad(at + ".path", "has a directory that differs from another file's only in case");
    }
    if (f.path.size() > kRelativePathMaxBytes - overhead) {
      return bad(at + ".path", "is too long once stored below <name>/<tag>/<version dir>, .staging or files/ (P1: 1024 bytes)");
    }
    if (f.size < 1 || f.size > kManifestMaxFileBytes) return bad(at + ".size", "must be 1..2^40");
    if (!is_lower_hex_digest(f.sha256)) return bad(at + ".sha256", "must be 64 lowercase hex characters");
    if (f.sources.size() > kManifestMaxSources) return bad(at + ".sources", "must list at most 8 URLs");
    for (std::size_t s = 0; s < f.sources.size(); ++s) {
      const std::string source_key = at + ".sources[" + std::to_string(s) + "]";
      const auto parsed = parse_source_url(f.sources[s]);
      if (const auto* problem = std::get_if<UrlProblem>(&parsed)) return bad(source_key, describe_url_problem(*problem));
      if (std::find(m.hosts.begin(), m.hosts.end(), std::get<HttpsUrl>(parsed).host) == m.hosts.end()) {
        return bad(source_key, "host must be listed in hosts");
      }
    }
    if (f.license) {
      if (const auto why = license_problem(*f.license, key, at + ".license")) return bad(key, *why);
    }
  }
  // A file path must not equal (case-folded) a directory prefix of another file.
  // Every string starting with `folded` sorts at or after it, so the first such
  // entry in the ordered set is the only candidate.
  for (std::size_t i = 0; i < m.files.size(); ++i) {
    const std::string folded = ascii_lower(m.files[i].path) + "/";
    const auto next = folded_files.lower_bound(folded);
    if (next != folded_files.end() && next->starts_with(folded)) {
      return bad("files[" + std::to_string(i) + "].path", "is also a directory of another file");
    }
  }
  if (total_bytes(m) > rules.max_total_bytes) {
    return ManifestIssue{AcquireReason::total_budget_exceeded, "files", "total size exceeds pull.max_total_bytes"};
  }
  if (m.profile != ManifestProfile::raw) {
    std::map<std::string, std::set<unsigned>> sets;
    std::map<std::string, unsigned> counts;
    for (const auto& f : m.files) {
      if (f.role != FileRole::weights) continue;
      if (const auto shard = shard_name(f.path)) {
        sets[shard->key].insert(shard->number);
        counts[shard->key] = shard->count;
      }
    }
    for (const auto& [set_key, numbers] : sets) {
      const unsigned count = counts[set_key];
      if (count == 0 || numbers.size() != count || *numbers.begin() != 1 || *numbers.rbegin() != count) {
        return bad("files", "every shard set must be complete (00001..MMMMM) in one directory");
      }
    }
  }
  // SP5 M1 Task 11 (D-9, Ruling T11-bb): schema 1 lists at most one mmproj artifact.
  unsigned mmproj_entries = 0;
  for (const auto& f : m.files) {
    if (f.role == FileRole::mmproj) ++mmproj_entries;
  }
  if (mmproj_entries > 1U) return bad("files", "at most one mmproj entry");
  const auto& launch = m.launch;
  const auto weights_file = [&](const std::optional<std::string>& value) {
    const auto* f = value ? find_file(m, *value) : nullptr;
    return f != nullptr && f->role == FileRole::weights;
  };
  switch (m.profile) {
    case ManifestProfile::gguf:
      // Model downloads (owner rule O4, 2026-10-23): .gguf weights and at most one mmproj, to be
      // prepared on this machine; no launch keys (never launched as a managed model).
      if (launch.native || launch.ple_gguf || launch.pack || launch.tokenizer_pack || launch.mmproj) {
        return bad("launch", "gguf has no launch keys");
      }
      for (std::size_t i = 0; i < m.files.size(); ++i) {
        const auto& f = m.files[i];
        if (f.role != FileRole::weights && f.role != FileRole::mmproj) {
          return bad("files[" + std::to_string(i) + "].role", "gguf lists only weights and mmproj files");
        }
        if (!ascii_lower(f.path).ends_with(".gguf")) return bad("files[" + std::to_string(i) + "].path", "gguf lists only .gguf files");
      }
      break;
    case ManifestProfile::raw:
      if (launch.native) return bad("launch.native", "raw has no launch keys");
      if (launch.ple_gguf) return bad("launch.ple_gguf", "raw has no launch keys");
      if (launch.pack) return bad("launch.pack", "raw has no launch keys");
      if (launch.tokenizer_pack) return bad("launch.tokenizer_pack", "raw has no launch keys");
      if (launch.mmproj) return bad("launch.mmproj", "raw has no launch keys");
      break;
    case ManifestProfile::deepseek:
    case ManifestProfile::raon1:
      // unified-engine design §3.2: the MAYA-lineage merged families carry only launch.native in
      // schema 1, like glm53 (their pack/tokenizer travel with the weights directory); text only.
      if (launch.ple_gguf) return bad("launch.ple_gguf", "deepseek and raon1 have only launch.native in schema 1");
      if (launch.pack) return bad("launch.pack", "deepseek and raon1 have only launch.native in schema 1");
      if (launch.tokenizer_pack) return bad("launch.tokenizer_pack", "deepseek and raon1 have only launch.native in schema 1");
      if (launch.mmproj) return bad("launch.mmproj", "deepseek and raon1 are text only (no launch.mmproj) in schema 1");
      if (!weights_file(launch.native)) return bad("launch.native", "must name a listed weights file");
      if (const auto shard = shard_name(*launch.native); shard && shard->number != 1) {
        return bad("launch.native", "must be shard 00001");
      }
      break;
    case ManifestProfile::gemma4:
      // Gemma 4 plan 1 Task 2 (spec §6.1, §7; D2 text only): the weights and the frontend's
      // tokenizer pack. Its own case: it never falls through to qwen's pack rules.
      if (launch.ple_gguf) return bad("launch.ple_gguf", "gemma4 has only launch.native and launch.tokenizer_pack in schema 1");
      if (launch.pack) return bad("launch.pack", "gemma4 has only launch.native and launch.tokenizer_pack in schema 1");
      if (launch.mmproj) return bad("launch.mmproj", "gemma4 is text only (no launch.mmproj) in schema 1");
      if (!weights_file(launch.native)) return bad("launch.native", "must name a listed weights file");
      if (const auto shard = shard_name(*launch.native); shard && shard->number != 1) {
        return bad("launch.native", "must be shard 00001");
      }
      if (!launch.tokenizer_pack || !is_directory_prefix(m, *launch.tokenizer_pack)) {
        return bad("launch.tokenizer_pack", "must name a directory of listed files");
      }
      break;
    case ManifestProfile::glm53:
      if (launch.ple_gguf) return bad("launch.ple_gguf", "glm53 has only launch.native in schema 1");
      if (launch.pack) return bad("launch.pack", "glm53 has only launch.native in schema 1");
      if (launch.tokenizer_pack) return bad("launch.tokenizer_pack", "glm53 has only launch.native in schema 1");
      // SP5 M1 Task 11: the GLM/Maya encoder is the M2 acceptance-kit piece (D-9/D-10).
      if (launch.mmproj) return bad("launch.mmproj", "glm53 has only launch.native in schema 1");
      [[fallthrough]];
    case ManifestProfile::qwen: {
      if (!weights_file(launch.native)) return bad("launch.native", "must name a listed weights file");
      if (const auto shard = shard_name(*launch.native); shard && shard->number != 1) {
        return bad("launch.native", "must be shard 00001");
      }
      if (m.profile == ManifestProfile::glm53) break;
      if (launch.ple_gguf && !weights_file(launch.ple_gguf)) return bad("launch.ple_gguf", "must name a listed weights file");
      if (launch.mmproj) {  // SP5 M1 Task 11 (D-9): the encoder reference names a listed mmproj file.
        const auto* encoder = find_file(m, *launch.mmproj);
        if (encoder == nullptr || encoder->role != FileRole::mmproj) return bad("launch.mmproj", "must name a listed mmproj file");
      }
      if (!launch.pack || !is_directory_prefix(m, *launch.pack)) return bad("launch.pack", "must name a directory of listed files");
      if (!launch.tokenizer_pack || *launch.tokenizer_pack != *launch.pack + "/tokenizer" ||
          !is_directory_prefix(m, *launch.tokenizer_pack)) {
        return bad("launch.tokenizer_pack", "must be <pack>/tokenizer, a directory of listed files");
      }
      break;
    }
  }
  return std::nullopt;
}

}  // namespace secure_frontend
