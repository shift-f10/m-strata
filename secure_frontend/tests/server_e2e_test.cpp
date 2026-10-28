#include "support/server_fixture.hpp"
#include "support/test_support.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <iphlpapi.h>
#else
#include <unistd.h>
#endif

namespace {
int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}
using secure_frontend_test::contains;
using secure_frontend_test::ServerFixture;

std::vector<std::string> with(std::vector<std::string> base, const std::vector<std::string>& extra) {
  base.insert(base.end(), extra.begin(), extra.end());
  return base;
}

// Every listening TCP socket on `port`: {"v4", "<hex addr>"} / {"v6", ...}.
std::vector<std::pair<std::string, std::string>> listeners_on_port(const std::uint16_t port, const long pid) {
  std::vector<std::pair<std::string, std::string>> found;
#if defined(_WIN32)
  for (const ULONG family : {static_cast<ULONG>(AF_INET), static_cast<ULONG>(AF_INET6)}) {
    DWORD size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, family, TCP_TABLE_OWNER_PID_LISTENER, 0);
    std::vector<unsigned char> buffer(size);
    if (GetExtendedTcpTable(buffer.data(), &size, FALSE, family, TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR) continue;
    if (family == AF_INET) {
      const auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buffer.data());
      for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto& row = table->table[i];
        if (static_cast<long>(row.dwOwningPid) == pid || ntohs(static_cast<u_short>(row.dwLocalPort)) == port) {
          found.emplace_back("v4", row.dwLocalAddr == htonl(INADDR_LOOPBACK) ? "loopback" : "other");
        }
      }
    } else {
      const auto* table = reinterpret_cast<const MIB_TCP6TABLE_OWNER_PID*>(buffer.data());
      for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto& row = table->table[i];
        if (static_cast<long>(row.dwOwningPid) == pid || ntohs(static_cast<u_short>(row.dwLocalPort)) == port) {
          found.emplace_back("v6", "other");
        }
      }
    }
  }
#else
  (void)pid;
  char port_hex[8];
  std::snprintf(port_hex, sizeof(port_hex), "%04X", port);
  for (const auto& [file, family] : {std::pair{"/proc/net/tcp", "v4"}, std::pair{"/proc/net/tcp6", "v6"}}) {
    std::ifstream table(file);
    std::string line;
    std::getline(table, line);  // header
    while (std::getline(table, line)) {
      std::istringstream fields(line);
      std::string slot, local, remote, state;
      fields >> slot >> local >> remote >> state;
      const auto colon = local.rfind(':');
      if (state != "0A" || colon == std::string::npos || local.substr(colon + 1) != port_hex) continue;
      const std::string address = local.substr(0, colon);
      found.emplace_back(family, address == "0100007F" ? "loopback" : "other");
    }
  }
#endif
  return found;
}

void cfg01_invalid_config_fails_before_listener_and_child() {
  ServerFixture f;
  const auto marker = f.dir / "engine-started";
  // (a) an invalid key value
  auto literals = f.base_literals();
  literals["limits.http.max_headers"] = "1";
  literals["engine.args"] = "[" + secure_frontend_test::json_literal("--marker=" + marker.string()) + "]";
  f.write_config(literals);
  const auto invalid = f.run({"--config", f.config.string()});
  expect(invalid.exit_code != 0 && contains(invalid.err, "limits.http.max_headers"), "invalid config exits: " + invalid.err);
  expect(!std::filesystem::exists(marker), "the engine must not start on an invalid config");
  expect(!secure_frontend_test::can_connect(f.port), "no listener remains");
  // (b) a valid config whose address cannot be bound (TEST-NET-1 is never local).
  // The engine executable is not executable as a program, so starting it fails
  // synchronously inside start(): whichever startup error is reported shows
  // whether the engine was spawned before the bind. (A marker written by a real
  // child cannot prove this: the child is killed before it reaches main().)
  const auto not_an_engine = f.dir / "bin" / "not-an-engine";
  secure_frontend_test::write_file(not_an_engine, "this is not a program\n", 0700);
  auto unbindable_literals = f.base_literals();
  unbindable_literals["engine.executable"] = secure_frontend_test::json_literal(not_an_engine.string());
  unbindable_literals["listener.host"] = secure_frontend_test::json_literal("192.0.2.10");
  unbindable_literals["listener.allow_remote"] = "true";
  f.write_config(unbindable_literals);
  const auto unbindable = f.run({"--expert", "--config", f.config.string()});
  expect(unbindable.exit_code != 0 && contains(unbindable.err, "Unable to bind"),
         "unbindable address exits with the bind error: " + unbindable.err);
  expect(!contains(unbindable.err, "Unable to start the engine supervisor") && !contains(unbindable.err, "engine generation"),
         "the engine must not be started before the listener is bound");
  expect(!std::filesystem::exists(marker), "the engine must not start when the listener cannot bind");
}

