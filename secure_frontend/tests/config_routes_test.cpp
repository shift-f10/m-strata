// The Settings tab's server half (spec docs/superpowers/specs/2026-10-28-server-window-design.md, Part 1; the owner:
// "a configuration tab that we can configure and manage it's settings like api key, etc"): the config file's listener,
// limits and folder rows, read and written by the server's own start-up rules, and the API key rotated in its store.
#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/config_file.hpp"
#include "secure_frontend/config_routes.hpp"
#include "secure_frontend/first_start.hpp"
#include "secure_frontend/frontend_config.hpp"
#include "secure_frontend/json.hpp"
#include "secure_frontend/secret_source.hpp"
#include "secure_frontend/token_reader.hpp"
#include "secure_frontend/token_verifier.hpp"
#include "support/test_support.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

bool has(const secure_frontend::HttpResponse& response, const std::string_view text) {
  return response.body.find(text) != std::string::npos;
}
std::string shown(const secure_frontend::HttpResponse& response) { return std::to_string(response.status) + " " + response.body; }

// A first start's data folder (as the server makes it), the routes over its config file and key store.
struct Fixture final {
  std::filesystem::path root = secure_frontend_test::make_scratch_directory();
  secure_frontend::DataFolder data = secure_frontend::data_folder_layout(root / "data");
  std::unique_ptr<secure_frontend::ConfigRoutes> routes;
  std::vector<std::string> logged;
  explicit Fixture(const bool key_in_store = true, const bool troubleshooting = false) {
    const auto bin = root / "bin";
    secure_frontend_test::make_private_directory(bin);
    const auto engine = bin / "multiengine.exe";
    secure_frontend_test::write_file(engine, "engine", 0700);
    (void)secure_frontend::ensure_data_folder(data, engine);
    secure_frontend::ConfigRouteSettings settings{.config_file = data.config_file, .troubleshooting = troubleshooting};
    if (key_in_store) {
      settings.secret_dir = data.secret;
      settings.token_file = "token";
    }
    settings.log = [this](const std::string_view line) { logged.emplace_back(line); };
    routes = std::make_unique<secure_frontend::ConfigRoutes>(std::move(settings));
  }
  secure_frontend::HttpResponse post(const secure_frontend::RouteId route, const std::string& body, const bool loopback = true) {
    return routes->answer(route, body, secure_frontend::RequestContext{.client_gone = {}, .loopback_peer = loopback});
  }
  [[nodiscard]] std::string file() const { return secure_frontend_test::read_file(data.config_file); }
};

// The settings the tab manages: each with its type, the file's value (null when the file leaves it to the default), the
// default, the bounds and the schema's own description - read from the config file.
void the_settings_are_listed_with_their_rules() {
  Fixture f;
  const auto listed = f.post(secure_frontend::RouteId::strata_config, "{}");
  expect(listed.status == 200 && has(listed, "\"editable\":true"), "the settings are read: " + shown(listed));
  for (const char* key : {"listener.host", "listener.port", "listener.allow_remote", "listener.allow_wildcard_bind",
                          "listener.restrict_tool_calls", "listener.tool_call_hosts",
                          "listener.host_aliases", "models.control_remote", "models.control_remote_networks",
                          "limits.engine.generation_timeout_s", "limits.engine.ready_timeout_s", "limits.chat.max_prompt_tokens",
                          "limits.chat.max_completion_tokens", "tools.max_rounds", "pull.connections", "models.downloads_root",
                          "models.default_root"}) {
    expect(has(listed, std::string{"{\"key\":\""} + key + "\""), std::string{"listed: "} + key);
  }
  expect(has(listed, R"({"key":"listener.port","type":"integer","value":8080,"default":)") &&
             has(listed, R"({"key":"limits.engine.generation_timeout_s","type":"integer","value":null,"default":1800,"min":1,"max":86400,)"),
         "a value the file holds, and one it leaves to the default: " + listed.body);
  // the rounds of tool calls one answer may make (plugin spec section 10; it was web search's own search.max_rounds)
  expect(has(listed, R"({"key":"tools.max_rounds","type":"integer","value":null,"default":8,"min":1,"max":8,)"),
         "tools.max_rounds, with the limits: " + listed.body);
  expect(has(listed, R"("key":{"rotatable":true,"inference":false})"),
         "the key lives in the data folder's store: it can be rotated; no inference-only key is held (gap L4)");
  expect(listed.header_value("Cache-Control") == std::optional<std::string_view>{"no-store"}, "never cached");
  const auto remote = f.post(secure_frontend::RouteId::strata_config, "{}", false);
  expect(remote.status == 403 && has(remote, "\"code\":\"control_not_allowed\""), "another PC is refused: " + shown(remote));
}

