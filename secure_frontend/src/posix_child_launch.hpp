#pragma once

// Internal, POSIX-only helpers shared by EngineProcess and EngineStdioProcess.
//
// Everything that allocates (argv, envp, the executable path, the descriptor
// sweep limit) is prepared BEFORE fork().  The child body, exec_child(), uses
// only async-signal-safe calls (prctl, getppid, setpgid, open, fcntl, dup2, chdir, close, syscall,
// sigaction, sigprocmask, execve, write, _exit), so it is safe even when other
// parent threads held the allocator lock at the moment of fork().
//
// SP4 launch seam (SP2 R4; SP4 spec §3.2; controller rulings RF8, RF11). A
// launcher other than the engine (SP4's confined downloader) passes
// ExecOptions as exec_child's last argument; the engine launchers pass none
// (ExecOptions{}), which is byte-for-byte the behaviour before the seam.
//   keep_fds  descriptors the child keeps. At the start of exec_child, before
//             the 0..kReportFd dup2 sequence, each is lifted with
//             fcntl(fd, F_DUPFD_CLOEXEC, kLiftFloor), so a parent descriptor
//             numbered 0..3 is never overwritten. The sweep spares the lifted
//             numbers; they stay close-on-exec, so only what the hook dup2s
//             (dup2 clears FD_CLOEXEC on the target) survives execve. At most
//             kMaxKeepFds entries (the child cannot allocate).
//   pre_exec  called after the sweep and the signal reset, immediately before
//             execve, with the lifted numbers in keep_fds order and `ctx`. It
//             must be async-signal-safe, and it must NOT close or dup2 onto
//             any of fds 0..kReportFd: a hook that clobbers kReportFd could
//             make a failure (its own or execve's) read as success (EOF).
//             0 = go on; non-zero (an errno value) =
//             the child writes kHookFailureMarker then that int (raw bytes) on
//             kReportFd and _exits 127. A keep_fds entry that cannot be lifted
//             is reported the same way with the fcntl errno (fail closed).
// Report format on the report pipe: nothing (EOF) = execve succeeded;
// sizeof(int) bytes = errno of a failed launch step or of execve (unchanged
// from before the seam); 1 + sizeof(int) bytes starting with 'H' = hook or
// lift failure. The parent reads it with read_report() and classifies it with
// classify_report(); both engine launchers report the two kinds with distinct
// texts (describe_report()). SP2 never reserves or touches fd 4.

#if !defined(_WIN32)

#include "secure_frontend/engine_process.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif

namespace secure_frontend::posix_launch {

// The child reports a pre-exec failure's errno through this descriptor.  It is
// close-on-exec, so a successful execve closes it and the parent reads EOF.
inline constexpr int kReportFd = 3;
// Descriptors the child needs are first lifted to >= this floor, so the dup2
// calls onto 0..kReportFd can never overwrite one of their own sources (which
// happens when the parent runs with some of 0..2 closed).
inline constexpr int kLiftFloor = 10;
// Upper bound for the fallback descriptor sweep, which closes one descriptor
// at a time below min(sysconf(_SC_OPEN_MAX), kSweepCap) (kSweepDefault when
// that is unknown). The fallback runs only where close_range(2) is missing or
// fails (Linux before 5.9, headers without SYS_close_range, or a non-Linux
// POSIX system); a sweep to an unbounded RLIMIT_NOFILE could take seconds per
// spawn. The listening and accepted sockets are close-on-exec since SP3 Task 8b
// (SOCK_CLOEXEC / accept4, FD_CLOEXEC elsewhere), so the sweep is defence in
// depth for them; a descriptor opened without O_CLOEXEC and numbered at or
// above kSweepCap would still reach the engine on this fallback path.
// close_range covers every number, so Linux 5.9+ has no such gap.
inline constexpr long kSweepCap = 65536;
inline constexpr int kSweepDefault = 1024;

// SP4 seam (see the top of this file). The hook returns 0 or an errno value.
using PreExecHook = int (*)(const void* ctx, std::span<const int> lifted) noexcept;
struct ExecOptions final {
  std::span<const int> keep_fds;  // lifted with F_DUPFD_CLOEXEC >= kLiftFloor before the 0..3 dup2 sequence; spared by the sweep
  PreExecHook pre_exec{nullptr};  // after the sweep, before execve; non-zero → 'H' + errno on kReportFd, _exit
  const void* ctx{nullptr};
};
// First byte of a hook (or keep_fds lift) failure report; an execve failure has no marker.
inline constexpr char kHookFailureMarker = 'H';
// The child lifts into a fixed array (no allocation after fork()); more entries fail closed with E2BIG.
inline constexpr std::size_t kMaxKeepFds = 16;

// argv/envp point into the storage vectors, so this object is filled in place
// and never copied or moved.
struct PreparedExec {
  PreparedExec() = default;
  PreparedExec(const PreparedExec&) = delete;
  PreparedExec& operator=(const PreparedExec&) = delete;
  // The parent's copy of the cgroup descriptor closes when `start` returns.
  // The forked child never runs this: it execs or _exits.
  ~PreparedExec() {
    if (cgroup_fd >= 0) (void)close(cgroup_fd);
  }

