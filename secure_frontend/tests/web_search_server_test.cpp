// Web search, phase 4 (docs/superpowers/specs/2026-10-27-web-search-design.md §4), as a tool plugin since 2026-10-30
// (plugin spec section 10): the whole chain through the real server - the search plugin in the plugins folder (its
// strata-search, its settings plugins.search.*), the server's tool hub, a fake SearXNG on loopback, and a fake engine
// whose first reply calls web_search and whose second answers - for a complete and a streamed chat. Also: a config's old
// search.* rows moved at the start, and a third-party tool plugin that runs once it is approved. Built only with
// SECURE_FRONTEND_WEB_SEARCH (strata-search).
#include "secure_frontend/config_file.hpp"
#include "secure_frontend/crt_environment.hpp"
#include "support/fake_searxng.hpp"
#include "support/server_harness.hpp"
#include "support/stream_client.hpp"
#include "support/test_support.hpp"
#include "support/test_paths.hpp"
#include "support/test_tokenizer.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
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

void append_bytes(std::vector<std::size_t>& ids, const std::string_view text) {
  for (const char c : text) ids.push_back(test_support::id_for_byte(static_cast<unsigned char>(c)));
}

std::string csv(const std::vector<std::size_t>& ids) {
  std::string out;
  for (const std::size_t id : ids) out += (out.empty() ? "" : ",") + std::to_string(id);
  return out;
}

constexpr std::size_t kImEnd = 257U;  // the byte pack's <|im_end|>

// The first round: it reasons, then calls web_search. The second: it reasons, then answers.
std::vector<std::size_t> calling_reply() {
  std::vector<std::size_t> ids;
  append_bytes(ids, "Need to search.");
  ids.push_back(test_support::kToolPackThinkClose);
  ids.push_back(test_support::kToolPackCallOpen);
  append_bytes(ids, "\n<function=web_search>\n<parameter=query>\nrust 1.90\n</parameter>\n</function>\n");
  ids.push_back(test_support::kToolPackCallClose);
  ids.push_back(kImEnd);
  return ids;
}
std::vector<std::size_t> answering_reply() {
  std::vector<std::size_t> ids;
  append_bytes(ids, "Found it.");
  ids.push_back(test_support::kToolPackThinkClose);
  append_bytes(ids, "Rust 1.90.0 is out.");
  ids.push_back(kImEnd);
  return ids;
}

#if defined(_WIN32)
const std::string kSearchProgram = "strata-search.exe";
const std::string kLookupProgram = "lookup.exe";
#else
const std::string kSearchProgram = "strata-search";
const std::string kLookupProgram = "lookup";
#endif

