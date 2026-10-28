#pragma once

#include "secure_frontend/api_errors.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace test_support {

inline constexpr std::string_view kHarnessTokenEnv = "STRATA_HARNESS_TOKEN";
// 43 base64url characters (32 random-looking bytes' worth): SP1 rejects tokens under 32 bytes.
inline constexpr std::string_view kHarnessToken = "SP2harnessToken_0123456789-abcdefghijklmnop";
inline constexpr std::string_view kHealthStarting = "{\"status\":\"unavailable\",\"engine\":\"starting\"}\n";
inline constexpr std::string_view kHealthReady = "{\"status\":\"ok\",\"engine\":\"ready\"}\n";
// Spec §7.2 bodies (SP3 Task 9): the SP2 generic bodies became the fixed OpenAI-shaped error rows.
inline const std::string kGeneric503 = secure_frontend::render_error_body(secure_frontend::ApiError::engine_unavailable);
inline const std::string kBadRequest = secure_frontend::render_error_body(secure_frontend::ApiError::invalid_request);
inline constexpr std::string_view kChatBody = R"({"model":"fake-model","messages":[{"role":"user","content":"Hi"}],"max_tokens":2})";

// POSIX: the direct children of `pid` (from every thread's /proc/<pid>/task/<tid>/children). Windows: empty.
std::vector<long> children_of(long pid);
// POSIX: every descendant of `pid`, breadth first. Windows: empty.
std::vector<long> descendants_of(long pid);
// Windows: TokenElevation of process `pid` (true when it cannot be read). POSIX: false.
bool process_token_elevated(long pid);

struct HttpResult final {
  int status{0};
  std::string body;
};

// One authenticated HTTP/1.1 request to 127.0.0.1:<port>; nullopt if no complete status line arrived in time.
std::optional<HttpResult> http_request(std::uint16_t port, std::string_view method, std::string_view target, std::string_view body,
                                       std::chrono::milliseconds timeout);
bool port_refuses(std::uint16_t port);

// A TCP connection to 127.0.0.1:<port> that never sends a byte; closed when
// destroyed. Spec C4 / final review I1: an idle peer must not stall shutdown.
class IdleConnection final {
 public:
  static std::unique_ptr<IdleConnection> open(std::uint16_t port);
  ~IdleConnection();
  IdleConnection(const IdleConnection&) = delete;
  IdleConnection& operator=(const IdleConnection&) = delete;

 private:
  explicit IdleConnection(std::uintptr_t socket) noexcept : socket_(socket) {}
  std::uintptr_t socket_;
};

// Model launcher (plan 2026-10-23): one model card the harness writes to <root>/cards/<n>.json
// (owner-only) over its own fixture: the engine copy, roots.models with fake-model.gguf, and the
// byte tokenizer pack (qwen) or the spm pack (gemma4).
struct CardOptions final {
  std::string name;
  std::string profile{"qwen"};
  std::vector<std::string> engine_args{};                 // the card's fixed engine.args
  std::string options_json{"{}"};                         // the card's "options" object
  std::map<std::string, std::string> config_literals{};   // more card config rows (key path -> JSON literal)
};

// Every member has a default initializer, so designated initializers that name
// only some of them keep compiling when later tasks add members (Task 18: state_dir).
struct ServerOptions final {
  std::vector<std::string> engine_args{};            // each passed as --engine-arg <value>
  std::vector<std::string> extra_args{};             // appended verbatim
  std::map<std::string, std::string> extra_env{};    // added to the server's own environment
  std::optional<std::filesystem::path> engine{};     // default: the fake engine
  std::optional<std::filesystem::path> state_dir{};  // --state-dir; default: <root>/state, created 0700
  // SP5 M1 Task 12: boot the MANAGED vision model instead of the SP1 expert fixture.
  // Installs fx m:t (qwen_payload_vision_pack: mmproj + launch.mmproj + the real byte
  // tokenizer pack) into <root>/models, drops --expert/--tokenizer-pack, and puts a
  // copy of strata-fake-vision-child beside the engine as "maya-vision" (the T11-y
  // sibling rule).
  bool managed_vision{false};
  // Key path -> JSON literal. Non-empty: <root>/config.json (owner-only) holds
  // schema_version 1 plus these keys and the server gets --config <path>.
  std::map<std::string, std::string> config_literals{};
  // listener.loopback_without_key cases: no --token-env (a keyless server refuses a key source).
  bool no_token{false};
  // Gemma 4 plan 1 Task 11: engine.profile for the expert launch, passed as --profile <name>;
  // absent, no flag (today's behaviour: the qwen default). "gemma4" also writes the spm_bpe
  // pack (write_spm_tokenizer_pack: vocab.json, merges.json, token_type.json) as
  // roots.tokenizer_pack instead of the byte pack, since that profile loads it in spm_bpe mode.
  std::optional<std::string> profile{};
  // Gemma 4 plan 1 Task 11: options for the supervision fake engine, written one per line to
  // <root>/bin/fake-engine.options beside the engine copy; the fake reads them after its
  // command line. engine.profile gemma4 lets only the strata_gemma4 allowlist through
  // engine.args, so the fake's own flags (--token-ids=..., --record-file=...) travel here.
  // Empty: no file.
  std::vector<std::string> fake_engine_options{};
  // Model launcher: non-empty boots launcher mode. The cards are written as above, the config
  // file gets models.cards, and the model flags (--expert, --profile, --models-root,
  // --tokenizer-pack, --engine, --model-id, --engine-arg) are left out: the cards hold them.
  std::vector<CardOptions> model_cards{};
  // Web search: the expert launch's pack is the byte pack with Qwen's reasoning and tool tags (tool_byte_tokenizer).
  bool tool_tokenizer{false};
};

