#pragma once

// strata-tui's application (spec docs/superpowers/specs/2026-10-28-server-tui-design.md): what the server answers (the
// page's routes), the six views drawn from it - Monitor (the default: the graph dashboard), Models, Downloads, Settings,
// Log, About - and the keys turned into the requests the page would send. No terminal and no network here: the program
// (tui_main.cpp) runs the requests and writes the screen.

#include "secure_frontend/json.hpp"
#include "secure_frontend/tui_input.hpp"
#include "secure_frontend/tui_screen.hpp"

#include <ctime>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace secure_frontend::tui {

enum class View { monitor, models, downloads, settings, log, about };

// A request to the server: what for ("metrics", "models", ... or an action), its method, path and JSON body.
struct Request final {
  std::string purpose;
  std::string method;
  std::string path;
  std::string body;
  friend bool operator==(const Request&, const Request&) = default;
};

struct Outcome final {
  std::vector<Request> requests;
  bool quit{false};
};

class App final {
 public:
  // `key`: the API key the server checks (shown only on request in Settings); none: not known yet (set_key).
  App(Capabilities capabilities, std::optional<std::string> key);

  // The server's side. `reachable`: false for a server listening on an address that is not this PC's loopback (this
  // program talks to the server over 127.0.0.1 or ::1 only, so its requests cannot reach it).
  void server_state(std::string text, bool up, bool reachable = true);  // "starting", "http://127.0.0.1:8080", "stopped (exit 1)"
  // Known once the server has started: its key, or none - a keyless server on this PC (`keyless`), or a key that could
  // not be read.
  void set_key(std::optional<std::string> key, bool keyless);
  // A request's answer (status 0: no answer). A poll's failure is said on the status line until that poll answers again.
  void answer(const Request& request, int status, std::string_view body);
  void log_line(std::string line);

  // The user's side: a key, and the polls due at each tick (the header's and the current view's).
  [[nodiscard]] Outcome key(const Key& key);
  [[nodiscard]] std::vector<Request> polls() const;

  // The clock at each turn of the program's loop: an action's words leave the status line after kStatusSeconds (a
  // poll's failure stays until that poll answers).
  void tick(std::time_t now);
  static constexpr std::time_t kStatusSeconds = 10;

  void draw(Screen& screen, std::time_t now) const;
  [[nodiscard]] View view() const noexcept { return view_; }
  [[nodiscard]] const std::string& status() const noexcept { return status_; }
  // A chart's samples, the newest last: its own history of the server's readings (one a second, kHistorySamples
  // kept; the server keeps a minute).
  [[nodiscard]] std::vector<double> history(std::string_view series) const;
  static constexpr std::size_t kHistorySamples = 300;

 private:
  // the views
  void draw_header(Screen& screen, std::time_t now) const;
  void draw_tabs(Screen& screen) const;
  void draw_keys(Screen& screen) const;
  void draw_monitor(Screen& screen, Rect area) const;
  void draw_models(Screen& screen, Rect area) const;
  void draw_downloads(Screen& screen, Rect area) const;
  void draw_settings(Screen& screen, Rect area) const;
  void draw_log(Screen& screen, Rect area) const;
  void draw_about(Screen& screen, Rect area) const;
  void draw_prompt(Screen& screen) const;
  // the keys
  void show(View view);  // a view switch (1-6, Tab, Shift-Tab, F1-F6): the option focus off, Settings read again
  [[nodiscard]] Outcome models_key(const Key& key);
  [[nodiscard]] Outcome downloads_key(const Key& key);
  [[nodiscard]] Outcome settings_key(const Key& key);
  [[nodiscard]] Outcome log_key(const Key& key);
  [[nodiscard]] Outcome prompt_key(const Key& key);
  // the selected card and its options as the start body has them
  [[nodiscard]] const JsonValue* cards() const;
  [[nodiscard]] const JsonValue* selected_card() const;
  [[nodiscard]] std::string options_body(const JsonValue& card) const;
  void change_option(const JsonValue& card, int direction);
  // the status line: words said (an action's, or a poll's failure), an action's words cleared
  void say(std::string words, bool error);
  void clear_words();
  // the polls' failures by poll ("metrics", ...), the one the status line says
  void poll_failed(const std::string& purpose, int status, const JsonValue* body);
  void poll_answered(const std::string& purpose);
  void show_next_problem();  // the status line on the first failure left (none: cleared, unless an action's words show)
  // a /metrics answer into the charts' own history
  void record_history(const JsonValue& metrics);

  Capabilities capabilities_;
  std::optional<std::string> key_;
  enum class KeyState { unknown, held, keyless, unreadable };
  KeyState key_state_{KeyState::unknown};
  View view_{View::monitor};
  std::string server_{"starting"};
  bool up_{false};
  bool reachable_{true};
  std::map<std::string, std::string> poll_problems_;
  std::string shown_problem_;
  std::optional<JsonValue> metrics_;
  std::optional<JsonValue> models_;
  std::optional<JsonValue> downloads_;
  std::optional<JsonValue> settings_;
  std::optional<JsonValue> about_;
  std::map<std::string, std::deque<double>> history_;  // by series, the newest last
  std::optional<long long> history_time_;               // the time of the answer last recorded
  std::deque<std::string> log_;
  std::string status_;
  bool status_error_{false};
  unsigned long long status_serial_{0};  // one more each time words are said
  unsigned long long ticked_serial_{0};  // the words tick() last saw, since status_since_
  std::time_t status_since_{0};
  // Models: the selected card, the option in focus (-1: the card list), edited values by card and option
  std::size_t card_{0};
  int option_{-1};
  std::map<std::string, std::map<std::string, std::string>> edited_;  // card -> option -> its JSON value
  // Downloads, Settings, Log: the selection, the pending setting changes, the log's scroll
  std::size_t download_{0};
  std::size_t setting_{0};
  std::map<std::string, std::string> pending_;  // key -> its JSON value ("null": back to the default)
  std::map<std::string, std::string> saving_;   // what the save under way sent
  bool settings_stale_{true};
  bool show_key_{false};
  int log_scroll_{0};           // lines above the newest
  mutable int log_rows_{20};    // the lines the Log's box showed when last drawn (the scroll stops at its first page)
  // a one-line prompt: a text field (a setting's value, ROTATE) or a yes/no question (quit; stop one card and start
  // another, or restart the running one, as the page asks)
  enum class Prompt { none, quit, edit_setting, rotate, start, remove, calibrate_first };
  Prompt prompt_{Prompt::none};
  std::string input_;
  std::optional<Request> confirm_;   // the start (or the card removal) a yes sends
  std::optional<Request> calibrate_;  // calibrate_first: the calibrate-then-start c sends (s sends confirm_)
  std::string question_;             // and its question
  std::optional<JsonValue> editing_;  // the setting the edit field was opened on (its key, type and range)
};

// make --debugging (owner 2026-10-29): before a debugging build's TUI starts the server it shows what that build logs
// (window::kDebuggingNotice) and starts only on y; any other key quits without starting it. A wheel turn is no answer.
enum class GateAnswer { waiting, accepted, declined };
[[nodiscard]] GateAnswer debugging_gate(const Key& key) noexcept;
// The notice as a screen: its title, its paragraphs wrapped at the screen's width, then the keys (on the last row when
// the text does not fit).
void draw_debugging_notice(Screen& screen, std::string_view notice);

}  // namespace secure_frontend::tui
