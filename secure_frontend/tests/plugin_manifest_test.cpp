// Plugins (the owner, 2026-10-30; docs/superpowers/specs/2026-10-30-engine-family-plugins-design.md, section 8): a
// plugin's plugin.json read and checked - its kind, its texts, this platform's files, a diagnostics plugin's switches -
// and every other key, a wrong id and a path out of its folder refused by name.
#include "secure_frontend/plugin_manifest.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <map>
#include <iostream>
#include <iterator>
#include <string>
#include <variant>

namespace {

int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

const std::string kDebugging = R"({
  "schema": "m-strata.plugin/1",
  "id": "debugging",
  "kind": "diagnostics",
  "name": "Debugging",
  "version": "1.0.0",
  "publisher": "M-Strata",
  "license": "AGPL-3.0-only",
  "about": "Verbose logs, to find out why a model does not start or calibrate.",
  "diagnostics": {"switches": ["logging.requests", "logging.engine_output", "troubleshooting"]}
})";

// a code kind's manifest, its files per platform
std::string engine_manifest(const std::string& files) {
  return R"({"schema": "m-strata.plugin/1", "id": "acme3", "kind": "engine", "name": "Acme-3", "version": "0.1.0",
             "publisher": "Example Labs", "license": "Apache-2.0", "about": "A family.", "files": )" + files +
         R"(, "engine": {}})";
}

std::string why(const std::variant<secure_frontend::PluginManifest, std::string>& parsed) {
  const auto* reason = std::get_if<std::string>(&parsed);
  return reason != nullptr ? *reason : std::string{"(accepted)"};
}

// A diagnostics plugin: its texts and switches, no files.
void a_diagnostics_manifest_is_read() {
  const auto parsed = secure_frontend::parse_plugin_manifest(kDebugging, "debugging");
  const auto* manifest = std::get_if<secure_frontend::PluginManifest>(&parsed);
  expect(manifest != nullptr, "read: " + why(parsed));
  if (manifest == nullptr) return;
  expect(manifest->id == "debugging" && manifest->kind == secure_frontend::PluginKind::diagnostics && manifest->name == "Debugging" &&
             manifest->version == "1.0.0" && manifest->publisher == "M-Strata" && manifest->license == "AGPL-3.0-only" &&
             manifest->about.find("Verbose logs") == 0U && manifest->files.empty(),
         "its texts");
  using S = secure_frontend::DiagnosticSwitch;
  expect(manifest->switches == std::vector<S>{S::log_requests, S::log_engine_output, S::troubleshooting}, "its switches");
  expect(secure_frontend::plugin_kind_name(manifest->kind) == "diagnostics" &&
             secure_frontend::diagnostic_switch_name(S::troubleshooting) == "troubleshooting",
         "the names");
}

// A code kind: this platform's files (relative, '/'-separated, inside its folder), its kind's block.
void a_code_plugin_names_this_platforms_files() {
  const auto parsed = secure_frontend::parse_plugin_manifest(
      engine_manifest(R"({"windows": ["multiengine_acme3.dll", "bin/helper.dll"], "linux": ["libmultiengine_acme3.so"]})"), "acme3");
  const auto* manifest = std::get_if<secure_frontend::PluginManifest>(&parsed);
  expect(manifest != nullptr, "read: " + why(parsed));
  if (manifest == nullptr) return;
#if defined(_WIN32)
  expect(manifest->files == std::vector<std::string>{"multiengine_acme3.dll", "bin/helper.dll"}, "Windows' files");
#else
  expect(manifest->files == std::vector<std::string>{"libmultiengine_acme3.so"}, "Linux's files");
#endif
  expect(manifest->kind == secure_frontend::PluginKind::engine, "kind engine");
  // no files for this platform, a path out of its folder, an absolute path, a backslash: refused
  for (const auto& [files, reason] : std::vector<std::pair<std::string, std::string>>{
           {R"({"windows": [], "linux": []})", "files"},
           {R"({"windows": ["../escape.dll"], "linux": ["../escape.so"]})", "files"},
           {R"({"windows": ["C:/abs.dll"], "linux": ["/abs.so"]})", "files"},
           {R"({"windows": ["a\\b.dll"], "linux": ["a\\b.so"]})", "files"},
           {R"({"windows": ["./x.dll"], "linux": ["./x.so"]})", "files"}}) {
    const auto refused = secure_frontend::parse_plugin_manifest(engine_manifest(files), "acme3");
    expect(why(refused).find(reason) != std::string::npos, "refused " + files + ": " + why(refused));
  }
}