// Start at boot (pinned and hot models, spec parts 5-6): the settings answer says what this PC starts the server with -
// the server's boot_state answer as it is, read only - and leaves it out when the server gives none.
void the_boot_start_is_said_with_the_settings() {
  Fixture f;
  expect(!has(f.post(secure_frontend::RouteId::strata_config, "{}"), "\"boot\""), "none given: no boot member");
  secure_frontend::ConfigRouteSettings settings{.config_file = f.data.config_file};
  settings.boot = [] { return std::string{R"({"system":"linux","starts":"none","says":"s","change":"c"})"}; };
  secure_frontend::ConfigRoutes routes(std::move(settings));
  const auto listed = routes.answer(secure_frontend::RouteId::strata_config, "{}",
                                    secure_frontend::RequestContext{.client_gone = {}, .loopback_peer = true});
  expect(listed.status == 200 && has(listed, R"(,"boot":{"system":"linux","starts":"none","says":"s","change":"c"}})"),
         "the boot member, last: " + shown(listed));
}

// The owner's external client (2026-10-31, gap O3: "A read endpoint would let the operator console display active
// defaults (thinking/temperature)"): the settings answer names the running model and the option values its start used,
// as the server says them, before "boot"; none given, no member.
void the_running_models_values_are_said_with_the_settings() {
  Fixture f;
  expect(!has(f.post(secure_frontend::RouteId::strata_config, ""), "\"running\""), "none given: no running member");
  secure_frontend::ConfigRouteSettings settings{.config_file = f.data.config_file};
  settings.running = [] { return std::string{R"({"name":"Alpha","options":{"thinking":"high","temperature":0.3}})"}; };
  settings.boot = [] { return std::string{R"({"system":"linux","starts":"none","says":"s","change":"c"})"}; };
  secure_frontend::ConfigRoutes routes(std::move(settings));
  const auto listed = routes.answer(secure_frontend::RouteId::strata_config, "",
                                    secure_frontend::RequestContext{.client_gone = {}, .loopback_peer = true});
  expect(listed.status == 200 &&
             has(listed, R"(,"running":{"name":"Alpha","options":{"thinking":"high","temperature":0.3}},"boot":{)"),
         "the running model's values, before boot: " + shown(listed));
}