void net01_default_listener_is_loopback_only() {
  ServerFixture f;
  secure_frontend_test::ChildProcess server;
  expect(f.start(server, f.base_flags()), "server starts with defaults: " + server.stderr_text());
  const auto listeners = listeners_on_port(f.port, server.pid());
  expect(!listeners.empty(), "the server listens");
  for (const auto& [family, scope] : listeners) {
    expect(family == "v4" && scope == "loopback", "only 127.0.0.1 listens by default, found " + family + "/" + scope);
  }
}

void net02_remote_addresses_need_opt_in() {
  ServerFixture f;
  const auto marker = f.dir / "engine-started";
  for (const char* host : {"0.0.0.0", "::", "172.16.1.10", "10.0.0.1", "203.0.113.5", "2001:db8::1", "fd00::1"}) {
    const auto result = f.run(with(f.base_flags(), {"--host", host, "--engine-arg", "--marker=" + marker.string()}));
    expect(result.exit_code != 0 && contains(result.err, "listener.host: a non-loopback address needs listener.allow_remote"),
           std::string{"remote address without opt-in must fail: "} + host);
  }
  expect(!std::filesystem::exists(marker), "no engine was started for any of them");
}

void net02_error_is_non_secret_and_exits_before_bind() {
  ServerFixture f;
  const auto marker = f.dir / "engine-started";
  const auto result = f.run(with(f.base_flags(), {"--host", "203.0.113.5", "--engine-arg", "--marker=" + marker.string()}));
  expect(result.exit_code != 0, "startup fails");
  expect(contains(result.err, "listener.host"), "the error names the key");
  expect(!contains(result.err, secure_frontend_test::kTestToken) && !contains(result.out, secure_frontend_test::kTestToken),
         "the error never contains the token");
  expect(!contains(result.err, "203.0.113.5"), "the error does not echo the value");
  expect(!std::filesystem::exists(marker), "the engine was not launched");
}

void ready_timeout_is_configurable() {
  const std::string chat = R"({"model":"qwen-local","messages":[{"role":"user","content":"Hi"}],"max_tokens":2})";
  for (const auto& [timeout_s, expected] : {std::pair{"1", "HTTP/1.1 503"}, std::pair{"5", "HTTP/1.1 200"}}) {
    ServerFixture f;
    auto literals = f.base_literals();
    literals["limits.engine.ready_timeout_s"] = timeout_s;
    literals["engine.args"] = "[\"--ready-delay-ms=2000\"]";
    f.write_config(literals);
    secure_frontend_test::ChildProcess server;
    expect(f.start(server, {"--expert", "--config", f.config.string()}), "server starts: " + server.stderr_text());
    // limits.engine.ready_timeout_s reaches the supervisor (supervisor_config):
    // at 1 s the 2 s READY is never seen and the generation is killed for it;
    // at 5 s the engine becomes ready and nothing is killed.
    const std::string_view kill_line = "killed_by_supervisor(ready_timeout)";
    if (std::string_view{timeout_s} == "1") {
      expect(!f.wait_engine_ready(std::chrono::seconds(4)), "ready_timeout_s=1: the engine never becomes ready");
      expect(contains(server.stderr_text(), kill_line), "ready_timeout_s=1: the supervisor kills it at the READY deadline: " +
                                                            server.stderr_text());
    } else {
      expect(f.wait_engine_ready(), "engine ready before the chat");
    }
    const auto response = f.request("POST", "/v1/chat/completions", f.bearer(), chat);
    expect(response.rfind(expected, 0) == 0,
           std::string{"ready_timeout_s="} + timeout_s + " against a 2 s READY must give " + expected + ", got " +
               response.substr(0, 16));
    if (std::string_view{timeout_s} == "5") {
      expect(!contains(server.stderr_text(), kill_line), "ready_timeout_s=5: no READY-deadline kill: " + server.stderr_text());
    }
  }
}

