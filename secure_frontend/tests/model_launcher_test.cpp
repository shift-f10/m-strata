// Model launcher (plan docs/superpowers/plans/2026-10-23-model-launcher.md Tasks 3-4; spec §5):
// the real strata-unified-server booted with model cards over the supervision fake engine. A
// card starts from the API with typed options, a switch waits for the generation in flight, a
// failed start leaves the server usable, one switch runs at a time, and the page's options are
// remembered.
#include "secure_frontend/api_errors.hpp"
#include "secure_frontend/calibration.hpp"
#include "secure_frontend/calibration_report.hpp"
#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/card_builder.hpp"
#include "secure_frontend/config_file.hpp"
#include "secure_frontend/first_start.hpp"
#include "secure_frontend/gguf_metadata.hpp"
#include "secure_frontend/json.hpp"
#include "secure_frontend/model_launcher.hpp"
#include "secure_frontend/model_runtime.hpp"
#include "secure_frontend/serve_monitor.hpp"
#include "secure_frontend/sha256.hpp"
#include "secure_frontend/tool_runner.hpp"
#include "support/config_json.hpp"
#include "support/fake_engine_dump.hpp"
#include "support/fake_supervised_process.hpp"
#include "support/gguf_builder.hpp"
#include "support/raw_http.hpp"
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

// A card's own entry in the models list, as text: {"name": ... } (empty: not there).
std::string card_entry(const ServerProcess& server, const std::string& name) {
  const auto listed = call(server, "GET", "/v1/strata/models");
  if (!listed || listed->status != 200) return {};
  const auto at = listed->body.find("{\"name\":\"" + name + "\",\"profile\":");
  if (at == std::string::npos) return {};
  // (the next card's entry: options are {"name": ...} objects too, but only a card has "profile")
  const auto next_card = listed->body.find("\",\"profile\":\"", at + 1U);
  const auto next = next_card == std::string::npos ? std::string::npos : listed->body.rfind("{\"name\":\"", next_card);
  return listed->body.substr(at, next == std::string::npos || next <= at ? std::string::npos : next - at);
}
// The number after "<key>": in `text` (nullopt: none there, or null).
std::optional<double> number_after(const std::string& text, const std::string& key) {
  const auto at = text.find("\"" + key + "\":");
  if (at == std::string::npos) return std::nullopt;
  const auto value = text.substr(at + key.size() + 3U);
  if (value.starts_with("null")) return std::nullopt;
  try {
    return std::stod(value);
  } catch (...) {
    return std::nullopt;
  }
}

// A start or stop as a client sends it: asked again while the launcher answers 409 switch_in_progress (one start or stop
// at a time; it is still finishing the one before), for up to 15 s.
std::optional<HttpResult> post_when_free(const ServerProcess& server, const std::string_view target, const std::string_view body) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  auto answer = call(server, "POST", target, body);
  while (has_code(answer, 409, "switch_in_progress") && std::chrono::steady_clock::now() < until) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    answer = call(server, "POST", target, body);
  }
  return answer;
}

