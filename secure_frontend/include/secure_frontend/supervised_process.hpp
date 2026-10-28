#pragma once

#include "secure_frontend/engine_protocol_client.hpp"
#include "secure_frontend/exit_status.hpp"
#include "secure_frontend/supervisor_wake.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>

namespace secure_frontend {

// What the supervisor needs from one engine child. Thread contract (spec
// §3.1): transport() is used by the serving thread while it holds a Lease;
// every other member is called only by the supervisor thread.
class SupervisedProcess {
 public:
  virtual ~SupervisedProcess() = default;
  [[nodiscard]] virtual EngineLineTransport& transport() noexcept = 0;
  // Non-blocking; reaps the child once it has ended and returns its raw status
  // (the same status on every later call).
  [[nodiscard]] virtual std::optional<ExitStatus> try_reap() = 0;
  // POSIX pidfd or Windows process HANDLE for the monitor; kNoMonitorHandle when none.
  [[nodiscard]] virtual std::intptr_t monitor_handle() const noexcept = 0;
  // Closes the frontend's end of engine stdin (the engine exits 0 on EOF).
  virtual void close_stdin() noexcept = 0;
  // POSIX: SIGTERM to the process group. Windows: no-op (no console signal reaches the engine).
  virtual void signal_group_terminate() noexcept = 0;
  // POSIX: SIGKILL to the process group, even after the leader was reaped
  // (spec §3.7a). Windows: TerminateJobObject(kSupervisorKillExitCode). Never waits.
  virtual void kill_group() noexcept = 0;
  // After the leader is reaped: forget the group id and release per-generation
  // resources (delegated cgroup), waiting at most `bound`.
  virtual void release_resources(std::chrono::milliseconds bound) noexcept = 0;
  // Leader pid for logs and sweeps; -1 when unknown.
  [[nodiscard]] virtual long pid() const noexcept = 0;
};

// A spawn that did not produce a running child. `message` is fixed text, no paths.
struct SpawnFailure final {
  std::string message;
};

using SpawnResult = std::variant<std::unique_ptr<SupervisedProcess>, SpawnFailure>;

// Called only on the supervisor thread: PR_SET_PDEATHSIG fires when the
// forking thread exits, so every engine is forked from that long-lived thread.
using ProcessFactory = std::function<SpawnResult(std::uint64_t generation)>;

}  // namespace secure_frontend