void cfg05_token_absent_from_child_argv_env_and_outputs() {
  ServerFixture f;
  const auto dump = f.dir / "engine-dump.txt";
  secure_frontend_test::ChildProcess server;
  expect(f.start(server, with(f.base_flags(), {"--engine-arg", "--dump=" + dump.string()})), "server starts: " + server.stderr_text());
  const std::string token{secure_frontend_test::kTestToken};
  std::string seen;
  expect(f.wait_engine_ready(), "engine ready before the chat");
  // A completed chat proves READY was read, so the engine has already written its dump.
  seen += f.request("POST", "/v1/chat/completions", f.bearer(),
                    R"({"model":"qwen-local","messages":[{"role":"user","content":"Hi"}],"max_tokens":2})");
  seen += f.request("GET", "/health", f.bearer());
  seen += f.request("GET", "/v1/models", f.bearer());
  seen += f.request("GET", "/health", "");
  seen += f.request("GET", "/health", "Bearer wrong-token-0123456789abcdefghijklmnop");
  seen += f.request("GET", "/does-not-exist", f.bearer());
  seen += f.request("POST", "/v1/chat/completions", f.bearer(), "{not json");
  expect(contains(seen, "HTTP/1.1 200") && contains(seen, "HTTP/1.1 401") && contains(seen, "HTTP/1.1 404") &&
             contains(seen, "HTTP/1.1 400"),
         "success, auth failure, not found and bad request were all exercised");
  expect(!contains(seen, token), "no response header or body carries the token");
#if !defined(_WIN32)
  const std::string cmdline = secure_frontend_test::read_file("/proc/" + std::to_string(server.pid()) + "/cmdline");
  expect(!cmdline.empty() && !contains(cmdline, token), "the server's /proc/<pid>/cmdline lacks the token");
#endif
  const std::string child = secure_frontend_test::read_file(dump);
  expect(contains(child, "argv:--serve"), "the fake engine dumped its argv and environment");
  expect(!contains(child, token), "the engine child's argv and environment lack the token");
  expect(!contains(child, secure_frontend_test::kTestTokenVariable), "the child does not inherit the token variable at all");
  server.terminate();
  expect(!contains(server.stdout_text(), token) && !contains(server.stderr_text(), token),
         "the server's stdout and stderr lack the token");
}

void auth01_socket_rejects_missing_and_wrong_token_on_every_route() {
  ServerFixture f;
  secure_frontend_test::ChildProcess server;
  expect(f.start(server, f.base_flags()), "server starts: " + server.stderr_text());
  for (const auto& [method, target] : {std::pair{"GET", "/health"}, std::pair{"GET", "/v1/models"},
                                       std::pair{"POST", "/v1/chat/completions"}, std::pair{"GET", "/does-not-exist"}}) {
    for (const std::string& authorization : {std::string{}, std::string{"Bearer wrong-token-0123456789abcdefghijklmnop"}}) {
      const auto response = f.request(method, target, authorization, std::string{method} == "POST" ? "{}" : "");
      expect(response.rfind("HTTP/1.1 401", 0) == 0, std::string{method} + " " + target + " over TCP must be 401");
      expect(!contains(response, "qwen-local") && !contains(response, "\"engine\""), "no route or model data leaks");
    }
    // Header forms the fixture's request() cannot build: a tab separator and a duplicate header.
    const std::string head = std::string{method} + " " + target + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(f.port) + "\r\n";
    const std::string tail = std::string{method} == "POST" ? "Content-Length: 2\r\n\r\n{}" : "\r\n";
    const std::string token{secure_frontend_test::kTestToken};
    for (const auto& [headers, status] :
         {std::pair{"Authorization: Bearer\t" + token + "\r\n", std::string{"HTTP/1.1 401"}},
          std::pair{"Authorization: Bearer " + token + "\r\nAuthorization: Bearer " + token + "\r\n",
                    std::string{"HTTP/1.1 400"}}}) {
      const auto response = secure_frontend_test::http_exchange(f.port, head + headers + tail);
      expect(response.rfind(status, 0) == 0, std::string{method} + " " + target + " over TCP must be " + status + ", got " +
                                                 response.substr(0, 16));
      expect(!contains(response, "qwen-local") && !contains(response, "\"engine\"") && !contains(response, token),
             "no route or model data or token leaks");
    }
  }
}

