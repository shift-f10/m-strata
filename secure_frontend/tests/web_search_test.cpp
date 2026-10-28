// Web search (docs/superpowers/specs/2026-10-27-web-search-design.md §2, phase 1): strata-search's SearXNG client and
// its MCP server - the URL rules, the one GET, the bounded reply, SearXNG's JSON as the model's text, the JSON-RPC
// messages upstream's MCP client sends, and the program over stdio against a fake SearXNG on loopback.
#include "secure_frontend/crt_environment.hpp"
#include "secure_frontend/engine_stdio_process.hpp"
#include "secure_frontend/mcp_client.hpp"
#include "secure_frontend/first_start.hpp"
#include "secure_frontend/search_mcp_server.hpp"
#include "secure_frontend/search_routes.hpp"
#include "secure_frontend/search_service.hpp"
#include "secure_frontend/web_search.hpp"
#include "support/fake_searxng.hpp"
#include "support/process_runner.hpp"
#include "support/test_support.hpp"

#include <atomic>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#include <winsock2.h>
#endif

namespace {

int failures = 0;
void expect(const bool ok, const std::string& what) {
  if (!ok) {
    ++failures;
    std::cerr << "FAILED: " << what << '\n';
  }
}

using secure_frontend::SearchOrigin;
using test_support::FakeSearxng;
using test_support::kSearxngReply;


void search_urls_are_checked() {
  const auto ok = [](const std::string_view url, const bool tls, const std::string& host, const std::uint16_t port,
                     const std::string& path) {
    const auto o = secure_frontend::parse_search_url(url);
    return o && o->tls == tls && o->host == host && o->port == port && o->base_path == path;
  };
  expect(ok("http://127.0.0.1:8888", false, "127.0.0.1", 8888, ""), "http with a port");
  expect(ok("HTTPS://Search.Example.ORG/searxng/", true, "search.example.org", 443, "/searxng"), "https, a path, its slash dropped");
  expect(ok("http://searx.lan", false, "searx.lan", 80, ""), "the default port");
  expect(ok("http://[::1]:8080/", false, "::1", 8080, ""), "a bracketed IPv6 address");
  for (const char* bad : {"ftp://x", "http://", "http://user@host", "http://host:0", "http://host:70000", "http://host:",
                          "http://host/a?b=c", "http://host/#x", "http://ho st", "http:/host", "host:8888", "http://host//x",
                          "http://[::1", "javascript:alert(1)"}) {
    expect(!secure_frontend::parse_search_url(bad).has_value(), std::string{"refused: "} + bad);
  }
}

void the_request_is_one_get_percent_encoded() {
  const auto origin = *secure_frontend::parse_search_url("http://127.0.0.1:8888/searx");
  expect(secure_frontend::search_request(origin, "rust 1.90 & \xC3\xA9t\xC3\xA9?") ==
             "GET /searx/search?q=rust%201.90%20%26%20%C3%A9t%C3%A9%3F&format=json HTTP/1.1\r\nHost: 127.0.0.1:8888\r\n"
             "Accept: application/json\r\nUser-Agent: strata-search\r\nConnection: close\r\n\r\n",
         "one GET: the query percent-encoded (UTF-8), the host and its port, JSON asked for, the connection closed after");
  expect(secure_frontend::search_request(*secure_frontend::parse_search_url("https://s.example"), "a").find("\r\nHost: s.example\r\n") !=
             std::string::npos,
         "a default port is not named");
}

void replies_are_read_by_length_chunks_or_close() {
  using secure_frontend::HttpReply;
  constexpr std::size_t kMax = 1U << 20U;
  const auto body_of = [](const std::optional<secure_frontend::HttpReplyResult>& r) {
    return r && std::holds_alternative<HttpReply>(*r) ? std::get<HttpReply>(*r).body : std::string{"<none>"};
  };
  const std::string head = "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\n";
  expect(!secure_frontend::parse_http_reply(head + "Content-Length: 5\r\n\r\nab", false, kMax).has_value(), "a short body waits");
  const auto whole = secure_frontend::parse_http_reply(head + "Content-Length: 5\r\n\r\nabcdeXX", false, kMax);
  expect(body_of(whole) == "abcde" && std::get<HttpReply>(*whole).status == 200 &&
             std::get<HttpReply>(*whole).content_type == "application/json; charset=utf-8",
         "Content-Length: the body, its status and type");
  expect(body_of(secure_frontend::parse_http_reply(head + "Transfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n2;x=y\r\nde\r\n0\r\n\r\n", false, kMax)) ==
             "abcde",
         "chunked: the chunks joined");
  expect(!secure_frontend::parse_http_reply(head + "Transfer-Encoding: chunked\r\n\r\n3\r\nab", false, kMax).has_value(), "a cut chunk waits");
  expect(body_of(secure_frontend::parse_http_reply(head + "\r\nall of it", true, kMax)) == "all of it", "neither: the body up to the close");
  expect(!secure_frontend::parse_http_reply(head + "\r\nmore to come", false, kMax).has_value(), "neither: waits for the close");
  const auto refused = [](const std::optional<secure_frontend::HttpReplyResult>& r) { return r && std::holds_alternative<std::string>(*r); };
  expect(refused(secure_frontend::parse_http_reply(head + "Content-Length: 5\r\n\r\nab", true, kMax)), "closed early: refused");
  expect(refused(secure_frontend::parse_http_reply("SSH-2.0-OpenSSH\r\n\r\n", true, kMax)), "not HTTP: refused");
  expect(refused(secure_frontend::parse_http_reply(head + "Content-Length: 5\r\n\r\n" + std::string(100, 'x'), false, 64U)), "too large: refused");
  expect(refused(secure_frontend::parse_http_reply(head + "Content-Length: 5x\r\n\r\nabcde", false, kMax)), "a bad length: refused");
}

void searxng_json_becomes_numbered_results() {
  const auto text = secure_frontend::format_search_results(kSearxngReply, "rust 1.90", 5U);
  expect(text.index() == 0U && std::get<0>(text) ==
                                                          "Web search results for \"rust 1.90\" (SearXNG):\n"
                                                          "\nAnswer: Rust 1.90.0 was released on 18 September 2026\n"
                                                          "\n1. Announcing Rust 1.90.0\n   https://blog.rust-lang.org/2026/09/18/Rust-1.90.0/\n"
                                                          "   Published: 2026-09-18T00:00:00\n"
                                                          "   The Rust team is happy to announce a new version, 1.90.0.\n"
                                                          "\n2. Rust Release Notes\n   https://doc.rust-lang.org/stable/releases.html\n",
         "the answers, then each result with a URL: title, URL, date, the snippet's whitespace tidied: " +
             (text.index() == 0U ? std::get<0>(text) : std::get<1>(text)));
  const auto one = secure_frontend::format_search_results(kSearxngReply, "rust 1.90", 1U);
  expect(one.index() == 0U && std::get<0>(one).find("\n2. ") == std::string::npos, "at most max_results");
  const auto none = secure_frontend::format_search_results(R"({"results": []})", "q", 5U);
  expect(none.index() == 0U && std::get<0>(none).find("No results.") != std::string::npos, "no results: said so");
  // owner 2026-10-28 (the model kept retrying searches the engines refused): an empty search says which engines did not
  // answer and why, as SearXNG's unresponsive_engines tells it
  const auto refused = secure_frontend::format_search_results(
      R"({"results": [], "unresponsive_engines": [["brave", "Suspended: too many requests"], ["duckduckgo", "CAPTCHA"], ["x", 7]]})", "q", 5U);
  expect(refused.index() == 0U &&
             std::get<0>(refused).find("\nNo results: the search engines did not answer (brave: Suspended: too many requests; "
                                       "duckduckgo: CAPTCHA), so searching again now is unlikely to help.\n") != std::string::npos,
         "an empty search names the engines that refused it: " + (refused.index() == 0U ? std::get<0>(refused) : std::get<1>(refused)));
  const auto partial = secure_frontend::format_search_results(
      R"({"results": [{"url": "https://x", "title": "t"}], "unresponsive_engines": [["brave", "CAPTCHA"]]})", "q", 5U);
  expect(partial.index() == 0U && std::get<0>(partial).find("brave") == std::string::npos, "with results, the refusals are left out");
  const std::string long_snippet = "{\"results\": [{\"url\": \"https://x\", \"title\": \"t\", \"content\": \"" + std::string(900, 'a') + "\"}]}";
  const auto cut = secure_frontend::format_search_results(long_snippet, "q", 5U);
  expect(cut.index() == 0U && std::get<0>(cut).find(std::string(500, 'a') + "...") != std::string::npos &&
             std::get<0>(cut).find(std::string(501, 'a')) == std::string::npos,
         "a snippet is cut at 500 bytes");
  for (const char* bad : {"<html>", "{\"no\": 1}", "[1]", "{\"results\": 3}"}) {
    expect(secure_frontend::format_search_results(bad, "q", 5U).index() == 1U, std::string{"not SearXNG's JSON: "} + bad);
  }
}

class ScriptedTransport final : public secure_frontend::SmtpTransport {
 public:
  explicit ScriptedTransport(std::deque<std::string> chunks, std::string* written) : chunks_(std::move(chunks)), written_(written) {}
  bool write(const std::string_view bytes) override {
    *written_ += bytes;
    return true;
  }
  std::optional<std::string> read_line() override { return std::nullopt; }
  bool start_tls() override { return false; }
  std::optional<std::string> read_chunk() override {
    if (chunks_.empty()) return std::nullopt;
    auto next = std::move(chunks_.front());
    chunks_.pop_front();
    return next;
  }

 private:
  std::deque<std::string> chunks_;
  std::string* written_;
};

void a_search_writes_one_request_and_reads_the_reply() {
  secure_frontend::SearchSettings settings;
  settings.origin = *secure_frontend::parse_search_url("http://127.0.0.1:8888");
  std::string written;
  const auto search = [&](std::deque<std::string> chunks) {
    written.clear();
    return secure_frontend::web_search(settings, "rust 1.90", 5U,
                                       [&](const SearchOrigin& origin, std::chrono::seconds, std::string*) {
                                         expect(origin.host == "127.0.0.1" && origin.port == 8888, "the configured origin only");
                                         return std::make_unique<ScriptedTransport>(std::move(chunks), &written);
                                       });
  };
  const std::string reply = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n" + [] {
    std::ostringstream hex;
    hex << std::hex << kSearxngReply.size();
    return hex.str();
  }() + "\r\n" + kSearxngReply + "\r\n0\r\n\r\n";
  const auto found = search({reply.substr(0, 40), reply.substr(40)});
  expect(found.index() == 0U && std::get<0>(found).find("1. Announcing Rust 1.90.0") != std::string::npos,
         "a reply in pieces: the results: " + (found.index() == 0U ? std::get<0>(found) : std::get<1>(found)));
  expect(written == secure_frontend::search_request(settings.origin, "rust 1.90"), "exactly the one GET was written");
  const auto failed = [&](const std::string& head) {
    const auto r = search({head + "Content-Length: 0\r\n\r\n"});
    return r.index() == 1U ? std::get<1>(r) : std::string{};
  };
  expect(failed("HTTP/1.1 302 Found\r\nLocation: http://elsewhere/\r\n").find("redirect") != std::string::npos, "a redirect is not followed");
  expect(failed("HTTP/1.1 403 Forbidden\r\n").find("search.formats") != std::string::npos, "403: JSON is not enabled there");
  expect(failed("HTTP/1.1 500 Oops\r\n").find("HTTP 500") != std::string::npos, "another status: named");
  expect(secure_frontend::web_search(settings, "", 5U, nullptr).index() == 1U &&
             secure_frontend::web_search(settings, std::string(513, 'q'), 5U, nullptr).index() == 1U,
         "an empty or overlong query is refused before anything connects");
  expect(secure_frontend::web_search(settings, "q", 5U, [](const SearchOrigin&, std::chrono::seconds, std::string*) {
           return std::unique_ptr<secure_frontend::SmtpTransport>{};
         }).index() == 1U,
         "an instance that cannot be reached: refused");
}

void the_mcp_server_answers_upstreams_client() {
  std::vector<std::pair<std::string, std::size_t>> asked;
  secure_frontend::SearchMcpServer server(
      [&asked](const std::string_view query, const std::size_t results) -> secure_frontend::SearchText {
        asked.emplace_back(std::string(query), results);
        if (query == "down") return secure_frontend::SearchText{std::in_place_index<1>, "the search instance could not be reached"};
        return secure_frontend::SearchText{std::in_place_index<0>, "RESULTS for " + std::string(query)};
      },
      5U);
  const auto one = [&server](const std::string& line) {
    const auto out = server.handle(line);
    return out.size() == 1U ? out.front() : std::string{"<" + std::to_string(out.size()) + " lines>"};
  };
  // upstream serve/mcp.py's handshake
  expect(one(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},)"
             R"("clientInfo":{"name":"strata","title":"Strata","version":"1"}}})") ==
             R"({"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2025-06-18","capabilities":{"tools":{}},)"
             R"("serverInfo":{"name":"strata-search","version":"1"}}})",
         "initialize: the protocol version, tools, its name");
  expect(server.handle(R"({"jsonrpc":"2.0","method":"notifications/initialized"})").empty(), "a notification has no reply");
  const std::string list = one(R"({"jsonrpc":"2.0","id":"l1","method":"tools/list","params":{}})");
  expect(list.rfind(R"({"jsonrpc":"2.0","id":"l1","result":{"tools":[{"name":"web_search","description":"Search the web)", 0) == 0U &&
             list.find(R"("inputSchema":{"type":"object","properties":{"query":{"type":"string")") != std::string::npos &&
             list.find(R"("required":["query"])") != std::string::npos,
         "tools/list: web_search, when to use it, its schema: " + list);
  expect(one(R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"web_search","arguments":{"query":"rust 1.90","max_results":3}}})") ==
             R"({"jsonrpc":"2.0","id":2,"result":{"content":[{"type":"text","text":"RESULTS for rust 1.90"}],"isError":false}})",
         "tools/call: the search's text");
  expect(asked.size() == 1U && asked[0] == std::pair<std::string, std::size_t>{"rust 1.90", 3U}, "the query and the count reach the search");
  (void)one(R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"web_search","arguments":{"query":"q"}}})");
  expect(asked.size() == 2U && asked[1].second == 5U, "no max_results: the default");
  expect(one(R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"web_search","arguments":{"query":"down"}}})") ==
             R"({"jsonrpc":"2.0","id":4,"result":{"content":[{"type":"text","text":"The search failed: the search instance could not be reached"}],"isError":true}})",
         "a failed search is a tool error the model reads");
  expect(one(R"({"jsonrpc":"2.0","id":5,"method":"ping"})") == R"({"jsonrpc":"2.0","id":5,"result":{}})", "ping");
  const auto code_of = [&one](const std::string& line) {
    const std::string out = one(line);
    const auto at = out.find("\"code\":");
    return at == std::string::npos ? 0 : std::atoi(out.c_str() + at + 7);
  };
  expect(code_of(R"({"jsonrpc":"2.0","id":6,"method":"resources/list"})") == -32601, "another method: -32601");
  expect(code_of("not json") == -32700, "not JSON: -32700");
  expect(code_of(R"([{"jsonrpc":"2.0","id":7,"method":"ping"}])") == -32600, "a batch: -32600 (2025-06-18 has none)");
  expect(code_of(R"({"jsonrpc":"1.0","id":8,"method":"ping"})") == -32600, "not JSON-RPC 2.0: -32600");
  expect(code_of(R"({"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"fetch","arguments":{}}})") == -32602, "another tool: -32602");
  expect(code_of(R"({"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"web_search","arguments":{}}})") == -32602, "no query: -32602");
  expect(code_of(R"({"jsonrpc":"2.0","id":11,"method":"tools/call","params":{"name":"web_search","arguments":{"query":"q","max_results":11}}})") ==
             -32602,
         "max_results past 10: -32602");
  expect(asked.size() == 3U, "no refused call reached the search");
}


