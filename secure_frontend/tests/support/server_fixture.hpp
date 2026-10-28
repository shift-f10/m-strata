#pragma once
#include "support/config_json.hpp"
#include "support/http_client.hpp"
#include "support/process_runner.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <chrono>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace secure_frontend_test {

// Every entry below `base` with its type, size and modification time, sorted.
// Two equal snapshots mean nothing was created, removed, renamed or written
// (a directory's mtime changes when an entry is added or removed). Used to
// prove that --check has no side effects.
inline std::vector<std::string> snapshot_tree(const std::filesystem::path& base) {
  std::vector<std::string> rows;
  std::error_code error;
  for (auto it = std::filesystem::recursive_directory_iterator(base, error);
       !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
    const auto relative = it->path().lexically_relative(base).generic_u8string();
    std::string row(reinterpret_cast<const char*>(relative.data()), relative.size());
    std::error_code ignored;
    const auto type = it->symlink_status(ignored).type();
    row += '|' + std::to_string(static_cast<int>(type));
    if (type == std::filesystem::file_type::regular) row += '|' + std::to_string(it->file_size(ignored));
    // file_time_type's rep is __int128 on libc++/mingw; 64 bits hold any real timestamp.
    const auto ticks = std::filesystem::last_write_time(it->path(), ignored).time_since_epoch().count();
    row += '|' + std::to_string(static_cast<long long>(ticks));
    rows.push_back(std::move(row));
  }
  std::sort(rows.begin(), rows.end());
  return rows;
}

inline constexpr const char* kTestTokenVariable = "SF_TEST_TOKEN";
inline constexpr std::string_view kTestToken = "sf-e2e-token-0123456789abcdefghijklmnopqrstuv";  // 45 bytes

// Printable ASCII, the GPT-2 byte symbols for space and newline, and the two
// Qwen ChatML control tokens: enough to encode any ASCII prompt.
inline void write_test_tokenizer_pack(const std::filesystem::path& root) {
  std::string vocab = "{";
  std::string types = "[";
  std::size_t id = 0;
  const auto add = [&](const std::string& token, const int type) {
    if (id > 0) { vocab += ','; types += ','; }
    vocab += json_literal(token) + ':' + std::to_string(id++);
    types += std::to_string(type);
  };
  for (int byte = 33; byte <= 126; ++byte) add(std::string(1, static_cast<char>(byte)), 1);
  add("\xC4\xA0", 1);  // U+0120, the byte-level symbol for ' '
  add("\xC4\x8A", 1);  // U+010A, the byte-level symbol for '\n'
  add("<|im_start|>", 3);
  add("<|im_end|>", 3);
  write_file(root / "vocab.json", vocab + "}");
  write_file(root / "token_type.json", types + "]");
  write_file(root / "merges.txt", "");
}

// A complete local installation in a scratch directory: models/ with
// qwen-local.gguf, a tokenizer pack, an empty secret root, a 0700 state root, a private copy of
// the fake engine, and a free loopback port. ARGS: [0] strata-secure-server, [1] fake engine.
class ServerFixture final {
 public:
  std::filesystem::path dir = make_scratch_directory();
  std::filesystem::path models = dir / "models";
  std::filesystem::path tokenizer = dir / "tok";
  std::filesystem::path secret = dir / "secret";
  std::filesystem::path state = dir / "state";  // roots.state (--state-dir): required to serve
  std::filesystem::path engine;
  std::filesystem::path config = dir / "config.json";
  std::filesystem::path server_executable;
  std::filesystem::path cli_executable;  // SP5 M1 Task 13: ARGS[2], the `strata` binary
  std::uint16_t port = pick_free_port();