void auth02_valid_token_succeeds_and_is_not_echoed() {
  ServerFixture f;
  secure_frontend_test::ChildProcess server;
  expect(f.start(server, f.base_flags()), "server starts: " + server.stderr_text());
  expect(f.wait_engine_ready(), "engine ready: /health is 503 while it starts");
  const auto health = f.request("GET", "/health", f.bearer());
  const auto models = f.request("GET", "/v1/models", f.bearer());
  expect(health.rfind("HTTP/1.1 200", 0) == 0 && models.rfind("HTTP/1.1 200", 0) == 0 && contains(models, "qwen-local"),
         "authorized requests succeed");
  expect(!contains(health, secure_frontend_test::kTestToken) && !contains(models, secure_frontend_test::kTestToken),
         "the token is not echoed in a response");
  server.terminate();
  expect(!contains(server.stdout_text(), secure_frontend_test::kTestToken) &&
             !contains(server.stderr_text(), secure_frontend_test::kTestToken),
         "nor written to stdout/stderr");
}

// Review Focus 1/3: serve prints the actionable reason (fixed text, never a
// token or a configured value), not only a generic line.
void serve_reports_why_the_token_and_tokenizer_pack_are_unusable() {
  ServerFixture f;
  const std::string short_token = "short-token-value";
  const auto token = secure_frontend_test::run_to_completion(secure_frontend_test::ProcessSpec{
      f.server_executable, f.base_flags(), {{secure_frontend_test::kTestTokenVariable, short_token}}});
  expect(token.exit_code != 0 && contains(token.err, "Unable to read API token") &&
             contains(token.err, "at least 32 bytes"),
         "a too-short token names the reason: " + token.err);
  expect(!contains(token.err, short_token) && !contains(token.out, short_token), "the token value is never printed");

  const auto empty_pack = f.dir / "empty-pack";
  secure_frontend_test::make_private_directory(empty_pack);
  auto flags = f.base_flags();
  for (std::size_t i = 0; i + 1 < flags.size(); ++i) if (flags[i] == "--tokenizer-pack") flags[i + 1] = empty_pack.string();
  const auto pack = f.run(flags);
  expect(pack.exit_code != 0 && contains(pack.err, "Unable to load local tokenizer pack") &&
             contains(pack.err, "required tokenizer pack file is missing"),
         "a missing tokenizer file names the reason: " + pack.err);
  expect(!contains(pack.err, secure_frontend_test::kTestToken), "never the token");
}

#if !defined(_WIN32)
// Review Focus 1/3: a symlinked .gguf below roots.models stops serve with the
// entry name (allowed, spec §6.1) and what to do, not only a generic line.
void serve_reports_symlinked_model_entry() {
  ServerFixture f;
  std::filesystem::create_symlink(f.models / "qwen-local.gguf", f.models / "linked.gguf");
  const auto marker = f.dir / "engine-started";
  const auto result = f.run(with(f.base_flags(), {"--engine-arg", "--marker=" + marker.string()}));
  expect(result.exit_code != 0 && contains(result.err, "Unable to inventory local models"), "serve stops: " + result.err);
  expect(contains(result.err, "linked.gguf") && contains(result.err, "roots.models"),
         "the error names the entry and roots.models: " + result.err);
  expect(!contains(result.err, f.models.string()), "the configured root is not echoed");
  expect(!contains(result.err, secure_frontend_test::kTestToken), "never the token");
  expect(!std::filesystem::exists(marker), "the engine was not launched");
}