// The program itself: upstream's client's messages on stdin, one reply a line on stdout, the search a real GET.
void strata_search_answers_over_stdio() {
  const std::filesystem::path program = secure_frontend_test::extra_arguments().at(0);
  FakeSearxng searxng;
  const auto dir = secure_frontend_test::make_scratch_directory();
  secure_frontend_test::write_file(dir / "in.jsonl",
                                   R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{}}})" "\n"
                                   R"({"jsonrpc":"2.0","method":"notifications/initialized"})" "\n"
                                   R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})" "\n"
                                   R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"web_search","arguments":{"query":"rust 1.90"}}})" "\n");
  const auto result = secure_frontend_test::run_to_completion(
      {.executable = program, .arguments = {"--url", "http://127.0.0.1:" + std::to_string(searxng.port()), "--results", "1"},
       .stdin_file = dir / "in.jsonl"});
  expect(result.exit_code == 0, "strata-search ends at its input's end: " + std::to_string(result.exit_code) + " " + result.err);
  std::vector<std::string> lines;
  for (std::size_t at = 0; at < result.out.size();) {
    const auto end = result.out.find('\n', at);
    lines.push_back(result.out.substr(at, end - at));
    at = end == std::string::npos ? result.out.size() : end + 1U;
  }
  expect(lines.size() == 3U && lines[0].find("\"protocolVersion\":\"2025-06-18\"") != std::string::npos &&
             lines[1].find("\"web_search\"") != std::string::npos,
         "one reply a line: initialize, tools/list, the call: " + result.out);
  expect(lines.size() == 3U && lines[2].find(R"(1. Announcing Rust 1.90.0\n   https://blog.rust-lang.org/2026/09/18/Rust-1.90.0/)") !=
                                   std::string::npos &&
             lines[2].find("2. Rust Release Notes") == std::string::npos && lines[2].find("\"isError\":false") != std::string::npos,
         "the call's text is the fake's first result (--results 1): " + (lines.size() == 3U ? lines[2] : result.out));
  expect(searxng.requests() == std::vector<std::string>{"GET /search?q=rust%201.90&format=json HTTP/1.1"}, "one GET reached the instance");
  expect(result.out.find('\r') == std::string::npos, "lines end in LF only");
  const auto bad = secure_frontend_test::run_to_completion({.executable = program, .arguments = {"--url", "ftp://x"}});
  expect(bad.exit_code == 2 && bad.err.find("--url") != std::string::npos, "a bad URL: exit 2, said so");
}

