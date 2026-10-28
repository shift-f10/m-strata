#include "secure_frontend/staging_plan.hpp"

#include "secure_frontend/model_index_model.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace secure_frontend {

StagingPlan plan_staging(const AcquireManifest& manifest, const std::span<const StagedEntry> staged) {
  std::map<std::string, const StagedEntry*, std::less<>> by_path;
  for (const auto& entry : staged) by_path[entry.path] = &entry;
  std::set<std::string> keep{std::string{kStagingManifestName}};
  std::set<std::string> remove;
  StagingPlan plan;
  for (std::size_t i = 0; i < manifest.files.size(); ++i) {
    const auto& file = manifest.files[i];
    const std::string part = file.path + std::string{kPartSuffix};
    const auto final_it = by_path.find(file.path);
    const auto part_it = by_path.find(part);
    const bool final_ok = final_it != by_path.end() && final_it->second->regular && final_it->second->size == file.size;
    if (final_it != by_path.end() && !final_ok) remove.insert(file.path);
    if (final_ok) {
      keep.insert(file.path);
      if (part_it != by_path.end()) remove.insert(part);
      plan.files.push_back({i, FileStep::rehash_verified_candidate, file.size});
      plan.staged_bytes += file.size;
      continue;
    }
    if (part_it == by_path.end()) {
      plan.files.push_back({i, FileStep::fetch_from_zero, 0});
      continue;
    }
    const auto& entry = *part_it->second;
    if (!entry.regular || entry.size > file.size) {
      remove.insert(part);
      plan.files.push_back({i, FileStep::fetch_from_zero, 0});
      continue;
    }
    keep.insert(part);
    plan.files.push_back({i, entry.size == file.size ? FileStep::rehash_complete_part : FileStep::rehash_then_resume, entry.size});
    plan.staged_bytes += entry.size;
  }
  for (const auto& entry : staged) {
    if (!keep.contains(entry.path) || !entry.regular) remove.insert(entry.path);
  }
  remove.erase(std::string{kStagingManifestName});
  plan.remove.assign(remove.begin(), remove.end());
  return plan;
}

std::vector<StagingKeyInfo> abandoned_keys(const std::span<const StagingKeyInfo> keys, const std::string_view own_key,
                                           const std::string_view name, const std::string_view tag) {
  std::vector<StagingKeyInfo> out;
  for (const auto& key : keys) {
    if (key.key != own_key && key.name == name && key.tag == tag) out.push_back(key);
  }
  return out;
}

}  // namespace secure_frontend