// Each refusal names what is wrong.
void a_wrong_manifest_is_refused_by_name() {
  const auto refuse = [](const std::string& text, const std::string& folder, const std::string& reason) {
    const auto parsed = secure_frontend::parse_plugin_manifest(text, folder);
    expect(why(parsed).find(reason) != std::string::npos, "refused for " + reason + ": " + why(parsed));
  };
  const auto with = [](const std::string& from, const std::string& to) {
    std::string text = kDebugging;
    const auto at = text.find(from);
    if (at != std::string::npos) text.replace(at, from.size(), to);
    return text;
  };
  refuse("not json", "debugging", "JSON");
  refuse(kDebugging, "other", "id");
  refuse(with("\"id\": \"debugging\"", "\"id\": \"Debugging\""), "Debugging", "id");
  refuse(with("m-strata.plugin/1", "m-strata.plugin/2"), "debugging", "schema");
  refuse(with("\"kind\": \"diagnostics\"", "\"kind\": \"theme\""), "debugging", "kind");
  refuse(with("\"name\": \"Debugging\"", "\"name\": \"\""), "debugging", "name");
  refuse(with("\"version\": \"1.0.0\"", "\"version\": \"1 0\""), "debugging", "version");
  refuse(with("\"name\": \"Debugging\",", "\"name\": \"Debugging\", \"extra\": 1,"), "debugging", "extra");
  refuse(with("\"logging.requests\"", "\"logging.anything\""), "debugging", "switches");
  refuse(with("\"diagnostics\": {\"switches\"", "\"engine\": {}, \"diagnostics\": {\"switches\""), "debugging", "engine");
  refuse(with("\"about\": \"Verbose logs", "\"about\": \"\\u0001 Verbose logs"), "debugging", "about");
  refuse(with(",\n  \"diagnostics\": {\"switches\": [\"logging.requests\", \"logging.engine_output\", \"troubleshooting\"]}", ""),
         "debugging", "diagnostics");
  refuse(engine_manifest(R"({"windows": ["a.dll"], "linux": ["a.so"]})").replace(
             engine_manifest(R"({"windows": ["a.dll"], "linux": ["a.so"]})").find(", \"engine\": {}"), 14, ""),
         "acme3", "engine");
  refuse(std::string(secure_frontend::kMaxPluginManifestBytes + 1U, ' '), "debugging", "large");
  expect(secure_frontend::valid_plugin_id("acme3_x") && !secure_frontend::valid_plugin_id("3acme") &&
             !secure_frontend::valid_plugin_id("") && !secure_frontend::valid_plugin_id(std::string(33U, 'a')) &&
             !secure_frontend::valid_plugin_id("a-b") && !secure_frontend::valid_plugin_id("../a"),
         "ids as the launcher takes them");
}

// A tool plugin (spec section 10): how its program runs - this platform's program (one of its files), arguments and
// environment as fixed texts or one setting each, what it connects to, its note - and its settings and presets.
const std::string kSearchTool = R"({"schema": "m-strata.plugin/1", "id": "search", "kind": "tool", "name": "Web search",
  "version": "1.0.0", "publisher": "M-Strata", "license": "AGPL-3.0-only", "about": "SearXNG.",
  "files": {"windows": ["strata-search.exe"], "linux": ["strata-search"]},
  "tool": {
    "program": {"windows": "strata-search.exe", "linux": "strata-search"},
    "arguments": ["--url", "{url}", "--results", "{results}"],
    "environment": {"SEARX_TOKEN": "{token}", "MODE": "plain"},
    "network": "the SearXNG instance at {url}",
    "note": "Today is {today}.",
    "call_timeout_s": 90,
    "settings": [
      {"name": "url", "kind": "url", "label": "SearXNG address", "help": "Its JSON format on.", "required": true},
      {"name": "results", "kind": "integer", "label": "Results", "help": "", "default": 5, "min": 1, "max": 10},
      {"name": "safe", "kind": "boolean", "label": "Safe search", "help": "", "default": false},
      {"name": "engine", "kind": "choice", "label": "Engine", "help": "", "default": "all", "choices": ["all", "news"]},
      {"name": "langs", "kind": "text_list", "label": "Languages", "help": "", "max_items": 3, "default": ["en"]},
      {"name": "token", "kind": "secret", "label": "Token", "help": "", "required": false}],
    "presets": [{"label": "Local", "values": {"url": "http://127.0.0.1:8080", "results": 3}}]}})";

