#pragma once

// The page's Settings tab, its server half (spec docs/superpowers/specs/2026-10-28-server-window-design.md, Part 1; the
// owner: "a configuration tab that we can configure and manage it's settings like api key, etc"). For the clients the
// other settings answer (loopback, and the ones ConfigRouteSettings::client_allowed lets in); key rotation for this PC
// only. Each body a small JSON object:
//
//   POST /v1/strata/config           {} -> {"editable", "debugging", "settings": [{"key", "type", "value", "default",
//                                            "min"?, "max"?, "help"}], "key": {"rotatable"}}   (the config file's values;
//                                            null: the file leaves it to the default. debugging: a make --debugging
//                                            build, the only one that lists and saves logging.engine_output, the page's
//                                            Troubleshooting section)
//   POST /v1/strata/config/settings  {"values": {"<key>": <value or null>, ...}} -> {"saved": true, "restart_needed": true}
//                                       (only the keys listed above; the whole file is checked after the change by the
//                                       rules the server starts with, and a value it would refuse is refused by name)
//   POST /v1/strata/key/rotate       {"confirm": "ROTATE"} -> {"key": "<the new key>", "restart_needed": true}
//                                       (a new key in the data folder's store, as `strata key rotate`; the running
//                                       server keeps the old one until it restarts)

#include "secure_frontend/api_router.hpp"
#include "secure_frontend/request_router.hpp"

#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace secure_frontend {

struct ConfigRouteSettings final {
  std::optional<std::filesystem::path> config_file{};  // the file the settings live in (unset: read-only)
  // The API key's store, when this server reads its key from one (token.source file:, not --token-env): rotation
  // writes there. Unset: the key cannot be rotated here.
  std::optional<std::filesystem::path> secret_dir{};
  std::string token_file{};  // below secret_dir
  std::function<void(std::string_view)> log{};
  // Who may read and save the settings (the server passes model_files_allowed, as for the other settings). Empty:
  // loopback clients only. Rotation answers loopback clients only, whatever this says.
  std::function<bool(const RequestContext& context)> client_allowed{};
};

class ConfigRoutes final {
 public:
  explicit ConfigRoutes(ConfigRouteSettings settings);
  [[nodiscard]] HttpResponse answer(RouteId route, std::string_view body, const RequestContext& context);

 private:
  [[nodiscard]] HttpResponse settings_json() const;
  [[nodiscard]] HttpResponse save(std::string_view body);
  [[nodiscard]] HttpResponse rotate(std::string_view body);

  ConfigRouteSettings settings_;
  std::mutex file_;  // one config or key write at a time
};

}  // namespace secure_frontend
