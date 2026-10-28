#include "secure_frontend/bundle_text.hpp"

#include "secure_frontend/sha256.hpp"

#include <algorithm>

namespace secure_frontend {
namespace {

std::vector<SumsLine> expected_lines(const std::string_view manifest_sha256, const AcquireManifest& manifest) {
  std::vector<SumsLine> lines;
  lines.push_back({std::string{manifest_sha256}, std::string{kBundleManifestName}});
  for (const auto& f : manifest.files) lines.push_back({f.sha256, std::string{kBundleFilesDir} + "/" + f.path});
  std::sort(lines.begin(), lines.end(), [](const SumsLine& a, const SumsLine& b) { return a.path < b.path; });
  return lines;
}

std::optional<std::vector<std::string_view>> split_lines(const std::string_view text) {
  if (text.empty() || text.back() != '\n') return std::nullopt;
  std::vector<std::string_view> lines;
  std::size_t start = 0;
  while (start < text.size()) {
    const auto end = text.find('\n', start);
    const auto line = text.substr(start, end - start);
    if (line.empty() || line.find('\r') != std::string_view::npos) return std::nullopt;
    lines.push_back(line);
    start = end + 1;
  }
  return lines;
}

}  // namespace

std::string render_sha256sums(const std::string_view manifest_sha256, const AcquireManifest& manifest) {
  std::string text;
  for (const auto& line : expected_lines(manifest_sha256, manifest)) text += line.sha256 + "  " + line.path + "\n";
  return text;
}

std::optional<std::vector<SumsLine>> parse_sha256sums(const std::string_view text) {
  const auto lines = split_lines(text);
  if (!lines) return std::nullopt;
  std::vector<SumsLine> out;
  for (const auto line : *lines) {
    if (line.size() < 67 || line.substr(64, 2) != "  " || !is_lower_hex_digest(line.substr(0, 64))) return std::nullopt;
    SumsLine parsed{std::string{line.substr(0, 64)}, std::string{line.substr(66)}};
    if (!out.empty() && !(out.back().path < parsed.path)) return std::nullopt;
    out.push_back(std::move(parsed));
  }
  return out;
}

bool sums_agree(const std::span<const SumsLine> lines, const std::string_view manifest_sha256, const AcquireManifest& manifest) {
  const auto expected = expected_lines(manifest_sha256, manifest);
  if (expected.size() != lines.size()) return false;
  for (std::size_t i = 0; i < expected.size(); ++i) {
    if (expected[i].path != lines[i].path || expected[i].sha256 != lines[i].sha256) return false;
  }
  return true;
}

std::string render_missing(const AcquireManifest& manifest) {
  std::vector<std::string> paths;
  for (const auto& f : manifest.files) {
    if (f.sources.empty()) paths.push_back(f.path);
  }
  std::sort(paths.begin(), paths.end());
  std::string text;
  for (const auto& p : paths) text += p + "\n";
  return text;
}

std::optional<std::vector<std::string>> parse_missing(const std::string_view text) {
  const auto lines = split_lines(text);
  if (!lines) return std::nullopt;
  return std::vector<std::string>(lines->begin(), lines->end());
}

}  // namespace secure_frontend
