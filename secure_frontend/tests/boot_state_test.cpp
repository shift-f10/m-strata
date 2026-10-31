#include "secure_frontend/boot_state.hpp"
#include "secure_frontend/json.hpp"
#include "support/test_support.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <variant>

namespace {

int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

// The answer's string member `name` ("" when it is not one).
std::string member(const std::string& json, const std::string_view name) {
  const secure_frontend::JsonLimits limits{.max_bytes = 4096U, .max_depth = 4U, .max_container_entries = 16U, .max_string_bytes = 1024U};
  const auto parsed = secure_frontend::parse_json(json, limits);
  const auto* root = std::get_if<secure_frontend::JsonValue>(&parsed);
  const auto* members = root != nullptr ? root->if_object() : nullptr;
  if (members == nullptr) return {};
  for (const auto& entry : *members) {
    if (entry.key == name) {
      const auto* text = entry.value.if_string();
      return text != nullptr ? *text : std::string{};
    }
  }
  return {};
}

// A Linux PC with these files and links.
secure_frontend::BootProbe linux_with(std::set<std::string> present) {
  return secure_frontend::BootProbe{
      .windows = false,
      .exists = [present = std::move(present)](const std::filesystem::path& path) { return present.contains(path.generic_string()); },
      .user_config = "/home/owner/.config",
      .user = "owner",
      .task_xml = {}};
}

constexpr std::string_view kUserUnit = "/home/owner/.config/systemd/user/m-strata.service";
constexpr std::string_view kUserWanted = "/home/owner/.config/systemd/user/default.target.wants/m-strata.service";
constexpr std::string_view kLinger = "/var/lib/systemd/linger/owner";
constexpr std::string_view kSystemUnit = "/etc/systemd/system/m-strata.service";
constexpr std::string_view kSystemWanted = "/etc/systemd/system/multi-user.target.wants/m-strata.service";

// Linux (install.sh --boot, spec part 5): the units install.sh writes, read as files - enabled is the link systemctl
// enable makes; a user service starts at boot only with lingering on, else when the owner logs in.
void linux_boot_is_read_from_the_units() {
  const auto none = secure_frontend::boot_state_json(linux_with({}));
  expect(member(none, "system") == "linux" && member(none, "starts") == "none", "none: " + none);
  expect(member(none, "change").find("install.sh --boot") != std::string::npos, "how to set it up: " + none);
  const auto user = secure_frontend::boot_state_json(linux_with({std::string{kUserUnit}, std::string{kUserWanted}, std::string{kLinger}}));
  expect(member(user, "starts") == "user" && member(user, "change") == "systemctl --user disable --now m-strata", "user: " + user);
  const auto login = secure_frontend::boot_state_json(linux_with({std::string{kUserUnit}, std::string{kUserWanted}}));
  expect(member(login, "starts") == "login" && member(login, "change") == "sudo loginctl enable-linger owner", "login: " + login);
  const auto off = secure_frontend::boot_state_json(linux_with({std::string{kUserUnit}, std::string{kLinger}}));
  expect(member(off, "starts") == "none" && member(off, "says").find("not enabled") != std::string::npos, "installed, not enabled: " + off);
  const auto system = secure_frontend::boot_state_json(linux_with({std::string{kSystemUnit}, std::string{kSystemWanted}}));
  expect(member(system, "starts") == "system" && member(system, "change") == "sudo systemctl disable --now m-strata", "system: " + system);
  for (const auto& json : {none, user, login, off, system}) expect(!member(json, "says").empty(), "a sentence: " + json);
}

// Windows (install.ps1 -Boot, spec part 6): the task M-Strata as schtasks shows it - its trigger says when; none, or
// none this account can read, is not with Windows. schtasks may print its XML as UTF-16.
void windows_boot_is_read_from_the_task() {
  const auto with = [](std::optional<std::string> xml) {
    return secure_frontend::BootProbe{.windows = true, .exists = {}, .user_config = {}, .user = {},
                                      .task_xml = [xml = std::move(xml)] { return xml; }};
  };
  const std::string boot = "<Task><Triggers><BootTrigger><Enabled>true</Enabled></BootTrigger></Triggers></Task>";
  const auto startup = secure_frontend::boot_state_json(with(boot));
  expect(member(startup, "system") == "windows" && member(startup, "starts") == "startup", "startup: " + startup);
  expect(member(startup, "change").find("schtasks /Delete /TN M-Strata /F") != std::string::npos, "how to remove it: " + startup);
  const auto logon = secure_frontend::boot_state_json(with("<Task><Triggers><LogonTrigger/></Triggers></Task>"));
  expect(member(logon, "starts") == "logon", "logon: " + logon);
  std::string wide;
  for (const char c : boot) {
    wide.push_back(c);
    wide.push_back('\0');
  }
  expect(member(secure_frontend::boot_state_json(with(wide)), "starts") == "startup", "UTF-16 output");
  const auto none = secure_frontend::boot_state_json(with(std::nullopt));
  expect(member(none, "starts") == "none" && member(none, "change").find("install.bat -Boot") != std::string::npos, "none: " + none);
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv,
                                         {
                                             {"linux_boot_is_read_from_the_units", linux_boot_is_read_from_the_units},
                                             {"windows_boot_is_read_from_the_task", windows_boot_is_read_from_the_task},
                                         },
                                         failures);
}