void a_tool_manifest_says_how_its_program_runs() {
  const auto parsed = secure_frontend::parse_plugin_manifest(kSearchTool, "search");
  const auto* manifest = std::get_if<secure_frontend::PluginManifest>(&parsed);
  expect(manifest != nullptr && manifest->run.has_value(), "read, with how it runs: " + why(parsed));
  if (manifest == nullptr || !manifest->run) return;
  const auto& run = *manifest->run;
  using T = secure_frontend::PluginTemplate;
#if defined(_WIN32)
  expect(run.program == "strata-search.exe", "Windows' program: " + run.program);
#else
  expect(run.program == "strata-search", "Linux's program: " + run.program);
#endif
  expect(run.arguments == std::vector<T>{{.text = "--url"}, {.setting = "url"}, {.text = "--results"}, {.setting = "results"}},
         "its arguments, fixed or a setting");
  expect(run.environment.size() == 2U && run.environment[0].first == "SEARX_TOKEN" && run.environment[0].second == T{.setting = "token"} &&
             run.environment[1].first == "MODE" && run.environment[1].second == T{.text = "plain"},
         "its environment");
  expect(run.network == "the SearXNG instance at {url}" && run.note == "Today is {today}.", "network and note");
  expect(run.call_timeout == std::chrono::seconds(90), "a call's time limit");
  using K = secure_frontend::SettingKind;
  expect(run.settings.size() == 6U && run.settings[0].kind == K::url && run.settings[0].required &&
             run.settings[1].kind == K::integer && run.settings[1].min == 1 && run.settings[1].max == 10 &&
             run.settings[1].default_value == secure_frontend::PluginValue{std::int64_t{5}} &&
             run.settings[3].choices == std::vector<std::string>{"all", "news"} && run.settings[4].max_items == 3U &&
             run.settings[5].kind == K::secret && !run.settings[5].default_value,
         "its settings");
  expect(run.presets.size() == 1U && run.presets[0].label == "Local" &&
             run.presets[0].values.at("results") == secure_frontend::PluginValue{std::int64_t{3}},
         "its presets");
  expect(secure_frontend::setting_kind_name(K::text_list) == "text_list", "the kinds' names");
}