  std::string executable;
  std::vector<std::string> argument_storage;
  std::vector<char*> argv;
  std::vector<std::string> environment_storage;
  std::vector<char*> envp;
  int sweep_limit{kSweepDefault};
  // The forking process, recorded before fork(); the child compares getppid() to it.
  pid_t expected_parent{-1};
  // Linux, delegated cgroup v2 only: the generation's cgroup.procs, opened
  // before fork() by open_cgroup(); -1 = no cgroup.
  int cgroup_fd{-1};
  // The engine's working directory (spec §3.12); empty: inherit the parent's.
  std::string working_directory{};
  // Linux: PR_SET_NO_NEW_PRIVS before execve (spec §3.12, G2).
  bool no_new_privileges{false};
};

inline void prepare(PreparedExec& out, const EngineCommand& command, const bool insert_serve) {
  out.expected_parent = getpid();
  out.executable = command.executable.string();
  out.working_directory = command.working_directory.string();
  out.no_new_privileges = command.no_new_privileges;
  out.argument_storage.reserve(command.arguments.size() + 2);
  out.argument_storage.push_back(out.executable);
  if (insert_serve) out.argument_storage.emplace_back("--serve");
  out.argument_storage.insert(out.argument_storage.end(), command.arguments.begin(), command.arguments.end());
  out.argv.reserve(out.argument_storage.size() + 1);
  for (auto& argument : out.argument_storage) out.argv.push_back(argument.data());
  out.argv.push_back(nullptr);
  out.environment_storage.reserve(command.environment.size());
  for (const auto& [name, value] : command.environment) out.environment_storage.push_back(name + "=" + value);
  out.envp.reserve(out.environment_storage.size() + 1);
  for (auto& entry : out.environment_storage) out.envp.push_back(entry.data());
  out.envp.push_back(nullptr);
  const long open_max = sysconf(_SC_OPEN_MAX);
  out.sweep_limit = open_max > 0 ? static_cast<int>(std::min(open_max, kSweepCap)) : kSweepDefault;
}

// Opens the generation's cgroup.procs (close-on-exec) when the command names
// one; called after prepare() (which stays void: SP4's seam calls it unchanged).
// False = the cgroup cannot be joined; the caller refuses to start. A
// prepare_generation() that returned nullopt leaves cgroup_procs empty, and
// the generation runs without a cgroup.
[[nodiscard]] inline bool open_cgroup(PreparedExec& out, const EngineCommand& command) {
  if (command.cgroup_procs) {
    out.cgroup_fd = open(command.cgroup_procs->c_str(), O_WRONLY | O_CLOEXEC);
    if (out.cgroup_fd < 0) return false;
  }
  return true;
}

// Spec §3.12, parent side: every signal is blocked in the forking thread from
// just before fork() until fork() returns, so no handler (the shutdown
// self-pipe) can run in the child between fork and exec.
class BlockAllSignals final {
 public:
  BlockAllSignals() noexcept {
    sigset_t all;
    sigfillset(&all);
    (void)pthread_sigmask(SIG_SETMASK, &all, &previous_);
  }
  ~BlockAllSignals() { (void)pthread_sigmask(SIG_SETMASK, &previous_, nullptr); }
  BlockAllSignals(const BlockAllSignals&) = delete;
  BlockAllSignals& operator=(const BlockAllSignals&) = delete;