// Phase 2's whole path: the server's MCP client on the pipes of strata-search started as the server starts a child
// (EngineStdioProcess, no --serve, the closed environment), its answer one line far past an engine record (ten results
// with long snippets), read whole.
void the_client_runs_strata_search_on_its_pipes() {
  std::string results;
  for (int i = 0; i < 10; ++i) {
    if (i != 0) results += ",";
    results += "{\"url\": \"https://example.org/" + std::to_string(i) + "/" + std::string(180, 'u') + "\", \"title\": \"" +
               std::string(150, 't') + "\", \"content\": \"" + std::string(600, 'c') + "\"}";
  }
  FakeSearxng searxng("{\"results\": [" + results + "]}");
  secure_frontend::EngineCommand command;
  command.executable = secure_frontend_test::extra_arguments().at(0);
  command.arguments = {"--url", "http://127.0.0.1:" + std::to_string(searxng.port()), "--results", "10"};
  command.insert_serve = false;
  command.max_read_line_bytes = 1U << 20U;
#if defined(_WIN32)
  for (const char* name : {"SystemRoot", "WINDIR", "SystemDrive"}) {
    if (const char* value = secure_frontend::crt_getenv(name)) command.environment.emplace(name, value);
  }
#else
  command.environment.emplace("PATH", "/usr/bin:/bin");
#endif
  secure_frontend::EngineStdioProcess process;
  expect(!process.start(command).has_value(), "strata-search starts as the server's child");
  secure_frontend::McpClient client(process, {});
  const auto failure = client.start();
  expect(!failure.has_value() && client.tools().size() == 1U && client.tools()[0].name == "web_search",
         "the handshake over the pipes lists web_search: " + failure.value_or(""));
  const auto found = client.call("web_search", R"({"query":"many"})", {});
  expect(found.ok && found.text.find("\n10. " + std::string(150, 't')) != std::string::npos && found.text.size() > 8192U,
         "ten long results come back whole (" + std::to_string(found.text.size()) + " bytes): " + found.text.substr(0, 200));
  process.close_stdin();
  expect(process.terminate(std::chrono::seconds(5)), "it ends when the server is done");
}