// strata-secure-server as a test child: fixture models (fake-model.gguf, 0600)
// and the byte tokenizer pack in 0700 directories, a free port, token from
// kHarnessTokenEnv; stdout/stderr go to <root>/stdout.log and <root>/stderr.log.
// The engine (options.engine, default the fake) is copied into a private
// <root>/bin (directory 0700, file 0700 on POSIX), as SP1's ServerFixture does:
// SP1's engine trust check refuses a group-writable build-tree file (umask 0002).
// Windows: started suspended with a non-elevated (LUA-restricted) token in its
// own console process group (so CTRL_BREAK_EVENT can target it) inside a
// kill-on-close harness Job used to count descendants and to clean up.
// Linux: the test process becomes a child subreaper, so descendants orphaned
// by a server that died are re-parented here and reaped by the destructor.
class ServerProcess final {
 public:
  enum class Stop { term, interrupt, kill, ctrl_break };

  static std::unique_ptr<ServerProcess> start(const ServerOptions& options);
  ~ServerProcess();
  ServerProcess(const ServerProcess&) = delete;
  ServerProcess& operator=(const ServerProcess&) = delete;

  [[nodiscard]] long pid() const noexcept { return pid_; }
  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }
  // The private engine copy the server was given (--engine); the server passes
  // it as argv[0], in the form the server canonicalises to: POSIX canonical()
  // of the copy; Windows: root() itself is already in GetFinalPathNameByHandleW
  // form (FILE_NAME_NORMALIZED, "\\?\" removed), so an 8.3 %TEMP% such as
  // C:\Users\RUNNER~1\... reads as the long names the server hands the engine.
  [[nodiscard]] const std::filesystem::path& engine_path() const noexcept { return engine_path_; }
  [[nodiscard]] std::optional<HttpResult> get(std::string_view target,
                                              std::chrono::milliseconds timeout = std::chrono::seconds(5)) const;
  [[nodiscard]] std::optional<HttpResult> post_chat(std::string_view body,
                                                    std::chrono::milliseconds timeout = std::chrono::seconds(5)) const;
  [[nodiscard]] bool wait_until_listening(std::chrono::milliseconds timeout);
  [[nodiscard]] bool wait_for_health(std::string_view body, std::chrono::milliseconds timeout) const;
  // POSIX: SIGTERM / SIGINT / SIGKILL (ctrl_break sends SIGINT). Windows: CTRL_BREAK_EVENT to the
  // server's console group for term/interrupt/ctrl_break, TerminateProcess for kill.
  void stop(Stop how);
  // Exit code; POSIX 128 + signal for a signal death.
  [[nodiscard]] std::optional<int> wait_exit(std::chrono::milliseconds timeout);
  [[nodiscard]] bool port_refuses() const;
  [[nodiscard]] std::string stderr_text() const;
  // POSIX: every descendant. Windows: every process in the harness Job except
  // the server and the console hosts (see active_processes()).
  [[nodiscard]] std::vector<long> descendants() const;
  // Windows: the processes in the harness Job, not counting console hosts
  // (%SystemRoot%\System32\conhost.exe). Windows starts a conhost for the
  // engine's own windowless console (CREATE_NO_WINDOW, spec §3.7 / C6), and it
  // runs in the engine's Job tree, so the harness Job holds it too; it is the
  // OS's console server, not a process the frontend or the engine spawned.
  // POSIX: 1 + descendants() while the server lives, else 0.
  [[nodiscard]] unsigned long active_processes();
  // Diagnostics for a failed count: every process in the harness Job as
  // "pid:image" (console hosts included). POSIX: the descendants' pids.
  [[nodiscard]] std::string describe_processes() const;

 private:
  ServerProcess() = default;
  bool launch(const std::vector<std::string>& arguments, const std::map<std::string, std::string>& environment);
  // POSIX: adds the server's current descendants to known_descendants_ (the destructor kills them all).
  void remember_descendants() const;

  std::filesystem::path root_;
  std::filesystem::path engine_path_;
  std::uint16_t port_{0};
  long pid_{-1};
  std::optional<int> exit_code_;
  mutable std::set<long> known_descendants_;
#if defined(_WIN32)
  void* process_{nullptr};
  void* job_{nullptr};
#endif
};

}  // namespace test_support
