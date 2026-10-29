// Model launcher (plan docs/superpowers/plans/2026-10-23-model-launcher.md Tasks 3-4; spec §5):
// the real strata-unified-server booted with model cards over the supervision fake engine. A
// card starts from the API with typed options, a switch waits for the generation in flight, a
// failed start leaves the server usable, one switch runs at a time, and the page's options are
// remembered.
#include "secure_frontend/api_errors.hpp"
#include "secure_frontend/calibration.hpp"
#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/card_builder.hpp"
#include "secure_frontend/config_file.hpp"
#include "secure_frontend/first_start.hpp"
#include "secure_frontend/gguf_metadata.hpp"
#include "secure_frontend/json.hpp"
#include "secure_frontend/model_launcher.hpp"
#include "secure_frontend/model_runtime.hpp"
#include "secure_frontend/tool_runner.hpp"
#include "support/config_json.hpp"
#include "support/fake_engine_dump.hpp"
#include "support/fake_supervised_process.hpp"
#include "support/gguf_builder.hpp"
#include "support/server_harness.hpp"
#include "support/test_paths.hpp"
#include "support/test_support.hpp"
#include "support/test_tokenizer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

int failures = 0;
void expect(const bool value, const std::string_view message) {
  if (!value) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

using test_support::CardOptions;
using test_support::HttpResult;
using test_support::ServerOptions;
using test_support::ServerProcess;

constexpr auto kRequest = std::chrono::seconds(10);

std::string chat_body(const std::string& model) {
  return R"({"model":")" + model + R"(","messages":[{"role":"user","content":"Hi"}],"max_tokens":2})";
}

std::optional<HttpResult> call(const ServerProcess& server, const std::string_view method, const std::string_view target,
                               const std::string_view body = "", const std::chrono::milliseconds timeout = kRequest) {
  return test_support::http_request(server.port(), method, target, body, timeout);
}

std::string shown(const std::optional<HttpResult>& result) {
  return result ? std::to_string(result->status) + " " + result->body : std::string{"(no answer)"};
}

bool has_code(const std::optional<HttpResult>& result, const int status, const std::string_view code) {
  return result && result->status == status && result->body.find("\"code\":\"" + std::string{code} + "\"") != std::string::npos;
}

// The state of card `name` in GET /v1/strata/models ("" when the list cannot be read).
std::string card_state(const ServerProcess& server, const std::string& name, std::string* error = nullptr) {
  const auto listed = call(server, "GET", "/v1/strata/models");
  if (!listed || listed->status != 200) return {};
  const auto parsed = secure_frontend::parse_json(
      listed->body, {.max_bytes = 1U << 20U, .max_depth = 16U, .max_container_entries = 4096U, .max_string_bytes = 4096U,
                     .allow_fractions = true});
  const auto* root = std::get_if<secure_frontend::JsonValue>(&parsed);
  if (root == nullptr || root->if_object() == nullptr) return {};
  for (const auto& member : *root->if_object()) {
    if (member.key != "data" || member.value.if_array() == nullptr) continue;
    for (const auto& card : *member.value.if_array()) {
      if (card.if_object() == nullptr) continue;
      std::string card_name;
      std::string state;
      for (const auto& field : *card.if_object()) {
        if (field.key == "name" && field.value.if_string()) card_name = *field.value.if_string();
        if (field.key == "state" && field.value.if_string()) state = *field.value.if_string();
        if (field.key == "error" && field.value.if_string() && error != nullptr) *error = *field.value.if_string();
      }
      if (card_name == name) return state;
    }
  }
  return {};
}

bool wait_for_card(const ServerProcess& server, const std::string& name, const std::string& state,
                   const std::chrono::milliseconds timeout = std::chrono::seconds(30)) {
  const auto until = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < until) {
    if (card_state(server, name) == state) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return card_state(server, name) == state;
}

std::vector<std::string> dumped_argv(const std::filesystem::path& file) {
  const auto dump = test_support::read_dump_file(file, std::chrono::seconds(5));
  return dump ? dump->argv : std::vector<std::string>{};
}

std::string joined(const std::vector<std::string>& argv) {
  std::string out;
  for (const auto& argument : argv) out += (out.empty() ? "" : " ") + argument;
  return out.empty() ? std::string{"(no dump)"} : out;
}

bool has_pair(const std::vector<std::string>& argv, const std::string& flag, const std::string& value) {
  for (std::size_t i = 0; i + 1U < argv.size(); ++i) {
    if (argv[i] == flag && argv[i + 1U] == value) return true;
  }
  return false;
}

// Two qwen cards over the fake engine: Alpha offers context, KV precision and KV streaming,
// Beta none. `alpha_args` / `beta_args` are their fixed engine arguments.
ServerOptions two_cards(std::vector<std::string> alpha_args = {}, std::vector<std::string> beta_args = {},
                        std::map<std::string, std::string> literals = {{"models.control", "true"}}) {
  ServerOptions options;
  options.model_cards = {
      CardOptions{.name = "Alpha",
                  .profile = "qwen",
                  .engine_args = std::move(alpha_args),
                  .options_json = R"({"max_context": {"default": 32768, "max": 65536},
                                      "kv": {"default": "int8", "choices": ["int8", "k8v4"]},
                                      "kv_resident": {"default": 0}})"},
      CardOptions{.name = "Beta", .profile = "qwen", .engine_args = std::move(beta_args)}};
  options.config_literals = std::move(literals);
  return options;
}

// Vision for cards (spec 2026-10-28-vision-for-cards-design.md; the owner: "a checkbox to also download vision files"):
// a qwen card with its picture encoder (vision.mmproj, the Pictures option) starts the encoder with its engine (upstream's
// strata-vision beside the engine), a chat with a picture reaches the engine as a GENI line, a stop takes the encoder
// down with the engine, and a text-only card refuses pictures.
std::string b64(const std::string& raw) {
  static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (std::size_t i = 0; i < raw.size(); i += 3U) {
    const unsigned b0 = static_cast<unsigned char>(raw[i]);
    const unsigned b1 = i + 1U < raw.size() ? static_cast<unsigned char>(raw[i + 1U]) : 0U;
    const unsigned b2 = i + 2U < raw.size() ? static_cast<unsigned char>(raw[i + 2U]) : 0U;
    out += table[b0 >> 2U];
    out += table[((b0 & 3U) << 4U) | (b1 >> 4U)];
    out += i + 1U < raw.size() ? table[((b1 & 15U) << 2U) | (b2 >> 6U)] : '=';
    out += i + 2U < raw.size() ? table[b2 & 63U] : '=';
  }
  return out;
}

std::string picture_body(const std::string& model) {
  const auto be32 = [](const unsigned v) {
    return std::string{static_cast<char>(v >> 24), static_cast<char>(v >> 16), static_cast<char>(v >> 8), static_cast<char>(v)};
  };
  const std::string png = std::string("\x89PNG\r\n\x1a\n", 8) + be32(13) + "IHDR" + be32(8) + be32(6) + std::string(9U, '\0');
  return R"({"model":")" + model + R"(","messages":[{"role":"user","content":[{"type":"text","text":"look"},)" +
         R"({"type":"image_url","image_url":{"url":"data:image/png;base64,)" + b64(png) + R"("}}]}],"max_tokens":2})";
}

void a_card_reads_pictures_with_its_encoder() {
  const auto record_file = test_support::make_temp_dir("seer") / "record.txt";
  ServerOptions options;
  options.model_cards = {CardOptions{.name = "Seer", .profile = "qwen", .engine_args = {"--record-file=" + test_support::utf8_path(record_file)},
                                     .options_json = R"({"vision": {"default": "cpu"}})", .vision = true},
                         CardOptions{.name = "Plain", .profile = "qwen"}};
  options.config_literals = {{"models.control", "true"}};
  auto server = ServerProcess::start(options);
  expect(server && server->wait_until_listening(std::chrono::seconds(20)), "the launcher boots");
  if (!server) return;
  const auto listed = call(*server, "GET", "/v1/strata/models");
  expect(listed && listed->body.find(R"("name":"vision","kind":"choice")") != std::string::npos,
         "Seer lists its Pictures option: " + shown(listed));
  const auto started = call(*server, "POST", "/v1/strata/models/start", R"({"name":"Seer"})");
  expect(started && started->status == 202, "Seer's start is accepted: " + shown(started));
  expect(wait_for_card(*server, "Seer", "running"), "Seer runs: " + server->stderr_text());
  expect(server->stderr_text().find("Seer: the picture encoder is running") != std::string::npos,
         "its encoder started with it: " + server->stderr_text());
  expect(server->descendants().size() == 2U, "two children: the engine and the encoder");
  const auto picture = call(*server, "POST", "/v1/chat/completions", picture_body("Seer"));
  expect(picture && picture->status == 200, "a chat with a picture answers on Seer: " + shown(picture));
  const auto record = secure_frontend_test::read_file(record_file);
  expect(record.find("GENI ") != std::string::npos, "the picture reached the engine as a GENI line: " + record.substr(0, 400));
  const auto stopped = call(*server, "POST", "/v1/strata/models/stop", "{}");
  expect(stopped && stopped->status == 202 && wait_for_card(*server, "Seer", "stopped"), "Seer stops");
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!server->descendants().empty() && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  expect(server->descendants().empty(), "the encoder stopped with the engine");
  const auto plain = call(*server, "POST", "/v1/strata/models/start", R"({"name":"Plain"})");
  expect(plain && plain->status == 202 && wait_for_card(*server, "Plain", "running"), "Plain runs");
  const auto refused = call(*server, "POST", "/v1/chat/completions", picture_body("Plain"));
  expect(refused && refused->status == 400, "a text-only card refuses pictures: " + shown(refused));
  server->stop(ServerProcess::Stop::ctrl_break);
  expect(server->wait_exit(std::chrono::seconds(20)) == std::optional<int>{0}, "a clean exit");
}

