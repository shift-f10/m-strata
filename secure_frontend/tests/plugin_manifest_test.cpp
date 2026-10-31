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

// An engine plugin's block (plugin spec section 3, as built): the least a family declares.
const std::string kMinimalEngine =
    R"({"abi": 1, "dialect": "strata_0143", "chat": "flint", "tokenizer": {"source": "gguf", "mode": "flint_bpe"},
        "models": {"gguf_architectures": ["acme3"], "context_key": "acme3.context_length"},
        "arguments": [{"flag": "--max-context", "value": "integer", "min": 1, "max": 262144}],
        "capabilities": {"vision": false, "web_search": false, "mtp": "none"}})";
// ... and all it may declare
const std::string kAcmeEngine = R"({
    "abi": 1, "dialect": "strata_0143", "chat": "flint",
    "tokenizer": {"source": "gguf", "mode": "flint_bpe"},
    "models": {"gguf_architectures": ["acme3", "acme3moe"], "context_key": "acme3.context_length", "mtp_key": "acme3.nextn_predict_layers"},
    "arguments": [
      {"flag": "--max-context", "value": "integer", "min": 4096, "max": 262144},
      {"flag": "--expert-vram", "value": "gibibytes", "min": 0, "max": 1024},
      {"flag": "--cpu-threads", "value": "integer", "min": 1, "max": 256},
      {"flag": "--pcie-frac", "value": "fraction"},
      {"flag": "--cpu-lane", "value": "on_off"},
      {"flag": "--no-graph", "value": "none"},
      {"flag": "--kv", "value": "choice", "choices": ["f16", "q8"]},
      {"flag": "--mtp", "value": "gguf_file"}],
    "options": [
      {"name": "max_context", "kind": "integer", "label": "Context length", "help": "Tokens the model reads.", "flag": "--max-context",
       "min": 4096, "max": 262144, "values": [8192, 32768, 65536, 131072], "default": 65536},
      {"name": "expert_vram", "kind": "integer_or_auto", "label": "Expert VRAM", "flag": "--expert-vram", "min": 0, "max": 1024, "default": "auto"},
      {"name": "pcie_frac", "kind": "decimal", "label": "PCIe share", "flag": "--pcie-frac", "min": 0, "max": 1},
      {"name": "cpu_lane", "kind": "boolean", "label": "CPU lane", "flag": "--cpu-lane", "default": true},
      {"name": "kv", "kind": "choice", "label": "KV cache", "flag": "--kv", "choices": ["f16", "q8"], "default": "q8"},
      {"name": "gpus", "kind": "devices", "label": "GPUs", "max": 8}],
    "sampling": {"temperature": 1.0, "top_p": 0.95, "top_k": 64, "min_p": 0.05},
    "calibration": {"before_first_start": true, "keep_margin": 0.03,
                    "steps": [{"flag": "--cpu-threads", "values": ["{cores}", "{cores*2/3}", "{cores/2}"]},
                              {"flag": "--pcie-frac", "values": ["0", "0.25", "0.5", "0.75"]}]},
    "monitor": {"experts_vram": "vram_slots", "experts_ram": "pinned_gib"},
    "capabilities": {"vision": false, "web_search": true, "mtp": "file"}
  })";

// a code kind's manifest, its files per platform (no engine block: an empty `engine`)
std::string engine_manifest(const std::string& files, const std::string& engine = kMinimalEngine) {
  return R"({"schema": "m-strata.plugin/1", "id": "acme3", "kind": "engine", "name": "Acme-3", "version": "0.1.0",
             "publisher": "Example Labs", "license": "Apache-2.0", "about": "A family.", "files": )" + files +
         (engine.empty() ? std::string{} : ", \"engine\": " + engine) + "}";
}
const std::string kAcmeFiles = R"({"windows": ["multiengine_acme3.dll"], "linux": ["libmultiengine_acme3.so"]})";

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
  refuse(engine_manifest(R"({"windows": ["a.dll"], "linux": ["a.so"]})", ""), "acme3", "engine");
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