// The owner's external client (2026-10-31, gap L4): the inference-only key is made here - a new key in the data folder's
// key store, shown once and accepted at once (the holder the router asks) - replaced by the next one made, and removed
// at once; from this PC only, and only where the server reads its key from the store; the settings say whether there is
// one.
void an_inference_key_is_made_and_removed_here() {
  Fixture f;
  auto held = std::make_shared<secure_frontend::InferenceKey>();
  secure_frontend::ConfigRouteSettings settings{.config_file = f.data.config_file};
  settings.secret_dir = f.data.secret;
  settings.token_file = "token";
  settings.inference = held;
  secure_frontend::ConfigRoutes routes(std::move(settings));
  const auto post = [&routes](const secure_frontend::RouteId route, const std::string& body, const bool loopback = true) {
    return routes.answer(route, body, secure_frontend::RequestContext{.client_gone = {}, .loopback_peer = loopback});
  };
  const auto key_in = [](const secure_frontend::HttpResponse& made) {
    constexpr std::string_view kField = "{\"key\":\"";
    if (!made.body.starts_with(kField)) return std::string{};
    return made.body.substr(kField.size(), made.body.find('"', kField.size()) - kField.size());
  };
  const auto file = f.data.secret / "inference-token";
  expect(has(post(secure_frontend::RouteId::strata_config, ""), R"("key":{"rotatable":true,"inference":false})"), "none yet");
  const auto made = post(secure_frontend::RouteId::strata_key_inference, R"({"action":"create"})");
  const std::string first = key_in(made);
  expect(made.status == 200 && first.size() >= 40U && has(made, R"(,"scope":"inference"})") && std::filesystem::exists(file),
         "a new key, shown once, kept in the store: " + std::to_string(made.status));
  expect(held->held() && held->matches(first), "accepted at once");
  expect(has(post(secure_frontend::RouteId::strata_config, ""), R"("key":{"rotatable":true,"inference":true})"), "the settings say so");
  const std::string second = key_in(post(secure_frontend::RouteId::strata_key_inference, R"({"action":"create"})"));
  expect(!second.empty() && second != first && held->matches(second) && !held->matches(first), "a new one replaces it at once");
  expect(post(secure_frontend::RouteId::strata_key_inference, R"({"action":"create"})", false).status == 403 && held->matches(second),
         "from another PC: refused");
  for (const char* body : {"{}", R"({"action":"rotate"})", R"({"action":"create","more":1})"}) {
    expect(post(secure_frontend::RouteId::strata_key_inference, body).status == 400, std::string{"not an action: "} + body);
  }
  const auto removed = post(secure_frontend::RouteId::strata_key_inference, R"({"action":"remove"})");
  expect(removed.status == 200 && has(removed, R"({"removed":true})") && !held->held() && !held->matches(second) &&
             !std::filesystem::exists(file),
         "removed at once: " + shown(removed));
  secure_frontend::ConfigRoutes no_store(secure_frontend::ConfigRouteSettings{.config_file = f.data.config_file});
  const auto refused = no_store.answer(secure_frontend::RouteId::strata_key_inference, R"({"action":"create"})",
                                       secure_frontend::RequestContext{.client_gone = {}, .loopback_peer = true});
  expect(refused.status == 422 && has(refused, "\"code\":\"settings_refused\""), "no key store here: refused: " + shown(refused));
}

