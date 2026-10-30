// The Plugins tab's server half (the owner, 2026-10-30: "we also need to have a plugin management tab to manage these"):
// the list with each plugin's state, approval (its SHA-256s pinned, kept in <roots.state>/plugins.json) and on/off, each
// applying at the next start; refusals with their reason; this PC only by default.
#include "secure_frontend/plugin_routes.hpp"
#include "secure_frontend/plugin_secrets.hpp"
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
  "version": "1.0.0", "publisher": "M-Strata", "license": "AGPL-3.0-only", "about": "", "tool": {"program": {"windows": "strata-search.exe", "linux": "strata-search"}},
  "files": {"windows": ["strata-search.exe"], "linux": ["strata-search"]}})";

// A notifier with settings of each kind, a secret among them, a preset and what it connects to (plugin spec section 10).
const std::string kMailer = R"({"schema": "m-strata.plugin/1", "id": "mailer", "kind": "notifier", "name": "Mailer",
  "version": "1.0.0", "publisher": "Example Labs", "license": "MIT", "about": "",
  "files": {"windows": ["mailer.exe"], "linux": ["mailer"]},
  "notifier": {"program": {"windows": "mailer.exe", "linux": "mailer"}, "arguments": ["--host", "{host}"],
    "environment": {"MAIL_PASSWORD": "{password}"}, "network": "the mail server {host}:{port}",
    "settings": [
      {"name": "host", "kind": "text", "label": "Mail server", "help": "Its name.", "required": true},
      {"name": "port", "kind": "integer", "label": "Port", "help": "", "default": 465, "min": 1, "max": 65535},
      {"name": "security", "kind": "choice", "label": "Security", "help": "", "default": "ssl", "choices": ["ssl", "starttls"]},
      {"name": "to", "kind": "text_list", "label": "To", "help": "", "required": true, "max_items": 5},
      {"name": "password", "kind": "secret", "label": "Password", "help": "", "required": true}],
    "presets": [{"label": "IONOS", "values": {"host": "smtp.ionos.com", "port": 465, "security": "ssl"}}]}})";

// An engine plugin, its library there: a kind this server does not run yet.
const std::string kEngine = R"({"schema": "m-strata.plugin/1", "id": "acme", "kind": "engine", "name": "Acme",
  "version": "0.1.0", "publisher": "Example Labs", "license": "Apache-2.0", "about": "", "engine": {},
  "files": {"windows": ["multiengine_acme.dll"], "linux": ["libmultiengine_acme.so"]}})";

