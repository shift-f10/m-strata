#include "secure_frontend/boot_state.hpp"

#include "secure_frontend/crt_environment.hpp"
#include "secure_frontend/json.hpp"

#include "service_ops.hpp"

#include <string_view>
#include <system_error>

#if !defined(_WIN32)
#include <pwd.h>
#include <unistd.h>
#endif

namespace secure_frontend {
namespace {

constexpr std::string_view kUnitName = "m-strata.service";
#if defined(_WIN32)
constexpr std::string_view kTaskName = "M-Strata";
#endif

std::string answer(const std::string_view system, const std::string_view starts, const std::string_view says,
                   const std::string_view change) {
  std::string out = "{\"system\":";
  append_json_string(&out, system);
  out += ",\"starts\":";
  append_json_string(&out, starts);
  out += ",\"says\":";
  append_json_string(&out, says);
  out += ",\"change\":";
  append_json_string(&out, change);
  out += "}";
  return out;
}

std::string linux_state(const BootProbe& probe) {
  const auto there = [&probe](const std::filesystem::path& path) { return probe.exists && probe.exists(path); };
  const auto user_dir = probe.user_config / "systemd" / "user";
  const bool user_unit = there(user_dir / kUnitName);
  const bool user_enabled = user_unit && there(user_dir / "default.target.wants" / kUnitName);
  const bool linger = there(std::filesystem::path{"/var/lib/systemd/linger"} / probe.user);
  const std::filesystem::path system_dir{"/etc/systemd/system"};
  const bool system_unit = there(system_dir / kUnitName);
  const bool system_enabled = system_unit && there(system_dir / "multi-user.target.wants" / kUnitName);
  if (system_enabled) {
    return answer("linux", "system", "The server starts at boot, as the system service m-strata (install.sh --boot system).",
                  "sudo systemctl disable --now m-strata");
  }
  if (user_enabled && linger) {
    return answer("linux", "user",
                  "The server starts at boot, as your systemd user service m-strata, with lingering on (install.sh --boot user).",
                  "systemctl --user disable --now m-strata");
  }
  if (user_enabled) {
    return answer("linux", "login",
                  "The server starts when you log in, as your systemd user service m-strata; with lingering off, not at boot.",
                  "sudo loginctl enable-linger " + probe.user);
  }
  if (user_unit || system_unit) {
    return answer("linux", "none", "The m-strata service is installed but not enabled: the server does not start at boot.",
                  user_unit ? "systemctl --user enable m-strata" : "sudo systemctl enable m-strata");
  }
  return answer("linux", "none", "The server does not start at boot.", "install.sh --boot user|system (README: Start at boot)");
}

std::string windows_state(const BootProbe& probe) {
  std::optional<std::string> xml = probe.task_xml ? probe.task_xml() : std::nullopt;
  if (xml) std::erase(*xml, '\0');  // (schtasks may print UTF-16)
  if (xml && xml->find("<BootTrigger") != std::string::npos) {
    return answer("windows", "startup", "The server starts with this PC, before anyone logs on (the task M-Strata, install.ps1 -Boot startup).",
                  "schtasks /Delete /TN M-Strata /F (as an administrator)");
  }
  if (xml && xml->find("<LogonTrigger") != std::string::npos) {
    return answer("windows", "logon", "At your log-on, strata-window opens in the tray and starts the server (the task M-Strata, install.ps1 -Boot logon).",
                  "schtasks /Delete /TN M-Strata /F");
  }
  return answer("windows", "none", "The server does not start with Windows.", "install.bat -Boot logon|startup (README: Start with Windows)");
}

}  // namespace

std::string boot_state_json(const BootProbe& probe) { return probe.windows ? windows_state(probe) : linux_state(probe); }

BootProbe this_pc_boot_probe() {
  BootProbe probe;
#if defined(_WIN32)
  probe.windows = true;
  probe.task_xml = []() -> std::optional<std::string> {
    // schtasks from System32 only (service_ops' run_argv: never a planted copy), read only
    const auto shown = service::run_argv({"schtasks", "/Query", "/TN", std::string{kTaskName}, "/XML"});
    if (shown.exit_code != 0) return std::nullopt;
    return shown.out;
  };
#else
  probe.exists = [](const std::filesystem::path& path) {
    std::error_code ignored;
    return std::filesystem::exists(std::filesystem::symlink_status(path, ignored));
  };
  const char* config = crt_getenv("XDG_CONFIG_HOME");
  const char* home = crt_getenv("HOME");
  if (config != nullptr && config[0] == '/') probe.user_config = config;
  else if (home != nullptr && home[0] == '/') probe.user_config = std::filesystem::path{home} / ".config";
  if (const passwd* account = ::getpwuid(::geteuid()); account != nullptr && account->pw_name != nullptr) probe.user = account->pw_name;
#endif
  return probe;
}

}  // namespace secure_frontend