// Email alerts as a notifier plugin (plugin spec 2026-10-30, section 10): the repository's plugins/email/plugin.json is a
// manifest the server reads - strata-email with the mail server's settings as arguments, the password only as
// SMTP_PASSWORD, the events it is told of (all by default), and IONOS as a preset.
void the_shipped_email_plugin_is_one() {
  std::ifstream in(std::string{SECURE_FRONTEND_REPO_PLUGINS} + "/email/plugin.json", std::ios::binary);
  const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  const auto parsed = secure_frontend::parse_plugin_manifest(text, "email");
  const auto* manifest = std::get_if<secure_frontend::PluginManifest>(&parsed);
  expect(manifest != nullptr && manifest->run.has_value(), "read: " + why(parsed));
  if (manifest == nullptr || !manifest->run) return;
  const auto& run = *manifest->run;
  using T = secure_frontend::PluginTemplate;
  expect(manifest->kind == secure_frontend::PluginKind::notifier && manifest->publisher == "M-Strata" && manifest->license == "AGPL-3.0-only",
         "a first-party notifier plugin");
  expect(run.arguments == std::vector<T>{{.text = "--host"}, {.setting = "host"}, {.text = "--port"}, {.setting = "port"},
                                         {.text = "--security"}, {.setting = "security"}, {.text = "--username"}, {.setting = "username"},
                                         {.text = "--from"}, {.setting = "from"}, {.text = "--to"}, {.setting = "to"}} &&
             run.environment == std::vector<std::pair<std::string, T>>{{"SMTP_PASSWORD", T{.setting = "password"}}} &&
             run.network == "the mail server {host}:{port}",
         "strata-email's arguments, the password only in its environment, its network");
  const auto setting = [&run](const std::string& name) -> const secure_frontend::PluginSetting* {
    for (const auto& s : run.settings) if (s.name == name) return &s;
    return nullptr;
  };
  const auto* events = setting("events");
  const auto* password = setting("password");
  expect(run.settings.size() == 8U && events != nullptr && events->choices.size() == 6U &&
             events->default_value == secure_frontend::PluginValue{std::vector<std::string>(events->choices)} && password != nullptr &&
             password->kind == secure_frontend::SettingKind::secret && password->required,
         "its settings: the mail server's, the events (all by default) and the password, a secret");
  expect(run.presets.size() == 1U && run.presets[0].label == "IONOS" &&
             run.presets[0].values.at("host") == secure_frontend::PluginValue{std::string{"smtp.ionos.com"}},
         "IONOS is a preset");
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

// An engine plugin's block, read: the family it adds, every part of it.
void an_engine_block_declares_its_family() {
  const auto parsed = secure_frontend::parse_plugin_manifest(engine_manifest(kAcmeFiles, kAcmeEngine), "acme3");
  const auto* manifest = std::get_if<secure_frontend::PluginManifest>(&parsed);
  expect(manifest != nullptr && manifest->engine.has_value(), "read: " + why(parsed));
  if (manifest == nullptr || !manifest->engine) return;
  const auto& e = *manifest->engine;
  using secure_frontend::PluginArgumentValue;
  expect(e.abi == 1 && e.dialect == secure_frontend::EngineProtocolFlavor::strata_0143 && e.chat == secure_frontend::NativeChatProfile::flint &&
             e.tokenizer_source == secure_frontend::TokenizerSource::gguf && e.tokenizer_mode == secure_frontend::TokenizerMode::flint_bpe,
         "its dialect, renderer and tokenizer");
  expect(e.gguf_architectures == std::vector<std::string>{"acme3", "acme3moe"} && e.context_key == "acme3.context_length" &&
             e.mtp_key == "acme3.nextn_predict_layers",
         "the models it cards");
  expect(e.arguments.size() == 8U && e.arguments[0].flag == "--max-context" && e.arguments[0].value == PluginArgumentValue::integer &&
             e.arguments[0].minimum == 4096 && e.arguments[0].maximum == 262144 && e.arguments[1].value == PluginArgumentValue::gibibytes &&
             e.arguments[3].value == PluginArgumentValue::fraction && e.arguments[4].value == PluginArgumentValue::on_off &&
             e.arguments[5].value == PluginArgumentValue::none && e.arguments[6].value == PluginArgumentValue::choice &&
             e.arguments[6].choices == std::vector<std::string>{"f16", "q8"} && e.arguments[7].value == PluginArgumentValue::gguf_file,
         "its flags' allowlist");
  using K = secure_frontend::ModelOptionKind;
  expect(e.options.size() == 6U, "its options");
  if (e.options.size() == 6U) {
    expect(e.options[0].name == "max_context" && e.options[0].kind == K::integer && e.options[0].label == "Context length" &&
               e.options[0].help == "Tokens the model reads." && e.options[0].flag == "--max-context" && e.options[0].minimum == 4096 &&
               e.options[0].maximum == 262144 && e.options[0].values == std::vector<std::int64_t>{8192, 32768, 65536, 131072} &&
               e.options[0].default_value == std::optional<std::string>{"65536"},
           "an integer option with its values and default");
    expect(e.options[1].kind == K::integer_or_auto && e.options[1].default_value == std::optional<std::string>{"auto"}, "auto");
    expect(e.options[2].kind == K::decimal && e.options[2].minimum_decimal == 0.0 && e.options[2].maximum_decimal == 1.0 &&
               !e.options[2].default_value,
           "a decimal");
    expect(e.options[3].kind == K::boolean && e.options[3].default_value == std::optional<std::string>{"true"}, "a switch");
    expect(e.options[4].kind == K::choice && e.options[4].choices == std::vector<std::string>{"f16", "q8"} &&
               e.options[4].default_value == std::optional<std::string>{"q8"},
           "a choice");
    expect(e.options[5].kind == K::devices && e.options[5].flag.empty() && e.options[5].maximum == 8, "the GPUs (no flag: --gpu is the server's)");
  }
  expect(e.sampling.temperature == 1.0 && e.sampling.top_p == 0.95 && e.sampling.top_k == std::optional<std::int64_t>{64} &&
             e.sampling.min_p == std::optional<double>{0.05},
         "its recommended sampling");
  expect(e.calibrates_before_first_start && e.keep_margin == 0.03 && e.calibration.size() == 2U && e.calibration[0].flag == "--cpu-threads" &&
             e.calibration[0].values == std::vector<std::string>{"{cores}", "{cores*2/3}", "{cores/2}"} &&
             e.calibration[1].values == std::vector<std::string>{"0", "0.25", "0.5", "0.75"},
         "its calibration");
  expect(e.monitor_experts_vram == "vram_slots" && e.monitor_experts_ram == "pinned_gib", "the Monitor's keys");
  expect(!e.vision && e.web_search && e.mtp == secure_frontend::DraftLayerKind::file, "what cards may offer");
  // the least it may declare: no options, the default sampling, no sweep, no Monitor keys
  const auto least = secure_frontend::parse_plugin_manifest(engine_manifest(kAcmeFiles), "acme3");
  const auto* minimal = std::get_if<secure_frontend::PluginManifest>(&least);
  expect(minimal != nullptr && minimal->engine && minimal->engine->options.empty() && minimal->engine->sampling.temperature == 1.0 &&
             minimal->engine->sampling.top_p == 0.95 && !minimal->engine->sampling.top_k && minimal->engine->calibration.empty() &&
             !minimal->engine->calibrates_before_first_start && minimal->engine->monitor_experts_vram.empty() &&
             minimal->engine->mtp == secure_frontend::DraftLayerKind::none,
         "the least an engine declares: " + why(least));
}

// Every part of the block checked, and a wrong one refused by name: nothing outside the closed lists, no flag the
// server composes, no option on a flag it did not declare or past that flag's bounds, the library among its files.
void a_wrong_engine_block_is_refused_by_name() {
  const auto refuse = [](const std::string& from, const std::string& to, const std::string& reason) {
    std::string engine = kAcmeEngine;
    const auto at = engine.find(from);
    expect(at != std::string::npos, "the test's own text is there: " + from);
    if (at != std::string::npos) engine.replace(at, from.size(), to);
    const auto parsed = secure_frontend::parse_plugin_manifest(engine_manifest(kAcmeFiles, engine), "acme3");
    expect(why(parsed).find(reason) != std::string::npos, from + " -> " + to + " refused for " + reason + ": " + why(parsed));
  };
  refuse(R"("abi": 1)", R"("abi": 2)", "abi");
  refuse(R"("dialect": "strata_0143")", R"("dialect": "maya_118")", "dialect");
  refuse(R"("chat": "flint")", R"("chat": "jinja")", "chat");
  refuse(R"("mode": "flint_bpe")", R"("mode": "wordpiece")", "tokenizer");
  refuse(R"("source": "gguf")", R"("source": "web")", "tokenizer");
  refuse(R"(["acme3", "acme3moe"])", "[]", "gguf_architectures");
  refuse(R"(["acme3", "acme3moe"])", R"(["Acme3"])", "gguf_architectures");
  refuse(R"("context_key": "acme3.context_length", )", "", "context_key");
  refuse(R"("flag": "--mtp")", R"("flag": "--native")", "--native");
  refuse(R"("flag": "--mtp")", R"("flag": "--gpu")", "--gpu");
  refuse(R"("flag": "--mtp")", R"("flag": "mtp")", "flag");
  refuse(R"("flag": "--mtp")", R"("flag": "--kv")", "twice");
  refuse(R"("value": "gguf_file")", R"("value": "path")", "value");
  refuse(R"("value": "integer", "min": 1, "max": 256})", R"("value": "integer"})", "min");
  refuse(R"("value": "fraction"})", R"("value": "fraction", "choices": ["a"]})", "choices");
  refuse(R"("flag": "--mtp", "value": "gguf_file"})", R"("flag": "--mtp", "value": "gguf_file", "extra": 1})", "extra");
  refuse(R"("flag": "--pcie-frac", "min")", R"("flag": "--nope", "min")", "--nope");
  refuse(R"("kind": "decimal", "label": "PCIe share")", R"("kind": "integer", "label": "PCIe share")", "pcie_frac");
  refuse(R"("min": 4096, "max": 262144, "values")", R"("min": 1, "max": 262144, "values")", "max_context");
  refuse("[8192, 32768, 65536, 131072]", "[32768, 8192]", "values");
  refuse(R"("default": 65536)", R"("default": 3)", "default");
  refuse(R"("choices": ["f16", "q8"], "default")", R"("choices": ["f16", "q4"], "default")", "kv");
  refuse(R"("name": "cpu_lane")", R"("name": "kv")", "twice");
  refuse(R"("label": "GPUs", "max": 8)", R"("label": "GPUs", "flag": "--kv", "max": 8)", "gpus");
  refuse(R"("temperature": 1.0)", R"("temperature": 3)", "temperature");
  refuse(R"("flag": "--cpu-threads", "values")", R"("flag": "--kv", "values")", "calibration");
  refuse(R"("{cores/2}")", R"("{gpus}")", "calibration");
  refuse(R"("{cores/2}")", R"("9999")", "calibration");
  refuse(R"("keep_margin": 0.03)", R"("keep_margin": 0.5)", "keep_margin");
  refuse(R"("experts_ram": "pinned_gib")", R"("experts_ram": "gpu_temp")", "monitor");
  refuse(R"("mtp": "file")", R"("mtp": "maybe")", "mtp");
  refuse(R"("abi": 1, )", R"("abi": 1, "extra": 1, )", "extra");
  const auto other_files = secure_frontend::parse_plugin_manifest(
      engine_manifest(R"({"windows": ["other.dll"], "linux": ["other.so"]})", kAcmeEngine), "acme3");
  expect(why(other_files).find("multiengine_acme3") != std::string::npos, "its library among its files: " + why(other_files));
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"the_shipped_debugging_plugin_is_one", the_shipped_debugging_plugin_is_one},
      {"the_shipped_search_plugin_is_one", the_shipped_search_plugin_is_one},
      {"the_shipped_email_plugin_is_one", the_shipped_email_plugin_is_one},
      {"a_diagnostics_manifest_is_read", a_diagnostics_manifest_is_read},
      {"a_code_plugin_names_this_platforms_files", a_code_plugin_names_this_platforms_files},
      {"a_wrong_manifest_is_refused_by_name", a_wrong_manifest_is_refused_by_name},
      {"an_engine_block_declares_its_family", an_engine_block_declares_its_family},
      {"a_wrong_engine_block_is_refused_by_name", a_wrong_engine_block_is_refused_by_name},
      {"a_tool_manifest_says_how_its_program_runs", a_tool_manifest_says_how_its_program_runs},
      {"a_wrong_run_block_is_refused_by_name", a_wrong_run_block_is_refused_by_name},
      {"values_are_checked_against_the_settings", values_are_checked_against_the_settings},
  }, failures);
}