// Phase 4: the server's tool - strata-search started on first use, web_search offered as the template shows it (its own
// name, description and schema; today's date after the description), a call run over the pipes, and a program that
// cannot start leaves chats without search for a while instead of failing them.
void the_search_service_offers_web_search_and_runs_it() {
  FakeSearxng searxng;
  secure_frontend::SearchServiceSettings settings;
  settings.program = secure_frontend_test::extra_arguments().at(0);
  settings.url = "http://127.0.0.1:" + std::to_string(searxng.port());
  settings.results = 3U;
  settings.timeout = std::chrono::seconds(10);
#if defined(_WIN32)
  for (const char* name : {"SystemRoot", "WINDIR", "SystemDrive"}) {
    if (const char* value = secure_frontend::crt_getenv(name)) settings.environment.emplace(name, value);
  }
#else
  settings.environment.emplace("PATH", "/usr/bin:/bin");
#endif
  settings.today = [] { return std::string{"2026-10-27"}; };
  std::vector<std::string> lines;
  settings.log = [&lines](const std::string_view line) { lines.emplace_back(line); };
  secure_frontend::SearchService service(settings);
  const auto offer = service.offer();
  expect(offer.has_value(), "web_search is offered");
  if (offer.has_value()) {
    expect(offer->declaration.name == "web_search" && offer->declaration.string_parameters == std::vector<std::string>{"query"},
           "its declaration: query is the string parameter");
    expect(offer->template_json.rfind(R"({"name": "web_search", "description": "Search the web through a SearXNG instance)", 0) == 0U &&
               offer->template_json.find(R"( Then answer from the results and cite the URLs you used. Today's date is 2026-10-27.", )"
                                         R"("parameters": {"type": "object", "properties": {"query": {"type": "string")") != std::string::npos,
           "as the template's tojson writes it, the date after the description: " + offer->template_json);
  }
  const secure_frontend::ToolCall call{"web_search", {{"query", secure_frontend::JsonValue::make_string("rust 1.90")},
                                                      {"max_results", secure_frontend::JsonValue::make_integer(2)}}};
  const auto found = service.run(call, [] { return false; });
  expect(found.ok && found.text.find("1. Announcing Rust 1.90.0") != std::string::npos && found.chars == found.text.size(),
         "the call comes back as the model reads it: " + found.text.substr(0, 120));
  expect(searxng.requests().size() == 1U && searxng.requests()[0] == "GET /search?q=rust%201.90&format=json HTTP/1.1",
         "one GET to the instance");
  expect(service.offer().has_value() && lines.size() == 1U, "the running program is reused (one start line)");

  secure_frontend::SearchServiceSettings missing = settings;
  missing.program = std::filesystem::path(secure_frontend_test::extra_arguments().at(0)).parent_path() / "no-such-strata-search";
  lines.clear();
  secure_frontend::SearchService broken(missing);
  expect(!broken.offer().has_value() && !broken.offer().has_value() && lines.size() == 1U,
         "a program that does not start: no tool, and no second try at once");
  const auto refused = broken.run(call, [] { return false; });
  expect(!refused.ok && refused.text.rfind("error: ", 0) == 0U, "a call then fails as text the model can read");
}

