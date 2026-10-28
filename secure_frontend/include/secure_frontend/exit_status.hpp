#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace secure_frontend {

// Raw end of an engine child, kept instead of discarded (spec §3.6).
// POSIX: `signaled` = WIFSIGNALED, `value` = WTERMSIG or WEXITSTATUS.
// Windows: `signaled` is false and `value` is GetExitCodeProcess.
struct ExitStatus final {
  bool signaled{false};
  std::uint32_t value{0};
  friend bool operator==(const ExitStatus&, const ExitStatus&) = default;
};

// Why the supervisor ended a generation (spec §3.5); `none` when it did not.
// `client_stop_drain_failed` (SP3 D-2) is a drain_failed whose STOP the serving
// side sent for a client that went away; it restarts without counting.
enum class KillReason { none, fatal_line, ready_timeout, drain_failed, write_timeout, protocol_failed, shutdown, client_stop_drain_failed, idle_unload };

// Diagnostic only: every kind counts as a failure (except our own
// client_stop_drain_failed kill, SP3 D-2), and the literal appears only
// in the frontend's own log line, never in a client response.
enum class ExitKind { clean_exit, error_exit, spawn_failed, abort, killed_external, killed_by_supervisor, crashed };

enum class ExitPlatform { posix, windows };
#if defined(_WIN32)
inline constexpr ExitPlatform kNativeExitPlatform = ExitPlatform::windows;
#else
inline constexpr ExitPlatform kNativeExitPlatform = ExitPlatform::posix;
#endif

struct ExitClassification final {
  ExitKind kind;
  KillReason reason;
};

// TerminateJobObject exit code: a customer code (bit 29), never an engine `return 1`.
inline constexpr std::uint32_t kSupervisorKillExitCode = 0x20000001U;
// SP5 M1 Task 11 (plan: the vision role's "distinct exit-code space"): a supervisor-killed
// maya-vision child terminates with this code instead of the engine's.
inline constexpr std::uint32_t kVisionKillExitCode = 0x20000002U;

// `sent_by_supervisor` is the reason recorded when the supervisor sent a kill
// (KillReason::none otherwise). The kill is credited to the supervisor only if
// the observed end is what it sends (POSIX SIGKILL/SIGTERM, Windows
// kSupervisorKillExitCode); otherwise the status decides.
[[nodiscard]] ExitClassification classify_exit(const ExitStatus& status, KillReason sent_by_supervisor,
                                               ExitPlatform platform = kNativeExitPlatform) noexcept;
[[nodiscard]] std::string_view exit_kind_literal(ExitKind kind) noexcept;
[[nodiscard]] std::string_view kill_reason_literal(KillReason reason) noexcept;
// "abort", or "killed_by_supervisor(fatal_line)".
[[nodiscard]] std::string describe_exit(const ExitClassification& classification);

}  // namespace secure_frontend
