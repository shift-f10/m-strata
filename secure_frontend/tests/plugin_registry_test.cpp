// Which plugins may run (the owner, 2026-10-30, decisions 2 and 4: approve each plugin with its SHA-256 pinned; plugins
// `make` built are trusted through the build record): a new plugin waits, an approved one runs until it changes, a
// first-party one runs from its build record, and every refusal says why.
#include "secure_frontend/plugin_registry.hpp"
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

const std::string kDebugging = R"({"schema": "m-strata.plugin/1", "id": "debugging", "kind": "diagnostics",
  "name": "Debugging", "version": "1.0.0", "publisher": "M-Strata", "license": "AGPL-3.0-only", "about": "Verbose logs.",
  "diagnostics": {"switches": ["logging.requests", "logging.engine_output", "troubleshooting"]}})";

std::string hex_sha256(const std::string& bytes) {
  secure_frontend::Sha256 hash;
  hash.update(bytes);
  const auto digest = hash.finish();
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  for (const auto byte : digest) {
    out += kDigits[byte >> 4U];
    out += kDigits[byte & 0x0FU];
  }
  return out;
}

// An install's plugins folder: owner-only, as make leaves it.
struct Plugins final {
  std::filesystem::path root = secure_frontend_test::make_scratch_directory();
  std::filesystem::path dir = root / "plugins";
  Plugins() { secure_frontend_test::make_private_directory(dir); }
  void add(const std::string& id, const std::string& manifest) const {
    secure_frontend_test::make_private_directory(dir / id);
    secure_frontend_test::write_file(dir / id / "plugin.json", manifest);
  }
  [[nodiscard]] std::vector<secure_frontend::PluginEntry> find(const std::vector<secure_frontend::FirstPartyPlugin>& first_party,
                                                               const secure_frontend::PluginChoices& choices) const {
    return secure_frontend::discover_plugins(dir, first_party, choices);
  }
};

const secure_frontend::PluginEntry* entry_of(const std::vector<secure_frontend::PluginEntry>& entries, const std::string& id) {
  for (const auto& entry : entries) {
    if (entry.id == id) return &entry;
  }
  return nullptr;
}

std::string name_of(const std::vector<secure_frontend::PluginEntry>& entries, const std::string& id) {
  const auto* entry = entry_of(entries, id);
  return entry == nullptr ? std::string{"(missing)"} : std::string{secure_frontend::plugin_state_name(entry->state)};
}

std::string state_of(const secure_frontend::PluginEntry* entry) {
  return entry == nullptr ? std::string{"(missing)"}
                          : std::string{secure_frontend::plugin_state_name(entry->state)} + " (" + entry->reason + ")";
}

// A new plugin waits; approved (its SHA-256s pinned) it is on, and turned off it is off.
void a_new_plugin_waits_then_runs_once_approved() {
  Plugins plugins;
  plugins.add("debugging", kDebugging);
  const auto first = plugins.find({}, {});
  const auto* waiting = entry_of(first, "debugging");
  expect(waiting != nullptr && waiting->state == secure_frontend::PluginState::waiting && waiting->manifest.has_value() &&
             waiting->pin.manifest == hex_sha256(kDebugging) && waiting->pin.files.empty() && !waiting->first_party,
         "a new plugin waits, its manifest hashed: " + state_of(waiting));
  if (waiting == nullptr) return;
  secure_frontend::PluginChoices choices;
  choices.approved["debugging"] = waiting->pin;
  expect(name_of(plugins.find({}, choices), "debugging") == "on", "approved: on");
  choices.enabled["debugging"] = false;
  expect(name_of(plugins.find({}, choices), "debugging") == "off", "turned off: off");
  expect(std::string{secure_frontend::plugin_state_name(secure_frontend::PluginState::waiting)} == "waiting", "the state's name");
}

// A plugin approved once whose manifest changed since waits for approval again, saying so.
void a_changed_plugin_waits_again() {
  Plugins plugins;
  plugins.add("debugging", kDebugging);
  secure_frontend::PluginChoices choices;
  const auto found = plugins.find({}, {});
  const auto* first = entry_of(found, "debugging");
  expect(first != nullptr, "found");
  if (first == nullptr) return;
  choices.approved["debugging"] = first->pin;
  std::string changed = kDebugging;
  changed.replace(changed.find("Verbose logs."), 13, "Other words.");
  plugins.add("debugging", changed);
  const auto after = plugins.find({}, choices);
  const auto* entry = entry_of(after, "debugging");
  expect(entry != nullptr && entry->state == secure_frontend::PluginState::changed && entry->reason.find("changed") != std::string::npos,
         "changed: " + state_of(entry));
}