// A search that is still running (another chat's) does not hold up the next chat's offer: the listed tool stands.
void a_running_search_does_not_hold_up_the_offer() {
  FakeSearxng slow(kSearxngReply, std::chrono::milliseconds(2000));
  secure_frontend::SearchServiceSettings settings;
  settings.program = secure_frontend_test::extra_arguments().at(0);
  settings.url = "http://127.0.0.1:" + std::to_string(slow.port());
  settings.timeout = std::chrono::seconds(10);
#if defined(_WIN32)
  for (const char* name : {"SystemRoot", "WINDIR", "SystemDrive"}) {
    if (const char* value = secure_frontend::crt_getenv(name)) settings.environment.emplace(name, value);
  }
#else
  settings.environment.emplace("PATH", "/usr/bin:/bin");
#endif
  secure_frontend::SearchService service(settings);
  const auto first = service.offer();
  expect(first.has_value(), "the tool is offered");
  const secure_frontend::ToolCall call{"web_search", {{"query", secure_frontend::JsonValue::make_string("rust 1.90")}}};
  std::atomic<bool> done{false};
  std::thread searching([&service, &call, &done] {
    (void)service.run(call, [] { return false; });
    done = true;
  });
  while (slow.requests().empty()) std::this_thread::sleep_for(std::chrono::milliseconds(10));  // the search waits on the instance
  const auto asked = std::chrono::steady_clock::now();
  const auto again = service.offer();
  const auto took = std::chrono::steady_clock::now() - asked;
  expect(again.has_value() && first.has_value() && again->template_json == first->template_json && !done,
         "the same tool is offered while the search runs");
  expect(took < std::chrono::milliseconds(500),
         "at once (" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(took).count()) + " ms)");
  searching.join();
}

