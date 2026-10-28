#pragma once

// Windows child launch into a kill-on-close Job (internal; SP2 R4, the SP4
// launch seam; SP4 spec §3.2; controller rulings RF11 and the R4 addendum).
//
// launch_in_job() is the engine's CreateProcessW + Job sequence, factored out
// of engine_stdio_process.cpp unchanged, so SP4's confined downloader reuses it
// instead of copying it:
//   - a Job with KILL_ON_JOB_CLOSE | DIE_ON_UNHANDLED_EXCEPTION is created first;
//   - the child is created CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP |
//     CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
//     bInheritHandles TRUE but restricted by PROC_THREAD_ATTRIBUTE_HANDLE_LIST to
//     the three standard handles plus extra_inherited_handles (in that order);
//   - lpCurrentDirectory = cwd (empty: inherit the parent's);
//   - it is assigned to the Job while still suspended, then resumed, so no
//     instruction runs outside the Job (PROC-05);
//   - the process error mode (no WER/critical-error dialogs) is set once per
//     process before the first launch and is inherited by every child.
// On failure nothing is left running and every handle launch_in_job created is
// closed; the caller's handles in the spec are never closed by it.
//
// Seam inputs (token, extra handles and CRT block default off = the engine's
// behaviour; the two Job options default ON, see below):
//   primary_token            non-null: CreateProcessAsUserW with this token.
//   extra_inherited_handles  each must be inheritable (HANDLE_FLAG_INHERIT),
//                            else the launch fails with ERROR_INVALID_HANDLE at
//                            step `attributes`; appended after the three standard
//                            handles. Each must be distinct from the three standard
//                            handles and from each other: a duplicate in the handle
//                            list makes CreateProcess fail (ERROR_INVALID_PARAMETER,
//                            step `create_process`). launch_in_job does not dedupe.
//   crt_fd_block             copied into STARTUPINFO.cbReserved2/lpReserved2:
//                            the Microsoft CRT descriptor-inheritance layout
//                            (int n, n flag bytes, n HANDLEs, packed); SP4 builds
//                            it. Empty: cbReserved2 = 0. At most 65535 bytes.
//   limit_active_process_to_one / restrict_ui
//                            SP4 spec §3.2 Windows column: JOB_OBJECT_LIMIT_ACTIVE_PROCESS
//                            with ActiveProcessLimit = 1, and every
//                            JOB_OBJECT_UILIMIT_* restriction. Both default to
//                            TRUE (controller ruling RF11, amended in R4 fix
//                            round 1): a new caller is confined by default, and
//                            SP4's positional 10-field initialiser gets this Job.
//                            The engine sets both to false explicitly, so its Job
//                            is unchanged (its own children must be allowed).
// Owned containers (not spans) so a spec never dangles; SP4's positional
// aggregate initialisation (sp4 plan Task 26) compiles unchanged against it.
//
// Result: WinLaunched (process, primary thread, Job; the caller owns and closes
// all three — SP4 must close WinLaunched::thread after launch_in_job; the
// thread is already resumed) or the GetLastError() of the failing
// call. `failed_step`, when given, names that call. ERROR_ELEVATION_REQUIRED
// (740) from create_process means the image demands elevation: the launch is
// never retried via runas, and the engine maps it to spawn_failed ("engine
// requires elevation; refused", spec §3.6/G3).

#if defined(_WIN32)

#include "secure_frontend/win32_prelude.hpp"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace secure_frontend::win_launch {

inline constexpr DWORD kAllUiRestrictions = JOB_OBJECT_UILIMIT_HANDLES | JOB_OBJECT_UILIMIT_READCLIPBOARD |
                                            JOB_OBJECT_UILIMIT_WRITECLIPBOARD | JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS |
                                            JOB_OBJECT_UILIMIT_DISPLAYSETTINGS | JOB_OBJECT_UILIMIT_GLOBALATOMS |
                                            JOB_OBJECT_UILIMIT_DESKTOP | JOB_OBJECT_UILIMIT_EXITWINDOWS;

struct WinLaunchSpec final {
  std::wstring executable;                      // absolute
  std::wstring command_line;                    // fully quoted argv
  std::wstring environment_block;               // NAME=VALUE\0...\0\0 (CREATE_UNICODE_ENVIRONMENT)
  std::wstring cwd;                             // empty: inherit the parent's
  HANDLE stdin_read{nullptr};                   // inheritable
  HANDLE stdout_write{nullptr};                 // inheritable
  HANDLE stderr_handle{nullptr};                // inheritable (NUL)
  HANDLE primary_token{nullptr};                // CreateProcessAsUserW when set
  std::vector<HANDLE> extra_inherited_handles;  // appended to PROC_THREAD_ATTRIBUTE_HANDLE_LIST
  std::vector<std::uint8_t> crt_fd_block;       // STARTUPINFO cbReserved2/lpReserved2
  bool limit_active_process_to_one{true};       // JOB_OBJECT_LIMIT_ACTIVE_PROCESS, ActiveProcessLimit = 1 (engine: false)
  bool restrict_ui{true};                       // JobObjectBasicUIRestrictions = kAllUiRestrictions (engine: false)
};

struct WinLaunched final {
  HANDLE process{nullptr};
  HANDLE thread{nullptr};  // already resumed
  HANDLE job{nullptr};     // KILL_ON_JOB_CLOSE | DIE_ON_UNHANDLED_EXCEPTION (+ the spec's optional limits)
};

// The call whose GetLastError() a failure carries.
enum class WinLaunchStep { none, job, attributes, create_process, assign_job, resume };

std::variant<WinLaunched, DWORD> launch_in_job(const WinLaunchSpec& spec, WinLaunchStep* failed_step = nullptr);

}  // namespace secure_frontend::win_launch

#endif  // defined(_WIN32)
