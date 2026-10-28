#include "descendant_sweep.hpp"

#include "secure_frontend/deadline.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>

#if defined(__linux__)
#include <csignal>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace secure_frontend {

bool become_child_subreaper() noexcept {
#if defined(__linux__)
  return prctl(PR_SET_CHILD_SUBREAPER, 1UL, 0UL, 0UL, 0UL) == 0;
#else
  return false;
#endif
}

std::vector<long> list_own_children() {
  std::vector<long> children;
#if defined(__linux__)
  // increment(error), never the throwing operator++: this runs on the
  // supervisor thread, where an escaped exception would open the circuit (RF4).
  std::error_code error;
  for (std::filesystem::directory_iterator task("/proc/self/task", error), end; !error && task != end; task.increment(error)) {
    std::ifstream input(task->path() / "children");
    long child = 0;
    while (input >> child) children.push_back(child);
  }
  std::sort(children.begin(), children.end());
  children.erase(std::unique(children.begin(), children.end()), children.end());
#endif
  return children;
}

std::size_t sweep_orphans(const std::vector<long>& keep, const std::chrono::milliseconds bound) {
  std::size_t reaped = 0;
#if defined(__linux__)
  const auto deadline = deadline_after(bound);
  for (;;) {
    bool found = false;
    for (const long child : list_own_children()) {
      if (std::find(keep.begin(), keep.end(), child) != keep.end()) continue;
      found = true;
      (void)kill(static_cast<pid_t>(child), SIGKILL);
      int status = 0;
      if (waitpid(static_cast<pid_t>(child), &status, WNOHANG) == static_cast<pid_t>(child)) ++reaped;
    }
    if (!found || remaining_until(deadline).count() == 0) return reaped;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
#else
  (void)keep;
  (void)bound;
  return reaped;
#endif
}

}  // namespace secure_frontend
