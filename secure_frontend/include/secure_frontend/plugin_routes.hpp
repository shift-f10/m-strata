#pragma once

// The Plugins tab's server half (the owner, 2026-10-30: "we also need to have a plugin management tab to manage these";
// docs/superpowers/specs/2026-10-30-engine-family-plugins-design.md, section 8). Every plugin of the install with its
// state (plugin_registry.hpp), the owner's approval - its SHA-256s pinned - and on/off, kept in
// <roots.state>/plugins.json; a change applies when the server restarts (what runs is decided at the start). For the
// clients the other settings answer (loopback, and the ones client_allowed lets in).
//
//   POST /v1/strata/plugins          {} -> {"folder", "editable", "plugins": [{"id", "kind", "name", "version",
//                                       "publisher", "license", "about", "first_party", "state", "reason", "running",
//                                       "manifest_sha256", "files": [{"path", "sha256"}], "switches": [...]}]}
//   POST /v1/strata/plugins/approve  {"id"} -> {"id", "state", "restart_needed": true}
//                                       (waiting or changed: its SHA-256s now are pinned, and it is turned on)
//   POST /v1/strata/plugins/switch   {"id", "on"} -> {"id", "state", "restart_needed": true}   (a trusted plugin only)
//   POST /v1/strata/plugins/settings {"id", "values": {name: value or null}} -> {"id", "saved": true, "restart_needed": true}
//                                       (a tool's or notifier's settings, plugin spec section 10: each checked against its
//                                       manifest; every value but a secret's in the config file as plugins.<id>.<name>, a
//                                       secret in the secret folder; null back to the default, or a secret removed)
//   POST /v1/strata/plugins/try      {"id", "tool", "arguments": {...}} -> {"ok", "text", "truncated", "ms"}
//                                       (one call of a running tool plugin's tool through the server's tool hub: the
//                                       Plugins tab's "Try", which replaced the page's test search)
//   POST /v1/strata/plugins/test     {"id"} -> {"ok", "reply"}   (a running notifier plugin's "Send a test" through the
//                                       server's notifier host, which replaced the page's test email)
// Each listed plugin also has its "settings" ({"name", "kind", "label", "help", "required", "default", "min"/"max",
// "choices", "max_items", and "value", or a secret's "set"}), "presets", "network" (its words, the values filled in),
// "settings_problem" (what keeps it from running, or null) and "tools" (a running tool plugin's, each {"name",
// "description", "string_parameters"}; none for any other).
// A refusal is plugin_refused with its reason (no such plugin, already trusted, refused, not trusted yet).

#include "secure_frontend/api_router.hpp"
#include "secure_frontend/mcp_client.hpp"
#include "secure_frontend/notifier_hub.hpp"
#include "secure_frontend/plugin_registry.hpp"
#include "secure_frontend/request_router.hpp"
#include "secure_frontend/tool_runner.hpp"

#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace secure_frontend {

struct PluginRouteSettings final {
  std::filesystem::path plugins_dir;                // <install>/plugins
  std::optional<std::filesystem::path> state_dir{}; // <roots.state>: the choices file's folder (unset: read-only)
  std::vector<FirstPartyPlugin> first_party{};      // engine/build.json's
  std::set<std::string> running{};                  // the plugins on when the server started
  std::optional<std::filesystem::path> config_file{}; // where plugins.<id>.<name> live (unset: settings read-only)
  std::optional<std::filesystem::path> secret_dir{};  // roots.secret: a secret setting's folder (unset: none kept)
  std::function<void(std::string_view)> log{};
  // The server's tool hub (tool_hub.hpp): the tools each running tool plugin lists, and one call for "Try". Unset: no tool
  // plugin runs in this server.
  std::function<std::map<std::string, std::vector<McpTool>>()> tools{};
  std::function<ToolResult(std::string_view id, std::string_view tool, std::string_view arguments_json)> try_tool{};
  // The server's notifier host (notifier_hub.hpp): one test of a running notifier plugin. Unset: no notifier plugin runs.
  std::function<NotifierTest(std::string_view id)> test_notifier{};
  // Who may read and change them (the server passes model_files_allowed, as for its other settings). Empty: loopback
  // clients only.
  std::function<bool(const RequestContext& context)> client_allowed{};
};

class PluginRoutes final {
 public:
  explicit PluginRoutes(PluginRouteSettings settings);
  [[nodiscard]] HttpResponse answer(RouteId route, std::string_view body, const RequestContext& context);

 private:
  [[nodiscard]] HttpResponse list() const;
  [[nodiscard]] HttpResponse approve(std::string_view body);
  [[nodiscard]] HttpResponse change(std::string_view body);
  [[nodiscard]] HttpResponse save_settings(std::string_view body);
  [[nodiscard]] HttpResponse try_tool(std::string_view body);
  [[nodiscard]] HttpResponse test_notifier(std::string_view body);
  [[nodiscard]] std::optional<PluginChoices> choices() const;
  [[nodiscard]] bool save(const PluginChoices& choices);

  PluginRouteSettings settings_;
  std::mutex file_;  // one choices write at a time
};

}  // namespace secure_frontend
