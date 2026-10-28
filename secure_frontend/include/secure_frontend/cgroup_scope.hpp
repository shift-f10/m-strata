#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <utility>

namespace secure_frontend {

// Linux cgroup v2 (spec §3.7d): used only when this process already runs in a
// delegated cgroup it can write to (systemd `Delegate=yes`, `systemd-run --user
// --scope`); never created with privileges. Off everywhere else.
class EngineCgroups final {
 public:
  // nullopt unless /proc/self/cgroup names a v2 cgroup whose cgroup.procs this
  // process can write and below which it can create a child with cgroup.kill (5.14+).
  [[nodiscard]] static std::optional<EngineCgroups> detect();
  // Creates generation_directory(generation) (0700) and returns its
  // cgroup.procs; nullopt when it cannot be created or already exists (a
  // directory this frontend did not just create is never joined).
  [[nodiscard]] std::optional<std::filesystem::path> prepare_generation(std::uint64_t generation) const;
  // <root>/strata-engine-<frontend pid>-<generation>: unique per frontend, so
  // two frontends in one delegated cgroup never share a generation directory.
  [[nodiscard]] std::filesystem::path generation_directory(std::uint64_t generation) const;
  [[nodiscard]] std::filesystem::path generation_directory(std::uint64_t generation, long frontend_pid) const;
  [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

 private:
  explicit EngineCgroups(std::filesystem::path root) : root_(std::move(root)) {}
  std::filesystem::path root_;
};

// How long removing a generation's cgroup may wait for "populated 0" when the
// caller has no larger bound (shutdown, exception path, spawn failure). Inside
// the 5 s shutdown budget: 2 + 1 + 1 + 0.25 + 0.1 = 4.35 s worst case.
inline constexpr std::chrono::milliseconds kCgroupRemoveBound{100};

// Writes "1" to <directory>/cgroup.kill: every member dies, escapes included.
bool kill_cgroup(const std::filesystem::path& directory) noexcept;
// Waits up to `bound` for "populated 0" in cgroup.events, then removes the directory.
bool remove_cgroup(const std::filesystem::path& directory, std::chrono::milliseconds bound) noexcept;

}  // namespace secure_frontend
