#include "secure_frontend/downloader.hpp"

#if !defined(_WIN32)
#include "landlock_linux.hpp"
#include "posix_child_launch.hpp"
#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/engine_process.hpp"
#include "secure_frontend/test_hooks.hpp"

#include <cerrno>
#include <csignal>
#include <ctime>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif

namespace secure_frontend {
namespace {

// Built before fork(); read by the async-signal-safe pre-exec hook.
struct HookContext final {
  int ruleset_index;  // position in keep_fds, -1 = none (reduced: no_landlock)
  int record_index;   // position in keep_fds, -1 = none (release builds)
  const char* cwd;
  bool fail_injected;
};

// §3.2 hook, in the binding order: dup2(record, 4) → PR_SET_NO_NEW_PRIVS →
// landlock_restrict_self → close(ruleset) → chdir. Returns 0 or errno.
int downloader_pre_exec(const void* raw, const std::span<const int> lifted) noexcept {
  const auto* context = static_cast<const HookContext*>(raw);
  if (context->fail_injected) return EPERM;
  if (context->record_index >= 0) {
    const int record = lifted[static_cast<std::size_t>(context->record_index)];
    if (::dup2(record, 4) < 0) return errno;  // dup2 clears FD_CLOEXEC on 4
    ::close(record);
  }
#if defined(__linux__)
  if (::prctl(PR_SET_NO_NEW_PRIVS, 1UL, 0UL, 0UL, 0UL) != 0) return errno;
  if (context->ruleset_index >= 0) {
    const int ruleset = lifted[static_cast<std::size_t>(context->ruleset_index)];
    if (landlock::restrict_self(ruleset) != 0) return errno;
    ::close(ruleset);
  }
#endif
  if (::chdir(context->cwd) != 0) return errno;
  return 0;
}

bool directory_exists(const std::string_view path) {
  struct stat info {};
  return ::stat(std::string{path}.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

void close_fd(int& fd) noexcept {
  if (fd >= 0) ::close(fd);
  fd = -1;
}

// Landlock is expected unless the kernel lacks it (or a hooks build forces that).
bool landlock_expected(const DownloaderOptions& options) noexcept {
  return landlock_abi() >= 1 && !(kTestHooksEnabled && options.test_force_no_landlock);
}

// fd >= 0: the ruleset; fd -1 and !failed: no Landlock (reduced: no_landlock);
// failed: Landlock expected but the ruleset could not be created (ruling B2).
struct Ruleset final {
  int fd{-1};
  bool failed{false};
};

// add_rule failures are ignored: a missing rule only narrows what curl may read.
Ruleset build_ruleset(const DownloaderOptions& options, const std::string& downloader) {
  Ruleset result;
#if defined(__linux__)
  if (!landlock_expected(options)) return result;
  const auto rights = landlock_rights_for_abi(landlock_abi());
  const landlock::RulesetAttr attr{rights.handled_fs, rights.handled_net};
  const int ruleset = landlock::create_ruleset(&attr, sizeof attr, 0);
  if (ruleset < 0) {
    result.failed = true;
    return result;
  }
  const auto plan = plan_linux_confinement(downloader, options.protected_paths, directory_exists);
  for (const auto& dir : plan.read_dirs) {
    const int fd = ::open(dir.c_str(), O_PATH | O_CLOEXEC | O_DIRECTORY);
    if (fd < 0) continue;
    const landlock::PathBeneathAttr rule{rights.directory_rule, fd};
    (void)landlock::add_rule(ruleset, landlock::kRulePathBeneath, &rule);
    ::close(fd);
  }
  for (const auto file : kLandlockReadableFiles) {
    const int fd = ::open(std::string{file}.c_str(), O_PATH | O_CLOEXEC);
    if (fd < 0) continue;
    const landlock::PathBeneathAttr rule{rights.file_rule, fd};
    (void)landlock::add_rule(ruleset, landlock::kRulePathBeneath, &rule);
    ::close(fd);
  }
  if (rights.handled_net != 0) {
    const landlock::NetPortAttr connect{landlock_access::kNetConnectTcp, options.proxy_port.value_or(443)};
    (void)landlock::add_rule(ruleset, landlock::kRuleNetPort, &connect);
  }
  result.fd = ruleset;
#else
  (void)options;
  (void)downloader;
#endif
  return result;
}

// One write with SIGPIPE blocked in this thread (a pending SIGPIPE it caused is
// consumed), so a downloader that exits before reading its stdin is an EPIPE
// here, whatever the process-wide disposition.
ssize_t write_without_sigpipe(const int fd, const char* data, const std::size_t count) noexcept {
  sigset_t block{};
  sigemptyset(&block);
  sigaddset(&block, SIGPIPE);
  sigset_t prior{};
  if (pthread_sigmask(SIG_BLOCK, &block, &prior) != 0) return -1;
  const ssize_t written = ::write(fd, data, count);
  const int saved_errno = errno;
  if (written < 0 && saved_errno == EPIPE && sigismember(&prior, SIGPIPE) == 0) {
    timespec zero{};
    (void)sigtimedwait(&block, nullptr, &zero);
  }
  (void)pthread_sigmask(SIG_SETMASK, &prior, nullptr);
  errno = saved_errno;
  return written;
}

}  // namespace

int landlock_abi() noexcept {
#if defined(__linux__)
  const int abi = landlock::create_ruleset(nullptr, 0, landlock::kCreateRulesetVersion);
  return abi > 0 ? abi : 0;
#else
  return 0;
#endif
}

AcquireReason classify_spawn_report(const std::string_view report) noexcept {
  const auto classified = posix_launch::classify_report(report);
  switch (classified.kind) {
    case posix_launch::ChildReportKind::hook_failed:
      return AcquireReason::downloader_confinement_failed;
    case posix_launch::ChildReportKind::exec_failed:
      if (classified.error == ENOENT) return AcquireReason::downloader_missing;
      if (classified.error == EACCES) return AcquireReason::downloader_untrusted_path;
      return AcquireReason::downloader_unsupported;
    case posix_launch::ChildReportKind::none:
    case posix_launch::ChildReportKind::malformed:
      break;
  }
  return AcquireReason::downloader_unsupported;
}

std::string ConfinedDownloader::confinement() const {
  // Ruling R4: plan with the canonical path (as the spawn will), else the lexical one.
  std::error_code error;
  auto downloader = std::filesystem::canonical(options_.executable, error);
  if (error) downloader = std::filesystem::absolute(options_.executable).lexically_normal();
  const auto plan = plan_linux_confinement(downloader.string(), options_.protected_paths, directory_exists);
  return confinement_label(landlock_expected(options_), plan.readable_key);
}

ConfinedDownloader::SpawnOutput ConfinedDownloader::spawn(const std::vector<std::string>& argv, const std::string_view stdin_bytes,
                                                          const std::function<bool(std::string_view)>& on_bytes) {
  SpawnOutput output;
  EngineCommand command;
  command.executable = argv.front();
  command.arguments.assign(argv.begin() + 1, argv.end());
  posix_launch::PreparedExec prepared;
  posix_launch::prepare(prepared, command, false);
  // The record descriptor is opened before the ruleset (ruling I7), so with fd 3
  // free it is fd 3 itself, which SP2's keep_fds lift protects from kReportFd.
  int record = -1;
#if defined(SECURE_FRONTEND_TEST_HOOKS)
  if (!options_.record_file.empty()) {
    record = ::open(options_.record_file.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  }
#endif
  const auto built = build_ruleset(options_, argv.front());
  int ruleset = built.fd;
  if (built.failed) {  // ruling B2: never execve curl unconfined after announcing `full`
    close_fd(record);
    output.failure = AcquireError{AcquireReason::downloader_confinement_failed, "the Landlock ruleset could not be created"};
    return output;
  }
  std::vector<int> keep;
  HookContext context{-1, -1, options_.cwd.c_str(), kTestHooksEnabled && options_.test_fail_pre_exec};
  if (ruleset >= 0) {
    context.ruleset_index = static_cast<int>(keep.size());
    keep.push_back(ruleset);
  }
  if (record >= 0) {
    context.record_index = static_cast<int>(keep.size());
    keep.push_back(record);
  }
  int in[2] = {-1, -1};
  int out[2] = {-1, -1};
  int report[2] = {-1, -1};
  if (::pipe2(in, O_CLOEXEC) != 0 || ::pipe2(out, O_CLOEXEC) != 0 || ::pipe2(report, O_CLOEXEC) != 0) {
    for (int* fd : {&in[0], &in[1], &out[0], &out[1], &report[0], &report[1]}) close_fd(*fd);
    close_fd(ruleset);
    close_fd(record);
    output.failure = AcquireError{AcquireReason::downloader_unsupported, "pipes cannot be created"};
    return output;
  }
  const posix_launch::ExecOptions exec_options{keep, downloader_pre_exec, &context};
  pid_t pid = -1;
  {
    const posix_launch::BlockAllSignals blocked;
    pid = ::fork();
    if (pid == 0) posix_launch::exec_child(prepared, in[0], out[1], true, report[1], exec_options);
  }
  close_fd(in[0]);
  close_fd(out[1]);
  close_fd(report[1]);
  close_fd(ruleset);
  close_fd(record);
  if (pid < 0) {
    close_fd(in[1]);
    close_fd(out[0]);
    close_fd(report[0]);
    output.failure = AcquireError{AcquireReason::downloader_unsupported, "fork failed"};
    return output;
  }
  // Pre-exec failures arrive on the report pipe; EOF means execve succeeded (ruling R2: SP2's reader).
  const std::string report_bytes = posix_launch::read_report(report[0]);
  close_fd(report[0]);
  if (!report_bytes.empty()) {
    close_fd(in[1]);
    close_fd(out[0]);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    output.failure = AcquireError{classify_spawn_report(report_bytes), "pull.downloader could not be started confined"};
    return output;
  }
  std::size_t written = 0;
  while (written < stdin_bytes.size()) {
    const ssize_t n = write_without_sigpipe(in[1], stdin_bytes.data() + written, stdin_bytes.size() - written);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    written += static_cast<std::size_t>(n);
  }
  close_fd(in[1]);
  char buffer[65536];
  while (true) {
    pollfd poll_fd{out[0], POLLIN, 0};
    const int ready = ::poll(&poll_fd, 1, 250);
    if (options_.interrupted != nullptr && options_.interrupted->load()) {
      ::killpg(pid, SIGKILL);  // exec_child put the child in its own process group
      output.killed = true;
      break;
    }
    if (ready < 0 && errno != EINTR) {
      ::killpg(pid, SIGKILL);
      output.killed = true;
      break;
    }
    if (ready <= 0) continue;  // timeout or EINTR: check the interrupt flag again
    const ssize_t n = ::read(out[0], buffer, sizeof buffer);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    if (!on_bytes(std::string_view{buffer, static_cast<std::size_t>(n)})) {
      ::killpg(pid, SIGKILL);
      output.killed = true;
      break;
    }
  }
  close_fd(out[0]);
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  if (!output.killed && WIFEXITED(status)) output.exit = WEXITSTATUS(status);
  return output;
}

}  // namespace secure_frontend
#endif