// P7 / defect e: the fixture's models root is configured through a symlink; the
// engine must receive the canonical root the inventory used, never the raw string.
void cfg03_engine_receives_canonical_models_root() {
  ServerFixture f;
  const auto link = f.dir / "models-link";
  std::filesystem::create_directory_symlink(f.models, link);
  const auto dump = f.dir / "engine-argv.txt";
  auto flags = f.base_flags();
  for (std::size_t i = 0; i + 1 < flags.size(); ++i) if (flags[i] == "--models-root") flags[i + 1] = link.string();
  flags.insert(flags.end(), {"--engine-arg", "--dump=" + dump.string()});
  secure_frontend_test::ChildProcess server;
  expect(f.start(server, flags), "server starts: " + server.stderr_text());
  // The engine is spawned after bind, so the dump may not exist when start()
  // returns. The fake engine closes the dump before it prints READY, and a chat
  // completion needs READY: after a 200 the dump is complete.
  const std::string chat = R"({"model":"qwen-local","messages":[{"role":"user","content":"Hi"}],"max_tokens":2})";
  expect(f.wait_engine_ready(), "engine ready before the chat");
  const auto response = f.request("POST", "/v1/chat/completions", f.bearer(), chat);
  expect(response.rfind("HTTP/1.1 200", 0) == 0, "a chat completion succeeds first: " + response.substr(0, 16));
  const std::string argv_dump = secure_frontend_test::read_file(dump);
  const std::string canonical = std::filesystem::canonical(f.models).string();
  expect(contains(argv_dump, "argv:--native\nargv:" + canonical + "/qwen-local.gguf\n"),
         "--native carries the canonical models root: " + argv_dump);
  expect(!contains(argv_dump, "models-link"), "the raw (symlinked) root never reaches the engine");
}

// K9: the server is non-dumpable with a zero core-size limit.
void server_process_is_not_dumpable() {
  ServerFixture f;
  secure_frontend_test::ChildProcess server;
  expect(f.start(server, f.base_flags()), "server starts: " + server.stderr_text());
  const std::string proc = "/proc/" + std::to_string(server.pid());
  const std::string limits = secure_frontend_test::read_file(proc + "/limits");
  const auto line_start = limits.find("Max core file size");
  std::istringstream fields(limits.substr(line_start == std::string::npos ? 0 : line_start + 18));
  std::string soft, hard;
  fields >> soft >> hard;
  expect(soft == "0" && hard == "0", "RLIMIT_CORE is 0/0, got " + soft + "/" + hard);
  if (geteuid() != 0) {
    // A non-dumpable process's /proc entries belong to root; its environ is unreadable to the same user.
    std::ifstream environ_file(proc + "/environ");
    std::string probe;
    const bool readable = environ_file.good() && static_cast<bool>(std::getline(environ_file, probe));
    expect(!readable, "PR_SET_DUMPABLE=0: /proc/<pid>/environ is not readable by the same user");
  }
}
#endif
}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"cfg01_invalid_config_fails_before_listener_and_child", cfg01_invalid_config_fails_before_listener_and_child},
      {"net01_default_listener_is_loopback_only", net01_default_listener_is_loopback_only},
      {"net02_remote_addresses_need_opt_in", net02_remote_addresses_need_opt_in},
      {"net02_error_is_non_secret_and_exits_before_bind", net02_error_is_non_secret_and_exits_before_bind},
      {"ready_timeout_is_configurable", ready_timeout_is_configurable},
      {"cfg05_token_absent_from_child_argv_env_and_outputs", cfg05_token_absent_from_child_argv_env_and_outputs},
      {"auth01_socket_rejects_missing_and_wrong_token_on_every_route", auth01_socket_rejects_missing_and_wrong_token_on_every_route},
      {"auth02_valid_token_succeeds_and_is_not_echoed", auth02_valid_token_succeeds_and_is_not_echoed},
      {"serve_reports_why_the_token_and_tokenizer_pack_are_unusable",
       serve_reports_why_the_token_and_tokenizer_pack_are_unusable},
#if !defined(_WIN32)
      {"serve_reports_symlinked_model_entry", serve_reports_symlinked_model_entry},
      {"cfg03_engine_receives_canonical_models_root", cfg03_engine_receives_canonical_models_root},
      {"server_process_is_not_dumpable", server_process_is_not_dumpable},
#endif
  }, failures);
}
