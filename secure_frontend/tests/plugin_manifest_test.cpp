// Plugins (the owner, 2026-10-30; docs/superpowers/specs/2026-10-30-engine-family-plugins-design.md, section 8): a
// plugin's plugin.json read and checked - its kind, its texts, this platform's files, a diagnostics plugin's switches -
// and every other key, a wrong id and a path out of its folder refused by name.
#include "secure_frontend/plugin_manifest.hpp"
#include "support/test_support.hpp"

#include <fstream>
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
      {"a_diagnostics_manifest_is_read", a_diagnostics_manifest_is_read},
      {"a_code_plugin_names_this_platforms_files", a_code_plugin_names_this_platforms_files},
      {"a_wrong_manifest_is_refused_by_name", a_wrong_manifest_is_refused_by_name},
  }, failures);
}
