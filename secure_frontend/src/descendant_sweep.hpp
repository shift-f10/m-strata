#pragma once

#include <chrono>
#include <cstddef>
#include <vector>

namespace secure_frontend {

// Linux (spec §3.7b): orphaned descendants of an engine re-parent to this
// process instead of init, so they can be found and reaped. No-op elsewhere.
bool become_child_subreaper() noexcept;
// Linux: this process's direct children (from every thread's
// /proc/self/task/<tid>/children). Empty elsewhere.
std::vector<long> list_own_children();
// Linux: SIGKILLs and reaps every direct child not in `keep`, repeating until
// none is left or `bound` passes. Returns how many were reaped. The production
// frontend has no children but engines (C7), so `keep` is the live leaders.
std::size_t sweep_orphans(const std::vector<long>& keep, std::chrono::milliseconds bound);

}  // namespace secure_frontend