// An install's plugins folder and a data folder's state, secret folder and config file, and the routes over them.
struct Fixture final {
  std::filesystem::path root = secure_frontend_test::make_scratch_directory();
  std::filesystem::path plugins = root / "plugins";
  std::filesystem::path state = root / "state";
  std::filesystem::path secret = root / "secret";
  std::filesystem::path config = root / "config.json";
  std::unique_ptr<secure_frontend::PluginRoutes> routes;
  std::vector<std::string> logged;
  std::vector<std::string> tried;   // each Try's id, tool and arguments, as the hub got them
  std::vector<std::string> tested;  // each notifier test's id, as the notifier host got it
  explicit Fixture(std::set<std::string> running = {}, std::vector<secure_frontend::FirstPartyPlugin> first_party = {},
                   const bool with_tools = false, const bool with_notifiers = false) {
    secure_frontend_test::make_private_directory(plugins);
    secure_frontend_test::make_private_directory(state);
    secure_frontend_test::make_private_directory(secret);
    secure_frontend_test::write_file(config, "{\"schema_version\": 1}\n");
    add("debugging", kDebugging);
    add("search", kTool);
    add("mailer", kMailer);
    add("acme", kEngine);
#if defined(_WIN32)
    secure_frontend_test::write_file(plugins / "mailer" / "mailer.exe", "notifier");
    secure_frontend_test::write_file(plugins / "acme" / "multiengine_acme.dll", "engine");
#else
    secure_frontend_test::write_file(plugins / "mailer" / "mailer", "notifier", 0700);
    secure_frontend_test::write_file(plugins / "acme" / "libmultiengine_acme.so", "engine", 0700);
#endif
#if defined(_WIN32)
    secure_frontend_test::write_file(plugins / "search" / "strata-search.exe", "tool");
#else
    secure_frontend_test::write_file(plugins / "search" / "strata-search", "tool", 0700);
#endif
    secure_frontend::PluginRouteSettings settings{.plugins_dir = plugins, .state_dir = state, .first_party = std::move(first_party),
                                                  .running = std::move(running), .config_file = config, .secret_dir = secret};
    settings.log = [this](const std::string_view line) { logged.emplace_back(line); };
    if (with_tools) {
      // the tool hub's side: search runs and lists web_search; a call answers what it was given
      settings.tools = [] {
        return std::map<std::string, std::vector<secure_frontend::McpTool>>{
            {"search", {secure_frontend::McpTool{"web_search", "Search the web.",
                                                 R"({"type": "object", "properties": {"query": {"type": "string"}, "max": {"type": "integer"}}})"}}}};
      };
      settings.try_tool = [this](const std::string_view id, const std::string_view tool, const std::string_view arguments) {
        tried.push_back(std::string{id} + " " + std::string{tool} + " " + std::string{arguments});
        return secure_frontend::ToolResult{true, "found 3 results", 15U, false, false, std::chrono::milliseconds(42)};
      };
    }
    if (with_notifiers) {
      // the notifier host's side: the mailer answers a test with its mail server's words
      settings.test_notifier = [this](const std::string_view id) {
        tested.emplace_back(id);
        return secure_frontend::NotifierTest{true, "sent (250 ok)"};
      };
    }
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
  expect(f.post(RouteId::strata_plugins_approve, R"({"id":"mailer"})").status == 200, "a notifier plugin can be approved");
  expect(f.post(RouteId::strata_plugins_switch, R"({"id":"mailer","on":true})").status == 200, "and turned on: notifiers run");
  expect(f.post(RouteId::strata_plugins_approve, R"({"id":"acme"})").status == 200, "an engine plugin can be approved");
  refused(RouteId::strata_plugins_switch, R"({"id":"acme","on":true})", "does not run engine plugins yet");
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

// The owner (2026-10-30, decision 6): a tool's or notifier's settings, listed with their values (a secret's only as set
// or not), its presets, what it connects to and what keeps it from running; saved - every value but a secret's in the
// config file as plugins.<id>.<name>, a secret in the secret folder - checked against its manifest, null putting one
// back to its default; refusals by name; the server log never holds a value.
void settings_are_listed_and_saved() {
  Fixture f;
  auto listed = f.post(RouteId::strata_plugins, "{}");
  expect(has(listed, R"({"name":"host","kind":"text","label":"Mail server","help":"Its name.","required":true,"default":null,"value":null})") &&
             has(listed, R"({"name":"port","kind":"integer","label":"Port","help":"","required":false,"default":465,"min":1,"max":65535,"value":null})") &&
             has(listed, R"("choices":["ssl","starttls"])") && has(listed, R"("max_items":5)") &&
             has(listed, R"({"name":"password","kind":"secret","label":"Password","help":"","required":true,"set":false})"),
         "each setting with its rules and value: " + listed.body);
  expect(has(listed, R"("presets":[{"label":"IONOS","values":{"host":"smtp.ionos.com","port":465,"security":"ssl"}}])") &&
             has(listed, R"("network":"the mail server {host}:465")") &&
             has(listed, R"("settings_problem":"its setting host is required: set it in the Plugins tab")"),
         "its presets, what it connects to, and what keeps it from running: " + listed.body);
  expect(has(listed, R"("id":"debugging")") && has(listed, R"("settings":[],"presets":[],"network":"","settings_problem":null)"),
         "a plugin without settings: " + listed.body);
  const auto saved = f.post(RouteId::strata_plugins_settings,
                            R"({"id":"mailer","values":{"host":"smtp.example.org","to":["a@example.org"],"password":"s3cret pw"}})");
  expect(saved.status == 200 && has(saved, R"({"id":"mailer","saved":true,"restart_needed":true})"), "saved: " + shown(saved));
  const std::string file = secure_frontend_test::read_file(f.config);
  expect(file.find("\"mailer\"") != std::string::npos && file.find("smtp.example.org") != std::string::npos &&
             file.find("a@example.org") != std::string::npos && file.find("s3cret") == std::string::npos,
         "the config file has the values, never the secret:\n" + file);
  expect(secure_frontend::read_plugin_secret(f.secret, "mailer", "password") == std::string{"s3cret pw"}, "the secret is in the secret folder");
  listed = f.post(RouteId::strata_plugins, "{}");
  expect(has(listed, R"("value":"smtp.example.org")") && has(listed, R"("set":true)") && has(listed, R"("network":"the mail server smtp.example.org:465")") &&
             has(listed, R"("id":"mailer")") && listed.body.find("s3cret") == std::string::npos,
         "listed with its values, the secret as set: " + listed.body);
  expect(listed.body.find(R"("settings_problem":"its setting)") == std::string::npos, "nothing keeps it from running now: " + listed.body);
  // null puts a setting back to its default, and removes a secret
  expect(f.post(RouteId::strata_plugins_settings, R"({"id":"mailer","values":{"host":null,"password":null}})").status == 200, "cleared");
  expect(secure_frontend_test::read_file(f.config).find("smtp.example.org") == std::string::npos &&
             !secure_frontend::plugin_secret_set(f.secret, "mailer", "password"),
         "gone from the file and the secret folder");
  for (const auto& line : f.logged) expect(line.find("smtp.example.org") == std::string::npos && line.find("s3cret") == std::string::npos, "no value in the log: " + line);
  // refusals, by name
  const auto refused = [&f](const std::string& body, const std::string& why) {
    const auto answer = f.post(RouteId::strata_plugins_settings, body);
    expect(answer.status == 422 && has(answer, "\"code\":\"plugin_refused\"") && has(answer, why), body + " refused for " + why + ": " + shown(answer));
  };
  refused(R"({"id":"ghost","values":{}})", "no plugin");
  refused(R"({"id":"debugging","values":{"x":1}})", "no settings");
  refused(R"({"id":"mailer","values":{"colour":"red"}})", "colour");
  refused(R"({"id":"mailer","values":{"port":70000}})", "port");
  refused(R"({"id":"mailer","values":{"security":"none"}})", "security");
  refused(R"({"id":"mailer","values":{"password":"two\nlines"}})", "password");
  refused(R"({"id":"mailer","values":{"to":["a","b","c","d","e","f"]}})", "to");
  expect(f.post(RouteId::strata_plugins_settings, R"({"id":"mailer"})").status == 400, "values are needed");
  expect(f.post(RouteId::strata_plugins_settings, R"({"id":"mailer","values":{"port":465}})", false).status == 403, "another PC is refused");
  expect(secure_frontend_test::read_file(f.config).find("70000") == std::string::npos, "a refused value is never written");
}

// The Plugins tab's "Try" (plugin spec section 10; it replaced the page's test search): a running tool plugin lists its
// tools (name, description, string parameters); Try makes one call through the server's tool hub and answers what came
// back; one not in the folder, not a tool, or a body that is not one is refused.
void tools_are_listed_and_tried() {
  Fixture f({"search"}, {}, true);
  const auto listed = f.post(RouteId::strata_plugins, "{}");
  expect(has(listed, R"("tools":[{"name":"web_search","description":"Search the web.","string_parameters":["query"]}])"),
         "a running tool plugin's tools: " + listed.body);
  expect(has(listed, R"("id":"mailer")") && listed.body.find(R"("tools":[{)") == listed.body.rfind(R"("tools":[{)"),
         "other plugins list none");
  const auto tried = f.post(RouteId::strata_plugins_try, R"({"id":"search","tool":"web_search","arguments":{"query":"rust 1.90"}})");
  expect(tried.status == 200 && has(tried, R"({"ok":true,"text":"found 3 results","truncated":false,"ms":42})"), "tried: " + shown(tried));
  expect(f.tried == std::vector<std::string>{R"(search web_search {"query":"rust 1.90"})"}, "the hub got the call, its arguments compact");
  const auto refused = [&f](const std::string& body, const std::string& why) {
    const auto answer = f.post(RouteId::strata_plugins_try, body);
    expect(answer.status == 422 && has(answer, "\"code\":\"plugin_refused\"") && has(answer, why), body + " refused for " + why + ": " + shown(answer));
  };
  refused(R"({"id":"ghost","tool":"x","arguments":{}})", "no plugin");
  refused(R"({"id":"debugging","tool":"x","arguments":{}})", "not a tool");
  expect(f.post(RouteId::strata_plugins_try, R"({"id":"search"})").status == 400, "a tool and its arguments are needed");
  expect(f.post(RouteId::strata_plugins_try, R"({"id":"search","tool":"web_search","arguments":{}})", false).status == 403, "another PC is refused");
  // a server without the hub (no tool plugin runs): nothing listed, and Try says so
  Fixture none;
  expect(has(none.post(RouteId::strata_plugins, "{}"), R"("tools":[])"), "no tools listed");
  const auto nothing = none.post(RouteId::strata_plugins_try, R"({"id":"search","tool":"web_search","arguments":{}})");
  expect(nothing.status == 422 && has(nothing, "runs no tool plugins"), "no hub: " + shown(nothing));
}

// The Plugins tab's "Send a test" (plugin spec section 10; it replaced the page's test email): one test through the
// server's notifier host, its answer as the plugin gave it; one not in the folder, not a notifier, or a body that is not
// one is refused, and so is a server that runs no notifier plugins.
void notifiers_are_tested() {
  Fixture f({"mailer"}, {}, false, true);
  const auto tested = f.post(RouteId::strata_plugins_test, R"({"id":"mailer"})");
  expect(tested.status == 200 && has(tested, R"x({"ok":true,"reply":"sent (250 ok)"})x"), "tested: " + shown(tested));
  expect(f.tested == std::vector<std::string>{"mailer"}, "the notifier host got the test");
  const auto refused = [&f](const std::string& body, const std::string& why) {
    const auto answer = f.post(RouteId::strata_plugins_test, body);
    expect(answer.status == 422 && has(answer, "\"code\":\"plugin_refused\"") && has(answer, why), body + " refused for " + why + ": " + shown(answer));
  };
  refused(R"({"id":"ghost"})", "no plugin");
  refused(R"({"id":"search"})", "not a notifier");
  expect(f.post(RouteId::strata_plugins_test, "{}").status == 400 && f.post(RouteId::strata_plugins_test, R"({"id":"mailer","x":1})").status == 400,
         "a body that is not one");
  expect(f.post(RouteId::strata_plugins_test, R"({"id":"mailer"})", false).status == 403, "another PC is refused");
  Fixture none;
  const auto nothing = none.post(RouteId::strata_plugins_test, R"({"id":"mailer"})");
  expect(nothing.status == 422 && has(nothing, "runs no notifier plugins"), "no notifier host: " + shown(nothing));
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"the_plugins_are_listed", the_plugins_are_listed},
      {"approval_and_switching_are_kept", approval_and_switching_are_kept},
      {"refusals_say_why", refusals_say_why},
      {"settings_are_listed_and_saved", settings_are_listed_and_saved},
      {"tools_are_listed_and_tried", tools_are_listed_and_tried},
      {"notifiers_are_tested", notifiers_are_tested},
  }, failures);
}
