// Notifier plugins through the real server (plugin spec 2026-10-30, section 10; email alerts first): a notifier plugin in
// the plugins folder - here the fake notifier program, first-party - is started with the server, told of its start as
// one JSON line, shown as running in the Plugins tab, and answers the tab's "Send a test" from its own folder; a config's
// old alerts.email.* rows are moved to the email plugin's settings at the start, alerts.email.enabled to its choice. And
// trust by key (section 11): a plugin signed with the install's own key runs without approval.
#include "secure_frontend/config_file.hpp"
#include "support/server_harness.hpp"
#include "support/test_support.hpp"
#include "support/test_paths.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

int failures = 0;
void expect(const bool ok, const std::string& what) {
  if (!ok) {
    ++failures;
    std::cerr << "FAILED: " << what << '\n';
  }
}

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

#if defined(_WIN32)
const std::string kNotifierProgram = "fake-notifier.exe";
#else
const std::string kNotifierProgram = "fake-notifier";
#endif

const std::string kManifest = R"({"schema": "m-strata.plugin/1", "id": "fakemail", "kind": "notifier", "name": "Fake mail",
  "version": "1.0.0", "publisher": "M-Strata", "license": "AGPL-3.0-only", "about": "A stand-in notifier.",
  "files": {"windows": ["fake-notifier.exe"], "linux": ["fake-notifier"]},
  "notifier": {"program": {"windows": "fake-notifier.exe", "linux": "fake-notifier"},
    "arguments": ["--record", "{record}"], "network": "nowhere",
    "settings": [
      {"name": "record", "kind": "text", "label": "Record", "help": "", "required": true},
      {"name": "events", "kind": "text_list", "label": "Alerts", "help": "", "default": ["server_started"],
       "choices": ["download_finished", "download_failed", "calibration_finished", "calibration_failed", "engine_stopped", "server_started"],
       "max_items": 6}]}})";

std::string json_string(const std::string& text) {
  std::string out = "\"";
  for (const char c : text) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out + "\"";
}

