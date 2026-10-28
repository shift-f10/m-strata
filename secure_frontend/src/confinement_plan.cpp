#include "secure_frontend/confinement_plan.hpp"

#include <algorithm>

namespace secure_frontend {

LandlockRights landlock_rights_for_abi(const int abi) noexcept {
  namespace la = landlock_access;
  std::uint64_t fs = (la::kFsMakeSym << 1U) - 1U;  // ABI 1: bits 0..12
  if (abi >= 2) fs |= la::kFsRefer;
  if (abi >= 3) fs |= la::kFsTruncate;
  if (abi >= 5) fs |= la::kFsIoctlDev;
  const std::uint64_t net = abi >= 4 ? (la::kNetBindTcp | la::kNetConnectTcp) : 0U;
  return LandlockRights{fs, net, la::kFsExecute | la::kFsReadFile | la::kFsReadDir, la::kFsReadFile};
}

bool path_is_beneath(const std::string_view dir, const std::string_view path) noexcept {
  if (!path.starts_with(dir)) return false;
  return path.size() == dir.size() || dir == "/" || path[dir.size()] == '/';
}

ConfinementPlan plan_linux_confinement(const std::string_view downloader_canonical,
                                       const std::span<const ProtectedPath> protected_paths,
                                       const std::function<bool(std::string_view)>& directory_exists) {
  ConfinementPlan plan;
  for (const auto dir : kLandlockSystemDirs) {
    if (directory_exists(dir)) plan.read_dirs.emplace_back(dir);
  }
  const auto slash = downloader_canonical.rfind('/');
  const std::string parent{downloader_canonical.substr(0, slash == 0 ? 1 : slash)};
  const bool covered = std::any_of(plan.read_dirs.begin(), plan.read_dirs.end(),
                                   [&](const std::string& d) { return path_is_beneath(d, parent); });
  if (!covered) plan.read_dirs.push_back(parent);
  for (const auto& p : protected_paths) {
    const bool readable = std::any_of(plan.read_dirs.begin(), plan.read_dirs.end(),
                                      [&](const std::string& d) { return path_is_beneath(d, p.canonical); });
    if (readable) {
      plan.readable_key = p.key_path;
      break;
    }
  }
  return plan;
}

std::string confinement_label(const bool landlock_available, const std::optional<std::string>& readable_key) {
  if (!landlock_available) return "reduced:no_landlock";
  if (readable_key) return "reduced:readable:" + *readable_key;
  return "full";
}

std::vector<std::uint8_t> build_crt_fd_block(const std::span<const std::uint64_t, 5> handles,
                                             const std::span<const std::uint8_t, 5> flags, const std::size_t handle_width) {
  std::vector<std::uint8_t> block;
  block.reserve(4 + kCrtBlockEntries * (1 + handle_width));
  block.push_back(static_cast<std::uint8_t>(kCrtBlockEntries));
  block.insert(block.end(), 3, std::uint8_t{0});
  for (const std::uint8_t flag : flags) block.push_back(flag);
  for (const std::uint64_t handle : handles) {
    for (std::size_t i = 0; i < handle_width; ++i) block.push_back(static_cast<std::uint8_t>(handle >> (8U * i)));
  }
  return block;
}

}  // namespace secure_frontend
