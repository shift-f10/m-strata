#pragma once
// SP7 M1 Task 4 (spec D-6): the uninstall plan. The planner is pure — it turns
// what the verb observed (health probe, unit-file presence, typed confirmation)
// into an ordered removal list under allowed roots, or a refusal. The CLI
// (Task 5) performs the stop and the removals; this header never touches the
// filesystem beyond reading. Internal header (src/): our verbs and tests only.
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "secure_frontend/config.hpp"

namespace secure_frontend {
namespace uninstall {

// What to do about a server that answers on the listener before removals.
enum class StopDecision { none, via_service_manager, foreground_running_refuse };

// Everything the planner decides from. Paths arrive already derived by the
// verb (config file, typed-config roots, the service unit path). allowed_roots
// bounds every removal: nothing outside them is ever listed (D-6).
struct UninstallEnv final {
  std::filesystem::path unit_path;   // empty when no unit is installed
  std::filesystem::path token_path;  // empty when the token has no file of ours
  std::filesystem::path config_path;
  std::filesystem::path state_root;
  std::filesystem::path models_root;
  std::filesystem::path logs_root;
  std::vector<std::filesystem::path> allowed_roots;
  bool unit_installed{false};
  bool manager_available{false};    // the user service manager answers probes
  bool server_listening{false};     // the loopback health probe got an HTTP answer
  bool install_location_ok{true};   // program dir/current match install metadata
  bool purge{false};
  bool purge_confirmed{false};      // the CLI obtained the typed confirmation word
};

struct UninstallPlan final {
  bool proceed{false};
  StopDecision stop{StopDecision::none};
  std::vector<std::filesystem::path> removals;  // ordered: unit, token, config, state, models
  std::vector<std::filesystem::path> kept;      // models (unless purged) and logs: printed, never removed
  std::string deny_reason;                      // human text when proceed == false
};

// The pure decision. Order of refusals: install metadata, allowed-root escapes,
// the --purge typed gate, then the health-probe stop table.
[[nodiscard]] UninstallPlan plan_uninstall(const UninstallEnv& env);

// True when a loopback GET /health on host:port receives any HTTP status line
// (a 401 still proves the server answers). Silent timeout/unreachable = false.
[[nodiscard]] bool server_answers_on(const IpAddress& host, std::uint16_t port);

}  // namespace uninstall
}  // namespace secure_frontend