// A first-party plugin (make's build record) needs no approval: it starts as its record says, the owner's on/off wins;
// with other SHA-256s than its record's it is not first-party (it waits).
void a_first_party_plugin_is_trusted_by_its_build_record() {
  Plugins plugins;
  plugins.add("debugging", kDebugging);
  const secure_frontend::PluginPin pin{.manifest = hex_sha256(kDebugging), .files = {}};
  const auto built_off = plugins.find({{.id = "debugging", .pin = pin, .on = false}}, {});
  const auto* off = entry_of(built_off, "debugging");
  expect(off != nullptr && off->first_party && off->state == secure_frontend::PluginState::off, "built off: " + state_of(off));
  expect(name_of(plugins.find({{.id = "debugging", .pin = pin, .on = true}}, {}), "debugging") == "on", "built on: on");
  secure_frontend::PluginChoices choices;
  choices.enabled["debugging"] = false;
  expect(name_of(plugins.find({{.id = "debugging", .pin = pin, .on = true}}, choices), "debugging") == "off", "the owner's off wins");
  const secure_frontend::PluginPin other{.manifest = std::string(64U, 'a'), .files = {}};
  const auto other_bytes = plugins.find({{.id = "debugging", .pin = other, .on = true}}, {});
  const auto* foreign = entry_of(other_bytes, "debugging");
  expect(foreign != nullptr && !foreign->first_party && foreign->state == secure_frontend::PluginState::waiting,
         "not the bytes make built: it waits: " + state_of(foreign));
}

// Refusals say why: a manifest that is not one, a file it names missing, a folder others can change, a kind this server
// does not run yet. A folder without plugin.json, and a file, are not plugins at all.
void refusals_say_why() {
  Plugins plugins;
  plugins.add("broken", "{\"schema\": \"m-strata.plugin/1\"}");
  plugins.add("acme3", R"({"schema": "m-strata.plugin/1", "id": "acme3", "kind": "engine", "name": "Acme-3", "version": "0.1.0",
    "publisher": "Example Labs", "license": "Apache-2.0", "about": "", "engine": {},
    "files": {"windows": ["multiengine_acme3.dll"], "linux": ["libmultiengine_acme3.so"]}})");
  plugins.add("search", R"({"schema": "m-strata.plugin/1", "id": "search", "kind": "tool", "name": "Web search", "version": "1.0.0",
    "publisher": "M-Strata", "license": "AGPL-3.0-only", "about": "", "tool": {},
    "files": {"windows": ["strata-search.exe"], "linux": ["strata-search"]}})");
#if defined(_WIN32)
  secure_frontend_test::write_file(plugins.dir / "search" / "strata-search.exe", "tool");
#else
  secure_frontend_test::write_file(plugins.dir / "search" / "strata-search", "tool", 0700);
#endif
  plugins.add("shared", R"({"schema": "m-strata.plugin/1", "id": "shared", "kind": "diagnostics", "name": "Shared", "version": "1",
    "publisher": "x", "license": "x", "about": "", "diagnostics": {"switches": ["troubleshooting"]}})");
#if defined(_WIN32)
  expect(secure_frontend_test::grant_everyone_write(plugins.dir / "shared"), "the test widens a folder");
#else
  std::filesystem::permissions(plugins.dir / "shared", std::filesystem::perms::others_write, std::filesystem::perm_options::add);
#endif
  secure_frontend_test::make_private_directory(plugins.dir / "empty");
  secure_frontend_test::write_file(plugins.dir / "stray.txt", "not a plugin");
  const auto entries = plugins.find({}, {});
  const auto refused = [&entries](const std::string& id, const std::string& why) {
    const auto* entry = entry_of(entries, id);
    expect(entry != nullptr && entry->state == secure_frontend::PluginState::refused && entry->reason.find(why) != std::string::npos,
           id + " refused for " + why + ": " + state_of(entry));
  };
  refused("broken", "id");
  refused("acme3", "missing");
  refused("shared", "other users");
  expect(entries.size() == 4U && entry_of(entries, "empty") == nullptr && entry_of(entries, "stray.txt") == nullptr,
         "only folders with a plugin.json: " + std::to_string(entries.size()));
  // a kind this server does not run yet: refused even when trusted, its manifest read and its file hashed
  secure_frontend::PluginChoices choices;
  const auto* search = entry_of(entries, "search");
  if (search == nullptr) return;
  choices.approved["search"] = search->pin;
  const auto approved = plugins.find({}, choices);
  const auto* tool = entry_of(approved, "search");
  expect(tool != nullptr && tool->state == secure_frontend::PluginState::refused && tool->reason.find("tool") != std::string::npos &&
             tool->pin.files.size() == 1U && tool->pin.files.begin()->second == hex_sha256("tool"),
         "a tool plugin does not run yet: " + state_of(tool));
  expect(secure_frontend::plugin_kind_runs(secure_frontend::PluginKind::diagnostics) &&
             !secure_frontend::plugin_kind_runs(secure_frontend::PluginKind::engine),
         "the kinds that run");
}