// A save is checked as the server checks its config at start (the whole file, after the change): a value it would refuse
// is refused by name and nothing is written; a good one is written, the rest of the file kept, and applies at the next
// start. null puts a row back to its default.
void a_save_is_held_to_the_start_up_rules() {
  Fixture f;
  const std::string before = f.file();
  const auto bad_port = f.post(secure_frontend::RouteId::strata_config_settings, R"({"values":{"listener.port":0}})");
  expect(bad_port.status == 422 && has(bad_port, "\"code\":\"settings_refused\"") && has(bad_port, "listener.port"),
         "a port the server would refuse: refused by name: " + shown(bad_port));
  const auto wildcard = f.post(secure_frontend::RouteId::strata_config_settings, R"({"values":{"listener.host":"0.0.0.0"}})");
  expect(wildcard.status == 422 && has(wildcard, "listener."), "a wildcard bind without its opt-ins: refused: " + shown(wildcard));
  const auto typed = f.post(secure_frontend::RouteId::strata_config_settings, R"({"values":{"listener.port":"8080"}})");
  expect(typed.status == 422 && has(typed, "listener.port"), "a value of the wrong type: refused: " + shown(typed));
  const auto other = f.post(secure_frontend::RouteId::strata_config_settings, R"({"values":{"engine.executable":"C:\\x.exe"}})");
  expect(other.status == 422 && has(other, "engine.executable"), "a row the tab does not manage: refused: " + shown(other));
  const auto subnet = f.post(secure_frontend::RouteId::strata_config_settings,
                             R"({"values":{"listener.tool_call_hosts":["192.0.2.0/24"]}})");
  expect(subnet.status == 422 && has(subnet, "listener.tool_call_hosts"), "subnets cannot widen the tool allowlist");
  secure_frontend::ConfigRouteSettings remote_settings{.config_file = f.data.config_file};
  remote_settings.client_allowed = [](const auto&) { return true; };
  secure_frontend::ConfigRoutes remote_routes(std::move(remote_settings));
  const secure_frontend::RequestContext remote{.client_gone = {}, .loopback_peer = false};
  expect(!has(remote_routes.answer(secure_frontend::RouteId::strata_config, "{}", remote), "listener.tool_call_hosts"),
         "remote model managers do not see the local tool access controls");
  const auto remote_save = remote_routes.answer(secure_frontend::RouteId::strata_config_settings,
      R"({"values":{"listener.restrict_tool_calls":false}})", remote);
  expect(remote_save.status == 403, "remote model managers cannot change tool access");
  expect(f.file() == before, "nothing was written");
  const auto saved = f.post(secure_frontend::RouteId::strata_config_settings,
                            R"({"values":{"limits.engine.generation_timeout_s":1800,"pull.connections":8,"listener.port":9090,"tools.max_rounds":3,)"
                            R"("listener.host_aliases":["127.0.0.1:9090","localhost:9090"],)"
                            R"("listener.restrict_tool_calls":true,"listener.tool_call_hosts":["127.0.0.1","192.0.2.7"]}})");
  expect(saved.status == 200 && has(saved, "\"saved\":true") && has(saved, "\"restart_needed\":true"), "saved: " + shown(saved));
  const auto reread = secure_frontend::parse_config_text(f.file(), f.data.root);
  const auto* draft = std::get_if<secure_frontend::FrontendConfigDraft>(&reread);
  expect(draft != nullptr && std::get<std::int64_t>(draft->entries.at("limits.engine.generation_timeout_s").value) == 1800 &&
             std::get<std::int64_t>(draft->entries.at("pull.connections").value) == 8 &&
             std::get<std::int64_t>(draft->entries.at("listener.port").value) == 9090 &&
             std::get<std::int64_t>(draft->entries.at("tools.max_rounds").value) == 3 &&
             draft->entries.contains("listener.tool_call_hosts") &&
             std::get<bool>(draft->entries.at("listener.restrict_tool_calls").value) &&
             draft->entries.contains("models.default_engine"),
         "written, and the rest of the file kept");
  const auto reset = f.post(secure_frontend::RouteId::strata_config_settings, R"({"values":{"pull.connections":null}})");
  const auto again = secure_frontend::parse_config_text(f.file(), f.data.root);
  const auto* back = std::get_if<secure_frontend::FrontendConfigDraft>(&again);
  expect(reset.status == 200 && back != nullptr && !back->entries.contains("pull.connections"), "null: back to the default");
  expect(!f.logged.empty() && f.logged.back().find("settings") != std::string::npos, "the server log says settings were saved");
}