 private:
  sigset_t previous_{};
};

// Spec §3.12, child side (async-signal-safe): SIG_IGN survives execve, so put
// every disposition back to SIG_DFL (the engine gets the default SIGPIPE, not
// the frontend's ignore), then clear the mask inherited from the parent.
inline void reset_signal_state() noexcept {
  struct sigaction defaults {};
  defaults.sa_handler = SIG_DFL;
  sigemptyset(&defaults.sa_mask);
  for (int signal_number = 1; signal_number < NSIG; ++signal_number) {
    if (signal_number == SIGKILL || signal_number == SIGSTOP) continue;
    (void)sigaction(signal_number, &defaults, nullptr);
  }
  sigset_t none;
  sigemptyset(&none);
  (void)sigprocmask(SIG_SETMASK, &none, nullptr);
}

[[noreturn]] inline void fail(const int report_fd) noexcept {
  const int saved_errno = errno;
  (void)!write(report_fd, &saved_errno, sizeof(saved_errno));
  _exit(127);
}

// One write of kHookFailureMarker + the errno, so the report arrives whole
// (a pipe write of at most PIPE_BUF bytes is atomic).
[[noreturn]] inline void fail_hook(const int report_fd, const int error) noexcept {
  char bytes[1 + sizeof(int)];
  bytes[0] = kHookFailureMarker;
  const auto* raw = reinterpret_cast<const char*>(&error);
  for (std::size_t index = 0; index < sizeof(int); ++index) bytes[1 + index] = raw[index];
  (void)!write(report_fd, bytes, sizeof(bytes));
  _exit(127);
}

inline int lift(const int fd) noexcept { return fd < 0 ? -1 : fcntl(fd, F_DUPFD_CLOEXEC, kLiftFloor); }

// Closes every descriptor above kReportFd except the `count` numbers in
// `kept` (sorted ascending, all > kReportFd). close_range per gap where the
// kernel has it, else (or if it fails) one close() per descriptor below the
// sweep limit, as before the seam.
inline void close_above_report(const int* kept, const std::size_t count, const int sweep_limit) noexcept {
  bool swept = false;
#if defined(__linux__) && defined(SYS_close_range)
  swept = true;
  unsigned first = static_cast<unsigned>(kReportFd + 1);
  for (std::size_t index = 0; index < count && swept; ++index) {
    const auto keep = static_cast<unsigned>(kept[index]);
    if (keep > first) swept = syscall(SYS_close_range, first, keep - 1U, 0U) == 0;
    first = keep + 1U;
  }
  swept = swept && syscall(SYS_close_range, first, ~0U, 0U) == 0;
#endif
  if (swept) return;
  std::size_t next = 0;
  for (int fd = kReportFd + 1; fd < sweep_limit; ++fd) {
    while (next < count && kept[next] < fd) ++next;
    if (next < count && kept[next] == fd) continue;
    (void)close(fd);
  }
}

// Runs in the forked child only.  stdin_fd/stdout_fd of -1 keep the inherited
// descriptor; null_stderr points fd 2 at /dev/null.  Every descriptor above
// kReportFd is closed before execve (except the lifted keep_fds), and
// kReportFd itself is close-on-exec. `options`: the SP4 seam (top of file);
// the default is the engine's behaviour.
[[noreturn]] inline void exec_child(const PreparedExec& prepared, const int stdin_fd, const int stdout_fd,
                                    const bool null_stderr, const int report_fd, const ExecOptions& options = {}) noexcept {
#if defined(__linux__)
  // Spec §3.7(c): the leader dies with the frontend. PDEATHSIG fires when the
  // forking *thread* exits, which is why only the long-lived supervisor thread
  // forks engines. If the parent already died between fork() and here,
  // getppid() differs and the child exits instead of running orphaned.
  if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0) fail(report_fd);
  if (getppid() != prepared.expected_parent) _exit(127);
#endif
  if (setpgid(0, 0) != 0) fail(report_fd);
  // Spec §3.7(d): join the generation's delegated cgroup before anything runs.
  if (prepared.cgroup_fd >= 0 && write(prepared.cgroup_fd, "0", 1) != 1) fail(report_fd);
  const int report = lift(report_fd);
  if (report < 0) fail(report_fd);
  // SP4 seam: lift the kept descriptors before anything is dup2'd onto 0..3,
  // so a parent descriptor numbered 0..3 is still the parent's file here.
  int lifted[kMaxKeepFds];
  const std::size_t kept = options.keep_fds.size();
  if (kept > kMaxKeepFds) fail_hook(report, E2BIG);
  for (std::size_t index = 0; index < kept; ++index) {
    lifted[index] = options.keep_fds[index] < 0 ? -1 : lift(options.keep_fds[index]);
    if (lifted[index] < 0) fail_hook(report, options.keep_fds[index] < 0 ? EBADF : errno);
  }
  const int in = lift(stdin_fd);
  const int out = lift(stdout_fd);
  if ((stdin_fd >= 0 && in < 0) || (stdout_fd >= 0 && out < 0)) fail(report);
  int err = -1;
  if (null_stderr) {
    const int sink = open("/dev/null", O_WRONLY | O_CLOEXEC);
    err = lift(sink);
    if (err < 0) fail(report);
  }
  // dup2 onto a different descriptor clears FD_CLOEXEC on the target, so 0..2
  // survive execve; the report descriptor gets FD_CLOEXEC back explicitly.
  if ((in >= 0 && dup2(in, STDIN_FILENO) < 0) || (out >= 0 && dup2(out, STDOUT_FILENO) < 0) ||
      (err >= 0 && dup2(err, STDERR_FILENO) < 0) || dup2(report, kReportFd) < 0 ||
      fcntl(kReportFd, F_SETFD, FD_CLOEXEC) != 0) {
    fail(report);
  }
  // Spec §3.12: the engine's cwd-relative files land in <state_dir>/engine.
  if (!prepared.working_directory.empty() && chdir(prepared.working_directory.c_str()) != 0) fail(kReportFd);
#if defined(__linux__)
  // Spec §3.12 (G2): neither the engine nor anything it execs gains privilege
  // through setuid/setgid bits or file capabilities.
  if (prepared.no_new_privileges && prctl(PR_SET_NO_NEW_PRIVS, 1UL, 0UL, 0UL, 0UL) != 0) fail(kReportFd);
#endif
  // The sweep spares the lifted descriptors: sort a copy (insertion sort, no allocation).
  int spared[kMaxKeepFds];
  for (std::size_t index = 0; index < kept; ++index) {
    std::size_t at = index;
    for (; at > 0 && spared[at - 1] > lifted[index]; --at) spared[at] = spared[at - 1];
    spared[at] = lifted[index];
  }
  close_above_report(spared, kept, prepared.sweep_limit);
  reset_signal_state();
  // SP4 seam: the hook runs last, immediately before execve.
  if (options.pre_exec != nullptr) {
    const int hook_error = options.pre_exec(options.ctx, std::span<const int>(lifted, kept));
    if (hook_error != 0) fail_hook(kReportFd, hook_error);
  }
  execve(prepared.executable.c_str(), prepared.argv.data(), prepared.envp.data());
  fail(kReportFd);
}

