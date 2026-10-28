#include "secure_frontend/cgroup_scope.hpp"

#include "secure_frontend/deadline.hpp"

#include <fstream>
#include <string>
#include <thread>

#if defined(__linux__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace secure_frontend {

std::optional<EngineCgroups> EngineCgroups::detect() {
#if defined(__linux__)
  std::ifstream input("/proc/self/cgroup");
  std::string line;
  std::optional<std::string> relative;
  while (std::getline(input, line)) {
    if (line.rfind("0::", 0) == 0) relative = line.substr(3);
  }
  if (!relative || relative->size() < 2U || relative->front() != '/') return std::nullopt;  // "/" is the root cgroup: never ours
  const std::filesystem::path root = std::filesystem::path("/sys/fs/cgroup") / relative->substr(1);
  if (access((root / "cgroup.procs").c_str(), W_OK) != 0) return std::nullopt;
  // cgroup v2 "no internal processes": a cgroup that already enables
  // controllers for its children (non-empty cgroup.subtree_control) cannot
  // also hold this process next to per-generation child cgroups. Not ours to use.
  {
    std::ifstream subtree(root / "cgroup.subtree_control");
    if (!subtree) return std::nullopt;
    std::string controllers;
    std::getline(subtree, controllers);
    if (controllers.find_first_not_of(" \t") != std::string::npos) return std::nullopt;
  }
  const auto probe = root / ("strata-probe-" + std::to_string(getpid()));
  if (mkdir(probe.c_str(), 0700) != 0) return std::nullopt;
  const bool has_kill = access((probe / "cgroup.kill").c_str(), W_OK) == 0;
  (void)rmdir(probe.c_str());
  if (!has_kill) return std::nullopt;
  return EngineCgroups(root);
#else
  return std::nullopt;
#endif
}

std::filesystem::path EngineCgroups::generation_directory(const std::uint64_t generation, const long frontend_pid) const {
  return root_ / ("strata-engine-" + std::to_string(frontend_pid) + "-" + std::to_string(generation));
}

std::filesystem::path EngineCgroups::generation_directory(const std::uint64_t generation) const {
#if defined(__linux__)
  return generation_directory(generation, static_cast<long>(getpid()));
#else
  return generation_directory(generation, -1L);
#endif
}

std::optional<std::filesystem::path> EngineCgroups::prepare_generation(const std::uint64_t generation) const {
#if defined(__linux__)
  const auto directory = generation_directory(generation);
  if (mkdir(directory.c_str(), 0700) != 0) return std::nullopt;  // EEXIST included: never join a directory we did not create
  return directory / "cgroup.procs";
#else
  (void)generation;
  return std::nullopt;
#endif
}

bool kill_cgroup(const std::filesystem::path& directory) noexcept {
#if defined(__linux__)
  const int fd = open((directory / "cgroup.kill").c_str(), O_WRONLY | O_CLOEXEC);
  if (fd < 0) return false;
  const bool written = write(fd, "1", 1) == 1;
  (void)close(fd);
  return written;
#else
  (void)directory;
  return false;
#endif
}

bool remove_cgroup(const std::filesystem::path& directory, const std::chrono::milliseconds bound) noexcept {
#if defined(__linux__)
  const auto deadline = deadline_after(bound);
  for (;;) {
    std::ifstream events(directory / "cgroup.events");
    std::string key;
    std::string value;
    bool populated = true;
    while (events >> key >> value) {
      if (key == "populated") populated = value != "0";
    }
    if (!populated) return rmdir(directory.c_str()) == 0;
    if (remaining_until(deadline).count() == 0) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
#else
  (void)directory;
  (void)bound;
  return false;
#endif
}

}  // namespace secure_frontend