// Rotation (as `strata key rotate`): a new key in the data folder's store, shown once, applying at the next start. Only
// this PC, only confirmed; a key from an environment variable (development) has no store to rotate.
void the_key_is_rotated_in_its_store() {
  Fixture f;
  const auto token = secure_frontend::make_protected_file_secret_source("token");
  const auto* source = std::get_if<secure_frontend::SecretSource>(&token);
  if (source == nullptr) {
    expect(false, "the test's token source parses");
    return;
  }
  const auto read_key = [&] {
    const auto read = secure_frontend::read_token(*source, f.data.secret);
    const auto* bytes = std::get_if<secure_frontend::SecretBytes>(&read);
    return bytes == nullptr ? std::string{} : std::string{bytes->view()};
  };
  const std::string old_key = read_key();
  expect(old_key.size() >= 40U, "the first start made a key");
  const auto unconfirmed = f.post(secure_frontend::RouteId::strata_key_rotate, "{}");
  expect(unconfirmed.status == 400 && read_key() == old_key, "not confirmed: nothing changes: " + shown(unconfirmed));
  const auto remote = f.post(secure_frontend::RouteId::strata_key_rotate, R"({"confirm":"ROTATE"})", false);
  expect(remote.status == 403 && read_key() == old_key, "from another PC: refused: " + shown(remote));
  const auto rotated = f.post(secure_frontend::RouteId::strata_key_rotate, R"({"confirm":"ROTATE"})");
  const std::string new_key = read_key();
  expect(rotated.status == 200 && new_key != old_key && new_key.size() == old_key.size() &&
             has(rotated, "\"key\":\"" + new_key + "\"") && has(rotated, "\"restart_needed\":true"),
         "rotated: the new key, shown once: " + std::to_string(rotated.status));
  expect(rotated.header_value("Cache-Control") == std::optional<std::string_view>{"no-store"}, "never cached");
  for (const auto& line : f.logged) expect(line.find(new_key) == std::string::npos && line.find(old_key) == std::string::npos, "no key logged");
  Fixture env(false);
  const auto listed = env.post(secure_frontend::RouteId::strata_config, "{}");
  expect(has(listed, R"("key":{"rotatable":false,"inference":false})"), "a key from the environment: not rotatable here");
  const auto refused = env.post(secure_frontend::RouteId::strata_key_rotate, R"({"confirm":"ROTATE"})");
  expect(refused.status == 422 && has(refused, "environment"), "and refused by reason: " + shown(refused));
}

// The owner (2026-10-29): "when the "--debugging on" was not provided in the install/builder, I want the settings to
// hide the Troubleshooting section"; then (2026-10-30) "we should make the email, websearch, debugging all plugins as
// well". The answer says whether the Debugging plugin's Troubleshooting is on, and only then are logging.engine_output,
// the Troubleshooting row, listed and saved; otherwise it is refused by name, as a row the tab does not manage (the
// config file still takes it).
void troubleshooting_is_offered_only_while_the_debugging_plugin_is_on() {
  for (const bool on : {false, true}) {
    Fixture f(true, on);
    const auto listed = f.post(secure_frontend::RouteId::strata_config, "{}");
    expect(listed.status == 200 && has(listed, std::string{"\"debugging\":"} + (on ? "true" : "false")),
           "the answer says whether the Debugging plugin's Troubleshooting is on: " + shown(listed));
    expect(has(listed, "{\"key\":\"logging.engine_output\"") == on,
           std::string{"the Troubleshooting row is listed only while it is on ("} + (on ? "on" : "off") + "): " + listed.body);
    const std::string before = f.file();
    const auto saved = f.post(secure_frontend::RouteId::strata_config_settings, R"({"values":{"logging.engine_output":true}})");
    if (on) {
      expect(saved.status == 200 && has(saved, "\"saved\":true"), "while it is on, it is saved: " + shown(saved));
    } else {
      expect(saved.status == 422 && has(saved, "\"code\":\"settings_refused\"") && has(saved, "logging.engine_output"),
             "otherwise it is refused by name: " + shown(saved));
      expect(f.file() == before, "and nothing is written");
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"the_settings_are_listed_with_their_rules", the_settings_are_listed_with_their_rules},
      {"a_save_is_held_to_the_start_up_rules", a_save_is_held_to_the_start_up_rules},
      {"the_key_is_rotated_in_its_store", the_key_is_rotated_in_its_store},
      {"troubleshooting_is_offered_only_while_the_debugging_plugin_is_on", troubleshooting_is_offered_only_while_the_debugging_plugin_is_on},
      {"the_boot_start_is_said_with_the_settings", the_boot_start_is_said_with_the_settings},
      {"the_running_models_values_are_said_with_the_settings", the_running_models_values_are_said_with_the_settings},
      {"an_inference_key_is_made_and_removed_here", an_inference_key_is_made_and_removed_here},
  }, failures);
}
