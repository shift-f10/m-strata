// SP7 M1 Task 1 (spec D-5/D-4): the hardened systemd user unit text and the
// `current`-target resolution seam. Pure functions; the verbs that consume
// them (Task 2) live beside them.
#include "service_ops.hpp"

#include "secure_frontend/win32_prelude.hpp"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <system_error>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace secure_frontend {
namespace service {
namespace {

bool has_whitespace(const std::string& text) {
  return text.find_first_of(" \t\n\r") != std::string::npos;
}

// A systemd unit names POSIX paths whatever the host, so "absolute" is the POSIX
// rule (a leading '/'), exactly path::is_absolute() on Linux. The host rule would
// refuse every unit on Windows, where the renderer stays host-runnable (Task 6).
bool posix_absolute(const std::string& text) { return !text.empty() && text.front() == '/'; }

// Fixed messages (the finding, never a configured path): systemd splits
// ExecStart on spaces and the ReadWritePaths list on spaces, so a path that
// contains one cannot be named unquoted, and we refuse to guess at quoting.
std::optional<ServiceError> check_unit(const ServiceUnit& unit) {
  const std::string exec = unit.exec_start.string();
  if (exec.empty()) return ServiceError{"service: ExecStart names no program"};
  if (has_whitespace(exec)) {
    return ServiceError{"service: the program path contains whitespace; systemd splits ExecStart on spaces and "
                        "the service refuses to guess at quoting — install strata below a path without spaces"};
  }
  if (!posix_absolute(exec)) return ServiceError{"service: the program path must be absolute"};
  if (unit.config_file.empty()) return ServiceError{"service: no config file was given to the service"};
  if (has_whitespace(unit.config_file) || !posix_absolute(unit.config_file)) {
    return ServiceError{"service: the config file path must be absolute and free of whitespace"};
  }
  if (unit.read_write_paths.empty()) {
    return ServiceError{"service: the served process always writes its state root; the unit needs at least one "
                        "ReadWritePaths entry"};
  }
  for (const auto& path : unit.read_write_paths) {
    if (path.empty() || has_whitespace(path) || !posix_absolute(path)) {
      return ServiceError{"service: every ReadWritePaths entry must be absolute and free of whitespace"};
    }
  }
  return std::nullopt;
}

}  // namespace

std::variant<std::string, ServiceError> render_systemd_unit(const ServiceUnit& unit) {
  if (auto failure = check_unit(unit)) return *failure;
  std::string read_write;
  for (const auto& path : unit.read_write_paths) {
    if (!read_write.empty()) read_write += ' ';
    read_write += path;
  }
  std::string unit_text = "[Unit]\n"
                          "Description=Strata local model server\n"
                          "\n"
                          "[Service]\n"
                          "ExecStart=";
  unit_text += unit.exec_start.string();
  unit_text += " --config ";
  unit_text += unit.config_file;
  unit_text += " serve\n"
               "Restart=on-failure\n"
               "RestartSec=5\n"
               "NoNewPrivileges=yes\n"
               "ProtectSystem=strict\n"
               "ProtectHome=read-only\n"
               "ReadWritePaths=";
  unit_text += read_write;
  unit_text += "\n"
               "PrivateTmp=yes\n"
               "RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6\n"
               "\n"
               "[Install]\n"
               "WantedBy=default.target\n";
  return unit_text;
}

std::variant<std::string, ServiceError> render_schtasks_create(const ServiceUnit& unit) {
  const auto unsafe = [](const std::filesystem::path& p) {
    const std::string text = p.string();
    if (text.empty()) return true;
    if (text.find_first_of(" \t\n&<>\"") != std::string::npos) return true;  // whitespace parity + XML quoting is not ours
    const bool drive_like = text.size() >= 3U && std::isalpha(static_cast<unsigned char>(text[0])) != 0 && text[1] == ':' &&
                            (text[2] == '\\' || text[2] == '/');
    return !(drive_like || text.front() == '/');
  };
  if (unsafe(unit.exec_start) || unsafe(std::filesystem::path{unit.config_file})) {
    return ServiceError{"the task paths must be absolute and free of whitespace and XML metacharacters"};
  }
  std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                    "<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\n"
                    "  <RegistrationInfo>\n"
                    "    <Description>Strata local model server</Description>\n"
                    "    <Author>strata</Author>\n"
                    "  </RegistrationInfo>\n"
                    "  <Triggers>\n"
                    "    <LogonTrigger>\n"
                    "      <Enabled>true</Enabled>\n"
                    "    </LogonTrigger>\n"
                    "  </Triggers>\n"
                    "  <Principals>\n"
                    "    <Principal id=\"Author\">\n"
                    "      <RunLevel>LeastPrivilege</RunLevel>\n"
                    "    </Principal>\n"
                    "  </Principals>\n"
                    "  <Settings>\n"
                    "    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\n"
                    "    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\n"
                    "    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\n"
                    "    <RestartOnFailure>\n"
                    "      <Interval>PT5M</Interval>\n"
                    "      <Count>3</Count>\n"
                    "    </RestartOnFailure>\n"
                    "    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\n"
                    "    <Priority>6</Priority>\n"
                    "  </Settings>\n"
                    "  <Actions Context=\"Author\">\n"
                    "    <Exec>\n"
                    "      <Command>";
  xml += unit.exec_start.string();
  xml += "</Command>\n";
  xml += "      <Arguments>--config ";
  xml += unit.config_file;
  xml += " serve</Arguments>\n";
  xml += "    </Exec>\n"
         "  </Actions>\n"
         "</Task>\n";
  return xml;
}

std::variant<std::filesystem::path, ServiceError> resolve_current_target(const std::filesystem::path& self_executable) {
  std::error_code ec;
  const auto self = std::filesystem::canonical(self_executable, ec);
  if (ec || !std::filesystem::is_regular_file(self)) {
    return ServiceError{"service: the running strata program could not be located on disk"};
  }
  // A versioned install (D-4) looks like <root>/<version>/bin/strata with a
  // `current` pointer at <root>; anything else is a plain single-directory
  // install, whose own canonical path is the target (M2's `update` is the verb
  // that refuses a plain install, not the service verbs).
  const auto version_dir = self.parent_path().parent_path();
  const auto install_root = version_dir.parent_path();
  if (install_root.empty()) return self;
#if defined(_WIN32)
  std::ifstream pointer{install_root / "current.txt"};
  std::string name;
  if (pointer >> name) {
    std::error_code ignored;
    if (!name.empty() && name.back() == '\r') name.pop_back();
    if (name == version_dir.filename().string()) return self;  // this version is current
  }
#else
  std::error_code link_error;
  const auto target = std::filesystem::read_symlink(install_root / "current", link_error);
  if (!link_error) {
    std::error_code ignored;
    if (std::filesystem::canonical(install_root / target, ignored) == version_dir) return self;  // this version is current
  }
#endif
  // Not through the pointer (plain install, or a version the pointer left):
  // the concrete path this program was started from is the honest ExecStart.
  return self;
}

#if defined(_WIN32)
CmdResult run_argv(const std::vector<std::string>& argv) { return run_argv_in(system_tool_directory(), argv); }
#else
CmdResult run_argv(const std::vector<std::string>& argv) {
  CmdResult result;
  if (argv.empty()) return result;
  int out_pipe[2] = {-1, -1};
  int err_pipe[2] = {-1, -1};
  if (pipe(out_pipe) != 0) return result;
  if (pipe(err_pipe) != 0) {
    close(out_pipe[0]); close(out_pipe[1]);
    return result;
  }
  const pid_t pid = fork();
  if (pid < 0) {
    close(out_pipe[0]); close(out_pipe[1]); close(err_pipe[0]); close(err_pipe[1]);
    return result;
  }
  if (pid == 0) {
    dup2(out_pipe[1], STDOUT_FILENO);
    dup2(err_pipe[1], STDERR_FILENO);
    close(out_pipe[0]); close(out_pipe[1]); close(err_pipe[0]); close(err_pipe[1]);
    std::vector<char*> cargv;
    for (const auto& part : argv) cargv.push_back(const_cast<char*>(part.c_str()));
    cargv.push_back(nullptr);
    execvp(cargv[0], cargv.data());
    _exit(127);  // the exec failed; a distinguishable non-zero like any missing binary
  }
  close(out_pipe[1]);
  close(err_pipe[1]);
  // Read both pipes until EOF (systemctl output is tiny, but never trust that: read
  // alternately so a full stderr buffer can never deadlock the pump).
  bool out_open = true;
  bool err_open = true;
  while (out_open || err_open) {
    bool progressed = false;
    if (out_open) {
      char buffer[4096];
      const ssize_t got = read(out_pipe[0], buffer, sizeof buffer);
      if (got > 0) { result.out.append(buffer, static_cast<std::size_t>(got)); progressed = true; }
      else if (got == 0) out_open = false;
      else if (errno != EINTR) out_open = false;
      else progressed = true;
    }
    if (err_open) {
      char buffer[4096];
      const ssize_t got = read(err_pipe[0], buffer, sizeof buffer);
      if (got > 0) { result.err.append(buffer, static_cast<std::size_t>(got)); progressed = true; }
      else if (got == 0) err_open = false;
      else if (errno != EINTR) err_open = false;
      else progressed = true;
    }
    if (!progressed && (out_open || err_open)) break;  // both at EOF or an error closed them
  }
  close(out_pipe[0]);
  close(err_pipe[0]);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  if (WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
  return result;
}
#endif

namespace {

// R-T2-c: only a bus-connect failure (systemd's fixed text) or an unstartable
// systemctl means "no user manager". A fresh install answers non-zero about the
// UNIT and proceeds. (Used by the POSIX branches; unused in a Windows build.)
[[maybe_unused]] bool manager_unreachable(const CmdResult& probe) {
  return probe.exit_code < 0 ||
         (probe.exit_code != 0 && probe.err.find("Failed to connect to bus") != std::string::npos);
}

[[maybe_unused]] std::string first_token(const std::string& text) {
  std::string token;
  for (const char c : text) {
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
      if (!token.empty()) break;
      continue;
    }
    token.push_back(c);
  }
  return token;
}

}  // namespace

#if defined(_WIN32)
std::filesystem::path system_tool_directory() {
  std::wstring buffer(MAX_PATH, L'\0');
  const UINT length = GetSystemDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
  if (length == 0U || length >= buffer.size()) return {};
  buffer.resize(length);
  return std::filesystem::path{buffer};
}

CmdResult run_argv_in(const std::filesystem::path& tool_directory, const std::vector<std::string>& argv) {
  CmdResult result;
  // A bare tool name only (no separator, drive or quote), so it can never name a
  // file outside `tool_directory`; the callers' argv is ASCII system commands.
  if (argv.empty() || tool_directory.empty() || argv.front().empty() || argv.front() == "." || argv.front() == ".." ||
      argv.front().find_first_of("\\/:\"") != std::string::npos) {
    return result;
  }
  std::filesystem::path application = tool_directory / std::filesystem::path{argv.front()};
  if (!application.has_extension()) application += L".exe";
  std::error_code missing;
  if (!std::filesystem::is_regular_file(application, missing)) return result;  // not there: no search anywhere else
  // lpApplicationName is the resolved path, so CreateProcessW searches nothing; the
  // command line repeats it, quoted, as the child's argv[0].
  const std::wstring application_text = application.wstring();
  std::wstring command = L"\"" + application_text + L"\"";
  for (std::size_t i = 1; i < argv.size(); ++i) {
    command += L' ';
    const std::wstring wide(argv[i].begin(), argv[i].end());  // argv here is ASCII system commands
    if (wide.find(L' ') != std::wstring::npos) { command += L'"'; command += wide; command += L'"'; }
    else command += wide;
  }
  SECURITY_ATTRIBUTES inherit{static_cast<DWORD>(sizeof(SECURITY_ATTRIBUTES)), nullptr, TRUE};
  HANDLE out_read = nullptr; HANDLE out_write = nullptr; HANDLE err_read = nullptr; HANDLE err_write = nullptr;
  if (CreatePipe(&out_read, &out_write, &inherit, 0) == 0) return result;
  if (CreatePipe(&err_read, &err_write, &inherit, 0) == 0) {
    CloseHandle(out_read); CloseHandle(out_write);
    return result;
  }
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = out_write;
  startup.hStdError = err_write;
  PROCESS_INFORMATION info{};
  std::vector<wchar_t> mutable_command(command.begin(), command.end());
  mutable_command.push_back(L'\0');
  const BOOL started = CreateProcessW(application_text.c_str(), mutable_command.data(), nullptr, nullptr, TRUE,
                                      CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info);
  CloseHandle(out_write);
  CloseHandle(err_write);
  if (started == 0) {
    CloseHandle(out_read); CloseHandle(err_read);
    return result;  // exit_code stays negative: the program could not be started
  }
  const auto drain = [](HANDLE pipe, std::string& into) {
    char buffer[4096];
    DWORD got = 0;
    while (ReadFile(pipe, buffer, sizeof buffer, &got, nullptr) != 0 && got != 0) into.append(buffer, got);
  };
  drain(out_read, result.out);
  drain(err_read, result.err);
  CloseHandle(out_read);
  CloseHandle(err_read);
  WaitForSingleObject(info.hProcess, INFINITE);
  DWORD code = 0;
  if (GetExitCodeProcess(info.hProcess, &code) != 0) result.exit_code = static_cast<int>(code);
  CloseHandle(info.hThread);
  CloseHandle(info.hProcess);
  return result;
}
#endif

ServiceOutcome plan_enable(const ServiceEnv& env, CommandRunner& runner) {
#if defined(_WIN32)
  // The Task Scheduler branch (Task 6): the XML is the golden text, the runner
  // is schtasks. Compile-proven here; runtime proof waits for the owner's box
  // (windows-runtime-labelled e2e). Never elevates (PX §6).
  const auto rendered = render_schtasks_create(env.unit);
  const auto* task_xml = std::get_if<std::string>(&rendered);
  if (task_xml == nullptr) return ServiceOutcome{false, std::get<ServiceError>(rendered).message};
  if (env.task_xml_path.empty() || env.task_xml_path != env.user_unit_dir / std::string{kTaskXmlFileName}) {
    return ServiceOutcome{false, "service: only " + (env.user_unit_dir / std::string{kTaskXmlFileName}).string() +
                                     " is strata's task file; any other location is never touched"};
  }
  const auto probe = runner({"schtasks", "/Query", "/TN", std::string{kServiceName}});
  if (probe.exit_code < 0) {
    return ServiceOutcome{false, "service: the Task Scheduler is not reachable (schtasks could not run); nothing was written"};
  }
  std::error_code ec;
  std::filesystem::create_directories(env.user_unit_dir, ec);
  if (ec) return ServiceOutcome{false, "service: the task directory could not be created: " + env.user_unit_dir.string()};
  const std::filesystem::path tmp = env.task_xml_path.string() + ".tmp";
  {
    std::ofstream out{tmp, std::ios::binary | std::ios::trunc};
    if (!out) return ServiceOutcome{false, "service: the task file could not be written: " + env.task_xml_path.string()};
    out << *task_xml;
    if (!out) return ServiceOutcome{false, "service: the task file could not be written: " + env.task_xml_path.string()};
  }
  std::filesystem::rename(tmp, env.task_xml_path, ec);
  if (ec) {
    std::filesystem::remove(tmp, ec);
    return ServiceOutcome{false, "service: the task file could not be put in place: " + env.task_xml_path.string()};
  }
  const auto create = runner({"schtasks", "/Create", "/F", "/TN", std::string{kServiceName}, "/XML", env.task_xml_path.string()});
  if (create.exit_code != 0) {
    return ServiceOutcome{false, "service: schtasks /Create failed (exit " + std::to_string(create.exit_code) +
                                     "); the task file stays at " + env.task_xml_path.string()};
  }
  return ServiceOutcome{true,
                        "service: strata's logon task registered at " + env.task_xml_path.string() +
                            " (schtasks /Create /F; current user, Priority Normal, never elevated)."};
#else
  const auto rendered = render_systemd_unit(env.unit);
  const auto* unit_text = std::get_if<std::string>(&rendered);
  if (unit_text == nullptr) return ServiceOutcome{false, std::get<ServiceError>(rendered).message};
  const auto probe = runner({"systemctl", "--user", "is-enabled", std::string{kServiceName}});
  if (manager_unreachable(probe)) {
    return ServiceOutcome{
        false,
        "service: the systemd user manager is not reachable; strata never falls back to the system scope and "
        "never runs loginctl for you. Start a real login session (its systemd user instance), and for "
        "boot-time start without a login run loginctl enable-linger yourself"};
  }
  std::error_code ec;
  std::filesystem::create_directories(env.user_unit_dir, ec);
  if (ec) return ServiceOutcome{false, "service: the user unit directory could not be created: " + env.user_unit_dir.string()};
  const std::filesystem::path tmp = env.unit_path.string() + ".tmp";
  {
    std::ofstream out{tmp, std::ios::binary | std::ios::trunc};
    if (!out) return ServiceOutcome{false, "service: the unit file could not be written: " + env.unit_path.string()};
    out << *unit_text;
    if (!out) return ServiceOutcome{false, "service: the unit file could not be written: " + env.unit_path.string()};
  }
  if (chmod(tmp.c_str(), 0644) != 0) {
    std::filesystem::remove(tmp, ec);
    return ServiceOutcome{false, "service: the unit file could not be set to mode 0644: " + env.unit_path.string()};
  }
  std::filesystem::rename(tmp, env.unit_path, ec);
  if (ec) {
    std::filesystem::remove(tmp, ec);
    return ServiceOutcome{false, "service: the unit file could not be put in place: " + env.unit_path.string()};
  }
  const auto enable = runner({"systemctl", "--user", "enable", "--now", std::string{kServiceName}});
  if (enable.exit_code != 0) {
    return ServiceOutcome{false, "service: systemctl --user enable --now strata failed (exit " +
                                     std::to_string(enable.exit_code) + "); the unit stays at " + env.unit_path.string()};
  }
  return ServiceOutcome{true,
                        "service: strata.service enabled and started at " + env.unit_path.string() +
                            " (systemctl --user enable --now). Boot-time start without a login session needs "
                            "loginctl enable-linger, which strata explains and never runs."};
#endif
}

ServiceOutcome plan_disable(const ServiceEnv& env, CommandRunner& runner) {
#if defined(_WIN32)
  const auto ours = env.user_unit_dir / std::string{kTaskXmlFileName};
  if (env.task_xml_path != ours) {
    return ServiceOutcome{false, "service: only " + ours.string() + " is strata's task file; a task file in any "
                                                                 "other location is never touched"};
  }
  const auto probe = runner({"schtasks", "/Query", "/TN", std::string{kServiceName}});
  if (probe.exit_code < 0) {
    return ServiceOutcome{false, "service: the Task Scheduler is not reachable (schtasks could not run); nothing was removed"};
  }
  if (probe.exit_code != 0) {  // no such task: remove only our own leftover file
    std::error_code ec;
    std::filesystem::remove(env.task_xml_path, ec);
    return ServiceOutcome{true, "service: no strata task was registered — nothing else to remove"};
  }
  (void)runner({"schtasks", "/End", "/TN", std::string{kServiceName}});  // stopping a not-running task is fine
  const auto removed = runner({"schtasks", "/Delete", "/F", "/TN", std::string{kServiceName}});
  if (removed.exit_code != 0) {
    return ServiceOutcome{false, "service: schtasks /Delete failed (exit " + std::to_string(removed.exit_code) +
                                     "); nothing was removed"};
  }
  std::error_code ec;
  std::filesystem::remove(env.task_xml_path, ec);
  return ServiceOutcome{true, "service: strata's logon task deleted and its task file removed (" + env.task_xml_path.string() + ")"};
#else
  const auto ours = env.user_unit_dir / std::string{kUnitFileName};
  if (env.unit_path != ours) {
    return ServiceOutcome{false,
                          "service: only " + ours.string() + " is strata's unit; a unit in any other location "
                                                             "(a system-scope unit, a renamed file) is never touched"};
  }
  const auto disable = runner({"systemctl", "--user", "disable", "--now", std::string{kServiceName}});
  if (disable.exit_code != 0) {
    return ServiceOutcome{false, "service: systemctl --user disable --now strata failed (exit " +
                                     std::to_string(disable.exit_code) + "); nothing was removed"};
  }
  std::error_code ec;
  const auto presence = std::filesystem::symlink_status(env.unit_path, ec);
  if (ec) return ServiceOutcome{false, "service: the unit file could not be inspected: " + env.unit_path.string()};
  if (presence.type() == std::filesystem::file_type::not_found) {
    return ServiceOutcome{true, "service: no strata.service unit was installed — nothing to remove"};
  }
  std::filesystem::remove(env.unit_path, ec);  // exactly the checked path, never a tree
  if (ec) return ServiceOutcome{false, "service: the unit file could not be removed: " + env.unit_path.string()};
  return ServiceOutcome{true, "service: strata.service disabled and its unit removed (" + env.unit_path.string() + ")"};
#endif
}

ServiceStatus plan_status(const ServiceEnv& env, CommandRunner& runner) {
  (void)env;
#if defined(_WIN32)
  const auto registered = runner({"schtasks", "/Query", "/TN", std::string{kServiceName}});
  if (registered.exit_code < 0) {
    return ServiceStatus{
        "service: enabled: the Task Scheduler is not reachable (nothing is assumed)",
        "service: active: the Task Scheduler is not reachable (nothing is assumed)"};
  }
  const auto verbose = runner({"schtasks", "/Query", "/TN", std::string{kServiceName}, "/V", "/FO", "LIST"});
  const auto status_at = verbose.out.find("Status:");
  std::string active = "unknown";
  if (status_at != std::string::npos) {
    const std::string value = verbose.out.substr(status_at + 7, verbose.out.find_first_of("\r\n", status_at) - status_at - 7);
    active = value.find("Running") != std::string::npos ? "running" : "not running";
  }
  return ServiceStatus{std::string{"service: enabled: "} + (registered.exit_code == 0 ? "registered" : "not registered"),
                       "service: active: " + (registered.exit_code == 0 ? active : std::string{"not registered"})};
#else
  const auto enabled = runner({"systemctl", "--user", "is-enabled", std::string{kServiceName}});
  const auto active = runner({"systemctl", "--user", "is-active", std::string{kServiceName}});
  if (manager_unreachable(enabled) || manager_unreachable(active)) {
    return ServiceStatus{
        "service: enabled: the systemd user manager is not reachable (nothing is assumed)",
        "service: active: the systemd user manager is not reachable (nothing is assumed)"};
  }
  const auto enabled_state = first_token(enabled.out);
  const auto active_state = first_token(active.out);
  return ServiceStatus{"service: enabled: " + (enabled_state.empty() ? std::string{"not installed"} : enabled_state),
                       "service: active: " + (active_state.empty() ? std::string{"inactive"} : active_state)};
#endif
}

bool user_manager_reachable(CommandRunner& runner) {
#if defined(_WIN32)
  const auto probe = runner({"schtasks", "/Query", "/TN", std::string{kServiceName}});
  return probe.exit_code >= 0;
#else
  const auto probe = runner({"systemctl", "--user", "is-enabled", std::string{kServiceName}});
  return !manager_unreachable(probe);
#endif
}

}  // namespace service
}  // namespace secure_frontend