// The owner's choices file round-trips, and anything else is refused.
void the_choices_round_trip() {
  secure_frontend::PluginChoices choices;
  choices.approved["debugging"] = {.manifest = std::string(64U, 'a'), .files = {{"bin/x.dll", std::string(64U, 'b')}}};
  choices.enabled["debugging"] = false;
  choices.enabled["search"] = true;
  const auto text = secure_frontend::render_plugin_choices(choices);
  const auto back = secure_frontend::parse_plugin_choices(text);
  expect(back.has_value() && back->approved == choices.approved && back->enabled == choices.enabled, "round trip: " + text);
  for (const char* bad : {"", "{}", "[]", R"({"schema":"m-strata.plugin-choices/1","approved":{"Bad":{}},"enabled":{}})",
                          R"({"schema":"m-strata.plugin-choices/1","approved":{},"enabled":{"debugging":"yes"}})",
                          R"({"schema":"m-strata.plugin-choices/1","approved":{"x":{"manifest":"nothex","files":{}}},"enabled":{}})"}) {
    expect(!secure_frontend::parse_plugin_choices(bad).has_value(), std::string{"refused: "} + bad);
  }
}

// The owner (2026-10-30): "we should make the email, websearch, debugging all plugins as well". What the diagnostics
// plugins that are on switch on, together; one that waits or is turned off switches nothing on.
void the_diagnostics_that_are_on_are_combined() {
  Plugins plugins;
  expect(!secure_frontend::diagnostics_on(plugins.find({}, {})).any(), "no plugin: nothing is on");
  plugins.add("debugging", kDebugging);
  expect(!secure_frontend::diagnostics_on(plugins.find({}, {})).any(), "a waiting plugin switches nothing on");
  const secure_frontend::FirstPartyPlugin built{.id = "debugging", .pin = {.manifest = hex_sha256(kDebugging), .files = {}}, .on = true};
  const auto on = secure_frontend::diagnostics_on(plugins.find({built}, {}));
  expect(on.log_requests && on.log_engine_output && on.troubleshooting && on.any(), "on: each of its switches");
  secure_frontend::PluginChoices choices;
  choices.enabled["debugging"] = false;
  expect(!secure_frontend::diagnostics_on(plugins.find({built}, choices)).any(), "turned off: nothing");
  const std::string quiet = R"({"schema": "m-strata.plugin/1", "id": "quiet", "kind": "diagnostics", "name": "Quiet",
    "version": "1", "publisher": "x", "license": "x", "about": "", "diagnostics": {"switches": ["logging.requests"]}})";
  plugins.add("quiet", quiet);
  choices.approved["quiet"] = {.manifest = hex_sha256(quiet), .files = {}};
  const auto one = secure_frontend::diagnostics_on(plugins.find({built}, choices));
  expect(one.log_requests && !one.log_engine_output && !one.troubleshooting, "another one on: only its own switch");
  choices.enabled.erase("debugging");
  const auto both = secure_frontend::diagnostics_on(plugins.find({built}, choices));
  expect(both.log_requests && both.log_engine_output && both.troubleshooting, "both on: what either switches on");
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"the_diagnostics_that_are_on_are_combined", the_diagnostics_that_are_on_are_combined},
      {"a_new_plugin_waits_then_runs_once_approved", a_new_plugin_waits_then_runs_once_approved},
      {"a_changed_plugin_waits_again", a_changed_plugin_waits_again},
      {"a_first_party_plugin_is_trusted_by_its_build_record", a_first_party_plugin_is_trusted_by_its_build_record},
      {"refusals_say_why", refusals_say_why},
      {"the_choices_round_trip", the_choices_round_trip},
  }, failures);
}