// Each refusal of a tool's or notifier's block names what is wrong.
void a_wrong_run_block_is_refused_by_name() {
  const auto refuse = [](const std::string& from, const std::string& to, const std::string& reason) {
    std::string text = kSearchTool;
    const auto at = text.find(from);
    expect(at != std::string::npos, "the test's text has: " + from);
    if (at != std::string::npos) text.replace(at, from.size(), to);
    const auto parsed = secure_frontend::parse_plugin_manifest(text, "search");
    expect(why(parsed).find(reason) != std::string::npos, "refused (" + reason + ") for " + to + ": " + why(parsed));
  };
  refuse(R"("program": {"windows": "strata-search.exe", "linux": "strata-search"})", R"("program": {"windows": "other.exe", "linux": "other"})",
         "program");
  refuse(R"("--results", "{results}")", R"("--token", "{token}")", "secret");
  refuse(R"("--results", "{results}")", R"("--results", "{nothing}")", "nothing");
  refuse(R"("--results", "{results}")", R"("--results={results}")", "arguments");
  refuse(R"("MODE": "plain")", R"("PATH": "plain")", "PATH");
  refuse(R"("MODE": "plain")", R"("STRATA_KEY": "plain")", "STRATA_");
  refuse(R"("MODE": "plain")", R"("mode": "plain")", "environment");
  refuse(R"("network": "the SearXNG instance at {url}")", R"("network": "at {token}")", "secret");
  refuse(R"("label": "Results")", R"("label": "Results", "colour": 1)", "colour");
  refuse(R"("kind": "integer")", R"("kind": "decimal")", "kind");
  refuse(R"("default": 5, "min": 1)", R"("default": 50, "min": 1)", "results");
  refuse(R"("default": "all")", R"("default": "old")", "engine");
  refuse(R"("required": false}])", R"("required": false, "default": "x"}])", "secret");
  refuse(R"({"name": "results")", R"({"name": "url")", "url");
  refuse(R"({"name": "results")", R"({"name": "Results")", "name");
  refuse(R"("max_items": 3, "default": ["en"])", R"("max_items": 1, "default": ["en", "de"])", "langs");
  refuse(R"("values": {"url": "http://127.0.0.1:8080", "results": 3})", R"("values": {"results": 30})", "results");
  refuse(R"("values": {"url": "http://127.0.0.1:8080", "results": 3})", R"("values": {"token": "x"})", "secret");
  refuse(R"("note": "Today is {today}.",)", R"("note": "Today is {today}.", "extra": 1,)", "extra");
  // a call's time limit: 1-600 seconds
  refuse(R"("call_timeout_s": 90,)", R"("call_timeout_s": 0,)", "call_timeout_s");
  refuse(R"("call_timeout_s": 90,)", R"("call_timeout_s": 601,)", "call_timeout_s");
  // a setting an argument names has a value: required, or a default
  refuse(R"("default": 5, "min": 1, "max": 10})", R"("min": 1, "max": 10})", "results");
  // a notifier has no note; only its own block
  std::string notifier = kSearchTool;
  notifier.replace(notifier.find(R"("kind": "tool")"), 14U, R"("kind": "notifier")");
  notifier.replace(notifier.find(R"("tool": {)"), 9U, R"("notifier": {)");
  const auto parsed = secure_frontend::parse_plugin_manifest(notifier, "search");
  expect(why(parsed).find("note") != std::string::npos, "a notifier has no note: " + why(parsed));
}

// A value is checked against its setting, and the values a plugin runs with are each setting's own or its default.
void values_are_checked_against_the_settings() {
  const auto parsed = secure_frontend::parse_plugin_manifest(kSearchTool, "search");
  const auto* manifest = std::get_if<secure_frontend::PluginManifest>(&parsed);
  if (manifest == nullptr || !manifest->run) {
    expect(false, "read: " + why(parsed));
    return;
  }
  const auto& run = *manifest->run;
  using V = secure_frontend::PluginValue;
  const auto setting = [&run](const std::string& name) -> const secure_frontend::PluginSetting& {
    return *std::find_if(run.settings.begin(), run.settings.end(), [&name](const auto& s) { return s.name == name; });
  };
  const auto fits = [](const secure_frontend::PluginSetting& s, const V& value) { return !secure_frontend::setting_value_problem(s, value); };
  expect(fits(setting("url"), V{std::string{"https://search.example.org/x"}}) && !fits(setting("url"), V{std::string{"ftp://x"}}) &&
             !fits(setting("url"), V{std::int64_t{1}}),
         "a url: http(s) only, a text");
  expect(fits(setting("results"), V{std::int64_t{10}}) && !fits(setting("results"), V{std::int64_t{11}}) &&
             !fits(setting("results"), V{std::string{"5"}}),
         "an integer within its bounds");
  expect(fits(setting("engine"), V{std::string{"news"}}) && !fits(setting("engine"), V{std::string{"old"}}), "a choice");
  expect(fits(setting("langs"), V{std::vector<std::string>{"en", "de", "fr"}}) &&
             !fits(setting("langs"), V{std::vector<std::string>{"a", "b", "c", "d"}}),
         "a text list within its items");
  expect(fits(setting("safe"), V{true}) && !fits(setting("safe"), V{std::string{"yes"}}), "a boolean");
  expect(secure_frontend::setting_value_problem(setting("results"), V{std::int64_t{11}})->find("results") != std::string::npos,
         "the reason names the setting");
  // the values it runs with
  const auto values = secure_frontend::plugin_values(run, {{"url", V{std::string{"http://127.0.0.1:8080"}}}, {"other", V{true}}}, {});
  const auto* got = std::get_if<std::map<std::string, V>>(&values);
  expect(got != nullptr && got->at("results") == V{std::int64_t{5}} && got->at("engine") == V{std::string{"all"}} && !got->contains("other") &&
             !got->contains("token"),
         "a setting's own value, else its default; an undeclared one left out; a secret never");
  const auto missing = secure_frontend::plugin_values(run, {}, {});
  expect(std::holds_alternative<std::string>(missing) && std::get<std::string>(missing).find("url") != std::string::npos,
         "a required setting missing: " + (std::holds_alternative<std::string>(missing) ? std::get<std::string>(missing) : std::string{}));
  const auto wrong = secure_frontend::plugin_values(run, {{"url", V{std::string{"http://x"}}}, {"results", V{std::int64_t{0}}}}, {});
  expect(std::holds_alternative<std::string>(wrong) && std::get<std::string>(wrong).find("results") != std::string::npos, "a value refused");
  expect(secure_frontend::value_text(V{std::vector<std::string>{"en", "de"}}) == "en,de" && secure_frontend::value_text(V{false}) == "false" &&
             secure_frontend::value_text(V{std::int64_t{7}}) == "7",
         "values as text");
}

// The search plugin this repository ships (plugins/search/plugin.json; the owner, 2026-10-30, decision 6): strata-search
// as a tool plugin, its SearXNG settings its own, its note today's search advice.
void the_shipped_search_plugin_is_one() {
  std::ifstream in(std::string{SECURE_FRONTEND_REPO_PLUGINS} + "/search/plugin.json", std::ios::binary);
  const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  const auto parsed = secure_frontend::parse_plugin_manifest(text, "search");
  const auto* manifest = std::get_if<secure_frontend::PluginManifest>(&parsed);
  expect(manifest != nullptr && manifest->run.has_value(), "read: " + why(parsed));
  if (manifest == nullptr || !manifest->run) return;
  const auto& run = *manifest->run;
  using T = secure_frontend::PluginTemplate;
  expect(manifest->kind == secure_frontend::PluginKind::tool && manifest->publisher == "M-Strata" && manifest->license == "AGPL-3.0-only",
         "a first-party tool plugin");
  expect(run.arguments == std::vector<T>{{.text = "--url"}, {.setting = "url"}, {.text = "--results"}, {.setting = "results"},
                                         {.text = "--timeout"}, {.setting = "timeout_s"}} &&
             run.environment.empty() && run.network == "the SearXNG instance at {url}" && run.call_timeout == std::chrono::seconds(365),
         "strata-search's arguments, its network, its time limit (each of its three steps at most 120 s, and a little more)");
  expect(run.settings.size() == 3U && run.settings[0].name == "url" && run.settings[0].required &&
             run.settings[1].default_value == secure_frontend::PluginValue{std::int64_t{5}} &&
             run.settings[2].default_value == secure_frontend::PluginValue{std::int64_t{30}},
         "its settings: the instance (needed), results per search, the time limit");
  expect(run.note.find("Today's date is {today}.") == 0U && run.note.find("web_search") != std::string::npos, "its note");
}

// The debugging plugin this repository ships (plugins/debugging/plugin.json; the owner, 2026-10-30: "we should make
// the email, websearch, debugging all plugins as well"): make puts it in every install, on with --debugging. It is a
// diagnostics plugin with each switch a make --debugging build had.
void the_shipped_debugging_plugin_is_one() {
  std::ifstream in(std::string{SECURE_FRONTEND_REPO_PLUGINS} + "/debugging/plugin.json", std::ios::binary);
  const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  expect(!text.empty(), "plugins/debugging/plugin.json is there");
  const auto parsed = secure_frontend::parse_plugin_manifest(text, "debugging");
  const auto* manifest = std::get_if<secure_frontend::PluginManifest>(&parsed);
  expect(manifest != nullptr, "read: " + why(parsed));
  if (manifest == nullptr) return;
  using S = secure_frontend::DiagnosticSwitch;
  expect(manifest->kind == secure_frontend::PluginKind::diagnostics && manifest->name == "Debugging" &&
             manifest->publisher == "M-Strata" && manifest->license == "AGPL-3.0-only" &&
             manifest->switches == std::vector<S>{S::log_requests, S::log_engine_output, S::troubleshooting},
         "a diagnostics plugin with each switch");
  for (const char* words : {"one line per request", "the engine's own output", "your prompts", "Settings > Troubleshooting"}) {
    expect(manifest->about.find(words) != std::string::npos, std::string{"its about says: "} + words);
  }
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"the_shipped_debugging_plugin_is_one", the_shipped_debugging_plugin_is_one},
      {"the_shipped_search_plugin_is_one", the_shipped_search_plugin_is_one},
      {"a_diagnostics_manifest_is_read", a_diagnostics_manifest_is_read},
      {"a_code_plugin_names_this_platforms_files", a_code_plugin_names_this_platforms_files},
      {"a_wrong_manifest_is_refused_by_name", a_wrong_manifest_is_refused_by_name},
      {"a_tool_manifest_says_how_its_program_runs", a_tool_manifest_says_how_its_program_runs},
      {"a_wrong_run_block_is_refused_by_name", a_wrong_run_block_is_refused_by_name},
      {"values_are_checked_against_the_settings", values_are_checked_against_the_settings},
  }, failures);
}
