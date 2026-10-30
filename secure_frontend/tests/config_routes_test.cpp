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
  explicit Fixture(const bool key_in_store = true) {
    const auto bin = root / "bin";
    secure_frontend_test::make_private_directory(bin);
    const auto engine = bin / "multiengine.exe";
    secure_frontend_test::write_file(engine, "engine", 0700);
    (void)secure_frontend::ensure_data_folder(data, engine);
    secure_frontend::ConfigRouteSettings settings{.config_file = data.config_file};
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
                          "listener.host_aliases", "models.control_remote", "models.control_remote_networks",
                          "limits.engine.generation_timeout_s", "limits.engine.ready_timeout_s", "limits.chat.max_prompt_tokens",
                          "limits.chat.max_completion_tokens", "pull.connections", "models.downloads_root", "models.default_root"}) {
    expect(has(listed, std::string{"{\"key\":\""} + key + "\""), std::string{"listed: "} + key);
  }
  expect(has(listed, R"({"key":"listener.port","type":"integer","value":8080,"default":)") &&
             has(listed, R"({"key":"limits.engine.generation_timeout_s","type":"integer","value":null,"default":1800,"min":1,"max":86400,)"),
         "a value the file holds, and one it leaves to the default: " + listed.body);
  expect(has(listed, R"("key":{"rotatable":true})"), "the key lives in the data folder's store: it can be rotated");
  expect(listed.header_value("Cache-Control") == std::optional<std::string_view>{"no-store"}, "never cached");
  const auto remote = f.post(secure_frontend::RouteId::strata_config, "{}", false);
  expect(remote.status == 403 && has(remote, "\"code\":\"control_not_allowed\""), "another PC is refused: " + shown(remote));
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
  expect(f.file() == before, "nothing was written");
  const auto saved = f.post(secure_frontend::RouteId::strata_config_settings,
                            R"({"values":{"limits.engine.generation_timeout_s":1800,"pull.connections":8,"listener.port":9090,)"
                            R"("listener.host_aliases":["127.0.0.1:9090","localhost:9090"]}})");
  expect(saved.status == 200 && has(saved, "\"saved\":true") && has(saved, "\"restart_needed\":true"), "saved: " + shown(saved));
  const auto reread = secure_frontend::parse_config_text(f.file(), f.data.root);
  const auto* draft = std::get_if<secure_frontend::FrontendConfigDraft>(&reread);
  expect(draft != nullptr && std::get<std::int64_t>(draft->entries.at("limits.engine.generation_timeout_s").value) == 1800 &&
             std::get<std::int64_t>(draft->entries.at("pull.connections").value) == 8 &&
             std::get<std::int64_t>(draft->entries.at("listener.port").value) == 9090 &&
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
  expect(has(listed, R"("key":{"rotatable":false})"), "a key from the environment: not rotatable here");
  const auto refused = env.post(secure_frontend::RouteId::strata_key_rotate, R"({"confirm":"ROTATE"})");
  expect(refused.status == 422 && has(refused, "environment"), "and refused by reason: " + shown(refused));
}

// The owner (2026-10-29): "when the "--debugging on" was not provided in the install/builder, I want the settings to
// hide the Troubleshooting section". The answer says which build this is, and only a debugging build (make --debugging)
// lists and saves logging.engine_output, the Troubleshooting row; any other build refuses it by name, as a row the tab
// does not manage (the config file still takes it).
void troubleshooting_is_offered_only_in_a_debugging_build() {
  Fixture f;
  const bool debugging = secure_frontend::debugging_build();
  const auto listed = f.post(secure_frontend::RouteId::strata_config, "{}");
  expect(listed.status == 200 && has(listed, std::string{"\"debugging\":"} + (debugging ? "true" : "false")),
         "the answer says whether this is a debugging build: " + shown(listed));
  expect(has(listed, "{\"key\":\"logging.engine_output\"") == debugging,
         std::string{"the Troubleshooting row is listed only in a debugging build (this one: "} + (debugging ? "yes" : "no") +
             "): " + listed.body);
  const std::string before = f.file();
  const auto saved = f.post(secure_frontend::RouteId::strata_config_settings, R"({"values":{"logging.engine_output":true}})");
  if (debugging) {
    expect(saved.status == 200 && has(saved, "\"saved\":true"), "a debugging build saves it: " + shown(saved));
  } else {
    expect(saved.status == 422 && has(saved, "\"code\":\"settings_refused\"") && has(saved, "logging.engine_output"),
           "any other build refuses it by name: " + shown(saved));
    expect(f.file() == before, "and writes nothing");
  }
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"the_settings_are_listed_with_their_rules", the_settings_are_listed_with_their_rules},
      {"a_save_is_held_to_the_start_up_rules", a_save_is_held_to_the_start_up_rules},
      {"the_key_is_rotated_in_its_store", the_key_is_rotated_in_its_store},
      {"troubleshooting_is_offered_only_in_a_debugging_build", troubleshooting_is_offered_only_in_a_debugging_build},
  }, failures);
}
