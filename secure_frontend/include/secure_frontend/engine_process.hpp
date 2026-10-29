#pragma once

#include "secure_frontend/exit_status.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace secure_frontend {

// Environment is an allowlist: no parent environment variables are inherited.
struct EngineCommand final {
  std::filesystem::path executable;
  std::vector<std::string> arguments;
  std::map<std::string, std::string, std::less<>> environment;
  // Linux, delegated cgroup v2 only: the child writes itself into this
  // cgroup.procs before execve, so even its escaping descendants stay inside.
  // `{}`: existing designated initializers that omit it keep compiling (-Werror).
  std::optional<std::filesystem::path> cgroup_procs{};
  // The child's working directory (spec §3.12): the engine writes its
  // cwd-relative `pack/full` default and `strata-stall-<pid>.dmp` there. Empty: inherit.
  // `{}`: existing designated initializers that omit it keep compiling (-Werror).
  std::filesystem::path working_directory{};
  // Linux: PR_SET_NO_NEW_PRIVS before execve (spec §3.12). The server turns it
  // on by default (owner decision 2026-10-13); --allow-new-privileges turns it off.
  // `{false}`: existing designated initializers that omit it keep compiling (-Werror).
  bool no_new_privileges{false};
  // SP5 M1 Task 11 (Ruling T11-x): the Windows TerminateJobObject code a supervisor
  // kill leaves on this child — kSupervisorKillExitCode for an engine, kVisionKillExitCode
  // for the vision role (distinct exit-code space). `{}`-default pattern as above.
  std::uint32_t kill_exit_code{kSupervisorKillExitCode};
  // SP5 M1 Task 11 (Ruling T11-ff): start() inserts the literal --serve unless this is
  // false. The pin's vision child takes no --serve (reader_args, serve/server.py:2556-2568),
  // so the vision role starts with false. `{true}`: existing designated initializers
  // that omit it keep compiling (-Werror).
  bool insert_serve{true};
  // The longest line the child may write (EngineStdioProcess::kMaxReadLineBytes for an engine, whose records are short);
  // the MCP search child writes a tool's result as one JSON line (web search, spec 2026-10-27-web-search-design §2).
  std::size_t max_read_line_bytes{8192U};
  // The Monitor's conversation cache (owner 2026-10-28; upstream ConvCacheLog): EngineStdioProcess reads the child's
  // stderr through a pipe and keeps only the numbers of its conversation cache lines (conversation_cache_log.hpp);
  // every line is dropped, none is kept or logged. Set for the Qwen engine only, the one that prints them. false:
  // stderr goes to NUL / /dev/null. `{false}`: designated initializers that omit it keep compiling (-Werror).
  bool read_conversation_cache{false};
  // The engine's own output (owner 2026-10-29, logging.engine_output: to see why an engine fails on a new GPU):
  // EngineStdioProcess reads the child's stderr through the same pipe and hands each line here, in order, as
  // printable_stderr_line made it. Called on the drain's thread. Empty: no stderr text leaves the drain. `{}`:
  // designated initializers that omit it keep compiling (-Werror).
  std::function<void(std::string_view)> stderr_lines{};
};

enum class ProcessErrorCode {
  invalid_command,
  already_running,
  spawn_failed,
  platform_failure,
};

struct ProcessError final {
  ProcessErrorCode code;
  std::string message;
};

// Owns one engine child and its process group (or Windows Job Object).
// start() always invokes the supplied executable directly; it never uses a shell.
// Test-only (SP0): no production code uses it; the server's engine runs under
// EngineStdioProcess and EngineSupervisor. It is deliberately not C1-hardened.
class EngineProcess final {
 public:
  EngineProcess() = default;
  ~EngineProcess();
  EngineProcess(const EngineProcess&) = delete;
  EngineProcess& operator=(const EngineProcess&) = delete;
  EngineProcess(EngineProcess&& other) noexcept;
  EngineProcess& operator=(EngineProcess&& other) noexcept;

  [[nodiscard]] std::optional<ProcessError> start(const EngineCommand& command);
  [[nodiscard]] bool running();
  // Returns an exit code when the child exits; nullopt means the timeout elapsed.
  [[nodiscard]] std::optional<int> wait_for(std::chrono::milliseconds timeout);
  // Terminates the whole child group, waits at most grace, then force-kills it.
  // POSIX: the group SIGKILL is sent only while the leader is unreaped. Unlike
  // spec C1 (EngineStdioProcess) it does not always SIGKILL the group: wait_for
  // reaps the leader, so a later group kill could hit a recycled pgid. Kept as
  // is by design (controller ruling RF12); used by tests only.
  [[nodiscard]] bool terminate(std::chrono::milliseconds grace);

 private:
#if defined(_WIN32)
  void* process_handle_{nullptr};
  void* job_handle_{nullptr};
#else
  int process_id_{-1};
#endif
  std::optional<int> exit_code_;

  void cleanup() noexcept;
};

}  // namespace secure_frontend