// Phase 6: the page's search settings over the routes - read from the config file, saved to it (checked; applied at the
// next start), a test search through the saved settings by strata-search - for this PC's clients only.
void search_routes_read_save_and_test() {
  FakeSearxng searxng;
  const auto root = secure_frontend_test::make_scratch_directory();
  const auto data = secure_frontend::data_folder_layout(root / "data");
  const auto engine = root / "engine.exe";
  secure_frontend_test::write_file(engine, "engine");
  expect(std::holds_alternative<secure_frontend::FirstStartResult>(secure_frontend::ensure_data_folder(data, engine)), "a data folder");
  secure_frontend::SearchRouteSettings settings{.config_file = data.config_file, .program = secure_frontend_test::extra_arguments().at(0)};
#if defined(_WIN32)
  for (const char* name : {"SystemRoot", "WINDIR", "SystemDrive"}) {
    if (const char* value = secure_frontend::crt_getenv(name)) settings.environment.emplace(name, value);
  }
#else
  settings.environment.emplace("PATH", "/usr/bin:/bin");
#endif
  settings.log = [](std::string_view) {};
  secure_frontend::SearchRoutes routes(std::move(settings));
  const auto post = [&routes](const secure_frontend::RouteId route, const std::string& body, const bool loopback = true) {
    return routes.answer(route, body, secure_frontend::RequestContext{.loopback_peer = loopback});
  };
  const auto has = [](const std::string& text, const std::string_view part) { return text.find(part) != std::string::npos; };
  const auto first = post(secure_frontend::RouteId::strata_search, "{}");
  expect(first.status == 200 && has(first.body, "\"built\":true") && has(first.body, "\"enabled\":false") &&
             has(first.body, "\"results\":5") && has(first.body, "\"max_rounds\":8") && has(first.body, "\"timeout_s\":30") &&
             has(first.body, "\"running\":false"),
         "the settings, off at first: " + first.body);
  expect(post(secure_frontend::RouteId::strata_search, "{}", false).status == 403, "this PC's clients only");
  expect(post(secure_frontend::RouteId::strata_search_test, "{}").status == 422, "no test search before a URL is saved");

  const std::string url = "http://127.0.0.1:" + std::to_string(searxng.port());
  const auto saved = post(secure_frontend::RouteId::strata_search_settings,
                          R"({"enabled":true,"url":")" + url + R"(","results":3,"max_rounds":2,"timeout_s":20})");
  expect(saved.status == 200 && has(saved.body, "\"restart_needed\":true"), "saved: " + saved.body);
  const auto text = secure_frontend_test::read_file(data.config_file);
  expect(has(text, "\"search\"") && has(text, "\"url\": \"" + url + "\"") && has(text, "\"listener\""), "into the config file, the rest kept: " + text);
  const auto bad = post(secure_frontend::RouteId::strata_search_settings, R"({"enabled":true,"url":"ftp://example.org"})");
  expect(bad.status == 422 && has(bad.body, "\"code\":\"settings_refused\"") && has(bad.body, "search.url"),
         "a bad URL is refused by name: " + bad.body);
  expect(has(secure_frontend_test::read_file(data.config_file), "\"url\": \"" + url + "\""), "and nothing is written");
  expect(post(secure_frontend::RouteId::strata_search_settings, R"({"engine":"google"})").status == 400, "an unknown field is refused");

  const auto tested = post(secure_frontend::RouteId::strata_search_test, R"({"query":"rust 1.90"})");
  expect(tested.status == 200 && has(tested.body, "\"ok\":true") && has(tested.body, "Announcing Rust 1.90.0"),
         "a test search through the saved settings: " + tested.body);
  expect(!searxng.requests().empty() && searxng.requests().back() == "GET /search?q=rust%201.90&format=json HTTP/1.1",
         "it asked the saved instance");
}

// Owner 2026-10-27 ("I don't see ... mcp settings" on a page opened from another PC): the routes let in the clients the
// server's rule lets in (it passes model_files_allowed, as for the model-file routes); without a rule, this PC's only.
void search_routes_let_in_the_clients_the_rule_lists() {
  const auto root = secure_frontend_test::make_scratch_directory();
  const auto data = secure_frontend::data_folder_layout(root / "data");
  const auto engine = root / "engine.exe";
  secure_frontend_test::write_file(engine, "engine");
  expect(std::holds_alternative<secure_frontend::FirstStartResult>(secure_frontend::ensure_data_folder(data, engine)), "a data folder");
  secure_frontend::SearchRouteSettings settings{.config_file = data.config_file, .program = secure_frontend_test::extra_arguments().at(0)};
  settings.log = [](std::string_view) {};
  settings.client_allowed = [](const secure_frontend::RequestContext& context) {
    return context.loopback_peer || (context.peer[0] == 4U && context.peer[1] == 172U && context.peer[2] == 16U && context.peer[3] == 1U);
  };
  secure_frontend::SearchRoutes routes(std::move(settings));
  const auto from = [&routes](const secure_frontend::RouteId route, const std::string& body, const unsigned char subnet) {
    secure_frontend::RequestContext context{.loopback_peer = false};
    context.peer[0] = 4U;
    context.peer[1] = 172U;
    context.peer[2] = 16U;
    context.peer[3] = subnet;
    context.peer[4] = 7U;
    return routes.answer(route, body, context);
  };
  const auto listed = from(secure_frontend::RouteId::strata_search, "{}", 1U);
  expect(listed.status == 200 && listed.body.find("\"built\":true") != std::string::npos, "a listed client sees the settings: " + listed.body);
  const auto saved = from(secure_frontend::RouteId::strata_search_settings, R"({"enabled":false,"url":"http://127.0.0.1:9"})", 1U);
  expect(saved.status == 200, "and saves them: " + saved.body);
  expect(from(secure_frontend::RouteId::strata_search, "{}", 3U).status == 403, "a client the rule does not list is refused");
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
  WSADATA data{};
  (void)WSAStartup(MAKEWORD(2, 2), &data);
#endif
  return secure_frontend_test::run_cases(argc, argv, {
      {"search_urls_are_checked", search_urls_are_checked},
      {"the_request_is_one_get_percent_encoded", the_request_is_one_get_percent_encoded},
      {"replies_are_read_by_length_chunks_or_close", replies_are_read_by_length_chunks_or_close},
      {"searxng_json_becomes_numbered_results", searxng_json_becomes_numbered_results},
      {"a_search_writes_one_request_and_reads_the_reply", a_search_writes_one_request_and_reads_the_reply},
      {"the_search_service_offers_web_search_and_runs_it", the_search_service_offers_web_search_and_runs_it},
      {"a_running_search_does_not_hold_up_the_offer", a_running_search_does_not_hold_up_the_offer},
      {"search_routes_read_save_and_test", search_routes_read_save_and_test},
      {"search_routes_let_in_the_clients_the_rule_lists", search_routes_let_in_the_clients_the_rule_lists},
      {"the_mcp_server_answers_upstreams_client", the_mcp_server_answers_upstreams_client},
      {"strata_search_answers_over_stdio", strata_search_answers_over_stdio},
      {"the_client_runs_strata_search_on_its_pipes", the_client_runs_strata_search_on_its_pipes},
  }, failures);
}