void launcher_boots_idle_and_starts_a_card() {
  // The engine may write only below its state directory: a relative dump lands in its cwd,
  // <state>/engine.
  auto server = ServerProcess::start(two_cards({"--dump-file=alpha.dump"}));
  expect(server && server->wait_until_listening(std::chrono::seconds(20)), "the launcher boots");
  if (!server) return;
  const auto dump = server->root() / "state" / "engine" / "alpha.dump";
  const auto health = call(*server, "GET", "/health");
  expect(health && health->status == 200 && health->body == "{\"status\":\"ok\",\"engine\":\"none\"}\n",
         "no engine at boot (D5): " + shown(health));
  expect(server->descendants().empty(), "nothing spawned before a start");
  expect(card_state(*server, "Alpha") == "stopped" && card_state(*server, "Beta") == "stopped", "both cards are listed, stopped");
  const auto listed = call(*server, "GET", "/v1/strata/models");
  expect(listed && listed->body.find("\"name\":\"kv_resident\"") != std::string::npos &&
             listed->body.find("\"choices\":[\"int8\",\"k8v4\"]") != std::string::npos,
         "the list describes each card's options: " + shown(listed));
  expect(has_code(call(*server, "POST", "/v1/chat/completions", chat_body("Alpha")), 503, "no_model_running"),
         "no chat before a start");
  const auto models = call(*server, "GET", "/v1/models");
  expect(models && models->body.find("\"data\":[]") != std::string::npos && models->body.find("\"strata_models\"") != std::string::npos,
         "/v1/models lists nothing running and the cards: " + shown(models));

  const auto started = call(*server, "POST", "/v1/strata/models/start",
                            R"({"name":"Alpha","options":{"max_context":65536,"kv":"k8v4","kv_resident":20480}})");
  expect(started && started->status == 202 && started->body.find("\"state\":\"starting\"") != std::string::npos,
         "a start is accepted: " + shown(started));
  expect(wait_for_card(*server, "Alpha", "running"), "Alpha runs: " + server->stderr_text());
  const auto argv = dumped_argv(dump);
  expect(has_pair(argv, "--max-context", "65536") && has_pair(argv, "--kv", "k8v4") && has_pair(argv, "--kv-resident", "20480"),
         "the page's options reach the engine's arguments: " + joined(argv));
  const auto chat = call(*server, "POST", "/v1/chat/completions", chat_body("Alpha"));
  expect(chat && chat->status == 200 && chat->body.find("\"model\":\"Alpha\"") != std::string::npos, "Alpha answers: " + shown(chat));
  expect(has_code(call(*server, "POST", "/v1/chat/completions", chat_body("Beta")), 409, "model_not_running"),
         "a request naming another card is told it is not running (D6)");
  expect(has_code(call(*server, "POST", "/v1/chat/completions", chat_body("nobody")), 404, "model_not_found"),
         "an unknown model is still not found");
  const auto running_models = call(*server, "GET", "/v1/models");
  expect(running_models && running_models->body.find("\"id\":\"Alpha\"") != std::string::npos, "/v1/models lists Alpha");
  const auto ready = call(*server, "GET", "/health");
  expect(ready && ready->status == 200 && ready->body.find("\"engine\":\"ready\"") != std::string::npos, "health follows the engine");

  const auto stopped = call(*server, "POST", "/v1/strata/models/stop", "{}");
  expect(stopped && stopped->status == 202, "a stop is accepted: " + shown(stopped));
  expect(wait_for_card(*server, "Alpha", "stopped"), "Alpha stops");
  const auto after = call(*server, "GET", "/health");
  expect(after && after->body.find("\"engine\":\"none\"") != std::string::npos, "the GPU is free again: " + shown(after));
  server->stop(ServerProcess::Stop::ctrl_break);
  const auto exit = server->wait_exit(std::chrono::seconds(20));
  expect(exit == std::optional<int>{0}, "a clean exit");
}

