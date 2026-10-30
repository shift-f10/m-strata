#pragma once

// strata-window's portable half (spec docs/superpowers/specs/2026-10-28-server-window-design.md, Part 2; the owner:
// "instead of a console application, we should make it into a window application ... minimized into the tray that
// has the start and stop models menu"). The window runs strata-unified-server as its child and shows the page; this
// is what it reads from the server's console, what it keeps of it, where its web view may go, the tray's model list,
// its requests to the server, and the server child itself. The native windows (WebView2, WebKitGTK) are thin over it.

#include "secure_frontend/first_start.hpp"

#include <chrono>
#include <cstdint>
#include <ctime>
#include <deque>
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

// --------------------------------------------------------------------------- the window's Log
// The server's console lines the window's Log shows (the owner: "in the window app version we need to add a log
// capability, the console have it but this one do not so we can troubleshoot"): the newest last, in memory, at most
// `max_lines` lines of `max_bytes` bytes in all. Each line is kept as text any view can show: valid UTF-8 (a broken
// sequence becomes U+FFFD), its control characters spaces (a tab stays), at most `max_bytes` long. A line holding a
// sign-in link is never kept (loggable()). A full log drops its oldest tenth at once, so a view shows every line again
// only now and then; the newest line always stays.
class LogLines final {
 public:
  LogLines(std::size_t max_lines, std::size_t max_bytes);
  void add(std::string_view line);
  [[nodiscard]] const std::deque<std::string>& lines() const noexcept { return lines_; }
  [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }
  // Every line kept has a number, one more than the line before it: lines()[0] is first(), the newest end() - 1.
  [[nodiscard]] std::uint64_t first() const noexcept { return first_; }
  [[nodiscard]] std::uint64_t end() const noexcept { return first_ + lines_.size(); }
  // What a view showing the lines [shown_first, shown_end) lacks: the index in lines() from which it adds the newer
  // ones, or nullopt when it shows every line again (lines it shows were dropped).
  [[nodiscard]] std::optional<std::size_t> newer_than(std::uint64_t shown_first, std::uint64_t shown_end) const;
  // Every line, each ending with `newline` (a Windows edit control and its clipboard take "\r\n"): what Copy copies.
  [[nodiscard]] std::string text(std::string_view newline = "\n") const;
  // The last `count` lines, the newest last (the page shown when the server ends).
  [[nodiscard]] std::vector<std::string> last(std::size_t count) const;
  // text() into `file` (replaced; a new one owner-only, as server.log); a message naming the problem (never the path)
  // when it could not be written.
  [[nodiscard]] std::optional<std::string> save(const std::filesystem::path& file) const;

 private:
  std::size_t max_lines_;
  std::size_t max_bytes_;
  std::size_t bytes_{0};
  std::uint64_t first_{0};
  std::deque<std::string> lines_;
};
// The file name Save offers, at the local time given: "m-strata-log-2026-10-29-140312.txt".
[[nodiscard]] std::string saved_log_name(const std::tm& when);

// --------------------------------------------------------------------------- the server child
// strata-unified-server run as the window's child: no console window, its stdout and stderr read as lines on a thread
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

// make --debugging (the owner, 2026-10-29: "it will have a pop up on the windows version that this is the debugging
// version and the tui you will have to accept and press y to start"): what a debugging build's window (a pop-up) and
// TUI (a screen) say before they start the server. Plain text, its paragraphs split by a blank line.
inline constexpr std::string_view kDebuggingNotice =
    "This is a debugging build of M-Strata (built with --debugging), made to find out why a model does not start or "
    "calibrate.\n\n"
    "It logs one line per request (never its text) and the engine's own output, which can hold what the engine prints of "
    "your prompts and the paths of your model files.\n\n"
    "Take care: run it on your own PC, keep the server's log private, look through a log before you share it and take "
    "out prompts, paths and names, and build again without --debugging for everyday use. The engine's output can be "
    "turned off without a new build: Settings > Troubleshooting.";
// Whether this build asks before it starts the server (a debugging build: debugging_build()).
[[nodiscard]] bool asks_before_starting() noexcept;

}  // namespace secure_frontend::window
