// The Plugins tab's server half (the owner, 2026-10-30: "we also need to have a plugin management tab to manage these"):
// the list with each plugin's state, approval (its SHA-256s pinned, kept in <roots.state>/plugins.json) and on/off, each
// applying at the next start; refusals with their reason; this PC only by default.
#include "secure_frontend/plugin_routes.hpp"
#include "support/test_support.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

namespace {

int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

bool has(const secure_frontend::HttpResponse& response, const std::string_view text) { return response.body.find(text) != std::string::npos; }
std::string shown(const secure_frontend::HttpResponse& response) { return std::to_string(response.status) + " " + response.body; }

const std::string kDebugging = R"({"schema": "m-strata.plugin/1", "id": "debugging", "kind": "diagnostics",
  "name": "Debugging", "version": "1.0.0", "publisher": "M-Strata", "license": "AGPL-3.0-only", "about": "Verbose logs.",
  "diagnostics": {"switches": ["logging.requests", "troubleshooting"]}})";
const std::string kTool = R"({"schema": "m-strata.plugin/1", "id": "search", "kind": "tool", "name": "Web search",
  "version": "1.0.0", "publisher": "M-Strata", "license": "AGPL-3.0-only", "about": "", "tool": {},
  "files": {"windows": ["strata-search.exe"], "linux": ["strata-search"]}})";

// An install's plugins folder and a data folder's state folder, and the routes over them.
struct Fixture final {
  std::filesystem::path root = secure_frontend_test::make_scratch_directory();
  std::filesystem::path plugins = root / "plugins";
  std::filesystem::path state = root / "state";
  std::unique_ptr<secure_frontend::PluginRoutes> routes;
  std::vector<std::string> logged;
  explicit Fixture(std::set<std::string> running = {}, std::vector<secure_frontend::FirstPartyPlugin> first_party = {}) {
    secure_frontend_test::make_private_directory(plugins);
    secure_frontend_test::make_private_directory(state);
    add("debugging", kDebugging);
    add("search", kTool);
#if defined(_WIN32)
    secure_frontend_test::write_file(plugins / "search" / "strata-search.exe", "tool");
#else
    secure_frontend_test::write_file(plugins / "search" / "strata-search", "tool", 0700);
#endif
    secure_frontend::PluginRouteSettings settings{.plugins_dir = plugins, .state_dir = state, .first_party = std::move(first_party),
                                                  .running = std::move(running)};
    settings.log = [this](const std::string_view line) { logged.emplace_back(line); };
    routes = std::make_unique<secure_frontend::PluginRoutes>(std::move(settings));
  }
  void add(const std::string& id, const std::string& manifest) const {
    secure_frontend_test::make_private_directory(plugins / id);
    secure_frontend_test::write_file(plugins / id / "plugin.json", manifest);
  }
  secure_frontend::HttpResponse post(const secure_frontend::RouteId route, const std::string& body, const bool loopback = true) {
    return routes->answer(route, body, secure_frontend::RequestContext{.client_gone = {}, .loopback_peer = loopback});
  }
};

using secure_frontend::RouteId;

