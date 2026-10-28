#pragma once

#include "secure_frontend/supervisor_wake.hpp"

namespace secure_frontend {

// Spec §3.7: SIGTERM/SIGINT (POSIX) and CTRL_C/CTRL_BREAK/CTRL_CLOSE (Windows
// console) set a process-wide flag and fire `wake`, so the supervisor thread
// runs the orderly shutdown. Install once, after the supervisor exists. The
// POSIX handler is async-signal-safe and preserves errno.
void install_shutdown_signals(SupervisorWake& wake) noexcept;
// Stops firing `wake` (the flag stays). Called by ~EngineSupervisor once its
// thread is joined, so a late signal never touches a destroyed wake.
void release_shutdown_signals(const SupervisorWake& wake) noexcept;
[[nodiscard]] bool shutdown_signalled() noexcept;
// Windows: lets a console handler that is holding the process open return.
void notify_shutdown_complete() noexcept;

}  // namespace secure_frontend