// ---- Parent side: the report pipe ------------------------------------------
enum class ChildReportKind { none, exec_failed, hook_failed, malformed };
struct ChildReport final {
  ChildReportKind kind{ChildReportKind::none};
  int error{0};
};

// Reads the report pipe to EOF (EINTR retried), keeping at most 16 bytes (it
// stops reading there; any report that long is malformed anyway, the largest
// valid one is 1 + sizeof(int)). Empty = the child's execve succeeded.
inline std::string read_report(const int fd) {
  constexpr std::size_t kMaxReportBytes = 16;
  std::string bytes;
  char buffer[kMaxReportBytes];
  while (bytes.size() < kMaxReportBytes) {
    const ssize_t count = read(fd, buffer, kMaxReportBytes - bytes.size());
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    bytes.append(buffer, static_cast<std::size_t>(count));
  }
  return bytes;
}

// SP4 classifies the same way: 1 + sizeof(int) bytes starting with 'H' is a
// hook (or keep_fds lift) failure; exactly sizeof(int) bytes is a launch-step
// or execve failure; nothing is success; anything else is malformed (never success).
inline ChildReport classify_report(const std::string_view bytes) noexcept {
  ChildReport report;
  if (bytes.empty()) return report;
  if (bytes.size() == 1U + sizeof(int) && bytes.front() == kHookFailureMarker) {
    report.kind = ChildReportKind::hook_failed;
    std::memcpy(&report.error, bytes.data() + 1, sizeof(int));
  } else if (bytes.size() == sizeof(int)) {
    report.kind = ChildReportKind::exec_failed;
    std::memcpy(&report.error, bytes.data(), sizeof(int));
  } else {
    report.kind = ChildReportKind::malformed;
  }
  return report;
}

// The spawn-failure text both engine launchers return. "execve failed: ..." is
// the text from before the seam.
inline std::string describe_report(const ChildReport& report) {
  switch (report.kind) {
    case ChildReportKind::exec_failed:
      return std::string("execve failed: ") + std::strerror(report.error);
    case ChildReportKind::hook_failed:
      return std::string("pre-exec hook failed: ") + std::strerror(report.error);
    case ChildReportKind::malformed:
      return "child launch report malformed";
    case ChildReportKind::none:
      break;
  }
  return {};
}

}  // namespace secure_frontend::posix_launch

#endif  // !defined(_WIN32)