void a_notifier_plugin_hears_the_start_and_answers_a_test() {
  const auto record = test_support::make_temp_dir("notifier") / "record.txt";
  test_support::ServerOptions options;
  options.model_cards = {test_support::CardOptions{.name = "Qwen", .profile = "qwen"}};
  options.plugins = {{.id = "fakemail", .manifest = kManifest, .on = true, .files = {{kNotifierProgram, std::filesystem::path{FAKE_NOTIFIER_PATH}}}}};
  options.config_literals = {{"models.control", "true"}, {"plugins.fakemail.record", json_string(test_support::utf8_path(record))}};
  auto server = test_support::ServerProcess::start(options);
  const bool listening = server != nullptr && server->wait_until_listening(20s);
  expect(listening, "the launcher boots with the notifier" + (server ? ": " + server->stderr_text() : std::string{}));
  if (!listening) return;
  std::string told;
  for (int i = 0; i < 50 && !has(told, "server_started"); ++i) {
    std::error_code error;
    if (std::filesystem::exists(record, error)) told = secure_frontend_test::read_file(record);
    if (!has(told, "server_started")) std::this_thread::sleep_for(100ms);
  }
  expect(has(told, R"({"event":"server_started","subject":"the server started","body":"The Strata server is listening on )") &&
             has(told, R"(","server":")") && has(told, R"(","time":")"),
         "the plugin is told of the start, one JSON line: " + told);
  expect(has(server->stderr_text(), "[strata] plugins: fakemail is running"), "the log says it runs");
  const auto listed = test_support::http_request(server->port(), "POST", "/v1/strata/plugins", "{}", 10s);
  expect(listed && listed->status == 200 && has(listed->body, R"("id":"fakemail")") && has(listed->body, R"("running":true)"),
         "the Plugins tab shows it running: " + (listed ? listed->body.substr(0, 400) : std::string{"(no answer)"}));
  const auto tested = test_support::http_request(server->port(), "POST", "/v1/strata/plugins/test", R"({"id":"fakemail"})", 70s);
  expect(tested && tested->status == 200 && has(tested->body, R"({"ok":true,"reply":"args: --record )"),
         "Send a test is answered by the plugin: " + (tested ? tested->body : std::string{"(no answer)"}));
  const auto cwd_at = tested ? tested->body.find("cwd: ") : std::string::npos;
  if (cwd_at != std::string::npos) {
    const std::string cwd = tested->body.substr(cwd_at + 5U, tested->body.find(';', cwd_at) - cwd_at - 5U);
    std::error_code same;
    expect(std::filesystem::equivalent(std::filesystem::path{cwd}, server->root() / "state" / "plugins" / "fakemail", same),
           "it runs in its own folder under the state root: " + cwd);
  }
  expect(has(secure_frontend_test::read_file(record), R"({"test":true,"server":")"), "the test line reached it");
}

// Email alerts became a plugin (the owner's decision 6): a config's alerts.email.* rows move to plugins.email.* at the
// start, in the config file, and alerts.email.enabled becomes the email plugin's choice; the log says both.
void old_email_rows_move_to_the_plugin() {
  test_support::ServerOptions options;
  options.config_literals = {{"alerts.email.enabled", "false"},
                             {"alerts.email.host", "\"smtp.ionos.com\""},
                             {"alerts.email.port", "587"},
                             {"alerts.email.security", "\"starttls\""},
                             {"alerts.email.username", "\"alerts@example.com\""},
                             {"alerts.email.from", "\"alerts@example.com\""},
                             {"alerts.email.to", "[\"owner@example.com\"]"}};
  auto server = test_support::ServerProcess::start(options);
  const bool ready = server != nullptr && server->wait_for_health(test_support::kHealthReady, 20s);
  expect(ready, "the server starts" + (server ? ": " + server->stderr_text() : std::string{}));
  if (!ready) return;
  const std::string log = server->stderr_text();
  expect(has(log, "[strata] config: email alerts are a plugin now: alerts.email.enabled, alerts.email.from, alerts.email.host, "
                  "alerts.email.port, alerts.email.security, alerts.email.to, alerts.email.username moved to plugins.email.*"),
         "the log says what moved: " + log);
  expect(has(log, "[strata] plugins: the email plugin is off, as alerts.email.enabled was"), "and the plugin's choice");
  const auto file = secure_frontend::read_config_file(server->root() / "config.json");
  const auto* draft = std::get_if<secure_frontend::FrontendConfigDraft>(&file);
  expect(draft != nullptr && draft->entries.contains("plugins.email.host") && draft->entries.contains("plugins.email.to") &&
             std::none_of(draft->entries.begin(), draft->entries.end(), [](const auto& row) { return row.first.rfind("alerts.", 0) == 0U; }),
         "the config file holds the plugin's settings, no alerts.* row");
  expect(has(secure_frontend_test::read_file(server->root() / "state" / "plugins.json"), R"("enabled":{"email":false})"),
         "plugins.json holds the choice");
}

// Trust by key (plugin spec section 11; the owner's decision 8): a third-party plugin signed with the install's own key
// (build.json's signing_key) runs, the Plugins tab saying so; without that key it waits, saying why; and a publisher key
// is added through the real router. (The fixture is scripts/plugin_signing.py's, signed with RFC 8032's TEST 1 key.)
void a_plugin_signed_with_the_install_key_runs() {
  const std::string rfc_key = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";
  const std::filesystem::path fixtures = std::filesystem::path{SECURE_FRONTEND_PLUGIN_FIXTURES} / "signedtool";
#if defined(_WIN32)
  const std::string program = "signed-tool.exe";
  const std::string sig = "plugin.sig.windows";
#else
  const std::string program = "signed-tool";
  const std::string sig = "plugin.sig.linux";
#endif
  for (const bool keyed : {true, false}) {
    test_support::ServerOptions options;
    options.model_cards = {test_support::CardOptions{.name = "Qwen", .profile = "qwen"}};
    options.plugins = {{.id = "signedtool", .manifest = secure_frontend_test::read_file(fixtures / "plugin.json"), .on = true,
                        .files = {{program, fixtures / "program"}, {"plugin.sig", fixtures / sig}}, .first_party = false}};
    options.signing_key = keyed ? rfc_key : std::string{};
    options.config_literals = {{"models.control", "true"}};
    auto server = test_support::ServerProcess::start(options);
    const bool listening = server != nullptr && server->wait_until_listening(20s);
    expect(listening, "the launcher boots" + (server ? ": " + server->stderr_text() : std::string{}));
    if (!listening) continue;
    const auto listed = test_support::http_request(server->port(), "POST", "/v1/strata/plugins", "{}", 30s);
    const std::string body = listed ? listed->body : std::string{"(no answer)"};
    if (keyed) {
      expect(has(body, R"("id":"signedtool")") && has(body, R"("state":"on")") && has(body, R"("signer":"this install's key")") &&
                 has(body, R"("keys":[{"key":")" + rfc_key + R"(","name":"this install's key","install":true}])"),
             "signed with the install's key: it is on, signed by that key: " + body.substr(0, 600));
      const auto added = test_support::http_request(server->port(), "POST", "/v1/strata/plugins/keys",
                                                    R"({"add":")" + std::string(64U, 'f') + R"(","name":"Example Labs"})", 10s);
      expect(added && added->status == 200 && has(added->body, R"("name":"Example Labs","install":false)"),
             "a publisher key is added through the router: " + (added ? added->body : std::string{"(no answer)"}));
    } else {
      expect(has(body, R"("state":"waiting")") && has(body, "does not trust") && has(body, R"("signer":null)"),
             "without the key: it waits, saying why: " + body.substr(0, 600));
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"a_notifier_plugin_hears_the_start_and_answers_a_test", a_notifier_plugin_hears_the_start_and_answers_a_test},
      {"old_email_rows_move_to_the_plugin", old_email_rows_move_to_the_plugin},
      {"a_plugin_signed_with_the_install_key_runs", a_plugin_signed_with_the_install_key_runs},
  }, failures);
}