std::string read_text(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// The search plugin as make ships it: its manifest from the repository, strata-search in its folder, a first-party one.
test_support::ServerOptions::Plugin search_plugin() {
  return {.id = "search", .manifest = read_text(std::string{SECURE_FRONTEND_REPO_PLUGINS} + "/search/plugin.json"), .on = true,
          .files = {{kSearchProgram, std::filesystem::path{STRATA_SEARCH_PATH}}}};
}

// Its settings: the instance (and the results per search, when given).
std::map<std::string, std::string> search_rows(const std::string& url, const std::string& results = {}) {
  std::map<std::string, std::string> rows{{"plugins.search.url", "\"" + url + "\""}};
  if (!results.empty()) rows["plugins.search.results"] = results;
  return rows;
}

std::string local_url(const test_support::FakeSearxng& searxng) { return "http://127.0.0.1:" + std::to_string(searxng.port()); }

// A GEN line's prompt as text: the tool pack's ids read back (bytes, then its special tokens by name).
std::string prompt_of(const std::string& gen) {
  static const std::map<std::size_t, std::string> kSpecials{
      {256U, "<|im_start|>"}, {257U, "<|im_end|>"},  {259U, "<think>"},          {260U, "</think>"},
      {261U, "<tool_call>"},  {262U, "</tool_call>"}, {263U, "<tool_response>"}, {264U, "</tool_response>"}};
  std::string text;
  std::stringstream ids(gen.substr(gen.rfind(' ') + 1U));
  for (std::string id; std::getline(ids, id, ',');) {
    const auto value = std::stoul(id);
    if (const auto special = kSpecials.find(value); special != kSpecials.end()) text += special->second;
    else if (value < 256U) text += static_cast<char>(test_support::byte_for_id(value));
  }
  return text;
}

std::vector<std::string> gen_lines(const std::filesystem::path& record) {
  std::vector<std::string> lines;
  std::istringstream in(secure_frontend_test::read_file(record));
  for (std::string line; std::getline(in, line);) {
    if (line.rfind("GEN ", 0) == 0) lines.push_back(line);
  }
  return lines;
}

void a_chat_searches_through_strata_search_and_answers() {
  test_support::FakeSearxng searxng;
  const auto record = test_support::make_temp_dir("search") / "record.txt";
  test_support::ServerOptions options;
  options.tool_tokenizer = true;
  options.engine_args = {"--token-ids=" + csv(calling_reply()), "--token-ids-next=" + csv(answering_reply()),
                         "--record-file=" + test_support::utf8_path(record)};
  options.plugins = {search_plugin()};
  options.config_literals = search_rows(local_url(searxng), "2");
  auto server = test_support::ServerProcess::start(options);
  const bool ready = server != nullptr && server->wait_for_health(test_support::kHealthReady, 20s);
  expect(ready, "the server starts with the search plugin" + (server ? ": " + server->stderr_text() : std::string{}));
  if (!ready) return;

  const auto complete = test_support::chat(*server, test_support::chat_body(R"("max_tokens":512)", "What is new in Rust?"), {.timeout = 60s});
  expect(complete.head.status == 200, "the complete chat answers: " + std::to_string(complete.head.status) + " " + complete.body);
  const auto& body = complete.body;
  expect(body.find(R"("content":"Rust 1.90.0 is out.")") != std::string::npos, "the answer is the second round's: " + body);
  expect(body.find(R"("reasoning_content":"Need to search.Found it.")") != std::string::npos, "the reasoning is every round's");
  expect(body.find(R"("strata_search":[{"event":"call","round":1,"name":"web_search","arguments":{"query":"rust 1.90"}},)"
                   R"({"event":"result","round":1,"ok":true,"text":"Web search results for \"rust 1.90\" (SearXNG):)") != std::string::npos,
         "the call and its result are reported");
  const std::string tokens = std::to_string(calling_reply().size() + answering_reply().size());
  expect(body.find("\"completion_tokens\":" + tokens + ",") != std::string::npos, "usage counts both rounds' tokens");
  const auto searched = searxng.requests();
  expect(searched == std::vector<std::string>{"GET /search?q=rust%201.90&format=json HTTP/1.1"}, "strata-search asked the instance once");
  const auto gens = gen_lines(record);
  expect(gens.size() == 2U, "two generations: " + std::to_string(gens.size()));
  if (gens.size() == 2U) {
    const std::string first = prompt_of(gens[0]);
    const std::string second = prompt_of(gens[1]);
    expect(first.find("# Tools\n\nYou have access to the following functions:\n\n<tools>\n{\"name\": \"web_search\"") != std::string::npos &&
               first.find("Today's date is ") != std::string::npos,
           "the first prompt offers web_search, with today's date");
    expect(second.rfind(first.substr(0, first.size() - std::string{"<|im_start|>assistant\n<think>\n"}.size()), 0) == 0U,
           "the second prompt goes on from the first");
    expect(second.find("<|im_start|>assistant\n<think>\nNeed to search.\n</think>\n\n<tool_call>\n<function=web_search>\n"
                       "<parameter=query>\nrust 1.90\n</parameter>\n</function>\n</tool_call><|im_end|>\n<|im_start|>user\n"
                       "<tool_response>\nWeb search results for \"rust 1.90\" (SearXNG):") != std::string::npos,
           "then the round: the call, then its result as a tool response");
  }

  const auto streamed = test_support::chat(*server, test_support::chat_body(R"("max_tokens":512,"stream":true)", "And now?"), {.timeout = 60s});
  expect(streamed.end == secure_frontend::ChatExchangeEnd::complete && streamed.done_seen, "the streamed chat completes");
  expect(test_support::stream_text(streamed) == "Rust 1.90.0 is out.", "only the answer streams as content: " + test_support::stream_text(streamed));
  expect(test_support::stream_finish(streamed) == "stop", "it ends as stop");
  std::size_t search_chunks = 0;
  for (const auto& item : streamed.items) {
    if (item.kind == secure_frontend::SseItemKind::data && item.text.find(R"("strata_search":{"event":)") != std::string::npos) ++search_chunks;
    expect(item.text.find("<tool_call>") == std::string::npos, "no chunk carries the call's text");
  }
  expect(search_chunks == 2U, "a call chunk and a result chunk: " + std::to_string(search_chunks));
  expect(searxng.requests().size() == 2U && gen_lines(record).size() == 4U, "one more search, two more generations");
  expect(server->stderr_text().find("[strata] plugins: search is running") != std::string::npos, "the log says the plugin runs");
}

// Owner 2026-10-28 (the chat's web search checkbox): "strata_search": false is a chat without the tool - no tools in
// the prompt, no call parsed, no search asked - on a server whose search is on.
void a_chat_can_turn_search_off() {
  test_support::FakeSearxng searxng;
  const auto record = test_support::make_temp_dir("search-off") / "record.txt";
  test_support::ServerOptions options;
  options.tool_tokenizer = true;
  options.engine_args = {"--token-ids=" + csv(answering_reply()), "--record-file=" + test_support::utf8_path(record)};
  options.plugins = {search_plugin()};
  options.config_literals = search_rows(local_url(searxng));
  auto server = test_support::ServerProcess::start(options);
  const bool ready = server != nullptr && server->wait_for_health(test_support::kHealthReady, 20s);
  expect(ready, "the server starts with the search plugin" + (server ? ": " + server->stderr_text() : std::string{}));
  if (!ready) return;
  const auto complete =
      test_support::chat(*server, test_support::chat_body(R"("max_tokens":512,"strata_search":false)", "What is new in Rust?"), {.timeout = 60s});
  expect(complete.head.status == 200 && complete.body.find(R"("content":"Rust 1.90.0 is out.")") != std::string::npos,
         "the chat answers: " + std::to_string(complete.head.status) + " " + complete.body);
  expect(complete.body.find("strata_search") == std::string::npos, "no search is reported");
  expect(searxng.requests().empty(), "the instance is never asked");
  const auto gens = gen_lines(record);
  expect(gens.size() == 1U && prompt_of(gens[0]).find("# Tools") == std::string::npos && prompt_of(gens[0]).find("web_search") == std::string::npos,
         "one generation, its prompt without the tool");
}

// Owner 2026-10-28 ("Flint did not either"): a model whose prompt cannot carry the tool (its vocabulary lacks the tool
// tags; flint's lacked a <tools> marker) is served without search - and the server log says so, once, instead of
// leaving it silent.
void a_model_that_cannot_carry_the_tool_says_so() {
  test_support::FakeSearxng searxng;
  test_support::ServerOptions options;  // the plain byte pack: ChatML, no tool tags
  options.engine_args = {"--token-ids=" + csv({test_support::id_for_byte('O'), test_support::id_for_byte('k'), kImEnd})};
  options.plugins = {search_plugin()};
  options.config_literals = search_rows(local_url(searxng));
  auto server = test_support::ServerProcess::start(options);
  const bool ready = server != nullptr && server->wait_for_health(test_support::kHealthReady, 20s);
  expect(ready, "the server starts with the search plugin" + (server ? ": " + server->stderr_text() : std::string{}));
  if (!ready) return;
  for (int i = 0; i < 2; ++i) {
    const auto complete = test_support::chat(*server, test_support::chat_body(R"("max_tokens":16)", "What is new?"), {.timeout = 60s});
    expect(complete.head.status == 200 && complete.body.find("strata_search") == std::string::npos,
           "the chat is answered without search: " + std::to_string(complete.head.status) + " " + complete.body);
  }
  expect(searxng.requests().empty(), "the instance is never asked");
  const std::string log = server->stderr_text();
  const std::string line = "[strata] plugins: this model's prompt cannot carry the plugins' tools";
  const auto first = log.find(line);
  expect(first != std::string::npos && log.find(line, first + 1U) == std::string::npos, "the log says so, once: " + log);
}

// The GUI-first server: a card started from the page gets the tool too (the launcher hands it to each model it starts).
void a_started_card_searches_too() {
  test_support::FakeSearxng searxng;
  const auto record = test_support::make_temp_dir("search-card") / "record.txt";
  test_support::ServerOptions options;
  options.tool_tokenizer = true;
  options.model_cards = {test_support::CardOptions{.name = "Qwen", .profile = "qwen"}};
  options.fake_engine_options = {"--token-ids=" + csv(calling_reply()), "--token-ids-next=" + csv(answering_reply()),
                                 "--record-file=" + test_support::utf8_path(record)};
  options.plugins = {search_plugin()};
  options.config_literals = search_rows(local_url(searxng));
  options.config_literals["models.control"] = "true";
  auto server = test_support::ServerProcess::start(options);
  const bool listening = server != nullptr && server->wait_until_listening(20s);
  expect(listening, "the launcher boots with the search plugin" + (server ? ": " + server->stderr_text() : std::string{}));
  if (!listening) return;
  const auto started = test_support::http_request(server->port(), "POST", "/v1/strata/models/start", R"({"name":"Qwen"})", 10s);
  expect(started && started->status == 202, "the card's start is accepted: " + (started ? started->body : std::string{"(no answer)"}));
  bool running = false;
  for (int i = 0; i < 300 && !running; ++i) {
    const auto listed = test_support::http_request(server->port(), "GET", "/v1/strata/models", "", 10s);
    running = listed && listed->body.find("\"state\":\"running\"") != std::string::npos;
    if (!running) std::this_thread::sleep_for(100ms);
  }
  expect(running, "the card runs: " + server->stderr_text());
  if (!running) return;
  const std::string body = R"({"model":"Qwen","messages":[{"role":"user","content":"What is new in Rust?"}],"max_tokens":512})";
  const auto answered = test_support::chat(*server, body, {.timeout = 60s});
  expect(answered.head.status == 200 && answered.body.find(R"("content":"Rust 1.90.0 is out.")") != std::string::npos &&
             answered.body.find(R"("strata_search":[{"event":"call")") != std::string::npos,
         "the started model searched and answered: " + answered.body);
  expect(searxng.requests().size() == 1U && gen_lines(record).size() == 2U, "one search, two generations");
}

// Owner 2026-10-28 ("a toggle in the card"): the card's Web search checkbox off, the started model gets no tool - its
// chats are served without it, however they ask.
void a_card_started_without_web_search_never_searches() {
  test_support::FakeSearxng searxng;
  const auto record = test_support::make_temp_dir("search-card-off") / "record.txt";
  test_support::ServerOptions options;
  options.tool_tokenizer = true;
  options.model_cards = {test_support::CardOptions{.name = "Qwen", .profile = "qwen"}};
  options.fake_engine_options = {"--token-ids=" + csv(answering_reply()), "--record-file=" + test_support::utf8_path(record)};
  options.plugins = {search_plugin()};
  options.config_literals = search_rows(local_url(searxng));
  options.config_literals["models.control"] = "true";
  auto server = test_support::ServerProcess::start(options);
  const bool listening = server != nullptr && server->wait_until_listening(20s);
  expect(listening, "the launcher boots with the search plugin" + (server ? ": " + server->stderr_text() : std::string{}));
  if (!listening) return;
  const auto listed = test_support::http_request(server->port(), "GET", "/v1/strata/models", "", 10s);
  expect(listed && listed->body.find(R"({"name":"web_search","kind":"boolean","label":"Web search",)") != std::string::npos,
         "the card offers the checkbox: " + (listed ? listed->body : std::string{"(no answer)"}));
  const auto started =
      test_support::http_request(server->port(), "POST", "/v1/strata/models/start", R"({"name":"Qwen","options":{"web_search":false}})", 10s);
  expect(started && started->status == 202, "the start with it off is accepted: " + (started ? started->body : std::string{"(no answer)"}));
  bool running = false;
  for (int i = 0; i < 300 && !running; ++i) {
    const auto now = test_support::http_request(server->port(), "GET", "/v1/strata/models", "", 10s);
    running = now && now->body.find("\"state\":\"running\"") != std::string::npos;
    if (!running) std::this_thread::sleep_for(100ms);
  }
  expect(running, "the card runs: " + server->stderr_text());
  if (!running) return;
  const std::string body = R"({"model":"Qwen","messages":[{"role":"user","content":"What is new in Rust?"}],"max_tokens":512})";
  const auto answered = test_support::chat(*server, body, {.timeout = 60s});
  expect(answered.head.status == 200 && answered.body.find(R"("content":"Rust 1.90.0 is out.")") != std::string::npos &&
             answered.body.find("strata_search") == std::string::npos,
         "the model answered without searching: " + answered.body);
  const auto gens = gen_lines(record);
  expect(searxng.requests().empty() && gens.size() == 1U && prompt_of(gens[0]).find("# Tools") == std::string::npos,
         "no search, one generation, no tool in its prompt");
}

// A real instance in place of the fake one (owner 2026-10-27: a SearXNG deployed to test): the server's strata-search
// asks it, and its results reach the second prompt as the tool response. SECURE_FRONTEND_SEARXNG_URL names the
// instance (its JSON output on); unset, skipped.
void a_real_searxng_answers_through_the_server() {
  const char* url = secure_frontend::crt_getenv("SECURE_FRONTEND_SEARXNG_URL");
  if (url == nullptr || *url == '\0') {
    std::cout << "SKIPPED: SECURE_FRONTEND_SEARXNG_URL names no SearXNG instance\n";
    return;
  }
  const auto record = test_support::make_temp_dir("search-real") / "record.txt";
  test_support::ServerOptions options;
  options.tool_tokenizer = true;
  options.engine_args = {"--token-ids=" + csv(calling_reply()), "--token-ids-next=" + csv(answering_reply()),
                         "--record-file=" + test_support::utf8_path(record)};
  options.plugins = {search_plugin()};
  options.config_literals = search_rows(url, "3");
  auto server = test_support::ServerProcess::start(options);
  const bool ready = server != nullptr && server->wait_for_health(test_support::kHealthReady, 20s);
  expect(ready, "the server starts with the instance" + (server ? ": " + server->stderr_text() : std::string{}));
  if (!ready) return;
  const auto complete = test_support::chat(*server, test_support::chat_body(R"("max_tokens":512)", "What is new in Rust?"), {.timeout = 90s});
  expect(complete.head.status == 200 && complete.body.find(R"("content":"Rust 1.90.0 is out.")") != std::string::npos,
         "the chat answers after its search: " + std::to_string(complete.head.status) + " " + complete.body);
  expect(complete.body.find(R"({"event":"result","round":1,"ok":true,"text":"Web search results for \"rust 1.90\" (SearXNG):\n\n1. )") !=
             std::string::npos,
         "the instance's results come back: " + complete.body.substr(0, 600));
  const auto gens = gen_lines(record);
  expect(gens.size() == 2U, "two generations: " + std::to_string(gens.size()));
  if (gens.size() == 2U) {
    const std::string second = prompt_of(gens[1]);
    const auto response = second.find("<tool_response>\nWeb search results for \"rust 1.90\" (SearXNG):\n\n1. ");
    expect(response != std::string::npos && second.find("https://", response) != std::string::npos &&
               second.find("</tool_response>", response) != std::string::npos,
           "the second prompt carries them as the tool response");
    if (response != std::string::npos) std::cout << "the tool response read: " << second.substr(response, 400) << '\n';
  }
}

// Web search became a plugin (owner 2026-10-30; plugin spec section 10): a config's old search.* rows move at the start -
// the instance and results to plugins.search.*, search.max_rounds to tools.max_rounds, search.enabled to the search
// plugin's on or off (plugins.json) - in the config file too, and the log says so. On, the chat searches as before; off,
// the plugin does not run and no chat gets the tool.
void old_search_rows_move_to_the_plugin() {
  for (const bool on : {true, false}) {
    const std::string which = on ? "search.enabled true: " : "search.enabled false: ";
    test_support::FakeSearxng searxng;
    const auto record = test_support::make_temp_dir("search-moved") / "record.txt";
    test_support::ServerOptions options;
    options.tool_tokenizer = true;
    options.engine_args = on ? std::vector<std::string>{"--token-ids=" + csv(calling_reply()), "--token-ids-next=" + csv(answering_reply()),
                                                        "--record-file=" + test_support::utf8_path(record)}
                             : std::vector<std::string>{"--token-ids=" + csv(answering_reply()), "--record-file=" + test_support::utf8_path(record)};
    options.plugins = {search_plugin()};
    options.config_literals = {{"search.enabled", on ? "true" : "false"},
                               {"search.url", "\"" + local_url(searxng) + "\""},
                               {"search.results", "2"},
                               {"search.max_rounds", "3"}};
    auto server = test_support::ServerProcess::start(options);
    const bool ready = server != nullptr && server->wait_for_health(test_support::kHealthReady, 20s);
    expect(ready, which + "the server starts" + (server ? ": " + server->stderr_text() : std::string{}));
    if (!ready) continue;
    const std::string log = server->stderr_text();
    expect(log.find("[strata] config: web search is a plugin now: search.enabled, search.max_rounds, search.results, search.url moved "
                    "to plugins.search.* and tools.max_rounds") != std::string::npos,
           which + "the log says what moved: " + log);
    expect(log.find(std::string{"[strata] plugins: the search plugin is "} + (on ? "on" : "off") + ", as search.enabled was") != std::string::npos,
           which + "and the plugin's choice");
    const auto file = secure_frontend::read_config_file(server->root() / "config.json");
    const auto* draft = std::get_if<secure_frontend::FrontendConfigDraft>(&file);
    expect(draft != nullptr, which + "the config file reads back");
    if (draft != nullptr) {
      const auto row = [draft](const std::string& key) { return draft->entries.find(key); };
      expect(row("search.url") == draft->entries.end() && row("search.enabled") == draft->entries.end() &&
                 row("search.max_rounds") == draft->entries.end() && row("search.results") == draft->entries.end(),
             which + "no search.* row is left");
      expect(row("plugins.search.url") != draft->entries.end() &&
                 std::get<std::string>(row("plugins.search.url")->second.value) == local_url(searxng) &&
                 row("plugins.search.results") != draft->entries.end() &&
                 std::get<std::int64_t>(row("plugins.search.results")->second.value) == 2 && row("tools.max_rounds") != draft->entries.end() &&
                 std::get<std::int64_t>(row("tools.max_rounds")->second.value) == 3,
             which + "they are the plugin's settings and tools.max_rounds now");
    }
    const std::string choices = read_text(server->root() / "state" / "plugins.json");
    expect(choices.find(std::string{R"("enabled":{"search":)"} + (on ? "true" : "false") + "}") != std::string::npos,
           which + "plugins.json holds the choice: " + choices);
    const auto complete = test_support::chat(*server, test_support::chat_body(R"("max_tokens":512)", "What is new in Rust?"), {.timeout = 60s});
    expect(complete.head.status == 200 && complete.body.find(R"("content":"Rust 1.90.0 is out.")") != std::string::npos,
           which + "the chat answers: " + complete.body);
    expect((complete.body.find(R"("strata_search":[{"event":"call")") != std::string::npos) == on &&
               searxng.requests().size() == (on ? 1U : 0U),
           which + (on ? "it searched once" : "it never searched"));
  }
}

// A third-party tool plugin (owner 2026-10-30: they run "Now"): in the plugins folder but not one make built, it does not
// run until the owner approves its files (plugins.json's pin); approved, its tool reaches the model like the search
// plugin's, its program started with its own arguments from its settings.
void a_third_party_tool_plugin_runs_once_approved() {
  const std::string manifest = R"({"schema": "m-strata.plugin/1", "id": "lookup", "kind": "tool", "name": "Lookup", "version": "1.0.0",
  "publisher": "Example Labs", "license": "MIT", "about": "A stand-in tool.",
  "files": {"windows": ["lookup.exe"], "linux": ["lookup"]},
  "tool": {"program": {"windows": "lookup.exe", "linux": "lookup"},
    "arguments": ["--tool", "web_search", "--label", "{label}"],
    "network": "none", "note": "Use web_search.",
    "settings": [{"name": "label", "kind": "text", "label": "Label", "help": "", "default": "plain"}]}})";
  const auto program_sum = test_support::file_sha256_hex(FAKE_MCP_TOOL_PATH);
  expect(program_sum.has_value(), "the fake tool program is there");
  if (!program_sum) return;
  for (const bool approved : {false, true}) {
    const std::string which = approved ? "approved: " : "not approved: ";
    const auto record = test_support::make_temp_dir("lookup") / "record.txt";
    test_support::ServerOptions options;
    options.tool_tokenizer = true;
    options.engine_args = approved ? std::vector<std::string>{"--token-ids=" + csv(calling_reply()), "--token-ids-next=" + csv(answering_reply()),
                                                              "--record-file=" + test_support::utf8_path(record)}
                                   : std::vector<std::string>{"--token-ids=" + csv(answering_reply()), "--record-file=" + test_support::utf8_path(record)};
    options.plugins = {{.id = "lookup", .manifest = manifest, .on = true, .files = {{kLookupProgram, std::filesystem::path{FAKE_MCP_TOOL_PATH}}},
                        .first_party = false}};
    if (approved) {
      options.plugin_choices = R"({"schema":"m-strata.plugin-choices/1","approved":{"lookup":{"manifest":")" + test_support::sha256_hex(manifest) +
                               R"(","files":{")" + kLookupProgram + R"(":")" + *program_sum + R"("}}},"enabled":{}})";
    }
    auto server = test_support::ServerProcess::start(options);
    const bool ready = server != nullptr && server->wait_for_health(test_support::kHealthReady, 20s);
    expect(ready, which + "the server starts" + (server ? ": " + server->stderr_text() : std::string{}));
    if (!ready) continue;
    const auto complete = test_support::chat(*server, test_support::chat_body(R"("max_tokens":512)", "What is new in Rust?"), {.timeout = 60s});
    expect(complete.head.status == 200 && complete.body.find(R"("content":"Rust 1.90.0 is out.")") != std::string::npos,
           which + "the chat answers: " + complete.body);
    const auto gens = gen_lines(record);
    if (!approved) {
      expect(gens.size() == 1U && prompt_of(gens[0]).find("# Tools") == std::string::npos && complete.body.find("strata_search") == std::string::npos,
             which + "it does not run: no tool in the prompt, no call");
      continue;
    }
    expect(complete.body.find(R"("strata_search":[{"event":"call","round":1,"name":"web_search","arguments":{"query":"rust 1.90"}},)") != std::string::npos &&
               complete.body.find(R"(web_search called with {\"query\": \"rust 1.90\"}; args: --tool web_search --label plain; token: none)") !=
                   std::string::npos,
           which + "its program answered the call, started with its arguments: " + complete.body);
    expect(gens.size() == 2U && prompt_of(gens[0]).find("Use web_search.") != std::string::npos, which + "its note reaches the prompt");
  }
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
  WSADATA data{};
  (void)WSAStartup(MAKEWORD(2, 2), &data);  // the fake SearXNG listens in this process
#endif
  return secure_frontend_test::run_cases(argc, argv, {
      {"a_chat_searches_through_strata_search_and_answers", a_chat_searches_through_strata_search_and_answers},
      {"a_chat_can_turn_search_off", a_chat_can_turn_search_off},
      {"a_model_that_cannot_carry_the_tool_says_so", a_model_that_cannot_carry_the_tool_says_so},
      {"a_started_card_searches_too", a_started_card_searches_too},
      {"a_card_started_without_web_search_never_searches", a_card_started_without_web_search_never_searches},
      {"a_real_searxng_answers_through_the_server", a_real_searxng_answers_through_the_server},
      {"old_search_rows_move_to_the_plugin", old_search_rows_move_to_the_plugin},
      {"a_third_party_tool_plugin_runs_once_approved", a_third_party_tool_plugin_runs_once_approved},
  }, failures);
}
