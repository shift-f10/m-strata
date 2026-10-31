#pragma once

// Start at boot (pinned and hot models, spec docs/superpowers/specs/2026-10-31-pinned-and-hot-models-design.md, parts 5
// and 6): what this PC starts the server with, read only - the Settings tab shows it, and the command that changes it.
// The server never changes it (the installers do, with the owner's sudo or administrator approval).
//
//   Linux (install.sh --boot): the systemd user unit <user config>/systemd/user/m-strata.service and its enablement
//   (default.target.wants), the lingering flag /var/lib/systemd/linger/<user> (a user service starts at boot only with
//   it), and the system unit /etc/systemd/system/m-strata.service and its enablement (multi-user.target.wants).
//   Windows (install.ps1 -Boot): the Task Scheduler task M-Strata, as `schtasks /Query /TN M-Strata /XML` shows it to
//   this account - a BootTrigger starts it with the PC, a LogonTrigger at the log-on.

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace secure_frontend {

struct BootProbe final {
  bool windows{false};
  // Linux: whether a file or a link is there (an enablement is a link)
  std::function<bool(const std::filesystem::path&)> exists{};
  std::filesystem::path user_config{};  // XDG_CONFIG_HOME, else HOME/.config
  std::string user{};                   // this account's name (its lingering flag)
  // Windows: the task's XML as schtasks prints it; nullopt: no task this account can read
  std::function<std::optional<std::string>()> task_xml{};
};

// {"system": "linux"|"windows", "starts": ..., "says": "<one sentence>", "change": "<the command that changes it>"}.
// starts - Linux: "system" (the system unit enabled), "user" (the user unit enabled, lingering on), "login" (the user unit
// enabled, lingering off: it starts when the owner logs in), "none"; Windows: "startup", "logon", "none".
[[nodiscard]] std::string boot_state_json(const BootProbe& probe);

// This PC's probe: the file system as this process sees it, this account's name, and schtasks on Windows.
[[nodiscard]] BootProbe this_pc_boot_probe();

}  // namespace secure_frontend