// The list: each plugin with what it is, its state and reason, whether it ran at this start, its SHA-256s and what it
// switches; never cached; another PC refused.
void the_plugins_are_listed() {
  Fixture f({"debugging"});
  const auto listed = f.post(RouteId::strata_plugins, "{}");
  expect(listed.status == 200 && has(listed, "\"editable\":true") && has(listed, "\"folder\":"), "listed: " + shown(listed));
  expect(has(listed, R"("id":"debugging","kind":"diagnostics","name":"Debugging","version":"1.0.0","publisher":"M-Strata")") &&
             has(listed, R"("first_party":false,"state":"waiting","reason":"waiting for the owner's approval","running":true)") &&
             has(listed, R"("switches":["logging.requests","troubleshooting"])") && has(listed, "\"manifest_sha256\":\""),
         "the debugging plugin: " + listed.body);
  expect(has(listed, R"("id":"search","kind":"tool")") && has(listed, "\"files\":[{\"path\":\"strata-search") &&
             has(listed, "\"running\":false"),
         "the tool plugin with its file: " + listed.body);
  expect(listed.header_value("Cache-Control") == std::optional<std::string_view>{"no-store"}, "never cached");
  expect(f.post(RouteId::strata_plugins, "{}", false).status == 403, "another PC is refused");
}

// Approval pins the SHA-256s now (in <roots.state>/plugins.json) and turns the plugin on; switching turns it off and on;
// each applies at the next start.
void approval_and_switching_are_kept() {
  Fixture f;
  const auto approved = f.post(RouteId::strata_plugins_approve, R"({"id":"debugging"})");
  expect(approved.status == 200 && has(approved, R"("id":"debugging","state":"on","restart_needed":true)"), "approved: " + shown(approved));
  const auto kept = secure_frontend::read_plugin_choices(f.state);
  expect(kept.has_value() && kept->approved.contains("debugging") && kept->approved.at("debugging").manifest.size() == 64U &&
             kept->enabled.at("debugging"),
         "kept in plugins.json: " + secure_frontend_test::read_file(f.state / "plugins.json"));
  expect(has(f.post(RouteId::strata_plugins, "{}"), R"("state":"on")"), "listed on");
  const auto off = f.post(RouteId::strata_plugins_switch, R"({"id":"debugging","on":false})");
  expect(off.status == 200 && has(off, R"("state":"off","restart_needed":true)"), "turned off: " + shown(off));
  expect(f.post(RouteId::strata_plugins_switch, R"({"id":"debugging","on":true})").status == 200, "and on again");
  expect(!f.logged.empty() && f.logged.back().find("debugging") != std::string::npos, "the server log says so");
}

// Refusals say why: no such plugin; one that waits cannot be switched; one of a kind this server does not run yet can be
// approved but not turned on; a trusted one is not approved again; a body that is not one; a choices file that is not one.
void refusals_say_why() {
  Fixture f;
  const auto refused = [&f](const RouteId route, const std::string& body, const std::string& why) {
    const auto answer = f.post(route, body);
    expect(answer.status == 422 && has(answer, "\"code\":\"plugin_refused\"") && has(answer, why), body + " refused for " + why + ": " + shown(answer));
  };
  refused(RouteId::strata_plugins_approve, R"({"id":"ghost"})", "no plugin");
  refused(RouteId::strata_plugins_switch, R"({"id":"debugging","on":true})", "approve");
  expect(f.post(RouteId::strata_plugins_approve, R"({"id":"search"})").status == 200, "a tool plugin can be approved");
  refused(RouteId::strata_plugins_switch, R"({"id":"search","on":true})", "does not run tool plugins yet");
  expect(f.post(RouteId::strata_plugins_approve, R"({"id":"debugging"})").status == 200, "approved");
  refused(RouteId::strata_plugins_approve, R"({"id":"debugging"})", "already");
  expect(f.post(RouteId::strata_plugins_approve, "{}").status == 400 && f.post(RouteId::strata_plugins_switch, R"({"id":"debugging"})").status == 400 &&
             f.post(RouteId::strata_plugins_approve, R"({"id":"debugging","more":1})").status == 400,
         "a body that is not one");
  secure_frontend_test::write_file(f.state / "plugins.json", "not json");
  refused(RouteId::strata_plugins, "{}", "plugins.json");
  refused(RouteId::strata_plugins_switch, R"({"id":"debugging","on":false})", "plugins.json");
  expect(secure_frontend_test::read_file(f.state / "plugins.json") == "not json", "a choices file that is not one is never overwritten");
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"the_plugins_are_listed", the_plugins_are_listed},
      {"approval_and_switching_are_kept", approval_and_switching_are_kept},
      {"refusals_say_why", refusals_say_why},
  }, failures);
}
