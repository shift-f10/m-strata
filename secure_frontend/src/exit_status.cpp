#include "secure_frontend/exit_status.hpp"

#if !defined(_WIN32)
#include <csignal>
#endif

namespace secure_frontend {
namespace {
// Linux signal numbers, spelled out so the POSIX table is testable on every host.
constexpr std::uint32_t kSigAbrt = 6U;
constexpr std::uint32_t kSigKill = 9U;
constexpr std::uint32_t kSigTerm = 15U;
#if defined(__linux__)
static_assert(SIGABRT == 6 && SIGKILL == 9 && SIGTERM == 15, "Linux signal numbers");
#endif
}  // namespace

ExitClassification classify_exit(const ExitStatus& status, const KillReason sent, const ExitPlatform platform) noexcept {
  if (platform == ExitPlatform::windows) {
    if (sent != KillReason::none && status.value == kSupervisorKillExitCode) return {ExitKind::killed_by_supervisor, sent};
    if (status.value == 0U) return {ExitKind::clean_exit, KillReason::none};
    if (status.value == 3U || status.value == 0xC0000409U) return {ExitKind::abort, KillReason::none};
    if ((status.value & 0xC0000000U) == 0xC0000000U) return {ExitKind::crashed, KillReason::none};
    return {ExitKind::error_exit, KillReason::none};
  }
  if (status.signaled) {
    if (sent != KillReason::none && (status.value == kSigKill || status.value == kSigTerm)) return {ExitKind::killed_by_supervisor, sent};
    if (status.value == kSigAbrt) return {ExitKind::abort, KillReason::none};
    if (status.value == kSigKill) return {ExitKind::killed_external, KillReason::none};
    return {ExitKind::crashed, KillReason::none};
  }
  if (status.value == 0U) return {ExitKind::clean_exit, KillReason::none};
  return {ExitKind::error_exit, KillReason::none};
}

std::string_view exit_kind_literal(const ExitKind kind) noexcept {
  switch (kind) {
    case ExitKind::clean_exit: return "clean_exit";
    case ExitKind::error_exit: return "error_exit";
    case ExitKind::spawn_failed: return "spawn_failed";
    case ExitKind::abort: return "abort";
    case ExitKind::killed_external: return "killed_external";
    case ExitKind::killed_by_supervisor: return "killed_by_supervisor";
    case ExitKind::crashed: return "crashed";
  }
  return "crashed";
}

std::string_view kill_reason_literal(const KillReason reason) noexcept {
  switch (reason) {
    case KillReason::none: return "none";
    case KillReason::fatal_line: return "fatal_line";
    case KillReason::ready_timeout: return "ready_timeout";
    case KillReason::drain_failed: return "drain_failed";
    case KillReason::client_stop_drain_failed: return "client_stop_drain_failed";
    case KillReason::write_timeout: return "write_timeout";
    case KillReason::protocol_failed: return "protocol_failed";
    case KillReason::shutdown: return "shutdown";
    case KillReason::idle_unload: return "idle_unload";  // SP5 M1 Task 6: intentional idle teardown
  }
  return "none";
}

std::string describe_exit(const ExitClassification& classification) {
  std::string text(exit_kind_literal(classification.kind));
  if (classification.kind == ExitKind::killed_by_supervisor) {
    text += '(';
    text += kill_reason_literal(classification.reason);
    text += ')';
  }
  return text;
}

}  // namespace secure_frontend
