#include "secure_frontend/manifest_diff.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace secure_frontend {
namespace {

std::string engine_text(const AcquireManifest& m) {
  return std::string{family_name(m.family)} + " >= " + render_engine_version(m.min_version);
}

bool same_license(const ManifestLicense& a, const ManifestLicense& b) {
  return a.id == b.id && a.url == b.url && a.bundle_allowed == b.bundle_allowed && a.notice == b.notice;
}

const char* bool_text(const bool value) { return value ? "true" : "false"; }

// Every license field that differs, as "<label>: old -> new" (F-21: a change that forces an
// interactive y must show what changed).
void license_delta(std::vector<std::string>& lines, const std::string& prefix, const ManifestLicense& a,
                   const ManifestLicense& b) {
  if (a.id != b.id) lines.push_back(prefix + "license: " + a.id + " -> " + b.id);
  if (a.url != b.url) lines.push_back(prefix + "license url: " + a.url + " -> " + b.url);
  if (a.bundle_allowed != b.bundle_allowed) {
    lines.push_back(prefix + "bundle_allowed: " + bool_text(a.bundle_allowed) + " -> " + bool_text(b.bundle_allowed));
  }
  if (a.notice != b.notice) lines.push_back(prefix + "notice: " + a.notice + " -> " + b.notice);
}

void list_delta(std::vector<std::string>& lines, const std::string& label, const std::vector<std::string>& before,
                const std::vector<std::string>& after) {
  const std::set<std::string> old_set(before.begin(), before.end());
  const std::set<std::string> new_set(after.begin(), after.end());
  std::string text;
  for (const auto& h : new_set) {
    if (!old_set.contains(h)) text += (text.empty() ? "" : ", ") + std::string{"+ "} + h;
  }
  for (const auto& h : old_set) {
    if (!new_set.contains(h)) text += (text.empty() ? "" : ", ") + std::string{"- "} + h;
  }
  if (!text.empty()) lines.push_back(label + ": " + text);
}

}  // namespace

ManifestDiff diff_manifests(const AcquireManifest& installed, const AcquireManifest& candidate) {
  ManifestDiff diff;
  auto& lines = diff.lines;
  if (installed.version != candidate.version || installed.sequence != candidate.sequence) {
    lines.push_back("version: " + installed.version + " (sequence " + std::to_string(installed.sequence) + ") -> " +
                    candidate.version + " (sequence " + std::to_string(candidate.sequence) + ")");
  }
  if (installed.profile != candidate.profile) {
    lines.push_back("profile: " + std::string{profile_name(installed.profile)} + " -> " +
                    std::string{profile_name(candidate.profile)});
  }
  if (engine_text(installed) != engine_text(candidate)) {
    lines.push_back("engine: " + engine_text(installed) + " -> " + engine_text(candidate));
  }
  license_delta(lines, "", installed.license, candidate.license);
  diff.license_changed = !same_license(installed.license, candidate.license);
  list_delta(lines, "hosts", installed.hosts, candidate.hosts);
  list_delta(lines, "redirect_hosts", installed.redirect_hosts, candidate.redirect_hosts);
  std::map<std::string, std::size_t> old_files;
  for (std::size_t i = 0; i < installed.files.size(); ++i) old_files[installed.files[i].path] = i;
  std::set<std::string> seen;
  std::vector<std::string> added;
  for (std::size_t i = 0; i < candidate.files.size(); ++i) {
    const auto& f = candidate.files[i];
    seen.insert(f.path);
    const auto it = old_files.find(f.path);
    if (it == old_files.end()) {
      added.push_back("file added: " + f.path + " (" + std::to_string(f.size) + " bytes, sha256 " + f.sha256 + ")");
      diff.bytes_to_download += f.size;
      continue;
    }
    const auto& old = installed.files[it->second];
    if (old.sha256 == f.sha256 && old.size == f.size) {
      diff.reusable_files.push_back(i);
    } else {
      lines.push_back("file changed: " + f.path + " size " + std::to_string(old.size) + " -> " + std::to_string(f.size) +
                      ", sha256 " + old.sha256 + " -> " + f.sha256);
      diff.bytes_to_download += f.size;
    }
    // Compare EFFECTIVE licenses (override, else top level), so an appearing or vanishing override
    // shows the real flip (notably bundle_allowed). Files with no override on either side are
    // covered by the top-level lines. An override equal to the top-level license is no change.
    if (old.license.has_value() || f.license.has_value()) {
      const auto& before = effective_license(installed, it->second);
      const auto& after = effective_license(candidate, i);
      if (!same_license(before, after)) {
        diff.license_changed = true;
        license_delta(lines, "file " + f.path + " ", before, after);
      }
    }
  }
  for (const auto& f : installed.files) {
    if (!seen.contains(f.path)) lines.push_back("file removed: " + f.path);
  }
  lines.insert(lines.end(), added.begin(), added.end());
  lines.push_back("bytes to download: " + std::to_string(diff.bytes_to_download));
  return diff;
}

}  // namespace secure_frontend
