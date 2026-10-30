#pragma once

// Notifier plugins (docs/superpowers/specs/2026-10-30-engine-family-plugins-design.md, section 10; the owner's decisions
// 6 and 7): the server's notifier host. Each notifier plugin that runs is started when the server starts (and again a
// minute after it ended), in its own folder with its manifest's arguments and environment and no new privileges, and is
// told of the server's events as one JSON line each on stdin - {"event", "subject", "body", "server", "time"} - the
// events its `events` setting names, or all. Events wait in a queue of at most 16 for each plugin, written in order by
// its own thread; one raised while its program is down, or with its queue full, is dropped and logged. The Plugins
// tab's "Send a test" writes {"test": true, "server"} and waits up to a minute for the answer line, {"ok", "reply"}. A
// notifier may answer an event with {"event", "ok", "reply"}, which the log shows. An event names a card or a model and
// what happened, never a path, a prompt or a key.

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace secure_frontend {

struct NotifierPlugin final {
  std::string id;
  std::filesystem::path program;                                // in its folder, checked when the server started
  std::vector<std::string> arguments;                           // its manifest's, each setting's value filled in
  std::map<std::string, std::string, std::less<>> environment;  // the closed environment and its declared variables
  std::filesystem::path working_directory{};                    // its own folder, <roots.state>/plugins/<id>
  std::optional<std::vector<std::string>> events{};             // the events it is told of (nullopt: all)
};

struct NotifierHubSettings final {
  std::vector<NotifierPlugin> plugins;
  std::string server_name;                       // what an event calls this server (server_display_name())
  std::function<void(std::string_view)> log{};   // one fixed line per start, end, drop and answer (default: none)
  bool log_stderr{false};                        // a program's stderr to the log, as the tool hub does it
  std::chrono::seconds restart_pause{60};        // after a program ended or did not start
  std::chrono::milliseconds test_timeout{std::chrono::seconds(60)};
  std::function<std::string()> now{};            // RFC 3339, UTC (default: rfc3339_now)
};

struct NotifierTest final {
  bool ok{false};
  std::string reply;  // the plugin's own words, or why there was no answer
};

class NotifierHub final {
 public:
  static constexpr std::size_t kMaxQueued = 16U;

  explicit NotifierHub(NotifierHubSettings settings);
  ~NotifierHub();
  NotifierHub(const NotifierHub&) = delete;
  NotifierHub& operator=(const NotifierHub&) = delete;

  // Tells each notifier that wants `event` (never blocks on a program).
  void raise(std::string_view event, const std::string& subject, const std::string& body);
  // The tab's "Send a test" to plugin `id`: its answer, or why there was none (waits up to test_timeout).
  [[nodiscard]] NotifierTest test(std::string_view id);
  // Every queue empty and nothing being written (tests), within `timeout`.
  [[nodiscard]] bool wait_idle(std::chrono::milliseconds timeout);
  void shutdown();

 private:
  struct Running;
  void run(Running& plugin);
  void say(const std::string& line) const;

  NotifierHubSettings settings_;
  std::vector<std::unique_ptr<Running>> plugins_;
};

// This PC's name as the events call it (printable ASCII, at most 64; "this PC" when unknown).
[[nodiscard]] std::string server_display_name();
// Now as RFC 3339 in UTC: "2026-10-30T12:00:00Z".
[[nodiscard]] std::string rfc3339_now();

}  // namespace secure_frontend
