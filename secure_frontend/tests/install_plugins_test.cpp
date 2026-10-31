// An install's plugins as its server runs them (the owner, 2026-10-30, decisions 6 and 7; plugin spec section 10): each
// tool plugin that is on, its program in its folder, its arguments and environment filled in from its settings (a
// secret read from the secret folder, only ever into the environment), over the closed environment the server gives it;
// one whose settings do not fit its manifest is left out, and why is said.
#include "secure_frontend/install_plugins.hpp"
#include "secure_frontend/plugin_secrets.hpp"
#include "secure_frontend/sha256.hpp"
#include "support/test_support.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

std::string hex_sha256(const std::string& bytes) {
  secure_frontend::Sha256 hash;
  hash.update(bytes);
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  for (const auto byte : hash.finish()) {
    out += kDigits[byte >> 4U];
    out += kDigits[byte & 0x0FU];
  }
  return out;
}

#if defined(_WIN32)
const std::string kProgram = "lookup.exe";
#else
const std::string kProgram = "lookup";
#endif

const std::string kLookup = R"({"schema": "m-strata.plugin/1", "id": "lookup", "kind": "tool", "name": "Lookup", "version": "1.0.0",
  "publisher": "Example Labs", "license": "MIT", "about": "",
  "files": {"windows": ["lookup.exe"], "linux": ["lookup"]},
  "tool": {"program": {"windows": "lookup.exe", "linux": "lookup"},
    "arguments": ["--url", "{url}", "--results", "{results}", "--fixed"],
    "environment": {"LOOKUP_TOKEN": "{token}", "LOOKUP_MODE": "plain", "LOOKUP_LANGS": "{langs}"},
    "network": "the service at {url}", "note": "Use lookup.", "call_timeout_s": 45,
    "settings": [
      {"name": "url", "kind": "url", "label": "Address", "help": "", "required": true},
      {"name": "results", "kind": "integer", "label": "Results", "help": "", "default": 5, "min": 1, "max": 10},
      {"name": "langs", "kind": "text_list", "label": "Languages", "help": "", "max_items": 3},
      {"name": "token", "kind": "secret", "label": "Token", "help": "", "required": true}]}})";

// An install: its engine folder (make's record of the plugin, on), its plugins folder, a secret folder and a state folder.
struct Install final {
  std::filesystem::path root = secure_frontend_test::make_scratch_directory();
  std::filesystem::path engine = root / "engine";
  std::filesystem::path secret = root / "secret";
  std::filesystem::path state = root / "state";
  Install() {
    for (const auto& folder : {engine, root / "plugins", root / "plugins" / "lookup", secret, state}) secure_frontend_test::make_private_directory(folder);
    secure_frontend_test::write_file(root / "plugins" / "lookup" / "plugin.json", kLookup);
    secure_frontend_test::write_file(root / "plugins" / "lookup" / kProgram, "program", 0700);
    secure_frontend_test::write_file(engine / "build.json",
                                     R"({"schema_version": 1, "built": "x", "commit": "c", "ggml": "g", "cuda_arch": "a", "engines": [],)"
                                     R"( "plugins": [{"id": "lookup", "manifest": ")" + hex_sha256(kLookup) + R"(", "files": {")" + kProgram +
                                         R"(": ")" + hex_sha256("program") + R"("}, "on": true}]})");
  }
  [[nodiscard]] std::vector<secure_frontend::PluginProgram> programs(const std::map<std::string, secure_frontend::ConfigDraftValue>& values,
                                                                     std::vector<std::pair<std::string, std::string>>* left_out) const {
    secure_frontend::PluginSettingsConfig config;
    if (!values.empty()) config.values["lookup"] = values;
    return secure_frontend::plugin_programs(secure_frontend::install_plugins(engine, state), secure_frontend::PluginKind::tool, config, secret,
                                            [](const std::string_view id) {
                                              return std::map<std::string, std::string, std::less<>>{{"BASE", "base-of-" + std::string{id}}};
                                            },
                                            left_out);
  }
};

void a_tool_runs_with_its_settings_filled_in() {
  Install install;
  expect(!secure_frontend::store_plugin_secret(install.secret, "lookup", "token", "s3cret").has_value(), "the token is stored");
  std::vector<std::pair<std::string, std::string>> left_out;
  const auto programs = install.programs({{"url", std::string{"https://lookup.example.org"}}, {"langs", std::vector<std::string>{"en", "de"}}},
                                         &left_out);
  expect(programs.size() == 1U && left_out.empty(), "the tool plugin runs");
  if (programs.size() != 1U) return;
  const auto& program = programs[0];
  expect(program.id == "lookup" && program.program == install.root / "plugins" / "lookup" / kProgram, "its program, in its folder");
  expect(program.arguments == std::vector<std::string>{"--url", "https://lookup.example.org", "--results", "5", "--fixed"},
         "its arguments: each setting's value, or its default");
  expect(program.environment.at("BASE") == "base-of-lookup" && program.environment.at("LOOKUP_TOKEN") == "s3cret" &&
             program.environment.at("LOOKUP_MODE") == "plain" && program.environment.at("LOOKUP_LANGS") == "en,de",
         "its environment: the closed one, its fixed values, the secret read from the secret folder, a list joined");
  expect(program.run.note == "Use lookup." && program.run.call_timeout == std::chrono::seconds(45), "its note and its call's limit");
  // an optional setting with no value: its variable is not set at all
  const auto without = install.programs({{"url", std::string{"https://lookup.example.org"}}}, &left_out);
  expect(without.size() == 1U && !without[0].environment.contains("LOOKUP_LANGS"), "an unset optional variable is left out");
}

