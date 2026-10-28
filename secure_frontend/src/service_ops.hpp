#pragma once
// SP7 M1 (spec D-5): the per-user service unit. Rendering is pure text; the
// verbs that write it (plan_enable/plan_disable/plan_status, Task 2) live in
// the same library. Internal header (src/, like strata_cli_internal.hpp): the
// only consumers are our own verbs and tests.
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace secure_frontend {
namespace service {

// One hardened service definition, all paths in portable generic (slash) form.
// exec_start is the resolved current-target executable (resolve_current_target);
// config_file is the verb's --config path; read_write_paths are the roots the
// served process may write (the server never writes anywhere else).
struct ServiceUnit final {
  std::filesystem::path exec_start;
  std::string config_file;
  std::vector<std::string> read_write_paths;
};

// Fixed human text. It never echoes a path (the whitespace rule is the finding;
// a configured path may name nothing the user did not already type).
struct ServiceError final {
  std::string message;
};

// The exact hardened systemd user unit (D-5, verbatim directives:
// NoNewPrivileges=yes, ProtectSystem=strict, ProtectHome=read-only,
// ReadWritePaths of the roots, PrivateTmp=yes,
// RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6, Restart=on-failure,
// RestartSec=5, ExecStart=<resolved target> --config <file> serve).
// Rejects any path that is empty, not absolute, or contains a space, tab or
// newline (systemd splits ExecStart and the ReadWritePaths list on spaces;
// quoting is not ours to get subtly right).
[[nodiscard]] std::variant<std::string, ServiceError> render_systemd_unit(const ServiceUnit& unit);

// SP7 M1 Task 6 (spec D-5, PX §6): the Windows side of the same unit — exact
// Task Scheduler XML for `schtasks /Create /TN strata /XML <file>`. "At log on"
// for the current user, deliberately NOT "run with highest privileges"
// (RunLevel LeastPrivilege — PX §6 never elevates, against upstream's hint),
// Priority Normal (6, not the Below-normal default), bounded restart
// (PT5M x3, the RestartSec=5 analogue). Unsafe path text is rejected:
// whitespace (the systemd parity rule), XML metacharacters (& < "), and empty
// or not-absolute paths — absolute here is portable text (POSIX leading '/'
// or a Windows drive), so the golden renders identically on any host.
[[nodiscard]] std::variant<std::string, ServiceError> render_schtasks_create(const ServiceUnit& unit);

// The ExecStart target (D-4): if self_executable sits under a `current`
// pointer (Linux symlink, Windows current.txt), the path is reported through
// its resolved version directory; a plain single-directory install reports its
// own canonical path. This is the seam SP7 M2's `update` will write through.
[[nodiscard]] std::variant<std::filesystem::path, ServiceError> resolve_current_target(const std::filesystem::path& self_executable);

// --- Task 2: the enable/disable/status verbs (spec D-5) -----------------------

// Runs argv with no shell; exit_code < 0 when the program could not be started
// at all. POSIX: argv[0] is searched through PATH (execvp). Windows: a bare
// argv[0] is a system tool, run_argv_in(system_tool_directory()). Production
// CommandRunners are this function; unit tests inject recording fakes; the POSIX
// e2e puts a PATH shim named systemctl first on PATH. Never runs a loginctl,
// never elevates.
struct CmdResult final {
  int exit_code{-1};
  std::string out;
  std::string err;
};
[[nodiscard]] CmdResult run_argv(const std::vector<std::string>& argv);
using CommandRunner = std::function<CmdResult(const std::vector<std::string>&)>;
#if defined(_WIN32)
// %SystemRoot%\System32 (GetSystemDirectoryW), where every tool strata runs
// ships (schtasks, nvidia-smi); empty if it cannot be read.
[[nodiscard]] std::filesystem::path system_tool_directory();
// run_argv with a bare argv[0] run as `tool_directory`\argv[0] (".exe" added when
// it has no extension) and nothing else. CreateProcessW given a bare name would
// search strata's own directory and the CURRENT directory before System32, then
// PATH, so a schtasks.exe planted where the user runs `strata service enable`
// would run instead (CWE-427; session log 2026-10-21). Not there: exit_code < 0.
[[nodiscard]] CmdResult run_argv_in(const std::filesystem::path& tool_directory, const std::vector<std::string>& argv);
#endif

// Where the unit lives and what it must say. user_unit_dir is the systemd user
// unit directory (XDG_CONFIG_HOME, else HOME/.config, plus /systemd/user —
// R-T2-a); unit_path is the unit this verb manages. disable removes unit_path
// only when it is exactly user_unit_dir/strata.service: a foreign location
// (a system-scope unit, anything renamed) is refused, never touched.
struct ServiceEnv final {
  std::filesystem::path user_unit_dir;
  std::filesystem::path unit_path;
  // Windows (Task 6): the task XML file the verb writes; it must be exactly
  // user_unit_dir/strata-task.xml — the same foreign-path refusal as the unit.
  std::filesystem::path task_xml_path;
  ServiceUnit unit;
};

inline constexpr std::string_view kServiceName = "strata";
inline constexpr std::string_view kUnitFileName = "strata.service";
inline constexpr std::string_view kTaskXmlFileName = "strata-task.xml";

// One human line out of every verb (the CLI prints it; ok=false means the verb
// exits non-zero).
struct ServiceOutcome final {
  bool ok{false};
  std::string message;
};

// enable: probe the user manager (systemctl --user is-enabled strata; R-T2-c
// classifies only a bus-connect failure or an unstartable systemctl as "no
// user manager" — a fresh install answers "disabled"/"No such file" and
// proceeds), then write the unit atomically at 0644, then systemctl --user
// enable --now. A missing user manager writes nothing and explains, including
// the loginctl enable-linger note it never runs.
[[nodiscard]] ServiceOutcome plan_enable(const ServiceEnv& env, CommandRunner& runner);

// disable: systemctl --user disable --now strata, then remove ONLY the exact
// user_unit_dir/strata.service. A foreign unit_path refuses before any run.
[[nodiscard]] ServiceOutcome plan_disable(const ServiceEnv& env, CommandRunner& runner);

// status: read-only — exactly one is-enabled probe and one is-active probe,
// one human line each.
struct ServiceStatus final {
  std::string enabled_line;
  std::string active_line;
};
[[nodiscard]] ServiceStatus plan_status(const ServiceEnv& env, CommandRunner& runner);

// One read-only probe: does a systemd user manager answer? The uninstall verb
// needs the same classification the plans use (R-T2-c), without planning anything.
[[nodiscard]] bool user_manager_reachable(CommandRunner& runner);


}  // namespace service
}  // namespace secure_frontend