// D3: a switch waits for the generation in flight; meanwhile chat answers model_switching.
void switch_waits_for_the_generation_then_swaps() {
  auto server = ServerProcess::start(two_cards({"--tokens=12", "--token-interval-ms=250"}));
  expect(server && server->wait_until_listening(std::chrono::seconds(20)), "the launcher boots");
  if (!server) return;
  expect(call(*server, "POST", "/v1/strata/models/start", R"({"name":"Alpha"})")->status == 202, "Alpha starts");
  expect(wait_for_card(*server, "Alpha", "running"), "Alpha runs");
  std::optional<HttpResult> long_chat;
  std::thread generating([&] {
    long_chat = call(*server, "POST", "/v1/chat/completions",
                     R"({"model":"Alpha","messages":[{"role":"user","content":"Hi"}],"max_tokens":12})", std::chrono::seconds(30));
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(700));  // the generation is under way
  const auto switched = call(*server, "POST", "/v1/strata/models/start", R"({"name":"Beta"})");
  expect(switched && switched->status == 202, "a switch is accepted while Alpha generates: " + shown(switched));
  const auto during = call(*server, "POST", "/v1/chat/completions", chat_body("Alpha"));
  expect(has_code(during, 503, "model_switching"), "a new chat during the switch: " + shown(during));
  const auto for_beta = call(*server, "POST", "/v1/chat/completions", chat_body("Beta"));
  expect(has_code(for_beta, 503, "model_switching"), "a chat naming the card being started: " + shown(for_beta));
  expect(has_code(call(*server, "POST", "/v1/strata/models/start", R"({"name":"Alpha"})"), 409, "switch_in_progress"),
         "one switch at a time");
  expect(has_code(call(*server, "POST", "/v1/strata/models/stop", "{}"), 409, "switch_in_progress"), "no stop during a switch");
  generating.join();
  expect(long_chat && long_chat->status == 200 && long_chat->body.find("\"completion_tokens\":12") != std::string::npos,
         "the generation in flight finished whole: " + shown(long_chat));
  expect(wait_for_card(*server, "Beta", "running") && card_state(*server, "Alpha") == "stopped", "Beta replaced Alpha");
  const auto beta = call(*server, "POST", "/v1/chat/completions", chat_body("Beta"));
  expect(beta && beta->status == 200 && beta->body.find("\"model\":\"Beta\"") != std::string::npos, "Beta answers: " + shown(beta));
  server->stop(ServerProcess::Stop::ctrl_break);
  expect(server->wait_exit(std::chrono::seconds(20)) == std::optional<int>{0}, "a clean exit");
}

// D4: a start that cannot load stays failed with a fixed reason; the server stays usable.
void failed_start_keeps_the_server_usable() {
  auto server = ServerProcess::start(two_cards({}, {"--exit-before-ready=3"}));
  expect(server && server->wait_until_listening(std::chrono::seconds(20)), "the launcher boots");
  if (!server) return;
  expect(call(*server, "POST", "/v1/strata/models/start", R"({"name":"Alpha"})")->status == 202, "Alpha starts");
  expect(wait_for_card(*server, "Alpha", "running"), "Alpha runs");
  expect(call(*server, "POST", "/v1/strata/models/start", R"({"name":"Beta"})")->status == 202, "the switch to Beta is accepted");
  expect(wait_for_card(*server, "Beta", "failed"), "Beta fails to load");
  std::string error;
  (void)card_state(*server, "Beta", &error);
  expect(error == "the engine ended before it was ready (see the server log)", "a fixed reason: " + error);
  expect(card_state(*server, "Alpha") == "stopped", "the old model is not restarted (D4)");
  const auto health = call(*server, "GET", "/health");
  expect(health && health->status == 200 && health->body.find("\"engine\":\"none\"") != std::string::npos, "nothing runs: " + shown(health));
  expect(call(*server, "POST", "/v1/strata/models/start", R"({"name":"Alpha"})")->status == 202 && wait_for_card(*server, "Alpha", "running"),
         "another card starts after the failure");
  server->stop(ServerProcess::Stop::ctrl_break);
  expect(server->wait_exit(std::chrono::seconds(20)) == std::optional<int>{0}, "a clean exit");
}

void start_refusals_name_the_problem() {
  auto server = ServerProcess::start(two_cards({"--ready-delay-ms=1500"}));
  expect(server && server->wait_until_listening(std::chrono::seconds(20)), "the launcher boots");
  if (!server) return;
  expect(has_code(call(*server, "POST", "/v1/strata/models/start", R"({"name":"Gamma"})"), 404, "model_not_found"), "an unknown card");
  const auto too_big = call(*server, "POST", "/v1/strata/models/start", R"({"name":"Alpha","options":{"max_context":131072}})");
  expect(has_code(too_big, 400, "invalid_option") && too_big->body.find("\"param\":\"max_context\"") != std::string::npos,
         "a value over the card's bound names the option: " + shown(too_big));
  expect(has_code(call(*server, "POST", "/v1/strata/models/start", R"({"name":"Alpha","options":{"kv":"q4_0"}})"), 400, "invalid_option"),
         "a choice the card does not offer");
  expect(has_code(call(*server, "POST", "/v1/strata/models/start", R"({"name":"Alpha","options":{"kv_resident":100}})"), 400, "invalid_option"),
         "KV streaming below the engine's floor");
  expect(has_code(call(*server, "POST", "/v1/strata/models/start", R"({"name":"Beta","options":{"kv":"int8"}})"), 400, "invalid_option"),
         "an option the card does not list");
  expect(has_code(call(*server, "POST", "/v1/strata/models/start", R"({"name":"Alpha","engine":"x"})"), 400, "invalid_request"),
         "no other member than name and options");
  expect(has_code(call(*server, "POST", "/v1/strata/models/start", R"(["Alpha"])"), 400, "invalid_request"), "an object");
  expect(has_code(call(*server, "POST", "/v1/strata/models/start", R"({"name":)"), 400, "invalid_json"), "JSON");
  expect(server->descendants().empty(), "no refusal spawned anything");
  expect(call(*server, "POST", "/v1/strata/models/start", R"({"name":"Alpha"})")->status == 202, "a good start is accepted");
  const auto health = call(*server, "GET", "/health");
  expect(health && health->status == 503 && health->body.find("\"engine\":\"starting\"") != std::string::npos,
         "health says starting while the card loads: " + shown(health));
  expect(has_code(call(*server, "POST", "/v1/chat/completions", chat_body("Alpha")), 503, "model_switching"), "chat waits for the load");
  expect(wait_for_card(*server, "Alpha", "running"), "Alpha runs");
  server->stop(ServerProcess::Stop::ctrl_break);
  expect(server->wait_exit(std::chrono::seconds(20)) == std::optional<int>{0}, "a clean exit");
}

// D7: the page's last options per card, in an owner-only file under roots.state, are the next
// start's values (and the list's), also after a restart.
void options_are_remembered() {
  const auto scratch = test_support::make_temp_dir("launcher");
  const auto state = scratch / "state";
  secure_frontend_test::make_owner_private_directory(state);
  const auto dump = state / "engine" / "alpha.dump";
  auto options = two_cards({"--dump-file=alpha.dump"});
  options.state_dir = state;
  {
    auto server = ServerProcess::start(options);
    expect(server && server->wait_until_listening(std::chrono::seconds(20)), "the launcher boots");
    if (!server) return;
    expect(call(*server, "POST", "/v1/strata/models/start", R"({"name":"Alpha","options":{"max_context":16384}})")->status == 202,
           "Alpha starts with a smaller context");
    expect(wait_for_card(*server, "Alpha", "running"), "Alpha runs");
    server->stop(ServerProcess::Stop::ctrl_break);
    expect(server->wait_exit(std::chrono::seconds(20)) == std::optional<int>{0}, "a clean exit");
  }
  expect(std::filesystem::exists(state / "model-options.json"), "the options file is under roots.state");
  auto server = ServerProcess::start(options);
  expect(server && server->wait_until_listening(std::chrono::seconds(20)), "the launcher boots again");
  if (!server) return;
  const auto listed = call(*server, "GET", "/v1/strata/models");
  expect(listed && listed->body.find("\"default\":32768,\"min\":") != std::string::npos &&
             listed->body.find("\"value\":16384") != std::string::npos,
         "the list offers the remembered value: " + shown(listed));
  std::filesystem::remove(dump);
  expect(call(*server, "POST", "/v1/strata/models/start", R"({"name":"Alpha"})")->status == 202 && wait_for_card(*server, "Alpha", "running"),
         "a start without options");
  expect(has_pair(dumped_argv(dump), "--max-context", "16384"), "uses the remembered value");
  server->stop(ServerProcess::Stop::ctrl_break);
  expect(server->wait_exit(std::chrono::seconds(20)) == std::optional<int>{0}, "a clean exit");
}

// D5 and the control switch: models.start starts a card at boot; with models.control off the
// control routes are no routes and the model still serves.
void models_start_and_control_off() {
  auto server = ServerProcess::start(two_cards({}, {}, {{"models.start", "\"Beta\""}}));
  expect(server && server->wait_until_listening(std::chrono::seconds(20)), "the launcher boots");
  if (!server) return;
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  std::optional<HttpResult> chat;
  while (std::chrono::steady_clock::now() < until) {
    chat = call(*server, "POST", "/v1/chat/completions", chat_body("Beta"));
    if (chat && chat->status == 200) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  expect(chat && chat->status == 200, "the card models.start names serves: " + shown(chat));
  for (const auto& [method, target] : std::vector<std::pair<std::string, std::string>>{
           {"GET", "/v1/strata/models"}, {"POST", "/v1/strata/models/start"}, {"POST", "/v1/strata/models/stop"},
           {"GET", "/v1/strata/downloads"}, {"POST", "/v1/strata/downloads/start"}}) {
    const auto r = call(*server, method, target, method == "POST" ? R"({"name":"Alpha"})" : "");
    expect(r && r->status == 404, method + " " + target + " is no route with models.control and models.downloads off: " + shown(r));
  }
  server->stop(ServerProcess::Stop::ctrl_break);
  expect(server->wait_exit(std::chrono::seconds(20)) == std::optional<int>{0}, "a clean exit with a model running");
  expect(server->descendants().empty(), "no engine left behind");
}

// D9: a bad card is refused by name at boot, the others load; the main config may not set a
// card's rows.
void bad_card_refused_at_boot_and_main_rows_refused() {
  auto options = two_cards();
  options.model_cards.push_back(CardOptions{.name = "Broken", .profile = "qwen", .options_json = R"({"kv": {"default": "fp32"}})"});
  auto server = ServerProcess::start(options);
  expect(server && server->wait_until_listening(std::chrono::seconds(20)), "the launcher boots with a bad card listed");
  if (!server) return;
  expect(server->stderr_text().find("model card 3 (Broken) refused: options.kv") != std::string::npos,
         "the bad card is refused by name: " + server->stderr_text());
  expect(card_state(*server, "Alpha") == "stopped" && card_state(*server, "Broken").empty(), "the good cards load, the bad one is not listed");
  server->stop(ServerProcess::Stop::ctrl_break);
  (void)server->wait_exit(std::chrono::seconds(20));

  auto conflicting = two_cards({}, {}, {{"models.control", "true"}, {"defaults.sampling.temperature", "0.5"}});
  auto refused = ServerProcess::start(conflicting);
  expect(refused != nullptr, "the server process starts");
  if (!refused) return;
  const auto exit = refused->wait_exit(std::chrono::seconds(20));
  expect(exit.has_value() && *exit != 0 &&
             refused->stderr_text().find("defaults.sampling.temperature: is set by each model card when models.cards is set") !=
                 std::string::npos,
         "a card row in the main config refuses the boot by name: " + refused->stderr_text());
}


// Model downloads (plan 2026-10-23-model-downloads Task 4): models.downloads serves the download
// routes (the embedded catalog and the drive's free space), makes the store and <state>/downloads
// and <state>/cards owner-only, and a card a prepared download wrote there loads at boot like a
// listed one.
void downloads_serve_the_catalog_and_load_download_cards() {
  const auto scratch = secure_frontend_test::make_scratch_directory();
  secure_frontend_test::make_owner_private_directory(scratch / "state");
  auto options = two_cards({}, {}, {{"models.control", "true"}, {"models.downloads", "true"},
                                    {"models.downloads_root", secure_frontend_test::json_literal(secure_frontend::path_to_utf8(scratch / "store"))}});
  options.state_dir = scratch / "state";
  auto first = ServerProcess::start(options);
  expect(first && first->wait_until_listening(std::chrono::seconds(20)), "the launcher boots with downloads on");
  if (!first) return;
  const auto listed = call(*first, "GET", "/v1/strata/downloads");
  expect(listed && listed->status == 200 && listed->body.find("\"disk\":{\"root\":") != std::string::npos &&
             listed->body.find("\"id\":\"qwen3.8-flash-next-iq2_xs\"") != std::string::npos &&
             listed->body.find("\"source\":\"maya\"") != std::string::npos,
         "the embedded catalog and the disk: " + shown(listed).substr(0, 400) + " " + first->stderr_text());
  std::error_code ec;
  expect(std::filesystem::is_directory(scratch / "store", ec) && std::filesystem::is_directory(scratch / "state" / "downloads", ec) &&
             std::filesystem::is_directory(scratch / "state" / "cards", ec),
         "the store, <state>/downloads and <state>/cards are made");
  expect(has_code(call(*first, "POST", "/v1/strata/downloads/start", R"({"id":"nope"})"), 404, "model_not_found"),
         "a start of an unknown id is refused");
  expect(has_code(call(*first, "POST", "/v1/strata/downloads/link", R"({"url":"https://example.com/a/b/resolve/main/m.gguf"})"), 400,
                  "invalid_request"),
         "a link off Hugging Face is refused (O4)");
  // The card a prepared download wrote: Alpha's own file under another name.
  std::ifstream alpha_file(first->root() / "cards" / "1.json", std::ios::binary);
  std::string card{std::istreambuf_iterator<char>(alpha_file), std::istreambuf_iterator<char>()};
  const auto named = card.find("\"name\": \"Alpha\"");
  expect(named != std::string::npos, "the harness card names Alpha");
  if (named == std::string::npos) return;
  card.replace(named, std::string_view{"\"name\": \"Alpha\""}.size(), "\"name\": \"Gamma\"");
  secure_frontend_test::write_file(scratch / "state" / "cards" / "gamma.json", card, 0600);
  first->stop(ServerProcess::Stop::ctrl_break);
  expect(first->wait_exit(std::chrono::seconds(20)) == std::optional<int>{0}, "a clean exit");

  auto second = ServerProcess::start(options);
  expect(second && second->wait_until_listening(std::chrono::seconds(20)), "the launcher boots again");
  if (!second) return;
  expect(card_state(*second, "Gamma") == "stopped" && card_state(*second, "Alpha") == "stopped",
         "the download's card loads at boot beside the listed ones: " + second->stderr_text());
  second->stop(ServerProcess::Stop::ctrl_break);
  expect(second->wait_exit(std::chrono::seconds(20)) == std::optional<int>{0}, "a clean exit again");
}

// A prepared download becomes a card while the server runs: the launcher adds it (listed at
// once), refuses a second card of the same name, and holds at most kMaxLauncherCards.
void cards_can_be_added_while_serving() {
  secure_frontend::ModelCard alpha;
  alpha.name = "Alpha";
  secure_frontend::LauncherSettings settings;
  settings.cards = {alpha};
  settings.log = [](std::string_view) {};
  secure_frontend::ModelLauncher launcher(std::move(settings));
  secure_frontend::ModelCard gamma;
  gamma.name = "Gamma";
  expect(launcher.add_card(gamma), "a card is added");
  expect(launcher.list_json().find("\"name\":\"Gamma\"") != std::string::npos, "it is listed at once");
  expect(launcher.served().extension.find("\"id\":\"Gamma\"") != std::string::npos, "and in /v1/models' card states");
  expect(!launcher.add_card(gamma), "a second card of the same name is refused");
  std::size_t added = 2;
  for (std::size_t i = 0; i < 2U * secure_frontend::kMaxLauncherCards && launcher.add_card([i] {
         secure_frontend::ModelCard more;
         more.name = "M" + std::to_string(i);
         return more;
       }());
       ++i) {
    ++added;
  }
  expect(added == secure_frontend::kMaxLauncherCards, "at most kMaxLauncherCards cards: " + std::to_string(added));
  launcher.shutdown();
}

// GUI-first start-up (spec 2026-10-27 §4.2): a card the page built from a GGUF names no tokenizer pack; a start reads
// the tokenizer from the model file's own header, in the profile's mode, and refuses one that is not the profile's.
void a_gguf_card_reads_its_tokenizer_from_the_gguf() {
  const auto root = secure_frontend_test::make_scratch_directory();
  const auto models = root / "models";
  secure_frontend_test::make_private_directory(models);
  const auto vocabulary = test_support::spm_tokenizer();
  secure_frontend_test::write_file(
      models / "tiny-gemma-Q8_0.gguf",
      test_support::gguf_with_tokenizer(vocabulary, "gemma4", "gemma4").u32("gemma4.context_length", 32768U).bytes());
  const auto bin = root / "bin";
  secure_frontend_test::make_private_directory(bin);
  const std::filesystem::path fake{STRATA_FAKE_ENGINE_PATH};
  const auto engine = bin / fake.filename();
  std::error_code copy_error;
  std::filesystem::copy_file(fake, engine, copy_error);
  expect(!copy_error, "the engine copy");
  const auto data = secure_frontend::data_folder_layout(root / "data");
  expect(std::holds_alternative<secure_frontend::FirstStartResult>(secure_frontend::ensure_data_folder(data, engine)),
         "a first start's data folder");
  const auto main = secure_frontend::parse_config_text(secure_frontend_test::read_file(data.config_file), data.root);
  expect(std::holds_alternative<secure_frontend::FrontendConfigDraft>(main), "its config reads");
  if (!std::holds_alternative<secure_frontend::FrontendConfigDraft>(main)) return;
  const auto compose = [&](const std::string& file) -> std::variant<secure_frontend::ComposedModel, secure_frontend::ComposeFailure> {
    const auto header = secure_frontend::parse_gguf_metadata(secure_frontend_test::read_file(models / file));
    const auto made = secure_frontend::card_for_gguf(std::get<secure_frontend::GgufMetadata>(header),
                                                     {.folder = models, .file = file, .engine = engine, .source = "folder"});
    const auto parsed = secure_frontend::parse_model_card(std::get<secure_frontend::CardText>(made).json, data.root);
    const auto& card = std::get<secure_frontend::ModelCard>(parsed);
    auto validated = secure_frontend::card_config(std::get<secure_frontend::FrontendConfigDraft>(main), card,
                                                  secure_frontend::default_option_values(card));
    if (const auto* issues = std::get_if<secure_frontend::ConfigErrors>(&validated)) {
      std::string why;
      for (const auto& issue : *issues) why += secure_frontend::format_issue(issue) + "; ";
      return secure_frontend::ComposeFailure{{why}, "the card does not validate", 1};
    }
    return secure_frontend::compose_model(*std::get<secure_frontend::ValidatedConfig>(validated).shared(), true);
  };
  const auto composed = compose("tiny-gemma-Q8_0.gguf");
  const auto* model = std::get_if<secure_frontend::ComposedModel>(&composed);
  std::string why;
  if (const auto* failure = std::get_if<secure_frontend::ComposeFailure>(&composed)) {
    for (const auto& message : failure->messages) why += message + "; ";
  }
  expect(model != nullptr, "the card composes without a tokenizer pack: " + why);
  if (model != nullptr) {
    expect(model->tokenizer.mode == secure_frontend::TokenizerMode::spm_bpe &&
               model->tokenizer.tokens_by_id == vocabulary.tokens_by_id && model->tokenizer.merges_by_rank == vocabulary.merges_by_rank,
           "the tokenizer is the GGUF's, in the profile's mode");
    expect(model->model_id == "tiny-gemma-Q8_0", "the served name is the card's");
  }
  // a gemma4 model whose header carries a byte-level tokenizer is not Gemma 4's: refused at the start
  secure_frontend_test::write_file(
      models / "wrong-Q8_0.gguf",
      test_support::gguf_with_tokenizer(test_support::byte_tokenizer(), "gpt2", "gemma4").u32("gemma4.context_length", 32768U).bytes());
  expect(std::holds_alternative<secure_frontend::ComposeFailure>(compose("wrong-Q8_0.gguf")), "a tokenizer not the profile's is refused");
}

// Owner 2026-10-27 ("Qwen 3.6 from the GGUF first"): a Qwen3.6 card built from its GGUF starts with no tokenizer pack -
// the byte-level tokenizer is the GGUF's, in Qwen's mode - and upstream's engine gets the file itself (--native) and
// the draft layer (--mtp 2).
void a_qwen36_card_reads_its_tokenizer_from_the_gguf() {
  const auto root = secure_frontend_test::make_scratch_directory();
  const auto models = root / "models";
  secure_frontend_test::make_private_directory(models);
  const auto vocabulary = test_support::byte_tokenizer();
  secure_frontend_test::write_file(models / "Qwen3.6-tiny-Q8_0.gguf",
                                   test_support::gguf_with_tokenizer(vocabulary, "gpt2", "qwen35moe")
                                       .u32("qwen35moe.context_length", 262144U)
                                       .u32("qwen35moe.nextn_predict_layers", 1U)
                                       .bytes());
  const auto bin = root / "bin";
  secure_frontend_test::make_private_directory(bin);
  const std::filesystem::path fake{STRATA_FAKE_ENGINE_PATH};
  const auto engine = bin / fake.filename();
  std::error_code copy_error;
  std::filesystem::copy_file(fake, engine, copy_error);
  const auto data = secure_frontend::data_folder_layout(root / "data");
  expect(std::holds_alternative<secure_frontend::FirstStartResult>(secure_frontend::ensure_data_folder(data, engine)),
         "a first start's data folder");
  const auto main = secure_frontend::parse_config_text(secure_frontend_test::read_file(data.config_file), data.root);
  if (!std::holds_alternative<secure_frontend::FrontendConfigDraft>(main)) return;
  const auto header = secure_frontend::parse_gguf_metadata(secure_frontend_test::read_file(models / "Qwen3.6-tiny-Q8_0.gguf"));
  const auto made = secure_frontend::card_for_gguf(std::get<secure_frontend::GgufMetadata>(header),
                                                   {.folder = models, .file = "Qwen3.6-tiny-Q8_0.gguf", .engine = engine, .source = "folder"});
  const auto* text = std::get_if<secure_frontend::CardText>(&made);
  expect(text != nullptr, "the card is made");
  if (text == nullptr) return;
  const auto parsed = secure_frontend::parse_model_card(text->json, data.root);
  const auto& card = std::get<secure_frontend::ModelCard>(parsed);
  auto validated = secure_frontend::card_config(std::get<secure_frontend::FrontendConfigDraft>(main), card,
                                                secure_frontend::default_option_values(card));
  std::string why;
  if (const auto* issues = std::get_if<secure_frontend::ConfigErrors>(&validated)) {
    for (const auto& issue : *issues) why += secure_frontend::format_issue(issue) + "; ";
  }
  expect(std::holds_alternative<secure_frontend::ValidatedConfig>(validated), "the card validates: " + why);
  if (!std::holds_alternative<secure_frontend::ValidatedConfig>(validated)) return;
  const auto composed = secure_frontend::compose_model(*std::get<secure_frontend::ValidatedConfig>(validated).shared(), true);
  const auto* model = std::get_if<secure_frontend::ComposedModel>(&composed);
  if (const auto* failure = std::get_if<secure_frontend::ComposeFailure>(&composed)) {
    for (const auto& message : failure->messages) why += message + "; ";
  }
  expect(model != nullptr, "the card composes without a tokenizer pack: " + why);
  if (model == nullptr) return;
  expect(model->tokenizer.mode == secure_frontend::TokenizerMode::byte_bpe && model->tokenizer.tokens_by_id == vocabulary.tokens_by_id &&
             model->tokenizer.merges_by_rank == vocabulary.merges_by_rank,
         "the tokenizer is the GGUF's, in Qwen's mode");
  const auto& argv = model->plan.command.arguments;
  expect(has_pair(argv, "--native", secure_frontend::path_to_utf8(models / "Qwen3.6-tiny-Q8_0.gguf")) && has_pair(argv, "--mtp", "2"),
         "the engine reads the file itself, with the draft layer: " + joined(argv));
}

// GUI-first start-up (spec 2026-10-27 §5): a launcher over the real (fake) engine process with Gemma 4 cards built from
// a GGUF, the calibration folder under its state, and a fake that reports a decode time and is twice as fast with one
// --cpu-threads value (the one the sweep must find).
struct CalibrationFixture final {
  std::filesystem::path root = secure_frontend_test::make_scratch_directory();
  std::filesystem::path models = root / "models";
  std::filesystem::path dump = root / "engine-argv.dump";
  secure_frontend::DataFolder data = secure_frontend::data_folder_layout(root / "data");
  std::string winner;  // the --cpu-threads value that is faster
  std::shared_ptr<std::vector<std::string>> events = std::make_shared<std::vector<std::string>>();  // alerts raised
  std::unique_ptr<secure_frontend::ModelLauncher> launcher;

  // operator_cards: the two cards are the operator's (listed), which never calibrate before a first start; factory:
  // how the engine processes are made (unset: the fake engine's real process)
  explicit CalibrationFixture(const std::vector<std::string>& extra_fake_options = {}, const bool operator_cards = false,
                              std::function<secure_frontend::ProcessFactory(secure_frontend::EngineLaunchPlan)> factory = {}) {
    secure_frontend_test::make_private_directory(models);
    const auto vocabulary = test_support::spm_tokenizer();
    for (const char* name : {"tiny-gemma-Q8_0.gguf", "other-gemma-Q8_0.gguf"}) {
      secure_frontend_test::write_file(models / name, test_support::gguf_with_tokenizer(vocabulary, "gemma4", "gemma4")
                                                          .u32("gemma4.context_length", 32768U)
                                                          .bytes());
    }
    const auto bin = root / "bin";
    secure_frontend_test::make_private_directory(bin);
    const std::filesystem::path fake{STRATA_FAKE_ENGINE_PATH};
    const auto engine = bin / fake.filename();
    std::error_code ec;
    std::filesystem::copy_file(fake, engine, ec);
    const auto plan = secure_frontend::sweep_plan(secure_frontend::EngineProfile::gemma4,
                                                  {std::max(1U, std::thread::hardware_concurrency()), secure_frontend::physical_cores()});
    winner = plan.back().values.back();
    std::string fake_options = "--tokens=8\n--token-ids=" + std::to_string(test_support::token_id(vocabulary, "a")) +
                               "\n--decode-ms=1000\n--faster-with=--cpu-threads:" + winner + ":2\n--dump-file=" +
                               secure_frontend::path_to_utf8(dump) + "\n";
    for (const auto& option : extra_fake_options) fake_options += option + "\n";
    secure_frontend_test::write_file(bin / "fake-engine.options", fake_options);
    (void)secure_frontend::ensure_data_folder(data, engine);
    const auto main = std::get<secure_frontend::FrontendConfigDraft>(
        secure_frontend::parse_config_text(secure_frontend_test::read_file(data.config_file), data.root));
    std::vector<secure_frontend::ModelCard> cards;
    for (const char* file : {"tiny-gemma-Q8_0.gguf", "other-gemma-Q8_0.gguf"}) {
      const auto header = secure_frontend::parse_gguf_metadata(secure_frontend_test::read_file(models / file));
      const auto made = secure_frontend::card_for_gguf(std::get<secure_frontend::GgufMetadata>(header),
                                                       {.folder = models, .file = file, .engine = engine, .source = "folder"});
      auto parsed = secure_frontend::parse_model_card(std::get<secure_frontend::CardText>(made).json, data.root);
      cards.push_back(std::get<secure_frontend::ModelCard>(std::move(parsed)));
    }
    secure_frontend::LauncherSettings settings{.main = main, .cards = operator_cards ? cards : std::vector<secure_frontend::ModelCard>{},
                                               .operator_cards = operator_cards ? cards.size() : 0U, .memory_file = {}, .gate = {}};
    settings.calibration_dir = data.state / "calibration";
    settings.log = [](std::string_view) {};
    if (factory) settings.factory = std::move(factory);
    settings.on_event = [list = events](const std::string_view event, const std::string& subject, const std::string&) {
      list->push_back(std::string{event} + ": " + subject);
    };
    launcher = std::make_unique<secure_frontend::ModelLauncher>(std::move(settings));
    if (!operator_cards) {
      for (auto& card : cards) (void)launcher->add_card(std::move(card));
    }
  }
  ~CalibrationFixture() { launcher->shutdown(); }
  CalibrationFixture(const CalibrationFixture&) = delete;
  CalibrationFixture& operator=(const CalibrationFixture&) = delete;
  [[nodiscard]] std::filesystem::path result_file(const std::string& card) const { return data.state / "calibration" / (card + ".json"); }
};

secure_frontend::JsonValue json_of(const std::string_view text) {
  return std::get<secure_frontend::JsonValue>(secure_frontend::parse_json(
      text, {.max_bytes = 4096U, .max_depth = 4U, .max_container_entries = 16U, .max_string_bytes = 256U, .allow_fractions = true}));
}

// §5.1-§5.2: the sweep over the engine's own starts finds the faster --cpu-threads, writes it with its key, and a later
// start at the same options carries it; at other options it is left off ("settings changed").
void calibration_finds_the_win_and_starts_with_it() {
  CalibrationFixture f;
  const std::string name = "tiny-gemma-Q8_0";
  expect(!f.launcher->calibrate(name, nullptr, false, std::nullopt, false).has_value(), "a calibration is accepted");
  expect(f.launcher->wait_until_settled(std::chrono::seconds(120)), "it ends");
  expect(f.launcher->state_of(name) == secure_frontend::CardState::stopped, "the card is stopped after it");
  const auto stored = secure_frontend::parse_calibration(secure_frontend_test::read_file(f.result_file(name)));
  expect(stored.has_value() && stored->args == std::vector<std::string>{"--cpu-threads", f.winner} &&
             stored->best_tok_s > stored->baseline_tok_s * 1.5,
         "the faster --cpu-threads " + f.winner + " is kept: " + (stored ? joined(stored->args) : std::string{"(no result)"}));
  const std::string listed = f.launcher->list_json();
  expect(listed.find("\"calibration\":{\"state\":\"fresh\",\"first_start\":true") != std::string::npos &&
             listed.find("\"args\":[\"--cpu-threads\",\"" + f.winner + "\"]") != std::string::npos,
         "the list shows the calibration: " + listed);
  expect(f.events->size() == 1U && f.events->front() == "calibration_finished: " + name + " is calibrated",
         "an alert says the calibration finished: " + (f.events->empty() ? std::string{"(none)"} : f.events->front()));

  expect(!f.launcher->start(name, nullptr, false).has_value() && f.launcher->wait_until_settled(std::chrono::seconds(60)) &&
             f.launcher->state_of(name) == secure_frontend::CardState::running,
         "the card starts");
  expect(has_pair(dumped_argv(f.dump), "--cpu-threads", f.winner), "with the kept flag: " + joined(dumped_argv(f.dump)));
  (void)f.launcher->stop(false);
  expect(f.launcher->wait_until_settled(std::chrono::seconds(60)), "it stops");

  const auto other = json_of(R"({"max_context": 8192})");
  expect(!f.launcher->start(name, &other, false).has_value() && f.launcher->wait_until_settled(std::chrono::seconds(60)) &&
             f.launcher->state_of(name) == secure_frontend::CardState::running,
         "a start at other options");
  const auto argv = dumped_argv(f.dump);
  expect(std::find(argv.begin(), argv.end(), "--cpu-threads") == argv.end(), "leaves the flag off: " + joined(argv));
  expect(f.launcher->list_json().find("\"calibration\":{\"state\":\"settings_changed\",") != std::string::npos,
         "and the card says its settings changed: " + f.launcher->list_json());
}

// §5.2: a card the page made that was never calibrated calibrates on its first start, then serves with the result.
void a_first_start_calibrates_first() {
  CalibrationFixture f;
  const std::string name = "other-gemma-Q8_0";
  expect(!f.launcher->start(name, nullptr, false).has_value(), "the start is accepted");
  expect(f.launcher->wait_until_settled(std::chrono::seconds(120)), "it ends");
  expect(f.launcher->state_of(name) == secure_frontend::CardState::running, "the card runs");
  expect(std::filesystem::exists(f.result_file(name)), "it was calibrated first");
  expect(has_pair(dumped_argv(f.dump), "--cpu-threads", f.winner), "and runs with the result: " + joined(dumped_argv(f.dump)));
}

// §5a: alerts - a calibration whose engine never starts raises calibration_failed; a start of a card that does not
// calibrate first (an operator card) whose engine ends before it is ready raises engine_stopped.
void failures_raise_alerts() {
  CalibrationFixture f({"--exit-before-ready=3"}, true);
  const std::string name = "tiny-gemma-Q8_0";
  expect(!f.launcher->calibrate(name, nullptr, false, std::nullopt, false).has_value(), "a calibration is accepted");
  expect(f.launcher->wait_until_settled(std::chrono::seconds(60)), "it ends");
  expect(f.launcher->state_of(name) == secure_frontend::CardState::failed, "the card failed");
  expect(f.events->size() == 1U && f.events->back().rfind("calibration_failed: ", 0) == 0U,
         "an alert says the calibration failed: " + (f.events->empty() ? std::string{"(none)"} : f.events->back()));
  expect(!f.launcher->start("other-gemma-Q8_0", nullptr, false).has_value() && f.launcher->wait_until_settled(std::chrono::seconds(60)),
         "a start of an operator card");
  expect(f.events->size() == 2U && f.events->back().rfind("engine_stopped: other-gemma-Q8_0", 0) == 0U,
         "whose engine ends before it is ready raises engine_stopped: " + (f.events->empty() ? std::string{"(none)"} : f.events->back()));
}

// §5.3: Cancel stops the sweep (the engine it started is stopped) and keeps nothing.
void a_calibration_can_be_cancelled() {
  CalibrationFixture f({"--ready-delay-ms=400"});
  const std::string name = "tiny-gemma-Q8_0";
  expect(!f.launcher->calibrate(name, nullptr, false, std::nullopt, false).has_value(), "accepted");
  for (int i = 0; i < 100 && f.launcher->state_of(name) != secure_frontend::CardState::calibrating; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  expect(f.launcher->state_of(name) == secure_frontend::CardState::calibrating, "the card is calibrating");
  for (int i = 0; i < 100 && f.launcher->list_json().find("\"running\":{\"stage\":") == std::string::npos; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  expect(f.launcher->list_json().find("\"running\":{\"stage\":") != std::string::npos, "with its progress in the list");
  expect(!f.launcher->cancel_calibration().has_value(), "a cancel is accepted");
  expect(f.launcher->wait_until_settled(std::chrono::seconds(60)), "it ends");
  expect(f.launcher->state_of(name) == secure_frontend::CardState::stopped && !std::filesystem::exists(f.result_file(name)),
         "stopped, nothing kept");
  expect(f.launcher->cancel_calibration().has_value(), "nothing left to cancel");
}

// Owner 2026-10-28 ("when I downloaded minimax and deepseek, I did not see the system calibrate when I ran them"): a
// MiniMax card (a maya-mm3 pack: its marker and a tokenizer with MiniMax's literals) on the fake engine in Maya's
// dialect, which logs each --calibrate-prefill run's command line; the out folder's LICENSE beside the engine's folder.
struct MinimaxFixture final {
  std::filesystem::path root = secure_frontend_test::make_scratch_directory();
  std::filesystem::path dump = root / "engine-argv.dump";
  std::filesystem::path prefill_log = root / "prefill.log";
  secure_frontend::DataFolder data = secure_frontend::data_folder_layout(root / "data");
  std::unique_ptr<secure_frontend::ModelLauncher> launcher;
  const std::string name = "MiniMax-M3.1";

  explicit MinimaxFixture(const std::vector<std::string>& extra_fake_options = {}) {
    const auto pack = root / "packs" / "minimax";
    std::filesystem::create_directories(pack / "tokenizer");
    (void)secure_frontend::write_pack_marker(pack, secure_frontend::kMinimaxModelName);
    auto vocabulary = test_support::byte_tokenizer();
    for (const auto& [literal, type] : std::vector<std::pair<std::string, int>>{
             {"]~!b[", 3}, {"]~b]", 3}, {"[e~[", 3}, {"<mm:think>", 4}, {"</mm:think>", 4}}) {
      vocabulary.tokens_by_id.push_back(literal);
      vocabulary.token_types.push_back(type);
    }
    for (const auto& [file, bytes] : test_support::tokenizer_pack_files(vocabulary)) secure_frontend_test::write_file(pack / "tokenizer" / file, bytes);
    std::string text;
    for (int i = 0; i < 400; ++i) text += "Everyone is permitted to copy and distribute verbatim copies of this license document. ";
    secure_frontend_test::write_file(root / "LICENSE", text);
    const auto bin = root / "bin";
    secure_frontend_test::make_private_directory(bin);
    const std::filesystem::path fake{STRATA_FAKE_ENGINE_PATH};
    const auto engine = bin / fake.filename();
    std::error_code ec;
    std::filesystem::copy_file(fake, engine, ec);
    std::string fake_options = "--dialect=maya\n--tokens=8\n--token-ids=" + std::to_string(test_support::token_id(vocabulary, "a")) +
                               "\n--dump-file=" + secure_frontend::path_to_utf8(dump) + "\n--prefill-log=" +
                               secure_frontend::path_to_utf8(prefill_log) + "\n";
    for (const auto& option : extra_fake_options) fake_options += option + "\n";
    secure_frontend_test::write_file(bin / "fake-engine.options", fake_options);
    (void)secure_frontend::ensure_data_folder(data, engine);
    const auto main = std::get<secure_frontend::FrontendConfigDraft>(
        secure_frontend::parse_config_text(secure_frontend_test::read_file(data.config_file), data.root));
    const auto made = secure_frontend::card_for_pack(
        {.family = secure_frontend::PackedFamily::minimax, .name = name, .engine = engine, .source = "download:mm", .pack = pack});
    auto parsed = secure_frontend::parse_model_card(made.json, data.root);
    secure_frontend::LauncherSettings settings{.main = main, .cards = {}, .operator_cards = 0U, .memory_file = {}, .gate = {}};
    settings.calibration_dir = data.state / "calibration";
    settings.log = [](std::string_view) {};
    launcher = std::make_unique<secure_frontend::ModelLauncher>(std::move(settings));
    expect(std::holds_alternative<secure_frontend::ModelCard>(parsed) && launcher->add_card(std::get<secure_frontend::ModelCard>(std::move(parsed))),
           "the MiniMax card is added");
  }
  ~MinimaxFixture() { launcher->shutdown(); }
  MinimaxFixture(const MinimaxFixture&) = delete;
  MinimaxFixture& operator=(const MinimaxFixture&) = delete;
  [[nodiscard]] std::filesystem::path calibration(const std::string& file) const { return data.state / "calibration" / file; }
  [[nodiscard]] std::vector<std::string> prefill_runs() const {
    std::vector<std::string> lines;
    std::istringstream in(std::filesystem::exists(prefill_log) ? secure_frontend_test::read_file(prefill_log) : std::string{});
    for (std::string line; std::getline(in, line);) {
      if (!line.empty()) lines.push_back(line);
    }
    return lines;
  }
  [[nodiscard]] bool start_and_stop() {
    const bool started = !launcher->start(name, nullptr, false).has_value() && launcher->wait_until_settled(std::chrono::seconds(120)) &&
                         launcher->state_of(name) == secure_frontend::CardState::running;
    (void)launcher->stop(false);
    return launcher->wait_until_settled(std::chrono::seconds(60)) && started;
  }
};

// Maya's start of a MiniMax config without a prefill calibration (needs_prefill_calibration) measures it once: the
// engine's own --calibrate-prefill over the out folder's LICENSE as token ids, no --serve; the record is kept, the card
// then runs, and a later start does not measure again. The thread sweep is not run unasked (a large model's).
void a_minimax_card_measures_its_prompt_reading_first() {
  MinimaxFixture f;
  expect(f.start_and_stop(), "the first start ends with the card running");
  const auto runs = f.prefill_runs();
  const std::string out = secure_frontend::path_to_utf8(f.calibration(f.name + ".prefill-out.json"));
  const std::string ids = secure_frontend::path_to_utf8(f.calibration(f.name + ".prefill-ids.txt"));
  expect(runs.size() == 1U && runs[0].find("--calibrate-prefill " + out) != std::string::npos &&
             runs[0].find("--ids @" + ids) != std::string::npos && runs[0].find("--serve") == std::string::npos,
         "one run of the engine's own measurement, over the text's tokens, not serving: " + (runs.empty() ? std::string{} : runs[0]));
  const auto record = secure_frontend::parse_prefill(secure_frontend_test::read_file(f.calibration(f.name + ".prefill.json")));
  expect(record.has_value() && record->finished && record->chunk == 2048 && record->gemm == "blas" && record->tok_s == 950.5,
         "the engine's choice is recorded");
  expect(!std::filesystem::exists(f.calibration(f.name + ".prefill-out.json")) && !std::filesystem::exists(f.calibration(f.name + ".prefill-ids.txt")),
         "its scratch files are gone");
  expect(!std::filesystem::exists(f.calibration(f.name + ".json")), "no thread sweep ran unasked");
  const auto argv = dumped_argv(f.dump);
  expect(std::find(argv.begin(), argv.end(), "--calibrate-prefill") == argv.end(), "the start itself serves: " + joined(argv));
  expect(f.launcher->list_json().find("\"calibration\":{\"state\":\"missing\",\"first_start\":false") != std::string::npos,
         "a large model's card does not calibrate before its first start: " + f.launcher->list_json());
  expect(f.launcher->list_json().find("\"prefill\":{\"state\":\"done\",\"chunk\":2048,\"gemm\":\"blas\"") != std::string::npos,
         "the list shows it: " + f.launcher->list_json());
  expect(f.start_and_stop() && f.prefill_runs().size() == 1U, "a later start does not measure again");
}

// An engine that stops before it finishes, at its largest chunk and again at the default one: the start goes on (the
// engine's defaults), the record says so, and only Calibrate measures again (Maya: "a failed one is not tried again by
// itself").
void a_failed_prefill_calibration_is_not_retried_by_itself() {
  MinimaxFixture f({"--prefill-fail"});
  expect(f.start_and_stop(), "the card still runs");
  const auto record = secure_frontend::parse_prefill(secure_frontend_test::read_file(f.calibration(f.name + ".prefill.json")));
  expect(record.has_value() && !record->finished, "the record says it did not finish");
  expect(f.launcher->list_json().find("\"prefill\":{\"state\":\"failed\"") != std::string::npos, "the list shows it");
  expect(f.prefill_runs().size() == 2U, "measured at the largest chunk, then at the default one: " + std::to_string(f.prefill_runs().size()));
  expect(f.start_and_stop() && f.prefill_runs().size() == 2U, "the next start does not try again: " + std::to_string(f.prefill_runs().size()));
}

// A GPU without the memory for the measurement's largest chunk (16 GB with MiniMax-M3.1: the engine refuses and advises a
// smaller --chunk): measured again at the default chunk, which it can run - the GEMMs and this PC's prompt speed.
void a_prefill_calibration_falls_back_to_the_default_chunk() {
  MinimaxFixture f({"--prefill-fail-above=1024"});
  expect(f.start_and_stop(), "the card runs");
  const auto runs = f.prefill_runs();
  expect(runs.size() == 2U && runs[0].find("--chunk") == std::string::npos && runs[1].find("--chunk 1024") != std::string::npos,
         "the second run at the default chunk: " + (runs.size() > 1U ? runs[1] : std::string{}));
  const auto record = secure_frontend::parse_prefill(secure_frontend_test::read_file(f.calibration(f.name + ".prefill.json")));
  expect(record.has_value() && record->finished && record->tok_s == 950.5, "and it finished");
}

// Owner 2026-10-28 (the "unreaped engine" after a model stop): a big model took longer to end after a stop than the
// server's 5 s bound, so it was left unreaped and the next model could start beside it. A stop now waits for an engine
// still ending after the kill (EngineSupervisor::stop_engine): the card is stopped with no engine left behind. The
// server's own shutdown during such a stop cuts that wait and keeps its bound.
void a_stop_waits_for_an_engine_still_ending() {
  test_support::FakeFactory fakes([](test_support::FakeChild& child, std::uint64_t) {
    child.reapable = false;  // EOF and the kill land; it ends once the test frees it, like a model freeing its memory
    child.push_line("READY 32768 stop");
  });
  CalibrationFixture f({}, true, [fakes](secure_frontend::EngineLaunchPlan) { return fakes.factory(); });
  const std::string name = "tiny-gemma-Q8_0";
  const auto start = [&f, &name] {
    return !f.launcher->start(name, nullptr, false).has_value() && f.launcher->wait_until_settled(std::chrono::seconds(30)) &&
           f.launcher->state_of(name) == secure_frontend::CardState::running;
  };
  expect(start(), "the card starts");
  // freed 5 s after the stop: past its EOF, TERM and kill waits (2 + 1 + 1 s), where it used to be left
  std::thread freed([child = fakes.child(1U)] {
    std::this_thread::sleep_for(std::chrono::seconds(5));
    if (!child) return;
    std::lock_guard lock(child->mutex);
    child->reapable = true;
  });
  (void)f.launcher->stop(false);
  expect(f.launcher->wait_until_settled(std::chrono::seconds(30)), "it stops");
  freed.join();
  expect(f.launcher->state_of(name) == secure_frontend::CardState::stopped && !f.launcher->engine_left_unreaped(),
         "the stop waited for the engine: none is left");

  expect(start(), "the card starts again");
  (void)f.launcher->stop(false);
  std::this_thread::sleep_for(std::chrono::milliseconds(4500));  // into the wait after the kill; this one never ends
  const auto asked = std::chrono::steady_clock::now();
  f.launcher->shutdown();
  expect(std::chrono::steady_clock::now() - asked < std::chrono::seconds(3), "the server's shutdown cuts the wait");
  expect(f.launcher->engine_left_unreaped(), "and reports the engine left behind");
}

// §4.5: a page card removed (the danger zone's delete) takes its calibration and its switch with it, so a later card
// of the same name starts uncalibrated.
void removing_a_card_removes_its_calibration() {
  CalibrationFixture f;
  const std::string name = "tiny-gemma-Q8_0";
  expect(!f.launcher->calibrate(name, nullptr, false, true, false).has_value() &&
             f.launcher->wait_until_settled(std::chrono::seconds(120)) && std::filesystem::exists(f.result_file(name)),
         "the card is calibrated");
  expect(!f.launcher->set_auto_recalibrate(name, true).has_value(), "its switch is on");
  const auto held = f.launcher->card(name);
  expect(held.has_value() && f.launcher->remove_card(name) == secure_frontend::CardChange::done, "the card is removed");
  expect(!std::filesystem::exists(f.result_file(name)), "and its calibration with it");
  expect(held.has_value() && f.launcher->add_card(held->card), "the name is added again");
  const std::string listed = f.launcher->list_json();
  expect(listed.find("\"calibration\":{\"state\":\"missing\",") != std::string::npos &&
             listed.find("\"auto\":true") == std::string::npos,
         "uncalibrated, without the old switch: " + listed);
}

// GUI-first start-up (spec 2026-10-27 §4.1, §4.4, §4.5): the cards the server wrote (added while serving, or loaded
// from <roots.state>/cards) can be replaced and removed; the operator's listed cards stay read-only.
void page_cards_can_be_replaced_and_removed() {
  using secure_frontend::CardChange;
  secure_frontend::ModelCard alpha;
  alpha.name = "Alpha";
  secure_frontend::ModelCard loaded;
  loaded.name = "Loaded";
  loaded.source = "download:loaded";
  secure_frontend::LauncherSettings settings;
  settings.cards = {alpha, loaded};
  settings.operator_cards = 1U;   // Alpha is the operator's (models.cards); Loaded the server wrote
  settings.log = [](std::string_view) {};
  secure_frontend::ModelLauncher launcher(std::move(settings));
  expect(launcher.replace_card(alpha) == CardChange::not_page_card && launcher.remove_card("Alpha") == CardChange::not_page_card,
         "an operator card is read-only");
  const auto held_alpha = launcher.card("Alpha");
  expect(held_alpha && !held_alpha->page_card, "and says so");
  const auto held_loaded = launcher.card("Loaded");
  expect(held_loaded && held_loaded->page_card && held_loaded->card.source == "download:loaded" &&
             held_loaded->state == secure_frontend::CardState::stopped,
         "a card the server wrote is the page's");

  secure_frontend::ModelCard gamma;
  gamma.name = "Gamma";
  gamma.profile = secure_frontend::EngineProfile::flint;
  gamma.source = "folder";
  gamma.config.entries["roots.models"] = secure_frontend::ConfigDraftEntry{std::string{"C:/models/a"}, secure_frontend::ConfigSource::file};
  gamma.config.entries["engine.model_id"] = secure_frontend::ConfigDraftEntry{std::string{"gamma-Q8_0"}, secure_frontend::ConfigSource::file};
  expect(launcher.add_card(gamma), "a page card is added");
  const std::string listed = launcher.list_json();
  expect(listed.find(R"("name":"Gamma","profile":"flint")") != std::string::npos &&
             listed.find(R"("page_card":true,"source":"folder","folder":"C:/models/a","file":"gamma-Q8_0")") != std::string::npos,
         "the list shows a page card's source, folder and file: " + listed);
  expect(listed.find(R"("name":"Alpha","profile":"qwen","state":"stopped","error":null,"page_card":false,"options")") != std::string::npos,
         "an operator card shows no folder: " + listed);

  gamma.config.entries["roots.models"] = secure_frontend::ConfigDraftEntry{std::string{"D:/moved"}, secure_frontend::ConfigSource::file};
  expect(launcher.replace_card(gamma) == CardChange::done, "a page card is replaced");
  const auto replaced = launcher.card("Gamma");
  expect(replaced && std::get<std::string>(replaced->card.config.entries.at("roots.models").value) == "D:/moved" && replaced->page_card,
         "the new folder is held");
  // every card held, in order, as the Download list reads them (owner 2026-10-28: what is already on this PC)
  const auto every = launcher.cards();
  expect(every.size() == 3U && every[0].name == "Alpha" && every[1].name == "Loaded" && every[1].source == "download:loaded" &&
             every[2].name == "Gamma" && std::get<std::string>(every[2].config.entries.at("roots.models").value) == "D:/moved",
         "the cards held, in order, with their sources and folders");
  secure_frontend::ModelCard nobody;
  nobody.name = "Nobody";
  expect(launcher.replace_card(nobody) == CardChange::not_found && launcher.remove_card("Nobody") == CardChange::not_found,
         "an unknown card is not found");

  expect(launcher.remove_card("Gamma") == CardChange::done, "a page card is removed");
  expect(!launcher.card("Gamma") && launcher.list_json().find("\"Gamma\"") == std::string::npos &&
             launcher.served().extension.find("\"Gamma\"") == std::string::npos,
         "it is gone from the lists");
  expect(launcher.remove_card("Gamma") == CardChange::not_found, "a second remove finds nothing");
  expect(launcher.add_card(gamma), "the name can be used again");
  expect(launcher.card("Loaded") && launcher.card("Alpha"), "the other cards are untouched");
  launcher.shutdown();
}

// Owner 2026-10-27 (item 46's fix): each card in the list says whether its profile takes a request's
// reasoning_effort (per_request_reasoning_effort_for, the chat schema's own rule), so the page sends the field only
// where it is accepted - a GLM card's profile refuses it (400 unsupported_parameter); raon1, with no options, takes it.
void cards_say_whether_their_profile_takes_reasoning_effort() {
  secure_frontend::LauncherSettings settings;
  for (const auto& [name, profile] : std::vector<std::pair<std::string, secure_frontend::EngineProfile>>{
           {"Q", secure_frontend::EngineProfile::qwen}, {"G", secure_frontend::EngineProfile::glm53},
           {"R", secure_frontend::EngineProfile::raon1}, {"E", secure_frontend::EngineProfile::gemma4}}) {
    secure_frontend::ModelCard card;
    card.name = name;
    card.profile = profile;
    settings.cards.push_back(card);
  }
  settings.log = [](std::string_view) {};
  secure_frontend::ModelLauncher launcher(std::move(settings));
  const std::string listed = launcher.list_json();
  const auto flag_of = [&listed](const std::string& name) -> std::string {
    const auto at = listed.find("{\"name\":\"" + name + "\"");
    const auto flag = at == std::string::npos ? std::string::npos : listed.find("\"reasoning_effort\":", at);
    if (flag == std::string::npos) return "absent";
    const auto from = flag + std::string{"\"reasoning_effort\":"}.size();
    return listed.substr(from, listed.find_first_of(",}", from) - from);
  };
  expect(flag_of("Q") == "true" && flag_of("R") == "true" && flag_of("E") == "true", "qwen, raon1 and gemma4 take it: " + listed);
  expect(flag_of("G") == "false", "the GLM card does not: " + listed);
  launcher.shutdown();
}

// Owner 2026-10-28 ("a toggle in the card that can enable search"): with the server's web search on, every card whose
// profile has the tool (qwen, glm53, raon1, flint, gemma4, deepseek, minimax: every profile) offers a Web search checkbox, on
// by default, even when its card file lists no options. Without the server's search no card shows it.
void cards_offer_web_search_when_the_server_has_it() {
  struct NoTool final : secure_frontend::ToolRunner {
    std::optional<secure_frontend::ToolOffer> offer() override { return std::nullopt; }
    secure_frontend::ToolResult run(const secure_frontend::ToolCall&, const std::function<bool()>&) override { return {}; }
  } tool;
  const auto listing = [](secure_frontend::ToolRunner* tools) {
    secure_frontend::LauncherSettings settings;
    for (const auto& [name, profile] : std::vector<std::pair<std::string, secure_frontend::EngineProfile>>{
             {"Q", secure_frontend::EngineProfile::qwen}, {"G", secure_frontend::EngineProfile::glm53},
             {"R", secure_frontend::EngineProfile::raon1}, {"F", secure_frontend::EngineProfile::flint},
             {"E", secure_frontend::EngineProfile::gemma4}, {"D", secure_frontend::EngineProfile::deepseek},
             {"M", secure_frontend::EngineProfile::minimax}}) {
      secure_frontend::ModelCard card;
      card.name = name;
      card.profile = profile;
      settings.cards.push_back(card);
    }
    settings.tools = tools;
    settings.log = [](std::string_view) {};
    secure_frontend::ModelLauncher launcher(std::move(settings));
    std::string listed = launcher.list_json();
    launcher.shutdown();
    return listed;
  };
  const auto options_of = [](const std::string& listed, const std::string& name) {
    const auto at = listed.find("{\"name\":\"" + name + "\",\"profile\"");
    const auto from = at == std::string::npos ? std::string::npos : listed.find("\"options\":", at);
    return from == std::string::npos ? std::string{} : listed.substr(from, listed.find("\"calibration\":", from) - from);
  };
  const std::string on = listing(&tool);
  for (const char* name : {"Q", "G", "R", "F", "E", "D", "M"}) {
    expect(options_of(on, name).find(R"({"name":"web_search","kind":"boolean","label":"Web search",)") != std::string::npos &&
               options_of(on, name).find(R"("default":true,"value":true})") != std::string::npos,
           std::string{name} + " offers Web search, on: " + options_of(on, name));
  }
  const std::string off = listing(nullptr);
  expect(off.find("web_search") == std::string::npos, "no server search, no checkbox: " + off);
  // Multi-GPU like upstream (owner 2026-10-28): every card offers its GPUs choice, auto unless its file says otherwise -
  // up to 16 GPUs, one on the single-GPU engines (flint, Gemma 4) - after Web search, the tables' order
  for (const char* name : {"Q", "G", "R", "F", "E", "D"}) {
    const bool one = std::string_view{name} == "F" || std::string_view{name} == "E";
    const std::string options = options_of(on, name);
    expect(options.find(R"({"name":"gpus","kind":"devices","label":"GPUs",)") != std::string::npos &&
               options.find(std::string{R"("default":"auto","max":)"} + (one ? "1" : "16") + R"(,"value":"auto"})") != std::string::npos &&
               (options.find("web_search") == std::string::npos || options.find("web_search") < options.find("\"gpus\"")),
           std::string{name} + " offers GPUs, auto: " + options);
  }
}

// Owner 2026-10-29 ("make sure the card matches the chat settings when the engine is started as default so each model
// reset it to the recommended"): a card whose file lists no sampling offers its model's recommended sampling (upstream's
// MODEL_CARD_SAMPLING, the shipped cards) in the profile's order; a value its file lists, or its config's
// defaults.sampling row, stays the card's own.
void cards_offer_their_models_recommended_sampling() {
  secure_frontend::LauncherSettings settings;
  for (const auto& [name, profile] : std::vector<std::pair<std::string, secure_frontend::EngineProfile>>{
           {"Q", secure_frontend::EngineProfile::qwen}, {"G", secure_frontend::EngineProfile::glm53},
           {"R", secure_frontend::EngineProfile::raon1}, {"F", secure_frontend::EngineProfile::flint},
           {"E", secure_frontend::EngineProfile::gemma4}, {"D", secure_frontend::EngineProfile::deepseek},
           {"M", secure_frontend::EngineProfile::minimax}}) {
    secure_frontend::ModelCard card;
    card.name = name;
    card.profile = profile;
    settings.cards.push_back(card);
  }
  const auto options_from = [](const secure_frontend::EngineProfile profile, const std::string_view json) {
    const auto parsed = secure_frontend::parse_json(json, {.max_bytes = 4096U, .max_depth = 8U, .max_container_entries = 64U,
                                                           .max_string_bytes = 256U, .allow_fractions = true});
    const auto limits = secure_frontend::parse_card_options(profile, std::get<secure_frontend::JsonValue>(parsed));
    return std::get<std::vector<secure_frontend::ModelOptionLimits>>(limits);
  };
  secure_frontend::ModelCard own;   // its own temperature, between its thinking and its Pictures
  own.name = "Own";
  own.profile = secure_frontend::EngineProfile::qwen;
  own.options = options_from(own.profile, R"({"thinking":{"default":"high"},"temperature":{"default":0.6},"vision":{"default":"gpu"}})");
  settings.cards.push_back(own);
  secure_frontend::ModelCard rows;  // a hand-written raon1 card's config rows (engine/fe-config/cards/raon1.json)
  rows.name = "Rows";
  rows.profile = secure_frontend::EngineProfile::raon1;
  rows.config.entries["defaults.sampling.top_k"] = {std::int64_t{40}, secure_frontend::ConfigSource::file};
  rows.config.entries["defaults.sampling.temperature"] = {0.8, secure_frontend::ConfigSource::file};
  settings.cards.push_back(rows);
  settings.log = [](std::string_view) {};
  secure_frontend::ModelLauncher launcher(std::move(settings));
  const std::string listed = launcher.list_json();
  launcher.shutdown();
  // the card's options as "name=default" in their listed order
  const auto defaults_of = [&listed](const std::string& name) {
    const auto at = listed.find("{\"name\":\"" + name + "\",\"profile\"");
    const auto from = at == std::string::npos ? std::string::npos : listed.find("\"options\":", at);
    const std::string options = from == std::string::npos ? std::string{} : listed.substr(from, listed.find("\"calibration\":", from) - from);
    std::string out;
    for (std::size_t pos = options.find("{\"name\":\""); pos != std::string::npos; pos = options.find("{\"name\":\"", pos + 1U)) {
      const auto name_end = options.find('"', pos + 9U);
      const auto value = options.find("\"default\":", name_end);
      const auto value_end = options.find_first_of(",}", value);
      out += (out.empty() ? "" : " ") + options.substr(pos + 9U, name_end - pos - 9U) + "=" + options.substr(value + 10U, value_end - value - 10U);
    }
    return out;
  };
  expect(defaults_of("Q") == "temperature=1 top_p=0.95 top_k=20 answer_minutes=30 gpus=\"auto\"", "Qwen3.8 / 3.6 (upstream's card): " + defaults_of("Q"));
  expect(defaults_of("G") == "ram_headroom=16 temperature=1 top_p=0.95 answer_minutes=30 gpus=\"auto\"", "GLM-5.3 (Maya's): " + defaults_of("G"));
  expect(defaults_of("R") == "ram_headroom=16 temperature=1 top_p=0.95 top_k=20 answer_minutes=30 gpus=\"auto\"", "raon1 (KRAFTON's): " + defaults_of("R"));
  expect(defaults_of("F") == "temperature=1 top_p=0.95 top_k=20 answer_minutes=30 gpus=\"auto\"", "flint (its card's): " + defaults_of("F"));
  expect(defaults_of("E") == "temperature=1 top_p=0.95 top_k=64 min_p=0.05 answer_minutes=30 gpus=\"auto\"", "Gemma 4 (Google's): " + defaults_of("E"));
  expect(defaults_of("D") == "temperature=1 top_p=1 answer_minutes=30 gpus=\"auto\"", "DeepSeek-V4.1 (upstream's card): " + defaults_of("D"));
  expect(defaults_of("M") == "temperature=1 top_p=0.95 answer_minutes=30 gpus=\"auto\"", "MiniMax (its card's): " + defaults_of("M"));
  expect(defaults_of("Own") == "thinking=\"high\" temperature=0.6 top_p=0.95 top_k=20 vision=\"gpu\" answer_minutes=30 gpus=\"auto\"",
         "a card's own temperature stays, the rest in the profile's order: " + defaults_of("Own"));
  expect(defaults_of("Rows") == "ram_headroom=16 temperature=0.8 top_p=0.95 top_k=40 answer_minutes=30 gpus=\"auto\"",
         "a card's own defaults.sampling rows are its defaults: " + defaults_of("Rows"));
}

// Owner 2026-10-29 ("make it an option in the card setting"): every GLM-engine card (glm53, raon1) offers the RAM its
// engine leaves free, in the profile's order (after its cache), at the server's own glm.ram_headroom_gb (16 unless the
// config moves it) unless its file lists it; no other card offers it.
void glm_cards_offer_their_ram_headroom() {
  const auto listed = [](const std::optional<std::int64_t> configured) {
    secure_frontend::LauncherSettings settings;
    if (configured) settings.main.entries["glm.ram_headroom_gb"] = {*configured, secure_frontend::ConfigSource::file};
    for (const auto& [name, profile] : std::vector<std::pair<std::string, secure_frontend::EngineProfile>>{
             {"G", secure_frontend::EngineProfile::glm53}, {"R", secure_frontend::EngineProfile::raon1},
             {"D", secure_frontend::EngineProfile::deepseek}, {"M", secure_frontend::EngineProfile::minimax}}) {
      secure_frontend::ModelCard card;
      card.name = name;
      card.profile = profile;
      settings.cards.push_back(card);
    }
    settings.log = [](std::string_view) {};
    secure_frontend::ModelLauncher launcher(std::move(settings));
    std::string text = launcher.list_json();
    launcher.shutdown();
    return text;
  };
  const auto option_of = [](const std::string& text, const std::string& card) {
    const auto at = text.find("{\"name\":\"" + card + "\",\"profile\"");
    const auto end = text.find("\"calibration\":", at);
    const auto option = text.find("{\"name\":\"ram_headroom\"", at);
    return at == std::string::npos || option == std::string::npos || option > end ? std::string{} : text.substr(option, text.find('}', option) - option);
  };
  const std::string plain = listed(std::nullopt);
  for (const char* card : {"G", "R"}) {
    const std::string option = option_of(plain, card);
    expect(option.find("\"kind\":\"integer\"") != std::string::npos && option.find("\"default\":16") != std::string::npos &&
               option.find("\"choices\":[6,8,12,16,24,32,48,64]") != std::string::npos && option.find("\"value\":16") != std::string::npos,
           std::string{card} + " offers the RAM headroom at the server's 16: " + option);
  }
  expect(option_of(plain, "D").empty() && option_of(plain, "M").empty(), "DeepSeek's and MiniMax's engines read none");
  const std::string moved = listed(std::int64_t{24});
  expect(option_of(moved, "G").find("\"default\":24") != std::string::npos, "the server's own value is the default: " + option_of(moved, "G"));
}

// Owner 2026-10-29 ("make the default timeout configurable per model on the card"): every card offers the longest
// answer in minutes, its default the server's own limits.engine.generation_timeout_s (30 minutes unless the file sets
// it; a part of a minute counts as a minute).
void cards_offer_their_longest_answer_at_the_servers_value() {
  const auto listed = [](const std::optional<std::int64_t> configured) {
    secure_frontend::LauncherSettings settings;
    if (configured) settings.main.entries["limits.engine.generation_timeout_s"] = {*configured, secure_frontend::ConfigSource::file};
    for (const auto& [name, profile] : std::vector<std::pair<std::string, secure_frontend::EngineProfile>>{
             {"Q", secure_frontend::EngineProfile::qwen}, {"G", secure_frontend::EngineProfile::glm53},
             {"M", secure_frontend::EngineProfile::minimax}}) {
      secure_frontend::ModelCard card;
      card.name = name;
      card.profile = profile;
      settings.cards.push_back(card);
    }
    settings.log = [](std::string_view) {};
    secure_frontend::ModelLauncher launcher(std::move(settings));
    std::string text = launcher.list_json();
    launcher.shutdown();
    return text;
  };
  const auto option_of = [](const std::string& text, const std::string& card) {
    const auto at = text.find("{\"name\":\"" + card + "\",\"profile\"");
    const auto end = text.find("\"calibration\":", at);
    const auto option = text.find("{\"name\":\"answer_minutes\"", at);
    return at == std::string::npos || option == std::string::npos || option > end ? std::string{} : text.substr(option, text.find('}', option) - option);
  };
  const std::string plain = listed(std::nullopt);
  for (const char* card : {"Q", "G", "M"}) {
    const std::string option = option_of(plain, card);
    expect(option.find("\"kind\":\"integer\"") != std::string::npos && option.find("\"default\":30") != std::string::npos &&
               option.find("\"choices\":[2,5,10,15,30,60,120,240,1440]") != std::string::npos,
           std::string{card} + " offers the longest answer at the server's 30 minutes: " + option);
  }
  expect(option_of(listed(std::int64_t{600}), "Q").find("\"default\":10") != std::string::npos, "the server's own value is the default");
  expect(option_of(listed(std::int64_t{90}), "Q").find("\"default\":2") != std::string::npos, "90 s reads as 2 minutes");
}

// The same over a start: a card made before cards carried sampling (the owner's Qwen3.8 card, 2026-10-29) samples as
// its model's card says when the chat names nothing (it ran greedy), and a chat's own values still win.
void a_card_without_sampling_starts_with_the_recommended() {
  const auto record_file = test_support::make_temp_dir("plain") / "record.txt";
  ServerOptions options;
  options.model_cards = {CardOptions{.name = "Plain", .profile = "qwen", .engine_args = {"--record-file=" + test_support::utf8_path(record_file)}}};
  options.config_literals = {{"models.control", "true"}};
  auto server = ServerProcess::start(options);
  expect(server && server->wait_until_listening(std::chrono::seconds(20)), "the launcher boots");
  if (!server) return;
  const auto started = call(*server, "POST", "/v1/strata/models/start", R"({"name":"Plain"})");
  expect(started && started->status == 202, "Plain's start is accepted: " + shown(started));
  expect(wait_for_card(*server, "Plain", "running"), "Plain runs: " + server->stderr_text());
  const auto plain = call(*server, "POST", "/v1/chat/completions", chat_body("Plain"));
  expect(plain && plain->status == 200, "a chat that names no sampling answers: " + shown(plain));
  const auto greedy = call(*server, "POST", "/v1/chat/completions",
                           R"({"model":"Plain","messages":[{"role":"user","content":"Hi"}],"max_tokens":2,"temperature":0})");
  expect(greedy && greedy->status == 200, "a greedy chat answers: " + shown(greedy));
  std::vector<std::string> generations;
  std::istringstream lines(secure_frontend_test::read_file(record_file));
  for (std::string line; std::getline(lines, line);) {
    if (line.rfind("GEN ", 0) == 0) generations.push_back(line);
  }
  expect(generations.size() == 2U, "two generations reached the engine: " + std::to_string(generations.size()));
  if (generations.size() != 2U) return;
  expect(generations[0].find(" temperature=1 top_p=0.95 top_k=20") != std::string::npos,
         "the recommended sampling reached the engine: " + generations[0]);
  expect(generations[1].find("temperature=") == std::string::npos && generations[1].find(" top_p=0.95 top_k=20") != std::string::npos,
         "the chat's temperature 0 wins (greedy), the card's others stay: " + generations[1]);
}

// The owner's GLM-5.3 calibration, 2026-10-29 ("a calibration reply failed (unsupported_parameter)", every time): a
// calibration's replies are chats its card's profile takes, and GLM-5.3 takes no reasoning_effort per request (its
// prompt always opens its think block). A GLM-5.3 card (a glm53 pack: its marker and a tokenizer with GLM's markers)
// on the fake engine in Maya's dialect measures every trial and keeps a result.
void a_glm_card_calibrates() {
  const auto root = secure_frontend_test::make_scratch_directory();
  const auto data = secure_frontend::data_folder_layout(root / "data");
  const std::string name = "GLM-5.3-Flash";
  const auto pack = root / "packs" / "glm";
  std::filesystem::create_directories(pack / "tokenizer");
  (void)secure_frontend::write_pack_marker(pack, secure_frontend::kGlmModelName);
  auto vocabulary = test_support::byte_tokenizer();
  for (const auto& [literal, type] : std::vector<std::pair<std::string, int>>{
           {"[gMASK]", 3}, {"<sop>", 3}, {"<|system|>", 3}, {"<|user|>", 3}, {"<|assistant|>", 3}, {"<|observation|>", 3},
           {"<|endoftext|>", 3}, {"<think>", 4}, {"</think>", 4}}) {
    vocabulary.tokens_by_id.push_back(literal);
    vocabulary.token_types.push_back(type);
  }
  for (const auto& [file, bytes] : test_support::tokenizer_pack_files(vocabulary)) secure_frontend_test::write_file(pack / "tokenizer" / file, bytes);
  const auto bin = root / "bin";
  secure_frontend_test::make_private_directory(bin);
  const std::filesystem::path fake{STRATA_FAKE_ENGINE_PATH};
  const auto engine = bin / fake.filename();
  std::error_code ec;
  std::filesystem::copy_file(fake, engine, ec);
  secure_frontend_test::write_file(bin / "fake-engine.options",
                                   "--dialect=maya\n--tokens=8\n--token-ids=" + std::to_string(test_support::token_id(vocabulary, "a")) + "\n");
  (void)secure_frontend::ensure_data_folder(data, engine);
  const auto main = std::get<secure_frontend::FrontendConfigDraft>(
      secure_frontend::parse_config_text(secure_frontend_test::read_file(data.config_file), data.root));
  const auto made = secure_frontend::card_for_pack(
      {.family = secure_frontend::PackedFamily::glm53, .name = name, .engine = engine, .source = "download:glm", .pack = pack});
  auto parsed = secure_frontend::parse_model_card(made.json, data.root);
  auto lines = std::make_shared<std::vector<std::string>>();
  auto lines_mutex = std::make_shared<std::mutex>();
  secure_frontend::LauncherSettings settings{.main = main, .cards = {}, .operator_cards = 0U, .memory_file = {}, .gate = {}};
  settings.calibration_dir = data.state / "calibration";
  settings.log = [lines, lines_mutex](const std::string_view line) {
    std::lock_guard lock(*lines_mutex);
    lines->emplace_back(line);
  };
  secure_frontend::ModelLauncher launcher(std::move(settings));
  expect(std::holds_alternative<secure_frontend::ModelCard>(parsed) && launcher.add_card(std::get<secure_frontend::ModelCard>(std::move(parsed))),
         "the GLM-5.3 card is added");
  expect(!launcher.calibrate(name, nullptr, false, std::nullopt, false).has_value(), "a calibration is accepted");
  expect(launcher.wait_until_settled(std::chrono::seconds(300)), "it ends");
  launcher.shutdown();
  std::string failed;
  {
    std::lock_guard lock(*lines_mutex);
    for (const auto& line : *lines) {
      if (line.find("calibration reply failed") != std::string::npos || line.find("did not start for the calibration") != std::string::npos) failed += line + "; ";
    }
  }
  expect(failed.empty(), "every trial's replies are answered: " + failed);
  const auto stored = secure_frontend::parse_calibration(secure_frontend_test::read_file(data.state / "calibration" / (name + ".json")));
  expect(stored.has_value(), "the calibration keeps a result");
}

// Owner 2026-10-23 ("Every model should be showing the number of experts loaded along with
// memory used"): the Monitor's VRAM and RAM cards read the expert split from the engine's startup
// INFO. A model shows its experts as soon as it is running, without a first chat.
void experts_show_as_soon_as_a_model_runs() {
  auto options = two_cards({"--ready-experts=212,384"}, {},
                           {{"models.control", "true"}, {"listener.monitor", "true"}});
  auto server = ServerProcess::start(options);
  expect(server && server->wait_until_listening(std::chrono::seconds(20)), "the launcher boots with the Monitor on");
  if (!server) return;
  const auto started = call(*server, "POST", "/v1/strata/models/start", R"({"name":"Alpha"})");
  expect(started && started->status == 202, "Alpha starts: " + shown(started));
  expect(wait_for_card(*server, "Alpha", "running"), "Alpha runs: " + server->stderr_text());
  // No chat has run; the experts must already be there from the startup INFO.
  const auto metrics = call(*server, "GET", "/metrics");
  expect(metrics && metrics->status == 200 && metrics->body.find("\"experts\"") != std::string::npos &&
             metrics->body.find("\"expert_slots\":384") != std::string::npos &&
             metrics->body.find("\"expert_slots_primary\":212") != std::string::npos,
         "the experts are shown right after the start, before any chat: " + shown(metrics));
  server->stop(ServerProcess::Stop::ctrl_break);
  expect(server->wait_exit(std::chrono::seconds(20)) == std::optional<int>{0}, "a clean exit");
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"launcher_boots_idle_and_starts_a_card", launcher_boots_idle_and_starts_a_card},
      {"a_card_reads_pictures_with_its_encoder", a_card_reads_pictures_with_its_encoder},
      {"switch_waits_for_the_generation_then_swaps", switch_waits_for_the_generation_then_swaps},
      {"failed_start_keeps_the_server_usable", failed_start_keeps_the_server_usable},
      {"start_refusals_name_the_problem", start_refusals_name_the_problem},
      {"options_are_remembered", options_are_remembered},
      {"models_start_and_control_off", models_start_and_control_off},
      {"experts_show_as_soon_as_a_model_runs", experts_show_as_soon_as_a_model_runs},
      {"downloads_serve_the_catalog_and_load_download_cards", downloads_serve_the_catalog_and_load_download_cards},
      {"cards_can_be_added_while_serving", cards_can_be_added_while_serving},
      {"page_cards_can_be_replaced_and_removed", page_cards_can_be_replaced_and_removed},
      {"cards_say_whether_their_profile_takes_reasoning_effort", cards_say_whether_their_profile_takes_reasoning_effort},
      {"cards_offer_web_search_when_the_server_has_it", cards_offer_web_search_when_the_server_has_it},
      {"cards_offer_their_models_recommended_sampling", cards_offer_their_models_recommended_sampling},
      {"glm_cards_offer_their_ram_headroom", glm_cards_offer_their_ram_headroom},
      {"cards_offer_their_longest_answer_at_the_servers_value", cards_offer_their_longest_answer_at_the_servers_value},
      {"a_card_without_sampling_starts_with_the_recommended", a_card_without_sampling_starts_with_the_recommended},
      {"a_gguf_card_reads_its_tokenizer_from_the_gguf", a_gguf_card_reads_its_tokenizer_from_the_gguf},
      {"a_qwen36_card_reads_its_tokenizer_from_the_gguf", a_qwen36_card_reads_its_tokenizer_from_the_gguf},
      {"calibration_finds_the_win_and_starts_with_it", calibration_finds_the_win_and_starts_with_it},
      {"a_first_start_calibrates_first", a_first_start_calibrates_first},
      {"a_minimax_card_measures_its_prompt_reading_first", a_minimax_card_measures_its_prompt_reading_first},
      {"a_glm_card_calibrates", a_glm_card_calibrates},
      {"a_failed_prefill_calibration_is_not_retried_by_itself", a_failed_prefill_calibration_is_not_retried_by_itself},
      {"a_prefill_calibration_falls_back_to_the_default_chunk", a_prefill_calibration_falls_back_to_the_default_chunk},
      {"a_calibration_can_be_cancelled", a_calibration_can_be_cancelled},
      {"removing_a_card_removes_its_calibration", removing_a_card_removes_its_calibration},
      {"a_stop_waits_for_an_engine_still_ending", a_stop_waits_for_an_engine_still_ending},
      {"failures_raise_alerts", failures_raise_alerts},
      {"bad_card_refused_at_boot_and_main_rows_refused", bad_card_refused_at_boot_and_main_rows_refused},
  }, failures);
}