void a_tool_whose_settings_do_not_fit_is_left_out() {
  Install install;
  std::vector<std::pair<std::string, std::string>> left_out;
  expect(install.programs({{"url", std::string{"https://lookup.example.org"}}}, &left_out).empty() && left_out.size() == 1U &&
             left_out[0].first == "lookup" && left_out[0].second.find("token") != std::string::npos,
         "a required secret not set: left out, saying which");
  expect(!secure_frontend::store_plugin_secret(install.secret, "lookup", "token", "s3cret").has_value(), "the token is stored");
  left_out.clear();
  expect(install.programs({{"url", std::string{"ftp://nope"}}}, &left_out).empty() && left_out.size() == 1U &&
             left_out[0].second.find("url") != std::string::npos,
         "a value its manifest refuses: left out, saying which");
  left_out.clear();
  expect(install.programs({{"url", std::string{"https://x"}}, {"results", std::int64_t{99}}}, &left_out).empty() && !left_out.empty() &&
             left_out[0].second.find("results") != std::string::npos,
         "a number out of its range: left out");
  // a plugin turned off is not a program at all (and not left out: it is off)
  secure_frontend::PluginChoices off;
  off.enabled["lookup"] = false;
  expect(secure_frontend::write_plugin_choices(install.state, off), "the choice is written");
  left_out.clear();
  expect(install.programs({{"url", std::string{"https://x"}}}, &left_out).empty() && left_out.empty(), "off: neither run nor left out");
  expect(secure_frontend::read_plugin_choices(install.state)->enabled.at("lookup") == false, "the choices file reads back");
}

#if defined(_WIN32)
const std::string kLibrary = "multiengine_acme3.dll";
#else
const std::string kLibrary = "libmultiengine_acme3.so";
#endif

const std::string kAcme3 = R"({"schema": "m-strata.plugin/1", "id": "acme3", "kind": "engine", "name": "Acme-3", "version": "0.1.0",
  "publisher": "Example Labs", "license": "Apache-2.0", "about": "",
  "files": {"windows": ["multiengine_acme3.dll"], "linux": ["libmultiengine_acme3.so"]},
  "engine": {"abi": 1, "dialect": "strata_0143", "chat": "qwen", "tokenizer": {"source": "pack", "mode": "byte_bpe"},
    "models": {"gguf_architectures": ["acme3"], "context_key": "acme3.context_length"},
    "arguments": [{"flag": "--max-context", "value": "integer", "min": 4096, "max": 262144}],
    "capabilities": {"vision": false, "web_search": false, "mtp": "none"}}})";

// The engine plugins an install runs are its families (plugin spec section 5): each one that is on, with its manifest and
// its library's SHA-256 (what a start hands the launcher). One waiting for approval, turned off, or changed since it was
// approved makes none; nor does a plugin of another kind.
void an_installs_engine_plugins_that_are_on_are_its_families() {
  Install install;
  secure_frontend_test::make_private_directory(install.root / "plugins" / "acme3");
  secure_frontend_test::write_file(install.root / "plugins" / "acme3" / "plugin.json", kAcme3);
  secure_frontend_test::write_file(install.root / "plugins" / "acme3" / kLibrary, "library", 0700);
  const auto families = [&install] { return secure_frontend::engine_families(secure_frontend::install_plugins(install.engine, install.state)); };
  expect(families().empty(), "an engine plugin never approved makes no family (and the tool plugin none ever)");
  secure_frontend::PluginChoices choices;
  choices.approved["acme3"] = {.manifest = hex_sha256(kAcme3), .files = {{kLibrary, hex_sha256("library")}}};
  expect(secure_frontend::write_plugin_choices(install.state, choices), "the approval is written");
  const auto approved = families();
  expect(approved.size() == 1U && approved[0].manifest.id == "acme3" && approved[0].manifest.engine.has_value() &&
             approved[0].library_sha256 == hex_sha256("library"),
         "approved, it is a family: its manifest and its library's SHA-256 (" + std::to_string(approved.size()) + ")");
  choices.enabled["acme3"] = false;
  expect(secure_frontend::write_plugin_choices(install.state, choices) && families().empty(), "turned off: none");
  choices.enabled["acme3"] = true;
  expect(secure_frontend::write_plugin_choices(install.state, choices) && families().size() == 1U, "on again: it is one again");
  secure_frontend_test::write_file(install.root / "plugins" / "acme3" / kLibrary, "another library", 0700);
  expect(families().empty(), "its library changed since it was approved: none until it is approved again");
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"a_tool_runs_with_its_settings_filled_in", a_tool_runs_with_its_settings_filled_in},
      {"a_tool_whose_settings_do_not_fit_is_left_out", a_tool_whose_settings_do_not_fit_is_left_out},
      {"an_installs_engine_plugins_that_are_on_are_its_families", an_installs_engine_plugins_that_are_on_are_its_families},
  }, failures);
}
