#pragma once

#include "secure_frontend/engine_process.hpp"
#include "secure_frontend/engine_protocol_client.hpp"
#include "secure_frontend/exit_status.hpp"
#include "secure_frontend/supervised_process.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

namespace secure_frontend {

// Owns a Strata --serve child and provides its line-delimited stdin/stdout
// protocol.  It deliberately starts a supplied executable directly, with an
// environment allowlist, and never invokes a shell.
class EngineStdioProcess final : public EngineLineTransport, public SupervisedProcess {
 public:
  static constexpr std::size_t kMaxReadLineBytes = 8192;              // engine → frontend records are short
  static constexpr std::size_t kMaxWriteLineBytes = kProtocolMaxLineBytes;  // GEN line; one constant shared with the protocol client

  EngineStdioProcess() = default;
  ~EngineStdioProcess() override;
  EngineStdioProcess(const EngineStdioProcess&) = delete;
  EngineStdioProcess& operator=(const EngineStdioProcess&) = delete;

  // Inserts the literal --serve argument immediately after the executable unless
  // command.insert_serve is false (SP5 M1 Task 11: the pin's vision child takes no
  // --serve). The caller must not supply it; this placement is required by the GLM
  // runner and is accepted by the original Strata command-line parser.
  [[nodiscard]] std::optional<ProcessError> start(const EngineCommand& command);
  [[nodiscard]] bool running();
  // Sends SIGTERM to the child's process group, waits up to `grace`, then
  // SIGKILLs the group and reaps the leader for at most 500 ms (Windows:
  // terminates the Job with kSupervisorKillExitCode, waits up to `grace` for
  // the process and records its status through try_reap()). Returns true once
  // the leader is reaped (or was never started). Returns false ONLY if the leader is still unreaped after that
  // bound: the object is then "abandoned" — do not start() it again; the
  // supervisor applies spec V10 (no respawn until reaped; past reap_timeout
  // the circuit opens).
  [[nodiscard]] bool terminate(std::chrono::milliseconds grace);
  // Non-blocking: reaps the child once it has ended and keeps its raw status
  // (POSIX waitpid status; Windows GetExitCodeProcess). Later calls return the
  // same status. Lifecycle call: only the owning (supervisor) thread uses it.
  [[nodiscard]] std::optional<ExitStatus> try_reap() override;
  [[nodiscard]] std::optional<ExitStatus> exit_status() const noexcept { return exit_status_; }

  // Spec §3.10: chunks of kStdinWriteChunk; each waits at most
  // min(kStdinWriteDeadline, time left to `deadline`), so a write fails both
  // when a chunk stalls and when the whole record misses `deadline`.
  [[nodiscard]] bool write_line(std::string_view line, MonotonicClock::time_point deadline, std::string& error) override;
  [[nodiscard]] EngineReadResult read_line(std::chrono::milliseconds timeout) override;

  // SupervisedProcess (spec §3.1): lifecycle calls from the supervisor thread only.
  [[nodiscard]] EngineLineTransport& transport() noexcept override { return *this; }
  [[nodiscard]] std::intptr_t monitor_handle() const noexcept override;
  void close_stdin() noexcept override;
  void signal_group_terminate() noexcept override;
  void kill_group() noexcept override;
  void release_resources(std::chrono::milliseconds bound) noexcept override;
  [[nodiscard]] long pid() const noexcept override;
  // The generation's private temp directory (Task 18). release_resources()
  // removes it, which the supervisor calls only after the leader is reaped.
  void set_generation_temp_dir(std::filesystem::path directory) { generation_temp_dir_ = std::move(directory); }
  // Test seam: bytes read from the engine but not yet returned as a line.
  [[nodiscard]] std::size_t pending_bytes_for_tests() const noexcept { return pending_stdout_.size(); }

#if defined(_WIN32)
  // Test seams (spec N6, R1): the stdin pipe's name and the frontend's end of it.
  [[nodiscard]] const std::wstring& stdin_pipe_name_for_tests() const noexcept { return stdin_pipe_name_; }
  [[nodiscard]] void* stdin_handle_for_tests() const noexcept { return stdin_write_; }
#endif
#if !defined(_WIN32)
  // Test seam: the POSIX child pid, so tests can inspect /proc/<pid> after terminate.
  [[nodiscard]] int pid_for_tests() const noexcept { return process_id_; }
#endif

 private:
#if defined(_WIN32)
  void* process_handle_{nullptr};
  void* job_handle_{nullptr};
  void* stdin_write_{nullptr};
  void* stdout_read_{nullptr};
  std::wstring stdin_pipe_name_;
#else
  int process_id_{-1};
  int process_group_{-1};
  int stdin_write_{-1};
  int stdout_read_{-1};
  int pidfd_{-1};  // readable once the child exits (Linux 5.3+); -1: the supervisor polls
  // The generation's delegated cgroup (spec §3.7d); nullopt when not delegated.
  std::optional<std::filesystem::path> cgroup_dir_;
#endif
  std::string pending_stdout_;
  std::optional<ExitStatus> exit_status_;
  std::optional<std::filesystem::path> generation_temp_dir_;
  // Ruling T11-x: the EngineCommand's kill_exit_code, captured by start().
  std::uint32_t kill_exit_code_{kSupervisorKillExitCode};
  // The EngineCommand's max_read_line_bytes (kMaxReadLineBytes for an engine), captured by start().
  std::size_t max_read_line_{kMaxReadLineBytes};

  // Removes and validates one complete line from pending_stdout_, or returns
  // nullopt when no newline has arrived yet.
  [[nodiscard]] std::optional<EngineReadResult> take_pending_line();
  void cleanup() noexcept;
};

}  // namespace secure_frontend