  ServerFixture() {
    const auto& args = extra_arguments();
    if (args.size() < 2U) {
      std::cerr << "ServerFixture needs ARGS <strata-secure-server> <fake engine>\n";
      std::exit(EXIT_FAILURE);
    }
    server_executable = args[0];
    if (args.size() > 2U) cli_executable = args[2];
    for (const auto& directory : {models, tokenizer, secret, dir / "bin"}) make_private_directory(directory);
    make_owner_private_directory(state);  // --state-dir: Windows owner must be the user, not Administrators
    write_file(models / "qwen-local.gguf", "GGUF");
#if defined(_WIN32)
    engine = dir / "bin" / "strata.exe";
#else
    engine = dir / "bin" / "strata";
#endif
    std::filesystem::copy_file(args[1], engine, std::filesystem::copy_options::overwrite_existing);
#if !defined(_WIN32)
    chmod(engine.c_str(), 0700);
#endif
    write_test_tokenizer_pack(tokenizer);
  }

  [[nodiscard]] std::vector<std::string> base_flags() const {
    return {"--expert", "--models-root", models.string(), "--tokenizer-pack", tokenizer.string(), "--engine", engine.string(),
            "--model-id", "qwen-local", "--token-env", kTestTokenVariable, "--port", std::to_string(port),
            "--state-dir", state.string()};
  }
  [[nodiscard]] std::map<std::string, std::string> base_literals() const {
    return {{"schema_version", "1"},
            {"listener.port", std::to_string(port)},
            {"roots.models", json_literal(models.string())},
            {"roots.tokenizer_pack", json_literal(tokenizer.string())},
            {"roots.state", json_literal(state.string())},
            {"engine.executable", json_literal(engine.string())},
            {"engine.model_id", json_literal("qwen-local")},
            {"token.source", json_literal(std::string{"env:"} + kTestTokenVariable)}};
  }
  [[nodiscard]] std::vector<std::pair<std::string, std::string>> environment() const {
    return {{kTestTokenVariable, std::string{kTestToken}}};
  }
  void write_config(const std::map<std::string, std::string>& literals) const {
    write_file(config, render_config_json(literals));
  }
  [[nodiscard]] ProcessResult run(const std::vector<std::string>& arguments) const {
    return run_to_completion(ProcessSpec{server_executable, arguments, environment()});
  }
  bool start(ChildProcess& child, const std::vector<std::string>& arguments) const {
    if (!child.start(ProcessSpec{server_executable, arguments, environment()})) return false;
    return wait_until_listening(child);
  }
  // SP5 M1 Task 13: the same for the `strata` CLI (`strata --config … serve`):
  // foreground serve listens on this fixture's port through the CLI's own process.
  bool start_cli(ChildProcess& child, const std::vector<std::string>& arguments) const {
    if (!child.start(ProcessSpec{cli_executable, arguments, environment()})) return false;
    return wait_until_listening(child);
  }
  [[nodiscard]] ProcessResult run_cli(const std::vector<std::string>& arguments) const {
    return run_to_completion(ProcessSpec{cli_executable, arguments, environment()});
  }
  // Shared by start()/start_cli(): alive-and-listening, false if the child died
  // first (30 s bound — generous on slow runners, loud on failure).
  bool wait_until_listening(ChildProcess& child) const {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
      if (child.wait_for(std::chrono::milliseconds(0)).has_value()) return false;
      if (can_connect(port)) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
  }

  [[nodiscard]] std::string request(const std::string& method, const std::string& target,
                                    const std::string& authorization, const std::string& body = {}) const {
    std::string text = method + " " + target + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\n";
    if (!authorization.empty()) text += "Authorization: " + authorization + "\r\n";
    if (!body.empty()) {
      text += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    }
    return http_exchange(port, text + "\r\n" + body);
  }
  [[nodiscard]] std::string bearer() const { return "Bearer " + std::string{kTestToken}; }
  // SP2: READY is consumed eagerly; until then /health is 503 and a chat is a
  // 503. Poll /health until the engine is ready before a case sends a chat or
  // expects a 200 /health. `authorization` defaults to bearer().
  [[nodiscard]] bool wait_engine_ready(const std::chrono::milliseconds timeout = std::chrono::seconds(10),
                                       const std::string& authorization = {}) const {
    const std::string credential = authorization.empty() ? bearer() : authorization;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (contains(request("GET", "/health", credential), "\"engine\":\"ready\"")) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
  }
};

}  // namespace secure_frontend_test
