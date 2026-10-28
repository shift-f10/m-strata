#pragma once

// strata-window's portable half (spec docs/superpowers/specs/2026-10-28-server-window-design.md, Part 2; the owner:
// "instead of a console application, we should make it into a window application ... minimized into the tray that
// has the start and stop models menu"). The window runs strata-secure-server as its child and shows the page; this
// is what it reads from the server's console, what it keeps of it, where its web view may go, the tray's model list,
// its requests to the server, and the server child itself. The native windows (WebView2, WebKitGTK) are thin over it.

#include "secure_frontend/first_start.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace secure_frontend::window {

// --------------------------------------------------------------------------- the server's console
// "Sign in: open http://127.0.0.1:8080/#login=XXXX-XXXX in a browser on this PC (...)": the link, only when it is http
// on a loopback host with a "#login=" fragment and nothing else (the one-time link the server prints for this PC).
[[nodiscard]] std::optional<std::string> sign_in_url(std::string_view line);
// "Secure server listening on http://<host:port> (...)": the page's origin, "http://<host:port>"; a wildcard bind's
// address (0.0.0.0, [::]) is not one a browser can open, so it becomes this PC's (127.0.0.1, [::1]).
[[nodiscard]] std::optional<std::string> listening_origin(std::string_view line);
// Whether a server line may be written to the window's log: every line but the sign-in link's (it signs in once,
// from this PC; the server prints it to its console only, never to a log).
[[nodiscard]] bool loggable(std::string_view line);
// Where the window opens the page: the server's one-time link or, with none, the page's origin (its own sign-in) -
// on the Monitor the first time (the owner: "the open window shows the Monitor statistics"; the page's
// #view=monitor / &view=monitor); later loads (a restart) leave the view to the page.
[[nodiscard]] std::string page_address(const std::optional<std::string>& sign_in, std::string_view origin, bool first);

// --------------------------------------------------------------------------- the web view
// Where a navigation may go: the page's own origin stays in the window; another http(s) address opens in the default
// browser; anything else (file:, javascript:, a custom scheme) is refused.
enum class Navigation { stay, browser, refuse };
[[nodiscard]] Navigation navigation_for(std::string_view url, std::string_view origin);

// --------------------------------------------------------------------------- the tray's models
// GET /v1/strata/models (the launcher's list): each card's name and state ("running", "starting", "stopped",
// "failed", ...), in the list's order, and the server's own state; nullopt when it is not that answer.
struct TrayModel final {
  std::string name;
  std::string state;
};
struct TrayModels final {
  std::vector<TrayModel> models;
  std::optional<std::string> running;  // the card that runs (or loads)
  std::string state;                   // "running", "starting", "stopped", ...
};
[[nodiscard]] std::optional<TrayModels> tray_models(std::string_view launcher_json);
// The start request's body for a card: its name only, so the server uses the options the page last started it with.
[[nodiscard]] std::string start_body(std::string_view card);

// --------------------------------------------------------------------------- requests to the server
// One request to the server on this PC (the tray's start, stop and model list). `authority` is the origin's host:port;
// the key, when the server has one, goes in the Authorization header and nowhere else.
[[nodiscard]] std::string request_text(std::string_view method, std::string_view path, std::string_view authority,
                                       const std::string* key, std::string_view json_body);
struct Answer final {
  int status{0};
  std::string body;
};
// A whole answer with a Content-Length body (the server's JSON routes); nullopt: not one.
[[nodiscard]] std::optional<Answer> parse_answer(std::string_view raw);
// The request sent over a loopback connection to the origin's host and port; nullopt: no answer within `timeout`.
[[nodiscard]] std::optional<Answer> loopback_request(std::string_view origin, std::string_view method, std::string_view path,
                                                     const std::string* key, std::string_view json_body,
                                                     std::chrono::milliseconds timeout);

// --------------------------------------------------------------------------- what the window starts
// The server the window starts and what it knows of its data: the server beside the window, the window's own
// arguments passed through unchanged, and the data folder that start uses (none with --config or a bring-up start).
struct ServerSetup final {
  std::filesystem::path server;
  std::vector<std::string> arguments;
  std::optional<DataFolder> data;
  // The window's environment when it started ("NAME=VALUE" each): every start of the server gets it, a restart too.
  // Reading a --token-env key removes that variable from the window's own environment (token_reader.cpp scrubs it).
  std::vector<std::string> environment;
};
// This process's environment now, "NAME=VALUE" each.
[[nodiscard]] std::vector<std::string> current_environment();
// A message naming the problem (an option, never a value) when the arguments are not the server's.
[[nodiscard]] std::variant<ServerSetup, std::string> server_setup(const std::vector<std::string>& arguments,
                                                                  const std::filesystem::path& install_dir);
// The key the running server checks, read as `strata key show` reads it (its config, then its token source: the key
// file or the --token-env variable); nullopt with keyless loopback or when it cannot be read. Held in memory only.
struct ServerKey final {
  std::optional<std::string> key;
  bool keyless{false};
};
[[nodiscard]] std::optional<ServerKey> server_key(const ServerSetup& setup);

// --------------------------------------------------------------------------- the server's log
// The server's console lines (but the sign-in link) in <data>/logs/server.log, at most two files of `max_bytes`: the
// current one and server.log.1 before it. Created owner-only in the data folder's private logs folder.
class RotatingLog final {
 public:
  RotatingLog(std::filesystem::path file, std::uintmax_t max_bytes);
  void write(std::string_view line);
  [[nodiscard]] const std::filesystem::path& file() const noexcept { return file_; }

 private:
  void open();
  std::filesystem::path file_;
  std::uintmax_t max_bytes_;
  std::uintmax_t size_{0};
  std::ofstream out_;
};

// --------------------------------------------------------------------------- the server child
// strata-secure-server run as the window's child: no console window, its stdout and stderr read as lines on a thread
// of its own (handed to `on_line`, then `on_exit` with the exit code once it ends), `environment` as its environment
// (ServerSetup::environment: a --token-env variable reaches it). The child ends with the window (a kill-on-close Job
// on Windows, the parent-death signal on Linux). stop() asks it to stop as a console's Ctrl-Break or SIGTERM would (it
// stops the model and exits), waits up to `grace`, then kills it. Never call stop() from the handlers (they run on the
// reader thread stop() joins).
class ServerChild final {
 public:
  using LineHandler = std::function<void(std::string line)>;
  using ExitHandler = std::function<void(int exit_code)>;
  ServerChild();
  ~ServerChild();
  ServerChild(const ServerChild&) = delete;
  ServerChild& operator=(const ServerChild&) = delete;
  // A message naming what failed (never a path) when it could not start.
  [[nodiscard]] std::optional<std::string> start(const std::filesystem::path& executable, const std::vector<std::string>& arguments,
                                                 const std::vector<std::string>& environment, LineHandler on_line,
                                                 ExitHandler on_exit);
  // true when it ended within `grace` of the request (an orderly stop); false when it had to be killed.
  bool stop(std::chrono::milliseconds grace);
  [[nodiscard]] bool running() const;

 private:
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace secure_frontend::window