// The owner's external client (2026-10-31, gap L1: "During starting, expose progress/phase/elapsed ... On failed, make
// error a stable machine-readable code"): a starting card's entry says what its start is doing and for how long -
// "start": {"phase", "elapsed_s", "expected_s", "progress"}, the expected time this card's last start that ran took
// (null until one did) and the progress estimated from it - and is null otherwise; a failed card names a stable
// "error_code" beside its sentence.
void a_start_says_its_phase_and_time_and_a_failure_its_code() {
  auto server = ServerProcess::start(two_cards({"--ready-delay-ms=1500"}, {"--exit-before-ready=3"}));
  expect(server && server->wait_until_listening(std::chrono::seconds(20)), "the launcher boots");
  if (!server) return;
  expect(card_entry(*server, "Alpha").find("\"start\":null") != std::string::npos, "stopped: no start: " + card_entry(*server, "Alpha"));
  expect(call(*server, "POST", "/v1/strata/models/start", R"({"name":"Alpha"})")->status == 202, "Alpha starts");
  std::string seen;
  for (int i = 0; i < 200 && seen.find("\"phase\":\"engine\"") == std::string::npos; ++i) {
    seen = card_entry(*server, "Alpha");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  expect(seen.find("\"state\":\"starting\"") != std::string::npos && seen.find("\"phase\":\"engine\"") != std::string::npos &&
             number_after(seen, "elapsed_s").has_value() && seen.find("\"expected_s\":null") != std::string::npos &&
             seen.find("\"progress\":null") != std::string::npos,
         "the engine loads, for this long, with nothing to expect yet: " + seen);
  expect(wait_for_card(*server, "Alpha", "running"), "Alpha runs");
  expect(card_entry(*server, "Alpha").find("\"start\":null") != std::string::npos, "running: no start");
  const auto stopped = post_when_free(*server, "/v1/strata/models/stop", "{}");
  expect(stopped && stopped->status / 100 == 2 && wait_for_card(*server, "Alpha", "stopped"), "Alpha stops: " + shown(stopped));
  const auto again = post_when_free(*server, "/v1/strata/models/start", R"({"name":"Alpha"})");
  expect(again && again->status == 202, "Alpha starts again: " + shown(again));
  seen.clear();
  for (int i = 0; i < 200 && seen.find("\"phase\":\"engine\"") == std::string::npos; ++i) {
    seen = card_entry(*server, "Alpha");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  const auto expected = number_after(seen, "expected_s");
  const auto progress = number_after(seen, "progress");
  expect(expected.has_value() && *expected >= 1.5 && progress.has_value() && *progress >= 0.0 && *progress < 1.0,
         "the last start's time is expected now, and the progress estimated from it: " + seen);
  expect(wait_for_card(*server, "Alpha", "running"), "Alpha runs again");
  const auto beta = post_when_free(*server, "/v1/strata/models/start", R"({"name":"Beta"})");
  expect(beta && beta->status == 202 && wait_for_card(*server, "Beta", "failed"), "Beta fails to load: " + shown(beta));
  const auto failed = card_entry(*server, "Beta");
  expect(failed.find("\"error\":\"the engine ended before it was ready (see the server log)\"") != std::string::npos &&
             failed.find("\"error_code\":\"engine_not_ready\"") != std::string::npos && failed.find("\"start\":null") != std::string::npos,
         "the sentence, and its stable code: " + failed);
  expect(card_entry(*server, "Alpha").find("\"error_code\":null") != std::string::npos, "no failure: no code");
  server->stop(ServerProcess::Stop::ctrl_break);
  expect(server->wait_exit(std::chrono::seconds(20)) == std::optional<int>{0}, "a clean exit");
}

// The owner's external client (2026-10-31, gap O3: "GET /v1/strata/config -> 405 ... A read endpoint would let the
// operator console display active defaults (thinking/temperature)"): GET /v1/strata/config reads the settings, and names
// the running model with the option values its start used (its defaults where the start named none); null while none
// runs.
void the_settings_say_the_running_models_values() {
  auto server = ServerProcess::start(two_cards());
  expect(server && server->wait_until_listening(std::chrono::seconds(20)), "the launcher boots");
  if (!server) return;
  const auto idle = call(*server, "GET", "/v1/strata/config");
  expect(idle && idle->status == 200 && idle->body.find("\"settings\":[") != std::string::npos &&
             idle->body.find("\"running\":null") != std::string::npos,
         "a GET reads the settings; nothing runs: " + shown(idle));
  const auto started = call(*server, "POST", "/v1/strata/models/start", R"({"name":"Beta","options":{"temperature":0.3}})");
  expect(started && started->status == 202 && wait_for_card(*server, "Beta", "running"), "Beta runs: " + shown(started));
  const auto read = call(*server, "GET", "/v1/strata/config");
  const auto at = read ? read->body.find("\"running\":{\"name\":\"Beta\",\"options\":{") : std::string::npos;
  const std::string running = at == std::string::npos ? std::string{} : read->body.substr(at, read->body.find('}', at) - at);
  // (this fixture's Qwen card has no Thinking option - its test tokenizer has no thinking tags - so a default the start
  // did not name is its top_p)
  expect(!running.empty() && running.find("\"temperature\":0.3") != std::string::npos && running.find("\"top_p\":0.95") != std::string::npos,
         "the running model, the temperature its start named and a default it did not: " + shown(read));
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

void card_speed_options_reach_child_and_are_remembered() {
  const auto state = test_support::make_temp_dir("speed-options") / "state";
  secure_frontend_test::make_owner_private_directory(state);
  const auto dump = state / "engine" / "alpha.dump";
  auto options = two_cards({"--dump-file=alpha.dump"});
  options.state_dir = state;
  options.model_cards.front().config_literals["engine.speed_settings"] = R"(["ROUTE_PRIOR=0.4"])";
  for (const int pass : {0, 1, 2, 3}) {
    auto server = ServerProcess::start(options);
    expect(server && server->wait_until_listening(std::chrono::seconds(20)), "speed-option launcher boots");
    if (!server) return;
    const auto listed = call(*server, "GET", "/v1/strata/models");
    expect(listed && listed->body.find(R"("accuracy_tradeoff":true,"accuracy_default":0)") != std::string::npos,
           "auto-offered controls advertise the accuracy baseline");
    const auto bad = call(*server, "POST", "/v1/strata/models/start",
                          R"({"name":"Alpha","options":{"speed_route_resident":1.1}})");
    expect(bad && bad->status == 400, "out-of-range margin refuses before spawn");
    const auto body = pass == 0 ? R"({"name":"Alpha","options":{"speed_route_resident":0.25,"speed_hc_req8":true,"speed_route_tail_skip":7}})" :
                      pass == 2 ? R"({"name":"Alpha","options":{"speed_route_resident":0,"speed_hc_req8":false,"speed_route_tail_skip":0,"speed_route_prior":0}})" :
                                  R"({"name":"Alpha"})";
    const auto started = call(*server, "POST", "/v1/strata/models/start", body);
    expect(started && started->status == 202 && wait_for_card(*server, "Alpha", "running"),
           "selected and remembered speed options start the model: " + server->stderr_text());
    const auto captured = test_support::read_dump_file(dump, std::chrono::seconds(5));
    expect(captured.has_value(), "child environment is captured");
    if (captured) {
      const std::vector<std::string> expected = pass < 2 ?
          std::vector<std::string>{"STRATA_ROUTE_RESIDENT=0.25", "MAYA_ROUTE_RESIDENT=0.25", "STRATA_HC_REQ8=1",
                                   "STRATA_ROUTE_TAIL_SKIP=7", "STRATA_ROUTE_PRIOR=0.4", "STRATA_SPEC_PROB=0"} :
          std::vector<std::string>{"STRATA_ROUTE_RESIDENT=0", "MAYA_ROUTE_RESIDENT=0", "STRATA_HC_REQ8=0",
                                   "STRATA_ROUTE_TAIL_SKIP=0", "STRATA_ROUTE_PRIOR=0", "STRATA_SPEC_PROB=0"};
      for (const auto& value : expected)
        expect(std::find(captured->env.begin(), captured->env.end(), value) != captured->env.end(),
               "child sees selected values and other accuracy defaults: " + value);
      expect(std::any_of(captured->env.begin(), captured->env.end(), [pass](const auto& value) {
        return value.starts_with("MSTRATA_SPEED_SETTINGS=") && value.find(pass < 2 ? "ROUTE_RESIDENT=0.25" : "ROUTE_RESIDENT=0;") != std::string::npos;
      }), "wrapper receives the validated explicit speed channel");
    }
    server->stop(ServerProcess::Stop::ctrl_break);
    expect(server->wait_exit(std::chrono::seconds(20)) == std::optional<int>{0}, "speed-option server exits cleanly");
    std::filesystem::remove(dump);
  }
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

// Pinned and hot models (owner 2026-10-31, spec part 1): one card at most is pinned to start at startup - the boot's pin
// to begin with - and the list says which. A pin is kept (save_pin: the config file's models.start) before it holds: a card
// the launcher does not hold, a pin that cannot be kept, a launcher with nowhere to keep it are refused; null unpins.
// Removing the pinned card unpins it, so the next start names no missing card.
void a_card_is_pinned_and_its_removal_unpins_it() {
  secure_frontend::ModelCard alpha;
  alpha.name = "Alpha";
  std::vector<std::optional<std::string>> saved;
  bool keeps = true;
  secure_frontend::LauncherSettings settings;
  settings.cards = {alpha};
  settings.log = [](std::string_view) {};
  settings.pinned = std::string{"Alpha"};
  settings.save_pin = [&saved, &keeps](const std::optional<std::string>& name) {
    if (keeps) saved.push_back(name);
    return keeps;
  };
  secure_frontend::ModelLauncher launcher(std::move(settings));
  const auto pinned_in = [&launcher](const std::string& name) {
    // the card's own "pinned", its last field: the first after its name (its options have names, never a pin)
    const std::string list = launcher.list_json();
    const auto at = list.find("\"name\":\"" + name + "\",\"profile\"");
    const auto pin = at == std::string::npos ? std::string::npos : list.find("\"pinned\":", at);
    if (pin == std::string::npos) return std::optional<bool>{};
    if (list.compare(pin, 13U, "\"pinned\":true") == 0) return std::optional<bool>{true};
    if (list.compare(pin, 14U, "\"pinned\":false") == 0) return std::optional<bool>{false};
    return std::optional<bool>{};
  };
  expect(pinned_in("Alpha") == std::optional<bool>{true} && launcher.pinned() == std::optional<std::string>{"Alpha"},
         "the boot's pin is listed: " + launcher.list_json().substr(0, 300));
  secure_frontend::ModelCard gamma;
  gamma.name = "Gamma";
  expect(launcher.add_card(gamma), "a page card is added");
  expect(!launcher.pin(std::string{"Gamma"}).has_value() && saved.size() == 1U && saved.back() == std::optional<std::string>{"Gamma"} &&
             pinned_in("Gamma") == std::optional<bool>{true} && pinned_in("Alpha") == std::optional<bool>{false},
         "pinning Gamma keeps it first, then moves the pin");
  const auto unknown = launcher.pin(std::string{"Nobody"});
  expect(unknown.has_value() && unknown->error == secure_frontend::ApiError::model_not_found && saved.size() == 1U,
         "a card it does not hold: refused, nothing kept");
  keeps = false;
  const auto unkept = launcher.pin(std::string{"Alpha"});
  expect(unkept.has_value() && unkept->error == secure_frontend::ApiError::settings_refused && !unkept->reason.empty() &&
             launcher.pinned() == std::optional<std::string>{"Gamma"},
         "a pin that cannot be kept: refused, saying why; the pin stays");
  keeps = true;
  expect(launcher.remove_card("Gamma") == secure_frontend::CardChange::done && saved.size() == 2U && !saved.back().has_value() &&
             !launcher.pinned().has_value() && pinned_in("Alpha") == std::optional<bool>{false},
         "removing the pinned card unpins it");
  expect(!launcher.pin(std::string{"Alpha"}).has_value() && !launcher.pin(std::nullopt).has_value() && saved.size() == 4U &&
             !saved.back().has_value() && !launcher.pinned().has_value(),
         "null unpins");
  launcher.shutdown();
  secure_frontend::LauncherSettings nowhere;
  nowhere.cards = {alpha};
  nowhere.log = [](std::string_view) {};
  secure_frontend::ModelLauncher fileless(std::move(nowhere));
  const auto refused = fileless.pin(std::string{"Alpha"});
  expect(refused.has_value() && refused->error == secure_frontend::ApiError::settings_refused, "nowhere to keep a pin: refused");
  fileless.shutdown();
}

// The pin through the server (spec part 1): a card pinned in the config (models.start) starts with the server and is
// listed pinned; POST /v1/strata/models/pin moves the pin in the config file, null takes it out, an unknown card is not
// found and a body that is not one is refused.
// One server, the apps manage it (owner 2026-10-31, spec section 6), over a real socket: started for a boot service
// (--no-login-link), the server prints no one-time login link; a key holder on this PC asks it for a fresh one, which the
// page's claim exchanges for the key once. Without the key there is none. Started without the flag, it prints its link.
void a_key_holder_gets_a_fresh_login_link() {
  const auto start = [](std::vector<std::string> args) {
    auto options = two_cards({}, {}, {{"models.control", "true"}, {"listener.web_ui", "true"}, {"listener.login_link", "true"}});
    options.extra_args = std::move(args);
    auto server = ServerProcess::start(options);
    expect(server && server->wait_until_listening(std::chrono::seconds(20)), "the launcher boots");
    return server;
  };
  const auto stdout_of = [](const ServerProcess& s) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::ifstream in(s.root() / "stdout.log", std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  };
  {
    auto printed = start({});
    if (!printed) return;
    const std::string out = stdout_of(*printed);
    expect(out.find("Sign in: open http://127.0.0.1:") != std::string::npos, "without the flag the link is printed: " + out);
  }
  auto server = start({"--no-login-link"});
  if (!server) return;
  const std::string out = stdout_of(*server);
  expect(out.find("Secure server listening") != std::string::npos && out.find("Sign in") == std::string::npos &&
             out.find("#login=") == std::string::npos,
         "--no-login-link: no link on its output: " + out);
  const std::string authority = "127.0.0.1:" + std::to_string(server->port());
  const auto unkeyed = raw_http::exchange("127.0.0.1", server->port(),
                                          raw_http::request("POST", "/v1/strata/login-link/new", authority,
                                                            {"Content-Type: application/json", "Connection: close"}, "{}"));
  expect(unkeyed && unkeyed->status == 401, "a fresh link needs the key");
  const auto fresh = call(*server, "POST", "/v1/strata/login-link/new", "{}");
  expect(fresh && fresh->status == 200, "a key holder gets a fresh link: " + shown(fresh));
  std::string code;
  if (fresh) {
    constexpr std::string_view kField = "{\"code\":\"";
    if (fresh->body.starts_with(kField)) code = fresh->body.substr(kField.size(), fresh->body.find('"', kField.size()) - kField.size());
  }
  expect(code.size() == 43U && fresh->body.find(test_support::kHarnessToken) == std::string::npos, "its code, never the key: " + shown(fresh));
  const auto claim = [&] {
    return raw_http::exchange("127.0.0.1", server->port(),
                              raw_http::request("POST", "/v1/strata/login-link", authority,
                                                {"Content-Type: application/json", "Connection: close"}, "{\"code\":\"" + code + "\"}"));
  };
  const auto claimed = claim();
  expect(claimed && claimed->status == 200 && claimed->body.find(test_support::kHarnessToken) != std::string::npos &&
             claimed->header("Cache-Control") == std::optional<std::string>{"no-store"},
         "the page's claim gets the key");
  const auto again = claim();
  expect(again && again->status == 403, "once");
}

void a_pinned_card_starts_with_the_server_and_the_pin_is_kept() {
  auto server = ServerProcess::start(two_cards({}, {}, {{"models.control", "true"}, {"models.start", "\"Beta\""}}));
  expect(server && server->wait_until_listening(std::chrono::seconds(20)), "the launcher boots");
  if (!server) return;
  expect(wait_for_card(*server, "Beta", "running"), "the pinned card starts with the server: " + server->stderr_text());
  const auto listed = call(*server, "GET", "/v1/strata/models");
  expect(listed && listed->body.find(R"("name":"Beta")") != std::string::npos &&
             listed->body.find(R"("pinned":true)") != std::string::npos,
         "it is listed pinned: " + shown(listed));
  const auto config = server->root() / "config.json";
  const auto start_row = [&config]() -> std::optional<std::string> {
    auto read = secure_frontend::read_config_file(config);
    const auto* draft = std::get_if<secure_frontend::FrontendConfigDraft>(&read);
    if (draft == nullptr) return std::string{"(unreadable)"};
    const auto found = draft->entries.find("models.start");
    if (found == draft->entries.end()) return std::nullopt;
    const auto* text = std::get_if<std::string>(&found->second.value);
    return text == nullptr ? std::string{"(not text)"} : *text;
  };
  const auto moved = call(*server, "POST", "/v1/strata/models/pin", R"({"name":"Alpha"})");
  expect(moved && moved->status == 200 && moved->body.find(R"("pinned":"Alpha")") != std::string::npos &&
             start_row() == std::optional<std::string>{"Alpha"},
         "the pin moves to Alpha, in the config file: " + shown(moved));
  const auto cleared = call(*server, "POST", "/v1/strata/models/pin", R"({"name":null})");
  expect(cleared && cleared->status == 200 && cleared->body.find(R"("pinned":null)") != std::string::npos && !start_row().has_value(),
         "null takes it out of the config file: " + shown(cleared));
  expect(has_code(call(*server, "POST", "/v1/strata/models/pin", R"({"name":"Gamma"})"), 404, "model_not_found"),
         "an unknown card is not found");
  for (const char* body : {"{}", R"({"name":1})", R"({"name":"Alpha","more":true})", "[]"}) {
    const auto bad = call(*server, "POST", "/v1/strata/models/pin", body);
    expect(bad && bad->status == 400, std::string{"a body that is not one is refused: "} + body + " " + shown(bad));
  }
  expect(card_state(*server, "Beta") == "running", "a pin changes nothing that runs");
  server->stop(ServerProcess::Stop::ctrl_break);
  expect(server->wait_exit(std::chrono::seconds(20)) == std::optional<int>{0}, "a clean exit");
}

// Pinned and hot models (owner 2026-10-31, spec part 2): every card offers Keep loaded, and a card started with it on
// starts and answers (its start writes no row the server refuses with model cards: never engine.idle_unload_s).
void a_card_kept_loaded_never_unloads_when_idle() {
  auto server = ServerProcess::start(two_cards({}, {}, {{"models.control", "true"}, {"listener.monitor", "true"}}));
  expect(server && server->wait_until_listening(std::chrono::seconds(20)), "the launcher boots");
  if (!server) return;
  const auto hot = call(*server, "POST", "/v1/strata/models/start", R"({"name":"Beta","options":{"hot":true}})");
  expect(hot && hot->status == 202 && wait_for_card(*server, "Beta", "running"), "Beta runs, kept loaded: " + shown(hot));
  expect(call(*server, "POST", "/v1/chat/completions", chat_body("Beta"))->status == 200, "Beta answers");
  const auto metrics = call(*server, "GET", "/metrics");
  expect(metrics && metrics->body.find("\"unloaded\":false") != std::string::npos, "it is loaded: " + shown(metrics));
  const auto listed = call(*server, "GET", "/v1/strata/models");
  expect(listed && listed->body.find(R"({"name":"hot","kind":"boolean","label":"Keep loaded")") != std::string::npos,
         "every card offers Keep loaded: " + shown(listed));
  server->stop(ServerProcess::Stop::ctrl_break);
  expect(server->wait_exit(std::chrono::seconds(20)) == std::optional<int>{0}, "a clean exit");
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

void fast_math_variant_is_selected_before_engine_start() {
  const auto root = secure_frontend_test::make_scratch_directory();
  const auto models = root / "models", bin = root / "engine", fast_dir = bin / "fast-math";
  secure_frontend_test::make_private_directory(models);
  secure_frontend_test::make_private_directory(bin);
  const std::filesystem::path fake{STRATA_FAKE_ENGINE_PATH};
  const auto accurate = bin / fake.filename(), fast = fast_dir / fake.filename();
  std::error_code copy_error;
  std::filesystem::copy_file(fake, accurate, copy_error);
  expect(!copy_error, "accurate fixture executable copied");
  const auto gguf = test_support::gguf_with_tokenizer(test_support::byte_tokenizer(), "gpt2", "qwen35moe")
                        .u32("qwen35moe.context_length", 262144U).bytes();
  secure_frontend_test::write_file(models / "variant-Q8_0.gguf", gguf);
  const auto data = secure_frontend::data_folder_layout(root / "data");
  expect(std::holds_alternative<secure_frontend::FirstStartResult>(secure_frontend::ensure_data_folder(data, accurate)),
         "variant fixture data folder created");
  const auto main = secure_frontend::parse_config_text(secure_frontend_test::read_file(data.config_file), data.root);
  if (!std::holds_alternative<secure_frontend::FrontendConfigDraft>(main)) return;
  const auto header = secure_frontend::parse_gguf_metadata(gguf);
  const auto made = secure_frontend::card_for_gguf(std::get<secure_frontend::GgufMetadata>(header),
      {.folder = models, .file = "variant-Q8_0.gguf", .engine = accurate, .source = "folder"});
  const auto parsed = secure_frontend::parse_model_card(std::get<secure_frontend::CardText>(made).json, data.root);
  auto card = std::get<secure_frontend::ModelCard>(parsed);
  card.config.entries["engine.lib_dirs"] = {std::vector<std::string>{secure_frontend::path_to_utf8(bin)}, secure_frontend::ConfigSource::file};
  const auto compose = [&](const bool enabled) -> std::variant<secure_frontend::ComposedModel, secure_frontend::ComposeFailure> {
    card.config.entries["engine.fast_math"] = {enabled, secure_frontend::ConfigSource::file};
    const auto validated = secure_frontend::card_config(std::get<secure_frontend::FrontendConfigDraft>(main), card,
                                                       secure_frontend::default_option_values(card));
    if (const auto* issues = std::get_if<secure_frontend::ConfigErrors>(&validated)) {
      std::string why;
      for (const auto& issue : *issues) why += secure_frontend::format_issue(issue) + "; ";
      expect(false, "variant card config validates: " + why);
      return secure_frontend::ComposeFailure{{why}, "invalid fixture", 1};
    }
    return secure_frontend::compose_model(*std::get<secure_frontend::ValidatedConfig>(validated).shared(), true);
  };
  const auto check_path = [&](const bool enabled, const std::filesystem::path& expected) {
    const auto result = compose(enabled);
    const auto* model = std::get_if<secure_frontend::ComposedModel>(&result);
    std::string why;
    if (const auto* error = std::get_if<secure_frontend::ComposeFailure>(&result))
      for (const auto& message : error->messages) why += message;
    expect(model != nullptr, "selected variant composes: " + why);
    if (!model) return;
    expect(model->plan.command.executable == expected, "the child launches the selected variant executable");
    expect(model->plan.environment && !model->plan.environment->lib_dirs.empty() &&
           model->plan.environment->lib_dirs.front() == expected.parent_path(),
           "selected variant libraries precede the explicitly configured original folder");
    expect(model->tokenizer.tokens_by_id == test_support::byte_tokenizer().tokens_by_id, "variant selection preserves model tokenizer");
  };
  check_path(false, accurate);
  const auto missing = compose(true);
  const auto* refused = std::get_if<secure_frontend::ComposeFailure>(&missing);
  expect(refused && refused->exit_code != 0 && std::string(refused->reason).find("fast") != std::string::npos,
         "a missing fast variant refuses clearly without falling back");
  secure_frontend_test::make_private_directory(fast_dir);
  std::filesystem::copy_file(fake, fast, copy_error);
  expect(!copy_error, "fast fixture executable copied");
  const auto bytes = secure_frontend_test::read_file(fast);
  const auto filename = secure_frontend_test::json_literal(secure_frontend::path_to_utf8(fake.filename()));
  const auto files = "[{\"path\":" + filename + ",\"bytes\":" + std::to_string(bytes.size()) +
      ",\"sha256\":\"" + secure_frontend::to_hex(secure_frontend::sha256(bytes)) + "\"}]";
  secure_frontend_test::write_file(bin / "variants.json", "{\"schema_version\":1,\"variants\":[" +
      std::string{"{\"name\":\"accurate\",\"directory\":\".\",\"fast_math\":false,\"executable\":"} + filename + ",\"files\":" + files + "}," +
      "{\"name\":\"fast-math\",\"directory\":\"fast-math\",\"fast_math\":true,\"executable\":" + filename + ",\"files\":" + files + "}]}", 0600);
  check_path(true, fast);
  check_path(false, accurate);
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
  secure_frontend::ServeMonitor monitor;  // the page's Monitor (metrics_json)
  std::unique_ptr<secure_frontend::ModelLauncher> launcher;

  // operator_cards: the two cards are the operator's (listed), which never calibrate before a first start; factory:
  // how the engine processes are made (unset: the fake engine's real process)
  explicit CalibrationFixture(const std::vector<std::string>& extra_fake_options = {}, const bool operator_cards = false,
                              std::function<secure_frontend::ProcessFactory(secure_frontend::EngineLaunchPlan)> factory = {},
                              const std::chrono::milliseconds warm_every = {}, const std::chrono::milliseconds restart_after = {}) {
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
    settings.benchmark_gpu = [](std::span<const unsigned>) { return secure_frontend::CalibrationGpuSignals{false,0U,{}}; };
    // a calibration's report for a bug report (owner 2026-10-30), with this fixed hardware
    settings.report_dir = report_dir();
    settings.report_machine = [] {
      secure_frontend::ReportMachine machine;
      machine.hardware.cpu = "Test CPU, 8 threads";
      machine.hardware.os = "Test OS 1.0";
      machine.physical_cores = 4U;
      machine.memory = secure_frontend::MemoryReading{.used_bytes = 4ULL << 30U, .total_bytes = 16ULL << 30U};
      machine.gpus = {{.index = 0U, .name = "Test GPU", .mem_total_bytes = 8192ULL << 20U, .mem_used_bytes = 512ULL << 20U,
                       .driver = "999.1", .compute_cap = "8.9", .pcie_gen_max = 4U, .pcie_width_max = 16U, .power_limit_w = 200.0}};
      return machine;
    };
    settings.log = [](std::string_view) {};
    if (factory) settings.factory = std::move(factory);
    settings.on_event = [list = events](const std::string_view event, const std::string& subject, const std::string&) {
      list->push_back(std::string{event} + ": " + subject);
    };
    settings.monitor = &monitor;
    if (warm_every.count() > 0) settings.warm_every = warm_every;
    if (restart_after.count() > 0) settings.restart_after = restart_after;
    launcher = std::make_unique<secure_frontend::ModelLauncher>(std::move(settings));
    if (!operator_cards) {
      for (auto& card : cards) (void)launcher->add_card(std::move(card));
    }
  }
  ~CalibrationFixture() { launcher->shutdown(); }
  CalibrationFixture(const CalibrationFixture&) = delete;
  CalibrationFixture& operator=(const CalibrationFixture&) = delete;
  [[nodiscard]] std::filesystem::path result_file(const std::string& card) const { return data.state / "calibration" / (card + ".json"); }
  [[nodiscard]] std::filesystem::path report_dir() const { return data.logs / "calibration-reports"; }
  // the reports written so far
  [[nodiscard]] std::vector<std::filesystem::path> reports() const {
    std::vector<std::filesystem::path> found;
    std::error_code ec;
    for (std::filesystem::directory_iterator entry(report_dir(), ec), end; !ec && entry != end; entry.increment(ec)) {
      found.push_back(entry->path());
    }
    return found;
  }
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
  // no trial failed: no report
  expect(f.reports().empty() && !f.launcher->calibration_report(name).has_value() && listed.find("\"report\":null") != std::string::npos,
         "no report when nothing failed: " + listed);

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
void calibration_keeps_the_options_it_measured() {
  CalibrationFixture f;
  const std::string name = "tiny-gemma-Q8_0";
  const auto selected = json_of(R"({"max_context":8192})");
  expect(!f.launcher->calibrate(name, &selected, false, std::nullopt, false), "explicit calibration accepted");
  expect(f.launcher->wait_until_settled(std::chrono::seconds(120)), "explicit calibration finishes");
  const auto listed = f.launcher->list_json();
  expect(listed.find("\"calibration\":{\"state\":\"fresh\"") != std::string::npos,
         "completed calibration is fresh at the values just measured");
  expect(!f.launcher->start(name, nullptr, false), "start with remembered calibrated values accepted");
  expect(f.launcher->wait_until_settled(std::chrono::seconds(60)), "start settles");
  expect(has_pair(dumped_argv(f.dump), "--max-context", "8192"), "the calibrated context is remembered");
  expect(has_pair(dumped_argv(f.dump), "--cpu-threads", f.winner), "fresh tuning is reused");
}

void benchmark_quality_failure_preserves_previous_calibration() {
  CalibrationFixture f;
  const std::string name="tiny-gemma-Q8_0";
  expect(!f.launcher->calibrate(name,nullptr,false,std::nullopt,false),"baseline calibration accepted");
  expect(f.launcher->wait_until_settled(std::chrono::seconds(120)),"baseline finishes");
  const auto original=secure_frontend_test::read_file(f.result_file(name));
  secure_frontend::BenchmarkOptions benchmark;
  expect(!f.launcher->calibrate(name,nullptr,false,std::nullopt,false,benchmark),"benchmark accepted");
  expect(f.launcher->wait_until_settled(std::chrono::seconds(120)),"benchmark finishes");
  expect(secure_frontend_test::read_file(f.result_file(name))==original,"failed known-answer check does not replace prior result");
  const auto listed=f.launcher->list_json();
  expect(listed.find("quality_failed")!=std::string::npos&&listed.find("not_applied")!=std::string::npos,"quality failure is visible");
  expect(listed.find("history")!=std::string::npos,"previous result remains in history");
}

void benchmark_resume_refuses_unmatched_checkpoint_and_restore_is_bounded() {
  CalibrationFixture f;
  const std::string name="tiny-gemma-Q8_0";
  secure_frontend::BenchmarkOptions benchmark;benchmark.resume=true;
  expect(!f.launcher->calibrate(name,nullptr,false,std::nullopt,false,benchmark),"resume request accepted for checking");
  expect(f.launcher->wait_until_settled(std::chrono::seconds(60)),"resume refusal settles");
  expect(!std::filesystem::exists(f.result_file(name)),"unmatched resume saves no tuning");
  expect(f.launcher->list_json().find("Resume refused")!=std::string::npos,"resume mismatch explained");
  expect(f.launcher->restore_calibration(name,"../escape").has_value(),"restore rejects traversal");
  expect(f.launcher->restore_calibration(name,std::string(24,'a')).has_value(),"restore rejects unknown history id");
}

void benchmark_success_preserves_private_input_and_restores_history() {
  CalibrationFixture f;
  const std::string name="tiny-gemma-Q8_0";
  const auto selected=json_of(R"({"max_context":8192})");
  secure_frontend::BenchmarkOptions benchmark;
  benchmark.workload=secure_frontend::BenchmarkWorkload::custom;
  benchmark.custom_prompt="Private benchmark input must never be persisted 73931";
  benchmark.label="Verified fixture";
  expect(!f.launcher->calibrate(name,&selected,false,std::nullopt,false,benchmark),"custom benchmark accepted");
  expect(f.launcher->wait_until_settled(std::chrono::seconds(120)),"custom benchmark finishes");
  const auto original=secure_frontend_test::read_file(f.result_file(name));
  const auto result=secure_frontend::parse_calibration(original);
  expect(result.has_value(),"successful measured workload saves calibration");
  const auto listed=f.launcher->list_json();
  expect(listed.find("\"outcome\": \"applied\"")!=std::string::npos,"result is explicitly applied");
  expect(listed.find("semantic accuracy is not established")!=std::string::npos,"custom result never claims semantic accuracy");
  const auto id=secure_frontend::to_hex(secure_frontend::sha256(original)).substr(0,24);
  const auto changed=json_of(R"({"max_context":4096})");
  expect(!f.launcher->start(name,&changed,false,false),"different context starts without recalibration");
  expect(f.launcher->wait_until_settled(std::chrono::seconds(60)),"changed start settles");
  expect(!f.launcher->stop(false) && f.launcher->wait_until_settled(std::chrono::seconds(60)),"changed model stops");
  expect(!f.launcher->restore_calibration(name,id),"verified prior calibration restores");
  expect(f.launcher->list_json().find("\"calibration\":{\"state\":\"fresh\"")!=std::string::npos,"restored options are fresh");
  expect(!f.launcher->start(name,nullptr,false,false) && f.launcher->wait_until_settled(std::chrono::seconds(60)),"restored model starts");
  expect(has_pair(dumped_argv(f.dump),"--max-context","8192"),"restore reinstates the measured context");
  for(const auto& file:std::filesystem::directory_iterator(f.data.state/"calibration")){
    if(file.is_regular_file())expect(secure_frontend_test::read_file(file.path()).find(benchmark.custom_prompt)==std::string::npos,"no custom prompt in calibration artifacts");
  }
  expect(!f.launcher->stop(false) && f.launcher->wait_until_settled(std::chrono::seconds(60)),"model stops before comparison");
  benchmark.compare_speed=true;
  expect(!f.launcher->calibrate(name,nullptr,false,std::nullopt,false,benchmark),"explicit comparison accepted");
  expect(f.launcher->wait_until_settled(std::chrono::seconds(120)),"comparison completes");
  expect(secure_frontend_test::read_file(f.result_file(name))==original,"comparison never replaces accepted calibration");
  const auto comparison=f.launcher->list_json();
  expect(comparison.find("comparison_complete")!=std::string::npos,"comparison completion is visible");
  expect(comparison.find("fast-math runtime is unavailable")!=std::string::npos,"missing fast runtime is explained and skipped");
}

void benchmark_cancel_resume_is_bound_to_all_options() {
  CalibrationFixture f({"--ready-delay-ms=250"});
  const std::string name="tiny-gemma-Q8_0";
  secure_frontend::BenchmarkOptions benchmark;
  benchmark.workload=secure_frontend::BenchmarkWorkload::custom;
  benchmark.custom_prompt="Resume fixture";
  expect(!f.launcher->calibrate(name,nullptr,false,std::nullopt,false,benchmark),"benchmark starts");
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
  while(std::chrono::steady_clock::now()<deadline && f.launcher->list_json().find("\"trials\": [{")==std::string::npos)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  expect(f.launcher->list_json().find("\"trials\": [{")!=std::string::npos,"at least one measured trial finishes before cancelling");
  expect(!f.launcher->cancel_calibration(),"cancel accepted");
  expect(f.launcher->wait_until_settled(std::chrono::seconds(60)),"cancellation settles");
  expect(f.launcher->list_json().find("\"paused\": true")!=std::string::npos,"cancel leaves resumable checkpoint");
  expect(!std::filesystem::exists(f.result_file(name)),"cancel does not apply unfinished tuning");
  benchmark.resume=true;
  const auto changed=json_of(R"({"temperature":0.5})");
  expect(!f.launcher->calibrate(name,&changed,false,std::nullopt,false,benchmark),"changed-options resume checked");
  expect(f.launcher->wait_until_settled(std::chrono::seconds(60)),"changed-options refusal settles");
  expect(f.launcher->list_json().find("Resume refused")!=std::string::npos,"even neutral option changes bind the checkpoint");
  expect(!f.launcher->calibrate(name,nullptr,false,std::nullopt,false,benchmark),"matching resume accepted");
  expect(f.launcher->wait_until_settled(std::chrono::seconds(120)),"resumed benchmark completes");
  const auto listed=f.launcher->list_json();
  expect(listed.find("\"reused\": true")!=std::string::npos,"matching completed trial is reused");
  expect(listed.find("\"outcome\": \"applied\"")!=std::string::npos,"resumed completed result applies");

}

void benchmark_route_accepts_bounded_custom_input_and_rejects_mixed_actions() {
  auto server=ServerProcess::start(two_cards());
  expect(server && server->wait_until_listening(std::chrono::seconds(20)),"benchmark route server starts");
  if(!server)return;
  for(const std::string body:{
      R"({"name":"Alpha","benchmark":{"workload":"custom"}})",
      R"({"name":"Alpha","benchmark":{"compare_speed":true},"then_start":true})",
      R"({"name":"Alpha","restore":"aaaaaaaaaaaaaaaaaaaaaaaa","benchmark":{}})",
      R"({"name":"Alpha","benchmark":{"unsupported":true}})"}){
    const auto answer=call(*server,"POST","/v1/strata/models/calibrate",body);
    expect(answer && answer->status==400,"invalid benchmark action rejected: "+shown(answer));
  }
  const std::string prompt(5000,'a');
  const auto accepted=call(*server,"POST","/v1/strata/models/calibrate",
      "{\"name\":\"Alpha\",\"benchmark\":{\"workload\":\"custom\",\"resume\":true,\"custom_prompt\":\""+prompt+"\"}}");
  expect(accepted && accepted->status==202,"bounded custom input above old 4KiB limit reaches resume validation: "+shown(accepted));
  expect(wait_for_card(*server,"Alpha","stopped"),"invalid resume settles without engine work");
  const auto oversized=call(*server,"POST","/v1/strata/models/calibrate",
      "{\"name\":\"Alpha\",\"benchmark\":{\"workload\":\"custom\",\"custom_prompt\":\""+std::string(16385,'a')+"\"}}");
  expect(oversized && oversized->status==400,"custom input above field bound rejected: "+shown(oversized));
  const auto ordinary=call(*server,"POST","/v1/strata/models/start","{\"name\":\""+prompt+"\"}");
  expect(ordinary && ordinary->status==413,"other management routes keep their small input bound: "+shown(ordinary));
  server->stop(ServerProcess::Stop::ctrl_break);
  expect(server->wait_exit(std::chrono::seconds(20))==std::optional<int>{0},"route fixture exits cleanly");
}

void benchmark_rejects_runtime_changes_during_trials() {
  for(const bool comparison:{false,true}){
    const auto changed=std::make_shared<bool>(false);
    CalibrationFixture f({},false,[changed](secure_frontend::EngineLaunchPlan plan){
      if(!*changed){
        *changed=true;
        secure_frontend_test::write_file(plan.command.executable.parent_path()/"updated-runtime.dll","changed library fixture");
      }
      return secure_frontend::make_stdio_process_factory(std::move(plan));
    });
    const std::string name="tiny-gemma-Q8_0";
    secure_frontend::BenchmarkOptions benchmark;
    benchmark.workload=secure_frontend::BenchmarkWorkload::custom;
    benchmark.custom_prompt="Runtime identity fixture";
    benchmark.compare_speed=comparison;
    expect(!f.launcher->calibrate(name,nullptr,false,std::nullopt,false,benchmark),"runtime-change benchmark accepted");
    expect(f.launcher->wait_until_settled(std::chrono::seconds(120)),"runtime-change benchmark finishes");
    const auto listed=f.launcher->list_json();
    expect(!std::filesystem::exists(f.result_file(name)),"measurements spanning different runtimes are never applied");
    expect(listed.find("changed during the benchmark")!=std::string::npos,"identity change is explained");
    expect(listed.find("comparison_complete")==std::string::npos,"invalidated comparison never claims successful completion");
  }
}

void a_first_start_calibrates_first() {
  CalibrationFixture f;
  const std::string name = "other-gemma-Q8_0";
  expect(!f.launcher->start(name, nullptr, false).has_value(), "the start is accepted");
  expect(f.launcher->wait_until_settled(std::chrono::seconds(120)), "it ends");
  expect(f.launcher->state_of(name) == secure_frontend::CardState::running, "the card runs");
  expect(std::filesystem::exists(f.result_file(name)), "it was calibrated first");
  expect(has_pair(dumped_argv(f.dump), "--cpu-threads", f.winner), "and runs with the result: " + joined(dumped_argv(f.dump)));
}

// The owner (2026-10-29): "starting an uncalibrated server recommend calibrating first and when answered yes, it will
// calibrate. However, during calibration, allow users to cancel calibration if they choose to."
// The answer "Start without calibrating" is a start told not to (calibrate false): a card that would calibrate before
// its first start (Gemma 4) starts at once, with nothing measured.
void a_start_without_calibrating_starts_at_once() {
  CalibrationFixture f;
  const std::string name = "other-gemma-Q8_0";
  expect(!f.launcher->start(name, nullptr, false, false).has_value(), "the start is accepted");
  expect(f.launcher->wait_until_settled(std::chrono::seconds(120)), "it ends");
  expect(f.launcher->state_of(name) == secure_frontend::CardState::running, "the card runs");
  expect(!std::filesystem::exists(f.result_file(name)), "nothing was calibrated");
  expect(!has_pair(dumped_argv(f.dump), "--cpu-threads", f.winner), "it runs on the card's own settings: " + joined(dumped_argv(f.dump)));
}

// Pinned and hot models (owner 2026-10-31, spec part 3): a card kept loaded is warmed at its start - one short generation
// reaches its engine before any request - and again whenever it has idled for warm_every; a card not kept loaded gets no
// generation until a request.
void a_card_kept_loaded_is_warmed_at_its_start_and_while_idle() {
  const auto record = secure_frontend_test::make_scratch_directory() / "record.txt";
  CalibrationFixture f({"--record-file=" + secure_frontend::path_to_utf8(record)}, false, {}, std::chrono::milliseconds(1500));
  const auto generations = [&record] {
    std::error_code ec;
    if (!std::filesystem::exists(record, ec)) return std::size_t{0};
    std::istringstream in(secure_frontend_test::read_file(record));
    std::size_t count = 0;
    for (std::string line; std::getline(in, line);) count += line.rfind("GEN ", 0) == 0 ? 1U : 0U;
    return count;
  };
  const auto waits_for = [&generations](const std::size_t at_least, const std::chrono::seconds bound) {
    const auto until = std::chrono::steady_clock::now() + bound;
    while (generations() < at_least && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return generations() >= at_least;
  };
  expect(!f.launcher->start("other-gemma-Q8_0", nullptr, false, false).has_value(), "a card as it is starts");
  expect(f.launcher->wait_until_settled(std::chrono::seconds(120)) && f.launcher->state_of("other-gemma-Q8_0") == secure_frontend::CardState::running,
         "it runs");
  std::this_thread::sleep_for(std::chrono::seconds(4));
  expect(generations() == 0U, "no generation reaches a card not kept loaded until a request: " + std::to_string(generations()));
  const auto options = secure_frontend::parse_json(
      R"({"hot":true})", secure_frontend::JsonLimits{.max_bytes = 256U, .max_depth = 4U, .max_container_entries = 8U, .max_string_bytes = 64U});
  expect(!f.launcher->start("tiny-gemma-Q8_0", &std::get<secure_frontend::JsonValue>(options), false, false).has_value(),
         "a card kept loaded starts");
  expect(f.launcher->wait_until_settled(std::chrono::seconds(120)) && f.launcher->state_of("tiny-gemma-Q8_0") == secure_frontend::CardState::running,
         "it runs");
  expect(waits_for(1U, std::chrono::seconds(10)), "it is warmed at its start, before any request: " + std::to_string(generations()));
  const auto first = generations();
  expect(waits_for(first + 1U, std::chrono::seconds(15)), "and again once it has idled: " + std::to_string(generations()));
}

// Pinned and hot models (owner 2026-10-31, spec part 4): a card kept loaded whose engine kept failing (its circuit open)
// is started again after the cool-down (restart_after here 1 s), and runs; a card not kept loaded stays down. The fake
// engine's first three spawns crash after READY (the counter file counts them), so the fourth - the restart's - lives.
void a_card_kept_loaded_is_started_again_after_its_engine_failed() {
  const auto lives = [](const bool hot) {
    const auto dir = secure_frontend_test::make_scratch_directory();
    const auto counter = dir / "spawns.txt";
    CalibrationFixture f({"--crash-first=3", "--counter-file=" + secure_frontend::path_to_utf8(counter), "--crash-delay-ms=300"}, false, {}, {},
                         std::chrono::milliseconds(1000));
    const std::string name = "tiny-gemma-Q8_0";
    const auto options = secure_frontend::parse_json(
        hot ? R"({"hot":true})" : "{}", secure_frontend::JsonLimits{.max_bytes = 256U, .max_depth = 4U, .max_container_entries = 8U, .max_string_bytes = 64U});
    expect(!f.launcher->start(name, &std::get<secure_frontend::JsonValue>(options), false, false).has_value(), "the card starts");
    expect(f.launcher->wait_until_settled(std::chrono::seconds(120)), "its start ends");
    const auto listed_state = [&f, &name] {
      const std::string list = f.launcher->list_json();
      const auto at = list.find("\"name\":\"" + name + "\",\"profile\"");
      const auto state = at == std::string::npos ? std::string::npos : list.find("\"state\":\"", at);
      return state == std::string::npos ? std::string{} : list.substr(state + 9U, list.find('"', state + 9U) - state - 9U);
    };
    const auto spawns = [&counter] {
      std::ifstream in(counter);
      int count = 0;
      in >> count;
      return count;
    };
    bool failed = false;
    for (int i = 0; i < 300 && !failed; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      failed = listed_state() == "failed";
    }
    expect(failed, std::string{hot ? "kept loaded" : "as it is"} + ": its engine kept failing: " + listed_state());
    bool again = false;
    for (int i = 0; i < 200 && !again; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      again = spawns() >= 4 && listed_state() == "running";
    }
    return std::pair<bool, std::vector<std::string>>{again, *f.events};
  };
  const auto [kept, kept_events] = lives(true);
  expect(kept, "a card kept loaded is started again after the cool-down, and runs");
  expect(std::any_of(kept_events.begin(), kept_events.end(), [](const std::string& e) { return e.rfind("engine_stopped: ", 0) == 0; }),
         "its stop was an alert first");
  const auto [plain, plain_events] = lives(false);
  expect(!plain, "a card as it is stays down");
}

// Pinned and hot models (spec part 4): the card kept loaded the owner started is kept in <roots.state>/hot.json (its name),
// forgotten when the owner stops it; a server started again with that state starts it by itself - unless a card is
// pinned, which wins.
void a_card_kept_loaded_starts_again_with_the_server() {
  const auto state = secure_frontend_test::make_scratch_directory() / "state";
  secure_frontend_test::make_owner_private_directory(state);
  const auto hot_file = state / "hot.json";
  const auto boot = [&state](std::map<std::string, std::string> literals) {
    auto options = two_cards({}, {}, std::move(literals));
    options.state_dir = state;
    return ServerProcess::start(options);
  };
  {
    auto server = boot({{"models.control", "true"}});
    expect(server && server->wait_until_listening(std::chrono::seconds(20)), "the launcher boots");
    if (!server) return;
    const auto hot = call(*server, "POST", "/v1/strata/models/start", R"({"name":"Beta","options":{"hot":true}})");
    expect(hot && hot->status == 202 && wait_for_card(*server, "Beta", "running"), "Beta runs, kept loaded: " + shown(hot));
    expect(secure_frontend_test::read_file(hot_file).find("\"Beta\"") != std::string::npos, "hot.json names it");
    const auto stopped = call(*server, "POST", "/v1/strata/models/stop", "{}");
    expect(stopped && stopped->status == 202 && wait_for_card(*server, "Beta", "stopped") && !std::filesystem::exists(hot_file),
           "the owner's stop forgets it");
    const auto again = call(*server, "POST", "/v1/strata/models/start", R"({"name":"Beta","options":{"hot":true}})");
    expect(again && again->status == 202 && wait_for_card(*server, "Beta", "running") && std::filesystem::exists(hot_file),
           "kept loaded again: kept again");
    server->stop(ServerProcess::Stop::ctrl_break);
    expect(server->wait_exit(std::chrono::seconds(20)) == std::optional<int>{0} && std::filesystem::exists(hot_file),
           "the server's own stop keeps it");
  }
  {
    auto server = boot({{"models.control", "true"}});
    expect(server && server->wait_until_listening(std::chrono::seconds(20)), "the launcher boots again");
    if (!server) return;
    expect(wait_for_card(*server, "Beta", "running"), "Beta starts again with the server: " + server->stderr_text());
    server->stop(ServerProcess::Stop::ctrl_break);
    (void)server->wait_exit(std::chrono::seconds(20));
  }
  {
    auto server = boot({{"models.control", "true"}, {"models.start", "\"Alpha\""}});
    expect(server && server->wait_until_listening(std::chrono::seconds(20)), "the launcher boots with a pin");
    if (!server) return;
    expect(wait_for_card(*server, "Alpha", "running") && card_state(*server, "Beta") == "stopped", "the pinned card wins");
    server->stop(ServerProcess::Stop::ctrl_break);
    (void)server->wait_exit(std::chrono::seconds(20));
  }
}

// The start route takes that answer as "calibrate": false, and nothing but a boolean there.
void the_start_route_takes_calibrate_false() {
  ServerOptions options;
  options.model_cards = {CardOptions{.name = "Plain", .profile = "qwen"}};
  options.config_literals = {{"models.control", "true"}};
  auto server = ServerProcess::start(options);
  expect(server && server->wait_until_listening(std::chrono::seconds(20)), "the launcher boots");
  if (!server) return;
  const auto refused = call(*server, "POST", "/v1/strata/models/start", R"({"name":"Plain","calibrate":"no"})");
  expect(refused && refused->status == 400, "calibrate is a boolean: " + shown(refused));
  const auto started = call(*server, "POST", "/v1/strata/models/start", R"({"name":"Plain","calibrate":false})");
  expect(started && started->status == 202, "a start without calibrating is accepted: " + shown(started));
  expect(wait_for_card(*server, "Plain", "running"), "Plain runs: " + server->stderr_text());
  expect(server->stderr_text().find("start Plain") != std::string::npos &&
             server->stderr_text().find("calibrate, then start Plain") == std::string::npos,
         "started, not calibrated: " + server->stderr_text());
}

// The owner (2026-10-29): "clicking calibrate does not stop the current running model and empty the vram before
// running the calibration". It does (the running model is stopped and its engine has ended before the first trial;
// its Monitor readings go), but the Monitor then said an engine was "starting", with no model, for the whole
// calibration, while each trial's engine filled the GPU again. It names the calibration instead.
void the_monitor_names_a_calibration_while_it_runs() {
  CalibrationFixture f({"--ready-delay-ms=400"});
  const std::string name = "tiny-gemma-Q8_0";
  expect(!f.launcher->start("other-gemma-Q8_0", nullptr, false, false).has_value() &&
             f.launcher->wait_until_settled(std::chrono::seconds(60)),
         "another card runs first");
  expect(f.launcher->metrics_json().find("\"model\":\"other-gemma-Q8_0\"") != std::string::npos, "the Monitor names it");
  expect(!f.launcher->calibrate(name, nullptr, false, std::nullopt, false).has_value(), "the calibration is accepted");
  for (int i = 0; i < 200 && f.launcher->list_json().find("\"running\":{\"stage\":") == std::string::npos; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  const std::string during = f.launcher->metrics_json();
  expect(f.launcher->state_of("other-gemma-Q8_0") == secure_frontend::CardState::stopped, "the running card was stopped");
  expect(during.find("\"health\":\"calibrating\"") != std::string::npos &&
             during.find("\"model\":\"tiny-gemma-Q8_0\"") != std::string::npos &&
             during.find("\"state\":\"calibrating\"") != std::string::npos &&
             during.find("other-gemma-Q8_0") == std::string::npos,
         "while the trials run the Monitor names the calibration, and no longer the stopped card: " + during);
  expect(!f.launcher->cancel_calibration().has_value() && f.launcher->wait_until_settled(std::chrono::seconds(60)), "cancelled");
  expect(f.launcher->metrics_json().find("\"health\":\"none\"") != std::string::npos, "then nothing runs");
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

// The owner (2026-10-30): "When calibrating a model and an error occurs, it should create a detailed message of the
// errors encountered along with a detailed list of the hardware, that way a user can submit this as a report for bug
// fixes". A calibration whose engine ends before it is ready writes a report to <logs>/calibration-reports: the card,
// its model file, each trial, the failed one with the server's lines for its engine (its exit code) and the engine's
// own last lines, and the hardware. The list points to it and the launcher hands it out; no path of this PC's folders
// is in it.
void a_failed_calibration_writes_a_report() {
  CalibrationFixture f({"--exit-before-ready=3"}, true);
  const std::string name = "tiny-gemma-Q8_0";
  expect(!f.launcher->calibrate(name, nullptr, false, std::nullopt, false).has_value(), "a calibration is accepted");
  expect(f.launcher->wait_until_settled(std::chrono::seconds(60)), "it ends");
  expect(f.launcher->state_of(name) == secure_frontend::CardState::failed, "the card failed");
  const auto files = f.reports();
  expect(files.size() == 1U, "one report: " + std::to_string(files.size()));
  if (files.size() != 1U) return;
  const std::string file = secure_frontend::path_to_utf8(files.front().filename());
  expect(file.rfind("calibration-" + name + "-", 0) == 0U && file.ends_with(".md"), "its name: " + file);
  const std::string text = secure_frontend_test::read_file(files.front());
  for (const std::string& part : {std::string{"# M-Strata calibration report"}, name + " (gemma4)", std::string{"tiny-gemma-Q8_0.gguf, "},
                                 std::string{"the calibration failed"}, std::string{"| 1 | defaults |"},
                                 std::string{"failed: the engine ended before it was ready"}, std::string{"exit code 3"},
                                 std::string{"diagnostic: private engine startup"}, std::string{"Test CPU, 8 threads (4 physical cores)"},
                                 std::string{"GPU 0: Test GPU, 8192 MiB (512 MiB in use), driver 999.1"}, std::string{"Test OS 1.0"}}) {
    expect(text.find(part) != std::string::npos, "the report has " + part + ": " + text);
  }
  expect(text.find(secure_frontend::path_to_utf8(f.root)) == std::string::npos, "no path of this PC's folders: " + text);
  const auto handed = f.launcher->calibration_report(name);
  expect(handed.has_value() && handed->file == file && handed->text == text, "the launcher hands it out");
  const std::string listed = f.launcher->list_json();
  expect(listed.find("\"report\":{\"file\":\"" + file + "\"") != std::string::npos, "the list points to it: " + listed);
  expect(!f.launcher->calibration_report("other-gemma-Q8_0").has_value(), "none for a card that was not calibrated");
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

// Upstream's low-RAM mode at a start (owner 2026-10-31, the Azure VM: "Yes, build it"): a Qwen3.8 card as a download
// makes it (card_for_pack: its pack, the model folder's first shard, the expert profile; 128K, int8), this PC's RAM and
// GPUs given (LauncherSettings::host_memory), the launcher's lines and every engine command recorded.
struct QwenPackFixture final {
  std::filesystem::path root = secure_frontend_test::make_scratch_directory();
  std::filesystem::path models = root / "models";
  std::filesystem::path pack = root / "packs" / "q";
  secure_frontend::DataFolder data = secure_frontend::data_folder_layout(root / "data");
  std::shared_ptr<std::mutex> mutex = std::make_shared<std::mutex>();
  std::shared_ptr<std::vector<std::vector<std::string>>> commands = std::make_shared<std::vector<std::vector<std::string>>>();
  std::shared_ptr<std::vector<std::string>> lines = std::make_shared<std::vector<std::string>>();
  test_support::FakeFactory fakes{[](test_support::FakeChild& child, std::uint64_t) { child.push_line("READY 32768 stop"); }};
  std::unique_ptr<secure_frontend::ModelLauncher> launcher;
  const std::string name = "Q";

  // extra_args: more of the card's own engine.args (after its pack's); qwen36: a Qwen3.6 card from its GGUF instead (no
  // pack: upstream's engine reads the file itself, its draft layer --mtp 2)
  QwenPackFixture(const std::uint64_t ram_bytes, const std::vector<std::uint64_t>& vram, const std::vector<std::string>& extra_args = {},
                  const bool qwen36 = false) {
    secure_frontend_test::make_private_directory(models);
    secure_frontend_test::make_private_directory(root / "packs");
    secure_frontend_test::make_private_directory(pack);
    secure_frontend_test::write_file(models / "q-00001-of-00002.gguf",
                                     test_support::gguf_with_tokenizer(test_support::byte_tokenizer(), "gpt2", "qwen4exp")
                                         .u32("qwen4exp.expert_count", 512U)
                                         .bytes());
    secure_frontend_test::write_file(models / "q-00002-of-00002.gguf", "shard two");
    // the VM pack's own header: 35,454,976,000 bytes of experts
    secure_frontend_test::write_file(pack / "native_experts.txt",
                                     "# strata native experts v3: layer gu_type d_type offset blob_bytes gate_off up_off down_off [shard] "
                                     "(n_expert 512, total 35454976000; absolute offsets in q-00001-of-00002.gguf, or in the named shard beside it)\n");
    const auto bin = root / "bin";
    secure_frontend_test::make_private_directory(bin);
    const std::filesystem::path fake{STRATA_FAKE_ENGINE_PATH};
    const auto engine = bin / fake.filename();
    std::error_code ec;
    std::filesystem::copy_file(fake, engine, ec);
    secure_frontend_test::make_private_directory(bin / "profiles");
    secure_frontend_test::write_file(bin / "profiles" / "expert-profile.bin", "profile");
    (void)secure_frontend::ensure_data_folder(data, engine);
    const auto main = std::get<secure_frontend::FrontendConfigDraft>(
        secure_frontend::parse_config_text(secure_frontend_test::read_file(data.config_file), data.root));
    auto made = secure_frontend::card_for_pack({.family = secure_frontend::PackedFamily::qwen38, .name = name, .engine = engine,
                                                .source = "download:q", .pack = pack, .folder = models, .model_id = "q-00001-of-00002",
                                                .expert_profile = bin / "profiles" / "expert-profile.bin"});
    if (qwen36) {
      secure_frontend_test::write_file(models / "Q.gguf", test_support::gguf_with_tokenizer(test_support::byte_tokenizer(), "gpt2", "qwen35moe")
                                                              .u32("qwen35moe.context_length", 262144U)
                                                              .u32("qwen35moe.nextn_predict_layers", 1U)
                                                              .bytes());
      const auto header = secure_frontend::parse_gguf_metadata(secure_frontend_test::read_file(models / "Q.gguf"));
      const auto gguf = secure_frontend::card_for_gguf(std::get<secure_frontend::GgufMetadata>(header),
                                                       {.folder = models, .file = "Q.gguf", .engine = engine, .source = "folder"});
      made = std::get<secure_frontend::CardText>(gguf);
      for (const auto& a : extra_args) made.json.insert(made.json.find("\"2\"]") + 3U, ", \"" + a + "\"");
    } else if (!extra_args.empty()) {
      std::string more;
      for (const auto& a : extra_args) more += ",\"" + a + "\"";
      made.json.insert(made.json.find("]},\"roots\""), more);
    }
    auto parsed = secure_frontend::parse_model_card(made.json, data.root);
    secure_frontend::LauncherSettings settings{.main = main, .cards = {}, .operator_cards = 0U, .memory_file = {}, .gate = {}};
    settings.log = [mutex = mutex, lines = lines](const std::string_view line) {
      std::lock_guard lock(*mutex);
      lines->emplace_back(line);
    };
    settings.factory = [mutex = mutex, commands = commands, fakes = fakes](secure_frontend::EngineLaunchPlan plan) {
      {
        std::lock_guard lock(*mutex);
        commands->push_back(plan.command.arguments);
      }
      return fakes.factory();
    };
    settings.host_memory = [ram_bytes, vram] {
      secure_frontend::HostMemory facts;
      facts.ram_bytes = ram_bytes;
      for (unsigned i = 0; i < vram.size(); ++i) facts.gpus.push_back({.index = i, .name = "Test GPU", .mem_total_bytes = vram[i]});
      return facts;
    };
    launcher = std::make_unique<secure_frontend::ModelLauncher>(std::move(settings));
    expect(std::holds_alternative<secure_frontend::ModelCard>(parsed) && launcher->add_card(std::get<secure_frontend::ModelCard>(std::move(parsed))),
           "the Qwen3.8 card is added");
  }
  ~QwenPackFixture() { launcher->shutdown(); }
  QwenPackFixture(const QwenPackFixture&) = delete;
  QwenPackFixture& operator=(const QwenPackFixture&) = delete;
  // the engine command of a start with these options (nullptr: none), the card stopped after
  [[nodiscard]] std::vector<std::string> started(const secure_frontend::JsonValue* options = nullptr) {
    const bool ran = !launcher->start(name, options, false).has_value() && launcher->wait_until_settled(std::chrono::seconds(30)) &&
                     launcher->state_of(name) == secure_frontend::CardState::running;
    expect(ran, "the card runs");
    (void)launcher->stop(false);
    expect(launcher->wait_until_settled(std::chrono::seconds(30)), "it stops");
    std::lock_guard lock(*mutex);
    return commands->empty() ? std::vector<std::string>{} : commands->back();
  }
  [[nodiscard]] std::string said(const std::string_view part) const {
    std::lock_guard lock(*mutex);
    for (const auto& line : *lines) {
      if (line.find(part) != std::string::npos) return line;
    }
    return {};
  }
};

bool has_argument(const std::vector<std::string>& argv, const std::string_view flag) {
  return std::find(argv.begin(), argv.end(), flag) != argv.end();
}

// The owner's VM: 27.4 GiB of RAM and a 16 GiB T4 cannot hold the pack's 35.45 GB of experts with 10 GB beside them, and
// the rest the T4 does not hold (about 26 GB) does not fit the RAM either: the card offers low_ram (auto) and starts
// with --mmap-experts, and the launcher says why. A start's auto stays auto (what is remembered is not the PC's answer).
void a_card_short_of_ram_starts_in_the_low_ram_mode() {
  QwenPackFixture f(static_cast<std::uint64_t>(27.4 * static_cast<double>(1ULL << 30U)), {16ULL << 30U});
  const std::string listed = f.launcher->list_json();
  expect(listed.find(R"("name":"low_ram","kind":"choice")") != std::string::npos &&
             listed.find(R"("choices":["auto","on","resident","mmap","off"],"value":"auto")") != std::string::npos,
         "the card offers the low-RAM mode, auto: " + listed);
  const auto argv = f.started();
  expect(has_argument(argv, "--mmap-experts") && !has_argument(argv, "--resident-experts"), "it starts mapped: " + joined(argv));
  expect(f.said("low-RAM mode").find("--mmap-experts") != std::string::npos, "the log says so: " + f.said("low-RAM mode"));
  expect(f.launcher->list_json().find(R"("choices":["auto","on","resident","mmap","off"],"value":"auto")") != std::string::npos,
         "auto is what is remembered");
  const auto off = json_of(R"({"low_ram":"off"})");
  const auto kept = f.started(&off);
  expect(!has_argument(kept, "--mmap-experts") && !has_argument(kept, "--resident-experts"), "off keeps them in RAM: " + joined(kept));
}

// Where the RAM holds what the GPU does not (32 GiB and 24 GiB: about 18 GB left, the card's expert profile there) the
// resident variant; where it holds them all with room beside (64 GiB), neither flag.
void a_card_with_room_keeps_its_experts_in_ram() {
  QwenPackFixture resident(32ULL << 30U, {24ULL << 30U});
  const auto copied = resident.started();
  expect(has_argument(copied, "--resident-experts") && !has_argument(copied, "--mmap-experts"), "32 GiB + 24 GiB: resident: " + joined(copied));
  QwenPackFixture roomy(64ULL << 30U, {24ULL << 30U});
  const auto whole = roomy.started();
  expect(!has_argument(whole, "--mmap-experts") && !has_argument(whole, "--resident-experts"), "64 GiB: neither: " + joined(whole));
}

// VRAM kept free (owner 2026-11-01, the Azure VM's T4: "it also may need low vram mode"): a Qwen3.6 card is offered
// upstream setup's --vram-reserve-mib, Off by default (the engine's own placement). Its engine has no such flag, so a
// start works the experts' share out from the GPU it runs on as upstream's setup does - the VRAM less 3 GiB and the
// reserve, at least 0 - and gives it as --expert-vram GiB, and says so. A Qwen3.8 pack's card is not offered it (its
// strata core has the server's engine.vram_reserve_mib row).
void a_qwen36_card_keeps_vram_free() {
  QwenPackFixture f(static_cast<std::uint64_t>(27.4 * static_cast<double>(1ULL << 30U)), {16ULL << 30U}, {}, true);
  const std::string listed = f.launcher->list_json();
  expect(listed.find(R"("name":"vram_reserve","kind":"integer")") != std::string::npos, "the card offers VRAM kept free: " + listed);
  const auto plain = f.started();
  expect(!has_argument(plain, "--expert-vram") && !has_argument(plain, "--vram-reserve-mib"), "Off: neither flag: " + joined(plain));
  const auto some = json_of(R"({"vram_reserve":1536})");
  const auto kept = f.started(&some);
  expect(has_pair(kept, "--expert-vram", "11.5") && !has_argument(kept, "--vram-reserve-mib"),
         "16 GiB less 3 and 1.5: 11.5 GiB for the experts: " + joined(kept));
  expect(f.said("VRAM kept free").find("11.5 GiB") != std::string::npos, "the log says so: " + f.said("VRAM kept free"));
  const auto all = json_of(R"({"vram_reserve":8192})");
  QwenPackFixture tight(32ULL << 30U, {8ULL << 30U}, {}, true);
  expect(has_pair(tight.started(&all), "--expert-vram", "0"), "a reserve past the card: 0, as upstream's max(..., 0)");
  QwenPackFixture none(32ULL << 30U, {}, {}, true);
  const auto blind = none.started(&some);
  expect(!has_argument(blind, "--expert-vram") && !has_argument(blind, "--vram-reserve-mib"), "no GPU read: no flag: " + joined(blind));
  expect(!none.said("no GPU").empty(), "and the log says why");
  QwenPackFixture pack(64ULL << 30U, {24ULL << 30U});
  expect(pack.launcher->list_json().find(R"("name":"vram_reserve")") == std::string::npos, "a Qwen3.8 pack card is not offered it");
}

// A card whose own engine.args already choose (an operator's --mmap-experts) keeps its choice and is not offered one.
void a_card_that_fixes_its_experts_flag_keeps_it() {
  QwenPackFixture f(64ULL << 30U, {24ULL << 30U}, {"--mmap-experts"});
  expect(f.launcher->list_json().find(R"("name":"low_ram")") == std::string::npos, "no low_ram on it: " + f.launcher->list_json());
  const auto argv = f.started();
  expect(std::count(argv.begin(), argv.end(), "--mmap-experts") == 1 && !has_argument(argv, "--resident-experts"),
         "its own flag, once: " + joined(argv));
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
  const auto history=f.data.state/"calibration"/("benchmark-"+secure_frontend::to_hex(secure_frontend::sha256(name)).substr(0,24)+".history.json");
  expect(std::filesystem::exists(history),"calibrated card has restorable history");
  const auto held = f.launcher->card(name);
  expect(held.has_value() && f.launcher->remove_card(name) == secure_frontend::CardChange::done, "the card is removed");
  expect(!std::filesystem::exists(f.result_file(name)), "and its calibration with it");
  expect(!std::filesystem::exists(history),"removal also forgets benchmark history");
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
  secure_frontend::ModelCard fast;
  fast.name = "Fast";
  fast.profile = secure_frontend::EngineProfile::minimax;
  fast.config.entries["engine.fast_math"] = {true, secure_frontend::ConfigSource::file};
  settings.cards.push_back(fast);
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
  // (a Qwen card without --pack runs a GGUF on the Qwen3.6 engine: it offers VRAM kept free, Off - owner 2026-11-01)
  const std::string qspeed = " speed_route_tail_skip=0 speed_route_resident=0 speed_route_prior=0 speed_route_resident_mtp=false speed_hc_req8=false speed_spec_prob=false speed_fast_math=false";
  const std::string glmspeed = " speed_glm_trunk_keep=8 speed_glm_disk_skip_from=8 speed_glm_disk_skip_w=0 speed_glm_miss_skip_w=0 speed_glm_mtp_keep=8 speed_glm_defer_cpu=false speed_fast_math=false";
  const std::string tailspeed = " speed_route_tail_skip=0 speed_fast_math=false";
  const std::string dsspeed = " speed_ds4_disk_skip_w=0 speed_ds4_ram_skip_w=0 speed_ds4_dspark_sample=false speed_ds4_yoco=1 speed_ds4_yoco_keep=0 speed_fast_math=false";
  expect(defaults_of("Q") == "vram_reserve=0 temperature=1 top_p=0.95 top_k=20 answer_minutes=30 hot=false gpus=\"auto\"" + qspeed,
         "Qwen3.6 (upstream's card): " + defaults_of("Q"));
  expect(defaults_of("G") == "ram_headroom=16 temperature=1 top_p=0.95 answer_minutes=30 hot=false gpus=\"auto\"" + glmspeed, "GLM-5.3 (Maya's): " + defaults_of("G"));
  expect(defaults_of("R") == "ram_headroom=16 temperature=1 top_p=0.95 top_k=20 answer_minutes=30 hot=false gpus=\"auto\"" + glmspeed, "raon1 (KRAFTON's): " + defaults_of("R"));
  expect(defaults_of("F") == "temperature=1 top_p=0.95 top_k=20 answer_minutes=30 hot=false gpus=\"auto\"" + tailspeed, "flint (its card's): " + defaults_of("F"));
  expect(defaults_of("E") == "temperature=1 top_p=0.95 top_k=64 min_p=0.05 answer_minutes=30 hot=false gpus=\"auto\"" + tailspeed, "Gemma 4 (Google's): " + defaults_of("E"));
  expect(defaults_of("D") == "temperature=1 top_p=1 answer_minutes=30 hot=false gpus=\"auto\"" + dsspeed, "DeepSeek-V4.1 (upstream's card): " + defaults_of("D"));
  expect(defaults_of("M") == "temperature=1 top_p=0.95 answer_minutes=30 hot=false gpus=\"auto\" speed_fast_math=false", "MiniMax (its card's): " + defaults_of("M"));
  expect(defaults_of("Own") ==
             "vram_reserve=0 thinking=\"high\" temperature=0.6 top_p=0.95 top_k=20 vision=\"gpu\" answer_minutes=30 hot=false gpus=\"auto\"" + qspeed,
         "a card's own temperature stays, the rest in the profile's order: " + defaults_of("Own"));
  expect(defaults_of("Rows") == "ram_headroom=16 temperature=0.8 top_p=0.95 top_k=40 answer_minutes=30 hot=false gpus=\"auto\"" + glmspeed,
         "a card's own defaults.sampling rows are its defaults: " + defaults_of("Rows"));
  expect(defaults_of("Fast") == "temperature=1 top_p=0.95 answer_minutes=30 hot=false gpus=\"auto\" speed_fast_math=true",
         "a card's explicit compiled variant remains its visible default: " + defaults_of("Fast"));
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
  // Explicit zero must reach Maya as well: omission plus other sampler keys can
  // activate temperature-one sampling. Require the exact greedy wire value.
  expect(generations[1].find(" temperature=0 top_p=0.95 top_k=20") != std::string::npos,
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
      {"a_start_says_its_phase_and_time_and_a_failure_its_code", a_start_says_its_phase_and_time_and_a_failure_its_code},
      {"the_settings_say_the_running_models_values", the_settings_say_the_running_models_values},
      {"options_are_remembered", options_are_remembered},
      {"fast_math_variant_is_selected_before_engine_start", fast_math_variant_is_selected_before_engine_start},
      {"card_speed_options_reach_child_and_are_remembered", card_speed_options_reach_child_and_are_remembered},
      {"models_start_and_control_off", models_start_and_control_off},
      {"experts_show_as_soon_as_a_model_runs", experts_show_as_soon_as_a_model_runs},
      {"downloads_serve_the_catalog_and_load_download_cards", downloads_serve_the_catalog_and_load_download_cards},
      {"cards_can_be_added_while_serving", cards_can_be_added_while_serving},
      {"a_card_is_pinned_and_its_removal_unpins_it", a_card_is_pinned_and_its_removal_unpins_it},
      {"a_pinned_card_starts_with_the_server_and_the_pin_is_kept", a_pinned_card_starts_with_the_server_and_the_pin_is_kept},
      {"a_key_holder_gets_a_fresh_login_link", a_key_holder_gets_a_fresh_login_link},
      {"a_card_kept_loaded_never_unloads_when_idle", a_card_kept_loaded_never_unloads_when_idle},
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
      {"calibration_keeps_the_options_it_measured", calibration_keeps_the_options_it_measured},
      {"benchmark_quality_failure_preserves_previous_calibration", benchmark_quality_failure_preserves_previous_calibration},
      {"benchmark_resume_refuses_unmatched_checkpoint_and_restore_is_bounded", benchmark_resume_refuses_unmatched_checkpoint_and_restore_is_bounded},
      {"benchmark_success_preserves_private_input_and_restores_history", benchmark_success_preserves_private_input_and_restores_history},
      {"benchmark_cancel_resume_is_bound_to_all_options", benchmark_cancel_resume_is_bound_to_all_options},
      {"benchmark_route_accepts_bounded_custom_input_and_rejects_mixed_actions", benchmark_route_accepts_bounded_custom_input_and_rejects_mixed_actions},
      {"benchmark_rejects_runtime_changes_during_trials", benchmark_rejects_runtime_changes_during_trials},
      {"a_first_start_calibrates_first", a_first_start_calibrates_first},
      {"a_start_without_calibrating_starts_at_once", a_start_without_calibrating_starts_at_once},
      {"a_card_kept_loaded_is_warmed_at_its_start_and_while_idle", a_card_kept_loaded_is_warmed_at_its_start_and_while_idle},
      {"a_card_kept_loaded_is_started_again_after_its_engine_failed", a_card_kept_loaded_is_started_again_after_its_engine_failed},
      {"a_card_kept_loaded_starts_again_with_the_server", a_card_kept_loaded_starts_again_with_the_server},
      {"the_start_route_takes_calibrate_false", the_start_route_takes_calibrate_false},
      {"the_monitor_names_a_calibration_while_it_runs", the_monitor_names_a_calibration_while_it_runs},
      {"a_minimax_card_measures_its_prompt_reading_first", a_minimax_card_measures_its_prompt_reading_first},
      {"a_glm_card_calibrates", a_glm_card_calibrates},
      {"a_failed_prefill_calibration_is_not_retried_by_itself", a_failed_prefill_calibration_is_not_retried_by_itself},
      {"a_prefill_calibration_falls_back_to_the_default_chunk", a_prefill_calibration_falls_back_to_the_default_chunk},
      {"a_calibration_can_be_cancelled", a_calibration_can_be_cancelled},
      {"removing_a_card_removes_its_calibration", removing_a_card_removes_its_calibration},
      {"a_card_short_of_ram_starts_in_the_low_ram_mode", a_card_short_of_ram_starts_in_the_low_ram_mode},
      {"a_qwen36_card_keeps_vram_free", a_qwen36_card_keeps_vram_free},
      {"a_card_with_room_keeps_its_experts_in_ram", a_card_with_room_keeps_its_experts_in_ram},
      {"a_card_that_fixes_its_experts_flag_keeps_it", a_card_that_fixes_its_experts_flag_keeps_it},
      {"a_stop_waits_for_an_engine_still_ending", a_stop_waits_for_an_engine_still_ending},
      {"failures_raise_alerts", failures_raise_alerts},
      {"a_failed_calibration_writes_a_report", a_failed_calibration_writes_a_report},
      {"bad_card_refused_at_boot_and_main_rows_refused", bad_card_refused_at_boot_and_main_rows_refused},
  }, failures);
}
