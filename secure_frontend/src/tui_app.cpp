#include "secure_frontend/tui_app.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <system_error>
#include <utility>
#include <variant>

namespace secure_frontend::tui {
namespace {

// The palette (256 colours): the brand's blue, the accent, the states, the charts' series.
constexpr int kBrand = 25;
constexpr int kAccent = 39;
constexpr int kText = 252;
constexpr int kMuted = 244;
constexpr int kBorder = 240;
constexpr int kGreen = 42;
constexpr int kYellow = 214;
constexpr int kRed = 203;
constexpr int kCyan = 45;
constexpr int kPurple = 141;
constexpr int kOrange = 208;
constexpr int kPanel = 235;

constexpr Style kNormal{kText, kDefault};
constexpr Style kDim{kMuted, kDefault};
constexpr Style kFrame{kBorder, kDefault};
constexpr Style kTitle{kAccent, kDefault, true};
constexpr Style kSelected{kText, kPanel, true};

constexpr JsonLimits kAnswerLimits{.max_bytes = 8U << 20U, .max_depth = 16U, .max_container_entries = 1U << 18U,
                                   .max_string_bytes = 1U << 20U, .allow_fractions = true};
constexpr std::size_t kLogLines = 2000U;

const JsonValue* member(const JsonValue* object, const std::string_view key) {
  if (object == nullptr || object->if_object() == nullptr) return nullptr;
  for (const auto& entry : *object->if_object()) {
    if (entry.key == key) return &entry.value;
  }
  return nullptr;
}

std::string text(const JsonValue* value, const std::string& fallback = {}) {
  if (value == nullptr) return fallback;
  if (const auto* s = value->if_string()) return *s;
  if (const auto* i = value->if_integer()) return std::to_string(*i);
  if (const auto* d = value->if_decimal()) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%g", *d);
    return buffer;
  }
  if (const auto* b = value->if_boolean()) return *b ? "on" : "off";
  return fallback;
}

// A list's items as one line, "a, b".
std::string items_text(const JsonValue& list) {
  std::string out;
  if (const auto* items = list.if_array()) {
    for (const auto& item : *items) out += (out.empty() ? "" : ", ") + text(&item);
  }
  return out;
}

// A setting's value as Settings shows it: a list by its items ("none" when empty), anything else as text().
std::string setting_text(const JsonValue& value) {
  if (value.if_array() == nullptr) return text(&value);
  const std::string out = items_text(value);
  return out.empty() ? "none" : out;
}

// A setting's value as its edit field starts: a list by its items, a text as itself, a number by its digits.
std::string editable_text(const JsonValue& value) { return value.if_array() != nullptr ? items_text(value) : text(&value); }

std::optional<double> number(const JsonValue* value) {
  if (value == nullptr) return std::nullopt;
  if (const auto* d = value->if_decimal()) return *d;
  if (const auto* i = value->if_integer()) return static_cast<double>(*i);
  return std::nullopt;
}

std::string json_string(const std::string_view value) {
  std::string out;
  append_json_string(&out, value);
  return out;
}

std::string fixed(const double value, const int decimals) {
  char buffer[32];
  std::snprintf(buffer, sizeof buffer, "%.*f", decimals, value);
  return buffer;
}

std::string gib(const double bytes) { return fixed(bytes / 1073741824.0, bytes >= 100.0 * 1073741824.0 ? 0 : 1); }

std::string clock_text(const std::time_t when) {
  std::tm local{};
#if defined(_WIN32)
  localtime_s(&local, &when);
#else
  localtime_r(&when, &local);
#endif
  char buffer[16];
  std::strftime(buffer, sizeof buffer, "%H:%M:%S", &local);
  return buffer;
}

// A history series of /metrics (null readings as NaN).
std::vector<double> series(const JsonValue* metrics, const std::string_view name) {
  std::vector<double> out;
  const auto* list = member(member(metrics, "history"), name);
  if (list == nullptr || list->if_array() == nullptr) return out;
  for (const auto& value : *list->if_array()) out.push_back(number(&value).value_or(std::nan("")));
  return out;
}

double highest(const std::vector<double>& values, const double floor) {
  double top = floor;
  for (const double v : values) {
    if (std::isfinite(v)) top = std::max(top, v);
  }
  return top;
}

std::optional<double> last_of(const std::vector<double>& values) {
  for (auto it = values.rbegin(); it != values.rend(); ++it) {
    if (std::isfinite(*it)) return *it;
  }
  return std::nullopt;
}

Style state_style(const std::string_view state) {
  if (state == "running" || state == "generating" || state == "ready" || state == "installed") return Style{kGreen, kDefault, true};
  if (state == "starting" || state == "calibrating" || state == "reading" || state == "downloading" || state == "preparing" ||
      state == "queued") {
    return Style{kYellow, kDefault, true};
  }
  if (state == "failed" || state == "error") return Style{kRed, kDefault, true};
  return kDim;
}

char32_t state_glyph(const std::string_view state) {
  if (state == "running") return char32_t{0x25CF};       // filled circle
  if (state == "starting" || state == "calibrating") return char32_t{0x25D0};  // half circle
  if (state == "failed") return char32_t{0x2716};        // heavy x
  return char32_t{0x25CB};                               // circle
}

// A chart panel: its box, its title with the current reading, the chart, and the top of its scale.
void chart_panel(Screen& screen, const Rect area, const std::string& title, const std::string& reading, const std::vector<double>& values,
                 const double top, const std::string& top_label, const int color, const Capabilities& capabilities) {
  if (area.width < 8 || area.height < 4) return;
  screen.box(area, title, kFrame, kTitle);
  const Rect inner = area.inner();
  screen.text(inner.x, inner.y, reading, Style{color, kDefault, true}, inner.width);
  const int label_width = text_width(top_label);
  if (inner.width > label_width + text_width(reading) + 2) screen.text(inner.x + inner.width - label_width, inner.y, top_label, kDim);
  const Rect plot{inner.x, inner.y + 1, inner.width, inner.height - 1};
  if (plot.height > 0) {
    for (int x = plot.x; x < plot.x + plot.width; ++x) screen.text(x, plot.y + plot.height - 1, " ", kDim);
    screen.chart(plot, values, top, Style{color, kDefault}, capabilities);
  }
}

// One labelled line of a panel: "label  value".
void row(Screen& screen, const Rect area, const int line, const std::string_view label, const std::string& value,
         const Style value_style = kNormal) {
  if (line >= area.height) return;
  screen.field(area.x, area.y + line, std::min(14, area.width), label, kDim);
  if (area.width > 15) screen.field(area.x + 15, area.y + line, area.width - 15, value, value_style);
}

void gauge_row(Screen& screen, const Rect area, const int line, const std::string_view label, const double used, const double total,
               const int color) {
  if (line >= area.height || total <= 0.0) return;
  const std::string figures = gib(used) + " / " + gib(total) + " GB";
  screen.field(area.x, area.y + line, std::min(14, area.width), label, kDim);
  const int bar = area.width - 15 - text_width(figures) - 1;
  if (bar >= 4) {
    screen.gauge(area.x + 15, area.y + line, bar, used / total, Style{color, kDefault}, Style{kBorder, kDefault});
    screen.text(area.x + 16 + bar, area.y + line, figures, kNormal);
  } else if (area.width > 15) {
    screen.field(area.x + 15, area.y + line, area.width - 15, figures, kNormal);
  }
}

constexpr std::array<std::pair<View, std::string_view>, 6> kViews{{{View::monitor, "Monitor"},
                                                                   {View::models, "Models"},
                                                                   {View::downloads, "Downloads"},
                                                                   {View::settings, "Settings"},
                                                                   {View::log, "Log"},
                                                                   {View::about, "About"}}};

std::size_t view_index(const View view) {
  for (std::size_t i = 0; i < kViews.size(); ++i) {
    if (kViews[i].first == view) return i;
  }
  return 0;
}

// The option kinds the page knows (app.js optionControl): only these are changed here and sent with a start.
bool sent_kind(const std::string_view kind) {
  return kind == "boolean" || kind == "integer" || kind == "decimal" || kind == "choice" || kind == "integer_or_auto" ||
         kind == "decimal_or_auto" || kind == "devices";
}

const JsonValue* data_list(const std::optional<JsonValue>& document) {
  const auto* list = member(document ? &*document : nullptr, "data");
  return list != nullptr && list->if_array() != nullptr ? list : nullptr;
}

// A download entry queued, under way or paused: its state says how that goes.
bool under_way(const JsonValue& entry) {
  const std::string state = text(member(&entry, "state"));
  return state == "queued" || state == "downloading" || state == "verifying" || state == "preparing" || state == "paused";
}

// What this PC already has of a download entry, from the list's "have" (owner 2026-10-28, as the page shows it):
// "Downloaded" or "Locally provided"; empty when it has neither, or while it is under way.
std::string on_this_pc(const JsonValue& entry) {
  if (under_way(entry)) return {};
  const std::string have = text(member(&entry, "have"));
  if (have == "downloaded") return "Downloaded";
  if (have == "local") return "Locally provided";
  return {};
}

// A card has a file of the entry's name and another size (another revision): said, and the download still offered.
bool other_version(const JsonValue& entry) { return !under_way(entry) && text(member(&entry, "have")) == "other"; }

// " (its card: <name>)", or nothing when the list names no card.
std::string its_card(const JsonValue& entry) {
  const std::string card = text(member(&entry, "have_card"));
  return card.empty() ? std::string{} : " (its card: " + card + ")";
}

}  // namespace

App::App(const Capabilities capabilities, std::optional<std::string> key)
    : capabilities_(capabilities), key_(std::move(key)), key_state_(key_ ? KeyState::held : KeyState::unknown) {}

void App::server_state(std::string text, const bool up, const bool reachable) {
  server_ = std::move(text);
  up_ = up;
  reachable_ = reachable;
}

void App::set_key(std::optional<std::string> key, const bool keyless) {
  key_state_ = key ? KeyState::held : keyless ? KeyState::keyless : KeyState::unreadable;
  key_ = std::move(key);
}

void App::log_line(std::string line) {
  log_.push_back(std::move(line));
  while (log_.size() > kLogLines) log_.pop_front();
}

void App::say(std::string words, const bool error) {
  status_ = std::move(words);
  status_error_ = error && !status_.empty();
  ++status_serial_;
}

void App::answer(const Request& request, const int status, const std::string_view body) {
  // (a refusal's body too: its error.message is said)
  const auto parsed = parse_json(body, kAnswerLimits);
  const auto* value = std::get_if<JsonValue>(&parsed);
  const bool poll = request.purpose == "metrics" || request.purpose == "models" || request.purpose == "downloads" ||
                    request.purpose == "settings" || request.purpose == "about";
  if (poll) {
    if (status != 200 || value == nullptr) {  // (kept: the last answer stays on screen)
      // (an answer still in flight from a view left since says nothing: its poll no longer runs to clear it)
      const auto running = polls();
      if (std::any_of(running.begin(), running.end(), [&request](const Request& r) { return r.purpose == request.purpose; })) {
        poll_failed(request.purpose, status, value);
      }
      return;
    }
    poll_answered(request.purpose);
    if (request.purpose == "metrics") {
      record_history(*value);
      metrics_ = *value;
    } else if (request.purpose == "models") {
      models_ = *value;
    } else if (request.purpose == "downloads") {
      downloads_ = *value;
    } else if (request.purpose == "about") {
      about_ = *value;
    } else {
      settings_ = *value;
      settings_stale_ = false;
    }
    return;
  }
  // an action's answer: said on the status line (the server's own words when it refused)
  if (status == 0) {
    say(request.purpose + ": the server did not answer", true);
    return;
  }
  if (status >= 200 && status < 300) {
    if (request.purpose == "save settings") {
      // what this save sent is saved; a change made while it was under way waits for the next
      for (const auto& [name, sent] : saving_) {
        if (const auto still = pending_.find(name); still != pending_.end() && still->second == sent) pending_.erase(still);
      }
      saving_.clear();
      settings_stale_ = true;
      say("Settings saved; they apply when the server starts again", false);
    } else if (request.purpose == "rotate key") {
      say("A new API key was made; it applies when the server starts again (strata key show)", false);
    } else {
      say(request.purpose + ": done", false);
    }
    return;
  }
  const auto* error = member(member(value, "error"), "message");
  say(request.purpose + ": " + (error != nullptr ? text(error) : "HTTP " + std::to_string(status)), true);
}

void App::poll_failed(const std::string& purpose, const int status, const JsonValue* body) {
  std::string said;
  if (status == 0) {
    said = !reachable_ ? "strata-tui cannot reach the server at " + server_ +
                             ": it listens on an address that is not this PC's loopback (strata-tui talks to it over 127.0.0.1 or ::1)"
           : up_       ? "No answer from the server at " + server_
                       : "The server is " + server_;
  } else if (status == 401) {
    said = std::string{"The server refused the API key (HTTP 401)"} +
           (key_state_ == KeyState::unreadable ? ": strata-tui could not read the key it checks" : "");
  } else {
    const auto* message = member(member(body, "error"), "message");
    said = purpose + ": " +
           (message != nullptr ? text(message) + " (HTTP " + std::to_string(status) + ")"
            : status == 200    ? std::string{"its answer could not be read"}
                               : "HTTP " + std::to_string(status));
  }
  auto& slot = poll_problems_[purpose];
  if (slot == said) return;  // the same failure again: said already (two polls failing in their own words do not flicker)
  slot = said;
  shown_problem_ = said;
  say(said, true);
}

void App::poll_answered(const std::string& purpose) {
  if (poll_problems_.erase(purpose) == 0U) return;
  show_next_problem();
}

void App::show_next_problem() {
  const std::string next = poll_problems_.empty() ? std::string{} : poll_problems_.begin()->second;
  if (next == shown_problem_) return;
  const bool showing = status_ == shown_problem_;  // (an action's words said since stay)
  shown_problem_ = next;
  if (showing) say(next, true);
}

void App::clear_words() {
  // an action's words leave; a poll's failure still unanswered is said again
  if (!status_.empty() && status_ != shown_problem_) say(shown_problem_, true);
}

void App::tick(const std::time_t now) {
  if (status_serial_ != ticked_serial_) {  // (new words: their ten seconds start now)
    ticked_serial_ = status_serial_;
    status_since_ = now;
    return;
  }
  if (now - status_since_ >= kStatusSeconds) clear_words();
}

// The charts' series the server keeps: the decode speed, the GPU's use, memory, temperature and power, the CPU, the
// disks' reads (/metrics' history names). RAM has none there: its chart keeps the readings ("ram_used").
constexpr std::array<std::string_view, 7> kSeries{"tok_s", "gpu_util", "cpu", "gpu_mem_used", "gpu_temp", "gpu_power", "disk_read_mb"};

void App::record_history(const JsonValue& metrics) {
  // The server keeps a minute, one sample a second. The first answer seeds each series with all of it; a later one adds
  // its newest samples, as many as seconds went by since the one before (none in the same second; one for an answer
  // without its time); a clock gone back starts again.
  const auto* time = member(&metrics, "time");
  const std::optional<long long> now = time != nullptr && time->if_integer() != nullptr ? std::optional<long long>{*time->if_integer()} : std::nullopt;
  bool seed = history_.empty();
  std::size_t fresh = 1;
  if (now && history_time_) {
    if (*now < *history_time_) seed = true;
    else fresh = static_cast<std::size_t>(*now - *history_time_);
  }
  if (now) history_time_ = now;
  if (seed) history_.clear();
  const auto live_tok = number(member(member(&metrics, "live"), "tok_s"));
  for (const std::string_view name : kSeries) {
    const auto values = series(&metrics, name);
    auto& kept = history_[std::string{name}];
    const std::size_t take = seed ? values.size() : std::min(fresh, values.size());
    for (std::size_t i = values.size() - take; i < values.size(); ++i) kept.push_back(values[i]);
    // the decode speed's newest is the live one (the server samples it once a second)
    if (name == "tok_s" && take > 0U && live_tok) kept.back() = *live_tok;
    while (kept.size() > kHistorySamples) kept.pop_front();
  }
  // RAM's reading, once for each second gone by, and no more of a long gap than the server's minute gives the other
  // series (so the charts keep one time scale); a reading the server did not give: a gap
  constexpr std::size_t kServerSamples = 60;  // (serve_monitor's kMonitorHistorySamples)
  auto& ram = history_["ram_used"];
  const double ram_used = number(member(member(&metrics, "hardware"), "ram_used")).value_or(std::nan(""));
  for (std::size_t i = 0; i < (seed ? 1U : std::min(fresh, kServerSamples)); ++i) ram.push_back(ram_used);
  while (ram.size() > kHistorySamples) ram.pop_front();
}

std::vector<double> App::history(const std::string_view name) const {
  const auto found = history_.find(std::string{name});
  return found == history_.end() ? std::vector<double>{} : std::vector<double>(found->second.begin(), found->second.end());
}

std::vector<Request> App::polls() const {
  std::vector<Request> out{{"metrics", "GET", "/metrics", {}}, {"models", "GET", "/v1/strata/models", {}}};
  if (view_ == View::downloads) out.push_back({"downloads", "GET", "/v1/strata/downloads", {}});
  if (view_ == View::settings && settings_stale_) out.push_back({"settings", "POST", "/v1/strata/config", "{}"});
  if (view_ == View::about && !about_) out.push_back({"about", "GET", "/v1/strata/about", {}});
  return out;
}

// ------------------------------------------------------------------------------------------------------------- keys

void App::show(const View view) {
  view_ = view;
  option_ = -1;
  if (view == View::settings) settings_stale_ = true;
  // a failure of a poll this view no longer runs (another view's downloads, settings, About) goes with that view
  const auto running = polls();
  for (auto it = poll_problems_.begin(); it != poll_problems_.end();) {
    const std::string& purpose = it->first;
    const bool polled = std::any_of(running.begin(), running.end(), [&purpose](const Request& r) { return r.purpose == purpose; });
    it = polled ? std::next(it) : poll_problems_.erase(it);
  }
  show_next_problem();
  clear_words();
}

Outcome App::key(const Key& key) {
  if (prompt_ != Prompt::none) return prompt_key(key);
  if (key.kind == Key::Kind::ctrl && key.text == "c") {
    prompt_ = Prompt::quit;
    return {};
  }
  if (key.kind == Key::Kind::character && key.text == "q" && option_ < 0) {
    prompt_ = Prompt::quit;
    return {};
  }
  // the views: 1-6, Tab and Shift-Tab (from a card's list, not its options), F1-F6
  if (key.kind == Key::Kind::character && key.text.size() == 1U && key.text[0] >= '1' && key.text[0] <= '6' && option_ < 0) {
    show(kViews[static_cast<std::size_t>(key.text[0] - '1')].first);
    return {};
  }
  if (key.kind == Key::Kind::function && key.number >= 1 && key.number <= 6) {
    show(kViews[static_cast<std::size_t>(key.number - 1)].first);
    return {};
  }
  if ((key.kind == Key::Kind::tab || key.kind == Key::Kind::back_tab) && option_ < 0) {
    const std::size_t at = view_index(view_);
    show(kViews[(at + (key.kind == Key::Kind::tab ? 1U : kViews.size() - 1U)) % kViews.size()].first);
    return {};
  }
  // the mouse wheel moves a list's selection (the Log scrolls by it)
  Key moved = key;
  if (view_ != View::log && key.kind == Key::Kind::wheel_up) moved.kind = Key::Kind::up;
  if (view_ != View::log && key.kind == Key::Kind::wheel_down) moved.kind = Key::Kind::down;
  switch (view_) {
    case View::models: return models_key(moved);
    case View::downloads: return downloads_key(moved);
    case View::settings: return settings_key(moved);
    case View::log: return log_key(moved);
    case View::monitor:
    case View::about: break;
  }
  return {};
}

Outcome App::prompt_key(const Key& key) {
  Outcome out;
  if (key.kind == Key::Kind::wheel_up || key.kind == Key::Kind::wheel_down) return out;  // (no answer: the question stays)
  if (prompt_ == Prompt::calibrate_first) {
    const bool calibrate = key.kind == Key::Kind::character && (key.text == "c" || key.text == "C");
    const bool start = key.kind == Key::Kind::character && (key.text == "s" || key.text == "S");
    if (calibrate && calibrate_) out.requests.push_back(std::move(*calibrate_));
    else if (start && confirm_) out.requests.push_back(std::move(*confirm_));
    prompt_ = Prompt::none;
    confirm_.reset();
    calibrate_.reset();
    question_.clear();
    return out;
  }
  if (prompt_ == Prompt::quit || prompt_ == Prompt::start || prompt_ == Prompt::remove) {
    const bool yes = key.kind == Key::Kind::character && (key.text == "y" || key.text == "Y");
    if (prompt_ == Prompt::quit) out.quit = yes;
    else if (yes && confirm_) out.requests.push_back(std::move(*confirm_));
    prompt_ = Prompt::none;
    confirm_.reset();
    question_.clear();
    return out;
  }
  if (key.kind == Key::Kind::escape) {
    prompt_ = Prompt::none;
    input_.clear();
    editing_.reset();
    return out;
  }
  if (key.kind == Key::Kind::backspace) {
    const auto points = code_points(input_);
    input_.clear();
    for (std::size_t i = 0; i + 1U < points.size(); ++i) input_ += utf8(points[i]);
    return out;
  }
  if (key.kind == Key::Kind::character) {
    if (input_.size() < 4096U) input_ += key.text;
    return out;
  }
  if (key.kind != Key::Kind::enter) return out;
  if (prompt_ == Prompt::rotate) {
    if (input_ == "ROTATE") out.requests.push_back({"rotate key", "POST", "/v1/strata/key/rotate", "{\"confirm\":\"ROTATE\"}"});
    else say("The key was not rotated (type ROTATE to rotate it)", true);
  } else if (prompt_ == Prompt::edit_setting) {
    // the setting the field was opened on (the list may have been read again since)
    if (editing_) {
      const JsonValue& entry = *editing_;
      const std::string name = text(member(&entry, "key"));
      const std::string type = text(member(&entry, "type"));
      if (input_.empty()) {
        pending_[name] = "null";  // back to the default
      } else if (type == "integer") {
        // the whole text one number, within the setting's range
        long long number_typed = 0;
        const char* const typed_begin = input_.data();
        const char* const typed_end = typed_begin + input_.size();
        const auto parsed = std::from_chars(typed_begin, typed_end, number_typed);
        const auto* lowest = member(&entry, "min");
        const auto* highest_allowed = member(&entry, "max");
        const auto lo = number(lowest);
        const auto hi = number(highest_allowed);
        const bool whole = parsed.ec == std::errc{} && parsed.ptr == typed_end;
        if (whole && (!lo || static_cast<double>(number_typed) >= *lo) && (!hi || static_cast<double>(number_typed) <= *hi)) {
          pending_[name] = std::to_string(number_typed);
        } else {
          say(name + " takes a whole number" + (lo && hi ? " from " + text(lowest) + " to " + text(highest_allowed) : std::string{}), true);
        }
      } else if (type == "string array") {
        std::string list_json = "[";
        std::size_t at = 0;
        while (at <= input_.size()) {
          const std::size_t comma = input_.find(',', at);
          std::string item = input_.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
          while (!item.empty() && item.front() == ' ') item.erase(item.begin());
          while (!item.empty() && item.back() == ' ') item.pop_back();
          if (!item.empty()) list_json += (list_json.size() > 1U ? "," : "") + json_string(item);
          if (comma == std::string::npos) break;
          at = comma + 1U;
        }
        pending_[name] = list_json + "]";
      } else {
        pending_[name] = json_string(input_);
      }
    }
  }
  prompt_ = Prompt::none;
  input_.clear();
  editing_.reset();
  return out;
}

const JsonValue* App::cards() const { return data_list(models_); }

const JsonValue* App::selected_card() const {
  const auto* list = cards();
  if (list == nullptr || list->if_array()->empty()) return nullptr;
  return &(*list->if_array())[std::min(card_, list->if_array()->size() - 1U)];
}

std::string App::options_body(const JsonValue& card) const {
  const std::string name = text(member(&card, "name"));
  std::string out = "{";
  const auto* options = member(&card, "options");
  const auto edits = edited_.find(name);
  if (options != nullptr && options->if_array() != nullptr) {
    // as the page sends them (app.js collectOptions): the kinds it knows (one it does not is shown, never sent: the
    // server keeps its value), each its value or else its default; the GPUs choice once changed here (the page offers
    // it only on a PC with two or more GPUs, and sends nothing otherwise)
    for (const auto& option : *options->if_array()) {
      const std::string key = text(member(&option, "name"));
      const std::string kind = text(member(&option, "kind"));
      if (!sent_kind(kind)) continue;
      std::string value;
      if (edits != edited_.end() && edits->second.contains(key)) {
        value = edits->second.at(key);
      } else {
        const auto* current = member(&option, "value");
        if (current == nullptr || current->kind() == JsonKind::null) current = member(&option, "default");
        if (kind == "devices" || current == nullptr || current->kind() == JsonKind::null) continue;
        value = current->if_string() != nullptr ? json_string(*current->if_string()) : python_json_text(*current);
      }
      out += (out.size() > 1U ? "," : "") + json_string(key) + ":" + value;
    }
  }
  return "{\"name\":" + json_string(name) + ",\"options\":" + out + "}}";
}

// The focused option one step along (a switch flips, a choice cycles, a number moves by a step within its range).
void App::change_option(const JsonValue& card, const int direction) {
  const auto* options = member(&card, "options");
  if (options == nullptr || options->if_array() == nullptr || option_ < 0 || static_cast<std::size_t>(option_) >= options->if_array()->size()) return;
  const auto& option = (*options->if_array())[static_cast<std::size_t>(option_)];
  const std::string name = text(member(&card, "name"));
  const std::string key = text(member(&option, "name"));
  const std::string kind = text(member(&option, "kind"));
  if (!sent_kind(kind)) return;  // (a kind the page does not know is shown, never changed or sent)
  auto& edits = edited_[name];
  const JsonValue* current = member(&option, "value");
  std::optional<JsonValue> edited;
  if (const auto found = edits.find(key); found != edits.end()) {
    auto parsed = parse_json(found->second, kAnswerLimits);
    if (auto* value = std::get_if<JsonValue>(&parsed)) edited = std::move(*value);
  }
  // the value as the start sends it: its edit, its value, else (none: the card's default applies) its default
  const JsonValue* value = edited ? &*edited : current;
  if (value == nullptr || value->kind() == JsonKind::null) value = member(&option, "default");
  const auto* choices = member(&option, "choices");
  if (kind == "boolean") {
    const bool on = value != nullptr && value->if_boolean() != nullptr && *value->if_boolean();
    edits[key] = on ? "false" : "true";
  } else if (choices != nullptr && choices->if_array() != nullptr && !choices->if_array()->empty()) {
    const auto& list = *choices->if_array();
    std::size_t at = 0;
    for (std::size_t i = 0; i < list.size(); ++i) {
      if (value != nullptr && python_json_text(list[i]) == python_json_text(*value)) at = i;
    }
    at = (at + (direction > 0 ? 1U : list.size() - 1U)) % list.size();
    edits[key] = python_json_text(list[at]);
  } else if (kind == "integer" || kind == "integer_or_auto" || kind == "decimal" || kind == "decimal_or_auto") {
    const bool whole = kind.starts_with("integer");
    const bool with_auto = kind.ends_with("_or_auto");
    const auto lo = number(member(&option, "min"));
    const auto hi = number(member(&option, "max"));
    // a decimal by 0.05; a whole number by 1, or in a range by the power of two nearest a 64th of it (a context length
    // by 4096); only the bounds the option has hold it
    const double span = lo && hi ? (*hi - *lo) / 64.0 : 0.0;
    const double step = !whole ? 0.05 : span <= 1.0 ? 1.0 : std::exp2(std::round(std::log2(span)));
    const auto now = number(value);  // (none for "auto")
    std::optional<double> next;
    if (!now) {
      // auto (or no number at all): Right starts at the default, else the minimum, else 0; Left keeps auto
      if (!with_auto || direction > 0) next = number(member(&option, "default")).value_or(lo.value_or(0.0));
    } else if (with_auto && *now + direction * step < lo.value_or(0.0)) {
      edits[key] = json_string("auto");  // below the minimum (0 without one): auto again
    } else {
      next = *now + direction * step;
    }
    if (next) {
      if (lo) next = std::max(*next, *lo);
      if (hi) next = std::min(*next, *hi);
      edits[key] = whole ? std::to_string(std::llround(*next)) : fixed(*next, 2);
    }
  } else if (kind == "devices") {
    const std::string now = value != nullptr ? text(value) : "auto";
    const int most = static_cast<int>(number(member(&option, "max")).value_or(1.0));
    // auto, then 0, 0,1, 0,1,2 ... up to the most this engine runs on
    std::vector<std::string> cycle{"auto"};
    std::string list;
    for (int i = 0; i < std::min(most, 8); ++i) {
      list += (i == 0 ? "" : ",") + std::to_string(i);
      cycle.push_back(list);
    }
    std::size_t at = 0;
    for (std::size_t i = 0; i < cycle.size(); ++i) {
      if (cycle[i] == now) at = i;
    }
    edits[key] = json_string(cycle[(at + (direction > 0 ? 1U : cycle.size() - 1U)) % cycle.size()]);
  }
}

Outcome App::models_key(const Key& key) {
  Outcome out;
  const auto* list = cards();
  const std::size_t count = list != nullptr ? list->if_array()->size() : 0U;
  const auto* card = selected_card();
  const auto* options = card != nullptr ? member(card, "options") : nullptr;
  const int option_count = options != nullptr && options->if_array() != nullptr ? static_cast<int>(options->if_array()->size()) : 0;
  const std::string name = card != nullptr ? text(member(card, "name")) : std::string{};
  switch (key.kind) {
    case Key::Kind::up:
      if (option_ >= 0) option_ = std::max(0, option_ - 1);
      else if (card_ > 0) --card_;
      return out;
    case Key::Kind::down:
      if (option_ >= 0) option_ = std::min(option_count - 1, option_ + 1);
      else if (card_ + 1U < count) ++card_;
      return out;
    case Key::Kind::left:
    case Key::Kind::right:
      if (option_ >= 0 && card != nullptr) change_option(*card, key.kind == Key::Kind::right ? 1 : -1);
      return out;
    case Key::Kind::escape:
      option_ = -1;
      return out;
    case Key::Kind::enter:
      if (card != nullptr) {
        Request start{"start " + name, "POST", "/v1/strata/models/start", options_body(*card)};
        option_ = -1;
        // while a card runs, asked first as the page asks: restart it with these options, or stop it for this one
        const auto* running = member(models_ ? &*models_ : nullptr, "running");
        const bool runs = running != nullptr && running->if_string() != nullptr && text(member(&*models_, "state")) == "running";
        // owner 2026-10-29 ("starting an uncalibrated server recommend calibrating first"): a card never calibrated is
        // asked about first, as the page asks - c calibrates, then starts; s starts without (calibrate false)
        const auto* calibration = member(card, "calibration");
        const auto* calibrating = member(calibration, "running");
        if (text(member(calibration, "state")) == "missing" && (calibrating == nullptr || calibrating->kind() == JsonKind::null)) {
          const std::string now = runs ? *running->if_string() : std::string{};
          question_ = (!runs ? "Start " + name + "?"
                       : now == name ? "Restart " + name + " with these options?" : "Stop " + now + " and start " + name + "?") +
                      " It is not calibrated";
          const std::string body = options_body(*card);
          confirm_ = Request{"start " + name, "POST", "/v1/strata/models/start", body.substr(0, body.size() - 1U) + ",\"calibrate\":false}"};
          calibrate_ = Request{"calibrate " + name, "POST", "/v1/strata/models/calibrate",
                               body.substr(0, body.size() - 1U) + ",\"then_start\":true}"};
          prompt_ = Prompt::calibrate_first;
        } else if (runs) {
          const std::string& now = *running->if_string();
          question_ = now == name ? "Restart " + name + " with these options?" : "Stop " + now + " and start " + name + "?";
          confirm_ = std::move(start);
          prompt_ = Prompt::start;
        } else {
          out.requests.push_back(std::move(start));
        }
      }
      return out;
    case Key::Kind::del:
      // owner 2026-10-28 ("remove an active model card but not delete files"): the card off the list, its model files
      // kept, asked first; the running card's model is stopped first. A card from the server's config stays.
      if (card != nullptr) {
        const auto* page = member(card, "page_card");
        if (page == nullptr || page->if_boolean() == nullptr || !*page->if_boolean()) {
          say(name + "'s card is in the server's config: remove it there", true);
        } else {
          const std::string state = text(member(card, "state"));
          question_ = state == "running" || state == "starting" ? "Stop " + name + " and remove its card? Its model files stay."
                                                                : "Remove the card " + name + "? Its model files stay on this PC.";
          confirm_ = Request{"remove " + name, "POST", "/v1/strata/cards/remove", "{\"name\":" + json_string(name) + "}"};
          prompt_ = Prompt::remove;
          option_ = -1;
        }
      }
      return out;
    case Key::Kind::character: break;
    default: return out;
  }
  if (card == nullptr) return out;
  if (key.text == "o") {
    option_ = option_ < 0 && option_count > 0 ? 0 : -1;
  } else if (key.text == "s") {
    out.requests.push_back({"stop", "POST", "/v1/strata/models/stop", "{}"});
  } else if (key.text == "c") {
    out.requests.push_back({"calibrate " + name, "POST", "/v1/strata/models/calibrate", options_body(*card)});
  } else if (key.text == "x") {
    out.requests.push_back({"cancel the calibration", "POST", "/v1/strata/models/calibrate/cancel", "{}"});
  } else if (key.text == "a") {
    const auto* calibration = member(card, "calibration");
    const auto* automatic = member(calibration, "auto");
    const bool on = automatic != nullptr && automatic->if_boolean() != nullptr && *automatic->if_boolean();
    out.requests.push_back({"recalibrate when settings change", "POST", "/v1/strata/models/calibrate/auto",
                            "{\"name\":" + json_string(name) + ",\"on\":" + (on ? "false" : "true") + "}"});
  } else if (key.text == "r") {
    edited_.erase(name);  // back to the card's own values
  }
  return out;
}

Outcome App::downloads_key(const Key& key) {
  Outcome out;
  const auto* list = data_list(downloads_);
  const std::size_t count = list != nullptr ? list->if_array()->size() : 0U;
  if (key.kind == Key::Kind::up && download_ > 0) --download_;
  if (key.kind == Key::Kind::down && download_ + 1U < count) ++download_;
  if (count == 0U) return out;
  const auto& entry = (*list->if_array())[std::min(download_, count - 1U)];
  const std::string id = text(member(&entry, "id"));
  const std::string body = "{\"id\":" + json_string(id) + "}";
  if (key.kind == Key::Kind::enter || (key.kind == Key::Kind::character && key.text == "d")) {
    // a model already on this PC is not downloaded again (the page offers no Download for it either)
    if (!on_this_pc(entry).empty()) say(text(member(&entry, "name")) + " is already on this PC" + its_card(entry), false);
    else out.requests.push_back({"download " + id, "POST", "/v1/strata/downloads/start", body});
  } else if (key.kind == Key::Kind::character && key.text == "p") {
    out.requests.push_back({"pause " + id, "POST", "/v1/strata/downloads/pause", body});
  } else if (key.kind == Key::Kind::character && key.text == "x") {
    out.requests.push_back({"cancel " + id, "POST", "/v1/strata/downloads/cancel", body});
  }
  return out;
}

Outcome App::settings_key(const Key& key) {
  Outcome out;
  const auto* list = member(settings_ ? &*settings_ : nullptr, "settings");
  const std::size_t count = list != nullptr && list->if_array() != nullptr ? list->if_array()->size() : 0U;
  if (key.kind == Key::Kind::up && setting_ > 0) --setting_;
  if (key.kind == Key::Kind::down && setting_ + 1U < count) ++setting_;
  if (key.kind == Key::Kind::page_up) setting_ = setting_ > 10U ? setting_ - 10U : 0U;
  if (key.kind == Key::Kind::page_down && count > 0U) setting_ = std::min(count - 1U, setting_ + 10U);
  if (key.kind == Key::Kind::character && key.text == "k") show_key_ = !show_key_;
  if (key.kind == Key::Kind::character && key.text == "R") {
    prompt_ = Prompt::rotate;
    input_.clear();
  }
  if (key.kind == Key::Kind::character && key.text == "w" && !pending_.empty()) {
    saving_ = pending_;
    std::string values = "{";
    for (const auto& [name, value] : pending_) values += (values.size() > 1U ? "," : "") + json_string(name) + ":" + value;
    out.requests.push_back({"save settings", "POST", "/v1/strata/config/settings", "{\"values\":" + values + "}}"});
  }
  if (key.kind == Key::Kind::character && key.text == "u") pending_.clear();
  if (key.kind == Key::Kind::enter && count > 0U) {
    const auto& entry = (*list->if_array())[std::min(setting_, count - 1U)];
    const std::string name = text(member(&entry, "key"));
    if (text(member(&entry, "type")) == "boolean") {
      const auto pending = pending_.find(name);
      const auto* value = member(&entry, "value");
      const auto* fallback = member(&entry, "default");
      const bool now = pending != pending_.end() ? pending->second == "true"
                       : value != nullptr && value->if_boolean() != nullptr ? *value->if_boolean()
                                                                           : fallback != nullptr && fallback->if_boolean() != nullptr && *fallback->if_boolean();
      pending_[name] = now ? "false" : "true";
    } else {
      prompt_ = Prompt::edit_setting;
      editing_ = entry;
      // the field starts with the change not yet saved, else the saved value: a list by its items, a text as itself
      const auto pending = pending_.find(name);
      if (pending != pending_.end() && pending->second != "null") {
        auto parsed = parse_json(pending->second, kAnswerLimits);
        const auto* value = std::get_if<JsonValue>(&parsed);
        input_ = value != nullptr ? editable_text(*value) : pending->second;
      } else {
        const auto* value = member(&entry, "value");
        input_ = value != nullptr ? editable_text(*value) : std::string{};
      }
    }
  }
  return out;
}

Outcome App::log_key(const Key& key) {
  // up to the oldest page the box shows, no further
  const int most = std::max(0, static_cast<int>(log_.size()) - log_rows_);
  log_scroll_ = std::min(log_scroll_, most);
  if (key.kind == Key::Kind::up || key.kind == Key::Kind::wheel_up) log_scroll_ = std::min(most, log_scroll_ + 1);
  if (key.kind == Key::Kind::down || key.kind == Key::Kind::wheel_down) log_scroll_ = std::max(0, log_scroll_ - 1);
  if (key.kind == Key::Kind::page_up) log_scroll_ = std::min(most, log_scroll_ + 20);
  if (key.kind == Key::Kind::page_down) log_scroll_ = std::max(0, log_scroll_ - 20);
  if (key.kind == Key::Kind::end) log_scroll_ = 0;
  if (key.kind == Key::Kind::home) log_scroll_ = most;
  return {};
}

// ---------------------------------------------------------------------------------------------------------- drawing

void App::draw(Screen& screen, const std::time_t now) const {
  screen.clear();
  draw_header(screen, now);
  draw_tabs(screen);
  const Rect body{0, 2, screen.width(), screen.height() - 3};
  switch (view_) {
    case View::monitor: draw_monitor(screen, body); break;
    case View::models: draw_models(screen, body); break;
    case View::downloads: draw_downloads(screen, body); break;
    case View::settings: draw_settings(screen, body); break;
    case View::log: draw_log(screen, body); break;
    case View::about: draw_about(screen, body); break;
  }
  draw_keys(screen);
  if (prompt_ != Prompt::none) draw_prompt(screen);
}

void App::draw_header(Screen& screen, const std::time_t now) const {
  const Style bar{kText, kBrand};
  screen.fill(Rect{0, 0, screen.width(), 1}, U' ', bar);
  const Style separator{kAccent, kBrand};
  const std::string bar_glyph = " " + utf8(char32_t{0x2502}) + " ";
  int x = screen.text(0, 0, " M-Strata ", Style{231, kBrand, true});
  x += screen.text(x, 0, bar_glyph, separator);
  const std::string server = up_ ? "server " + server_ : "server: " + server_;
  x += screen.text(x, 0, server, Style{up_ ? 194 : 223, kBrand});
  const auto* running = member(models_ ? &*models_ : nullptr, "running");
  const std::string state = text(member(models_ ? &*models_ : nullptr, "state"));
  // (a server that stopped runs no model: its last answer is not shown as if live)
  const std::string model = !up_                                                ? std::string{"-"}
                            : running != nullptr && running->if_string() != nullptr ? *running->if_string() + " (" + state + ")"
                                                                                    : std::string{"no model running"};
  if (screen.width() > x + 4) {
    x += screen.text(x, 0, bar_glyph, separator);
    x += screen.text(x, 0, "model " + model, bar);
  }
  const std::string stamp = clock_text(now) + " ";
  if (screen.width() > x + text_width(stamp) + 2) screen.text(screen.width() - text_width(stamp), 0, stamp, bar);
}

void App::draw_tabs(Screen& screen) const {
  screen.fill(Rect{0, 1, screen.width(), 1}, U' ', Style{kText, kPanel});
  int x = 1;
  for (std::size_t i = 0; i < kViews.size(); ++i) {
    const bool on = kViews[i].first == view_;
    const std::string label = " " + std::to_string(i + 1) + " " + std::string{kViews[i].second} + " ";
    const Style style = on ? Style{231, kAccent, true} : Style{kMuted, kPanel};
    if (x + text_width(label) >= screen.width()) break;
    x += screen.text(x, 1, label, style) + 1;
  }
}

void App::draw_keys(Screen& screen) const {
  const int y = screen.height() - 1;
  screen.fill(Rect{0, y, screen.width(), 1}, U' ', Style{kText, kPanel});
  std::vector<std::pair<std::string, std::string>> keys{{"q", "Quit"}, {"Tab", "View"}};
  switch (view_) {
    case View::models:
      if (option_ >= 0) keys.insert(keys.end(), {{"Up/Down", "Option"}, {"Left/Right", "Change"}, {"Enter", "Start"}, {"Esc", "Cards"}});
      else keys.insert(keys.end(), {{"Enter", "Start"}, {"s", "Stop"}, {"o", "Options"}, {"c", "Calibrate"}, {"x", "Cancel"}, {"a", "Auto"}, {"Del", "Remove"}});
      break;
    case View::downloads: keys.insert(keys.end(), {{"Enter", "Download"}, {"p", "Pause"}, {"x", "Cancel"}}); break;
    case View::settings: keys.insert(keys.end(), {{"Enter", "Edit"}, {"w", "Save"}, {"u", "Undo"}, {"k", "Key"}, {"R", "Rotate"}}); break;
    case View::log: keys.insert(keys.end(), {{"Up/Down", "Scroll"}, {"End", "Newest"}}); break;
    case View::monitor:
    case View::about: break;
  }
  int x = 1;
  // a message first, whole (a value refused as it is typed, an action's answer): the keys take the room it leaves until
  // it clears (owner 2026-10-31: after the keys, a refusal's reason was cut to its first words)
  if (!status_.empty()) {
    const int width = std::min(text_width(status_), screen.width() - 2);
    screen.field(x, y, width, status_, Style{status_error_ ? kRed : kGreen, kPanel});
    x += width + 2;
  }
  for (const auto& [name, what] : keys) {
    if (x + text_width(name) + text_width(what) + 3 >= screen.width()) break;
    x += screen.text(x, y, " " + name + " ", Style{kText, kBorder, true});
    x += screen.text(x, y, " " + what + "  ", Style{kMuted, kPanel});
  }
}

void App::draw_prompt(Screen& screen) const {
  const int width = std::min(screen.width() - 4, 72);
  const bool calibrate_first = prompt_ == Prompt::calibrate_first;
  const Rect box{(screen.width() - width) / 2, screen.height() / 2 - 2, width, calibrate_first ? 7 : 5};
  screen.fill(box, U' ', Style{kText, kPanel});
  const char* const title = prompt_ == Prompt::quit     ? "Quit"
                            : prompt_ == Prompt::rotate ? "Rotate the API key"
                            : prompt_ == Prompt::start || calibrate_first ? "Start"
                            : prompt_ == Prompt::remove ? "Remove card"
                                                        : "Edit";
  screen.box(box, title, Style{kAccent, kPanel}, Style{kAccent, kPanel, true});
  const Rect inner = box.inner();
  if (calibrate_first) {
    screen.field(inner.x + 1, inner.y + 1, inner.width - 2, question_, Style{kText, kPanel});
    screen.field(inner.x + 1, inner.y + 2, inner.width - 2, "c: calibrate, then start (recommended)", Style{kText, kPanel, true});
    screen.field(inner.x + 1, inner.y + 3, inner.width - 2, "s: start without calibrating   any other key: cancel", Style{kText, kPanel});
    return;
  }
  if (prompt_ == Prompt::quit || prompt_ == Prompt::start || prompt_ == Prompt::remove) {
    const std::string question = prompt_ == Prompt::quit ? std::string{"Quit, and stop the server and its model?"} : question_;
    screen.field(inner.x + 1, inner.y + 1, inner.width - 2, question + "  y / n", Style{kText, kPanel});
    return;
  }
  const std::string question = prompt_ == Prompt::rotate ? "Type ROTATE and Enter (Esc: keep the key)" : "The new value, Enter to keep it, Esc to leave it";
  screen.field(inner.x + 1, inner.y, inner.width - 2, question, Style{kMuted, kPanel});
  screen.field(inner.x + 1, inner.y + 1, inner.width - 2, input_ + "_", Style{231, kPanel, true});
}

namespace {

// A disk's rate as the page says it (upstream's tile): MB/s, GB/s past 1000.
std::string rate_text(const double mb) { return mb >= 1000.0 ? fixed(mb / 1024.0, 2) + " GB/s" : fixed(mb, mb < 10.0 ? 1 : 0) + " MB/s"; }

// A count in at most five cells: 1234, 12.3k, 123k, 1.2M, 12M.
std::string count_text(const double n) {
  if (n < 10000.0) return fixed(n, 0);
  if (n < 1e6) return fixed(n / 1000.0, n < 1e5 ? 1 : 0) + "k";
  return fixed(n / 1e6, n < 1e7 ? 1 : 0) + "M";
}

std::string ago_text(const long long seconds) {
  if (seconds < 60) return std::to_string(std::max(0LL, seconds)) + " s ago";
  if (seconds < 3600) return std::to_string(seconds / 60) + " min ago";
  return std::to_string(seconds / 3600) + " h ago";
}

std::optional<bool> flag(const JsonValue* object, const std::string_view key) {
  const auto* value = member(object, key);
  return value != nullptr && value->if_boolean() != nullptr ? std::optional<bool>{*value->if_boolean()} : std::nullopt;
}

// One labelled line of a panel with a bar filled to `fraction` and its figures after it (the figures alone where the bar
// has no room).
void meter_row(Screen& screen, const Rect area, const int line, const std::string_view label, const double fraction,
               const std::string& figures, const int color) {
  if (line >= area.height) return;
  screen.field(area.x, area.y + line, std::min(14, area.width), label, kDim);
  const int bar = area.width - 15 - text_width(figures) - 1;
  if (bar >= 4) {
    screen.gauge(area.x + 15, area.y + line, bar, fraction, Style{color, kDefault}, Style{kBorder, kDefault});
    screen.text(area.x + 16 + bar, area.y + line, figures, kNormal);
  } else if (area.width > 15) {
    screen.field(area.x + 15, area.y + line, area.width - 15, figures, kNormal);
  }
}

// The Monitor's rows: the eight charts in rows of `columns`, then the three panels side by side, then the last requests
// across the width. A short console drops the last requests first, then the panels; what the charts leave goes to the
// last requests (more of them), else to the panels.
struct MonitorLayout final {
  int columns{1};
  int cell_height{4};
  int panels_height{0};    // 0: none
  int requests_height{0};  // 0: none
};

MonitorLayout monitor_layout(const Rect area) {
  constexpr int kCharts = 8;
  constexpr int kCellLeast = 5;      // a chart's border, its reading and two rows of dots
  constexpr int kCellMost = 10;
  constexpr int kPanels = 14;        // the Engine panel's twelve lines and its border
  constexpr int kPanelsWidth = 120;  // a third of it each: the 23-cell value column every panel line fits
  constexpr int kRequestsLeast = 6;  // the border, the header and three requests
  MonitorLayout out;
  out.columns = area.width >= 120 ? 4 : area.width >= 80 ? 2 : 1;
  const int rows = kCharts / out.columns;
  if (area.width >= kPanelsWidth && area.height >= rows * kCellLeast + kPanels) out.panels_height = kPanels;
  if (area.height >= rows * kCellLeast + out.panels_height + kRequestsLeast) out.requests_height = kRequestsLeast;
  out.cell_height = std::clamp((area.height - out.panels_height - out.requests_height) / rows, 4, kCellMost);
  const int left = area.height - rows * out.cell_height - out.panels_height - out.requests_height;
  if (left > 0 && out.requests_height > 0) out.requests_height += left;
  else if (left > 0 && out.panels_height > 0) out.panels_height += left;
  return out;
}

// Engine: the model, whether it is loaded (unloaded after idling, stopped: said plainly, and none of a loaded engine's
// figures), its context, the request it is on, its memory, what it served.
void engine_panel(Screen& screen, const Rect area, const JsonValue* metrics, const bool up) {
  screen.box(area, "Engine", kFrame, kTitle);
  const Rect e = area.inner();
  const auto* engine = member(metrics, "engine");
  const auto* live = up ? member(metrics, "live") : nullptr;
  const bool loaded = up && flag(engine, "loaded").value_or(true);  // (an older server does not say: as it reads)
  const std::string health = text(member(engine, "health"), "-");
  const std::string now = text(member(live, "state"), "-");
  // not loaded: unloaded after idling, else what the server's live state says (stopped, starting, error); "none" (no
  // model) is stopped
  const std::string said = !loaded ? (now != "-" ? now : health) : health;
  const std::string status = !up                                                    ? std::string{"stopped"}
                             : !loaded && flag(engine, "unloaded").value_or(false) ? std::string{"unloaded after idling"}
                             : said == "none" || said == "-"                       ? std::string{"stopped"}
                                                                                    : said;
  const std::string model = text(member(engine, "model"), "-");
  row(screen, e, 0, "Model", model.empty() ? "-" : model, Style{kText, kDefault, true});
  row(screen, e, 1, "Status", status, state_style(status));
  const auto context = loaded ? number(member(engine, "max_context")) : std::nullopt;
  row(screen, e, 2, "Context", context ? fixed(*context, 0) + " tokens" : "-");
  const auto queued = number(member(live, "queued"));
  row(screen, e, 3, "Now", now + (queued && *queued > 0.0 ? ", " + count_text(*queued) + " queued" : std::string{}), state_style(now));
  // the request under way (each value fits the 23 cells a 120-column console gives it: counts in at most five)
  const bool working = loaded && (now == "generating" || now == "reading");
  const auto generated = working ? number(member(live, "generated")) : std::nullopt;
  const auto most = working ? number(member(live, "max_tokens")) : std::nullopt;
  row(screen, e, 4, "Answer",
      generated ? count_text(*generated) + (most ? " of " + count_text(*most) : std::string{}) + " tokens" : std::string{"-"});
  const auto prompt = working ? number(member(live, "prompt_tokens")) : std::nullopt;
  const auto prefill = working ? number(member(live, "prefill_tok_s")) : std::nullopt;
  row(screen, e, 5, "Prompt",
      prompt ? count_text(*prompt) + (prefill ? " at " + fixed(*prefill, 1) + " tok/s" : std::string{" tokens"}) : std::string{"-"});
  const auto elapsed = working ? number(member(live, "elapsed_s")) : std::nullopt;
  row(screen, e, 6, "Elapsed", elapsed ? fixed(*elapsed, 1) + " s" : "-");
  const auto* memory = loaded ? member(engine, "memory") : nullptr;
  const auto experts = number(member(memory, "experts_ram"));
  const auto experts_bytes = number(member(memory, "experts_ram_bytes"));
  row(screen, e, 7, "Experts (RAM)",
      experts && experts_bytes ? fixed(*experts, 0) + ", " + gib(*experts_bytes) + " GB"
      : experts                ? fixed(*experts, 0)
      : experts_bytes          ? gib(*experts_bytes) + " GB"
                               : std::string{"-"});
  const auto in_vram = number(member(memory, "experts_vram"));
  row(screen, e, 8, "Experts (VRAM)", in_vram ? fixed(*in_vram, 0) : "-");
  const auto process = number(member(memory, "process_bytes"));
  row(screen, e, 9, "Process", process ? gib(*process) + " GB" : "-");
  const auto* totals = member(metrics, "totals");
  const auto served = number(member(totals, "requests"));
  row(screen, e, 10, "Served", served ? count_text(*served) + (*served == 1.0 ? " request" : " requests") : "-");
  const auto tokens_out = number(member(totals, "output_tokens"));
  row(screen, e, 11, "Tokens out", tokens_out ? count_text(*tokens_out) : "-");
}

// Machine: the GPU, its memory and RAM against their totals, the disks' rates, the PCIe link, the CPU.
void machine_panel(Screen& screen, const Rect area, const JsonValue* metrics, const bool up, const bool has_gpu) {
  screen.box(area, "Machine", kFrame, kTitle);
  const Rect m = area.inner();
  const auto* hardware = up ? member(metrics, "hardware") : nullptr;
  const auto* fixed_hw = member(metrics, "hardware_static");
  std::string gpu_name = has_gpu ? text(member(fixed_hw, "gpu_name"), "-") : std::string{"none"};
  if (gpu_name.starts_with("NVIDIA ")) gpu_name.erase(0, 7);  // ("GeForce RTX 4080 SUPER" fits a 120-column console's panel)
  row(screen, m, 0, "GPU", gpu_name);
  const auto against =[&screen, m](const int line, const std::string_view label, const std::optional<double> used,
                                    const std::optional<double> total, const int color) {
    if (used && total && *total > 0.0) meter_row(screen, m, line, label, *used / *total, gib(*used) + " / " + gib(*total) + " GB", color);
    else row(screen, m, line, label, "-");
  };
  if (has_gpu) against(1, "GPU memory", number(member(hardware, "gpu_mem_used")), number(member(hardware, "gpu_mem_total")), kPurple);
  against(2, "RAM", number(member(hardware, "ram_used")), number(member(hardware, "ram_total")), kOrange);
  const auto reads = number(member(hardware, "disk_read_mb"));
  const auto writes = number(member(hardware, "disk_write_mb"));
  row(screen, m, 3, "Disk reads", reads ? rate_text(*reads) : "-");
  row(screen, m, 4, "Disk writes", writes ? rate_text(*writes) : "-");
  const auto gen = number(member(hardware, "gpu_pcie_gen"));
  row(screen, m, 5, "PCIe",
      gen ? "gen " + fixed(*gen, 0) + " of " + text(member(hardware, "gpu_pcie_gen_max"), "-") + ", x" + text(member(hardware, "gpu_pcie_width"), "-")
          : std::string{"-"});
  row(screen, m, 6, "CPU threads", text(member(fixed_hw, "threads"), "-"));
  if (const auto* note = member(fixed_hw, "gpu_note"); note != nullptr && note->if_string() != nullptr) row(screen, m, 7, "Note", *note->if_string(), kDim);
}

// Conversation cache: whether it is on and its room, what is parked of what it may hold, its parks, restores and
// evictions, its last event, and how much of the prompts it gave back. "Not used by this engine" for an engine without
// it, "no model running" with none loaded; every figure "-" from a server that does not report it.
void cache_panel(Screen& screen, const Rect area, const JsonValue* metrics, const bool up) {
  screen.box(area, "Conversation cache", kFrame, kTitle);
  const Rect c = area.inner();
  const auto* cache = member(metrics, "conversation_cache");
  if (cache == nullptr || cache->if_object() == nullptr) {
    for (int line = 0; const char* label : {"Cache", "Parked", "Held", "Parks", "Restores", "Evictions", "Last event", "Last event at",
                                            "Reused tokens", "Requests"}) {
      row(screen, c, line++, label, "-");
    }
    return;
  }
  const auto supported = flag(cache, "supported");
  if (!up || !supported || !*supported) {
    row(screen, c, 0, "Cache", up && supported ? "Not used by this engine" : "no model running", kDim);
    return;
  }
  const auto slots = number(member(cache, "slots"));
  const auto budget = number(member(cache, "budget_mib"));
  const double budget_bytes = budget ? *budget * 1048576.0 : 0.0;
  std::string room;
  if (slots) room = fixed(*slots, 0) + " slots";
  if (budget) room += (room.empty() ? "" : ", ") + gib(budget_bytes) + " GB";
  row(screen, c, 0, "Cache", flag(cache, "enabled").value_or(false) ? (room.empty() ? std::string{"on"} : "on (" + room + ")") : std::string{"off"});
  const auto parked = number(member(cache, "parked"));
  if (parked && slots && *slots > 0.0) meter_row(screen, c, 1, "Parked", *parked / *slots, fixed(*parked, 0) + " of " + fixed(*slots, 0), kCyan);
  else row(screen, c, 1, "Parked", parked ? fixed(*parked, 0) : "-");
  const auto held = number(member(cache, "bytes"));
  if (held && budget_bytes > 0.0) meter_row(screen, c, 2, "Held", *held / budget_bytes, gib(*held) + " / " + gib(budget_bytes) + " GB", kPurple);
  else row(screen, c, 2, "Held", held ? gib(*held) + " GB" : "-");
  row(screen, c, 3, "Parks", text(member(cache, "parks"), "-"));
  row(screen, c, 4, "Restores", text(member(cache, "restores"), "-"));
  row(screen, c, 5, "Evictions", text(member(cache, "evictions"), "-"));
  const auto* event = member(cache, "last_event");
  const bool happened = event != nullptr && event->if_string() != nullptr;
  const auto tokens = number(member(cache, "last_tokens"));
  const auto at = number(member(cache, "last_at"));
  const auto server_now = number(member(metrics, "time"));
  row(screen, c, 6, "Last event", happened ? *event->if_string() + (tokens ? " " + count_text(*tokens) + " tokens" : std::string{}) : "none yet");
  row(screen, c, 7, "Last event at", happened && at && server_now ? ago_text(static_cast<long long>(*server_now - *at)) : "-");
  const auto reused = number(member(cache, "reused_tokens"));
  const auto prompted = number(member(cache, "prompt_tokens"));
  row(screen, c, 8, "Reused tokens",
      !reused || !prompted ? std::string{"-"}
      : *prompted <= 0.0   ? std::string{"none yet"}
                           : count_text(*reused) + " of " + count_text(*prompted) + " (" + fixed(*reused / *prompted * 100.0, 0) + "%)");
  const auto requests = number(member(cache, "requests"));
  const auto requests_reused = number(member(cache, "requests_reused"));
  row(screen, c, 9, "Requests", requests && requests_reused ? count_text(*requests_reused) + " of " + count_text(*requests) + " reused" : "-");
}

// Last requests, across the width, the newest first: time, model, prompt and reused tokens, output tokens, decode and
// prefill speed, finish.
void requests_panel(Screen& screen, const Rect area, const JsonValue* metrics) {
  screen.box(area, "Last requests", kFrame, kTitle);
  const Rect r = area.inner();
  static constexpr std::array<int, 8> kWidths{8, 0, 7, 7, 6, 7, 8, 8};  // (the model's: the rest)
  static constexpr std::array<bool, 8> kRight{false, false, true, true, true, true, true, false};
  const int model_width = std::min(36, r.width - (8 + 7 + 7 + 6 + 7 + 8 + 8 + 2 * 7));  // (the figures follow the name)
  const auto columns = [&screen, r, model_width](const int line, const std::array<std::string, 8>& cells, const Style style) {
    int x = r.x;
    for (std::size_t i = 0; i < cells.size(); ++i) {
      const int room = i == 1U ? model_width : kWidths[i];
      if (room < 4) continue;               // (a model column with no room: left out)
      if (x + room > r.x + r.width) break;  // (the columns past the box's edge: left out)
      std::string shown = cells[i];
      if (kRight[i] && text_width(shown) < room) shown = std::string(static_cast<std::size_t>(room - text_width(shown)), ' ') + shown;
      screen.field(x, r.y + line, room, shown, style);
      x += room + 2;
    }
  };
  if (r.height < 1) return;
  columns(0, {"time", "model", "prompt", "reused", "out", "tok/s", "prefill", "finish"}, kDim);
  const auto* list = member(metrics, "requests");
  if (list == nullptr || list->if_array() == nullptr) return;
  int line = 1;
  // (the server lists them the newest first)
  for (auto it = list->if_array()->begin(); it != list->if_array()->end() && line < r.height; ++it, ++line) {
    const auto* entry = &*it;
    const auto when = number(member(entry, "time"));
    const auto prompt_ms = number(member(entry, "prompt_ms"));
    const auto prompt_tokens = number(member(entry, "prompt_tokens"));
    const auto decode = number(member(entry, "decode_tok_s"));
    // the prompt's speed over the tokens it read (those the cache gave back took no time), as the page shows it
    const double read = prompt_tokens ? std::max(0.0, *prompt_tokens - number(member(entry, "reused")).value_or(0.0)) : 0.0;
    const std::string prefill = read > 0.0 && prompt_ms && *prompt_ms > 0.0 ? fixed(read / (*prompt_ms / 1000.0), 0) : std::string{"-"};
    columns(line,
            {when ? clock_text(static_cast<std::time_t>(*when)) : std::string{"-"}, text(member(entry, "model"), "-"),
             text(member(entry, "prompt_tokens"), "-"), text(member(entry, "reused"), "-"), text(member(entry, "output_tokens"), "-"),
             decode ? fixed(*decode, 1) : std::string{"-"}, prefill, text(member(entry, "finish"), "-")},
            kNormal);
  }
}

}  // namespace

void App::draw_monitor(Screen& screen, const Rect area) const {
  const JsonValue* metrics = metrics_ ? &*metrics_ : nullptr;
  if (metrics == nullptr) {
    // (a poll's failure in full: the status line may cut it)
    const std::string waiting = !up_ ? "The server is " + server_ + " ..."
                                : shown_problem_.empty() ? std::string{"Waiting for the server's readings ..."}
                                                         : shown_problem_;
    screen.field(area.x + 2, area.y + 1, area.width - 4, waiting, shown_problem_.empty() || !up_ ? kDim : Style{kRed, kDefault});
    return;
  }
  // the readings of now (none from a server that stopped: its last ones are not shown as if live)
  const auto* hardware = up_ ? member(metrics, "hardware") : nullptr;
  const auto* live = up_ ? member(metrics, "live") : nullptr;
  const auto* fixed_hw = member(metrics, "hardware_static");
  // the charts from their own history (about five minutes; the server keeps one)
  const auto tok = history("tok_s");
  const auto gpu = history("gpu_util");
  const auto cpu = history("cpu");
  const auto ram = history("ram_used");
  const auto memory = history("gpu_mem_used");
  const auto temperature = history("gpu_temp");
  const auto power = history("gpu_power");
  const auto disk = history("disk_read_mb");
  // a GPU when the server names one or reads one; without, its charts say so, with no scale over nothing
  const bool has_gpu = !text(member(fixed_hw, "gpu_name")).empty() || number(member(hardware, "gpu_util")).has_value() ||
                       number(member(hardware, "gpu_mem_used")).has_value() || number(member(hardware, "gpu_temp")).has_value() ||
                       number(member(hardware, "gpu_power")).has_value();
  static const std::vector<double> kNoValues;
  const double memory_total = number(member(hardware, "gpu_mem_total")).value_or(highest(memory, 1.0));
  const double power_limit = number(member(hardware, "gpu_power_limit")).value_or(highest(power, 100.0));
  const auto ram_used = number(member(hardware, "ram_used"));
  const auto ram_total = number(member(hardware, "ram_total"));
  const auto disk_read = number(member(hardware, "disk_read_mb"));
  const auto disk_peak = last_of(disk) ? std::optional<double>{highest(disk, 0.0)} : std::nullopt;
  const std::string live_state = text(member(live, "state"), "-");
  const auto live_tok = number(member(live, "tok_s"));
  const std::string decode = (live_tok ? fixed(*live_tok, 1) : last_of(tok) && up_ ? fixed(*last_of(tok), 1) : std::string{"-"}) + " tok/s";

  const MonitorLayout layout = monitor_layout(area);
  const int columns = layout.columns;
  const int cell_height = layout.cell_height;
  const int charts_height = (8 / columns) * cell_height;
  struct Chart {
    std::string title;
    std::string reading;
    const std::vector<double>* values;
    double top;
    std::string top_label;
    int color;
  };
  const auto value_or_dash = [](const std::optional<double> v, const int decimals, const std::string& unit) {
    return v ? fixed(*v, decimals) + unit : std::string{"-"};
  };
  const double tok_top = highest(tok, 1.0) * 1.15;
  const auto gpu_chart = [has_gpu](std::string title, std::string reading, const std::vector<double>* values, const double top,
                                   std::string top_label, const int color) {
    if (!has_gpu) return Chart{std::move(title), "no GPU", &kNoValues, top, {}, color};
    return Chart{std::move(title), std::move(reading), values, top, std::move(top_label), color};
  };
  const std::vector<Chart> charts{
      {"Decode", decode + "  " + live_state, &tok, tok_top, "max " + fixed(highest(tok, 0.0), 1), kCyan},
      gpu_chart("GPU use", value_or_dash(number(member(hardware, "gpu_util")), 0, "%"), &gpu, 100.0, "100%", kGreen),
      {"CPU", value_or_dash(number(member(hardware, "cpu")), 0, "%"), &cpu, 100.0, "100%", kOrange},
      {"RAM",
       ram_used && ram_total ? gib(*ram_used) + " / " + gib(*ram_total) + " GB"
       : ram_used            ? gib(*ram_used) + " GB"
                             : std::string{"-"},
       &ram, ram_total.value_or(highest(ram, 1.0)), ram_total ? gib(*ram_total) + " GB" : std::string{}, kOrange},
      gpu_chart("GPU memory",
                number(member(hardware, "gpu_mem_used")) ? gib(*number(member(hardware, "gpu_mem_used"))) + " / " + gib(memory_total) + " GB"
                                                         : std::string{"-"},
                &memory, memory_total, gib(memory_total) + " GB", kPurple),
      gpu_chart("GPU temperature", value_or_dash(number(member(hardware, "gpu_temp")), 0, " C"), &temperature, 100.0, "100 C", kRed),
      gpu_chart("GPU power", value_or_dash(number(member(hardware, "gpu_power")), 0, " W"), &power, power_limit, fixed(power_limit, 0) + " W",
                kYellow),
      {"Disk read", disk_read ? rate_text(*disk_read) : std::string{"-"}, &disk, std::max(disk_peak.value_or(0.0) * 1.15, 1.0),
       disk_peak ? "peak " + rate_text(*disk_peak) : std::string{}, kCyan}};
  // the charts: two rows of four from 120 columns, four rows of two from 80, else one a row; each the same width
  const int cell_width = area.width / columns;
  const int charts_x = area.x + (area.width - columns * cell_width) / 2;
  for (std::size_t i = 0; i < charts.size(); ++i) {
    const int column = static_cast<int>(i) % columns;
    const int chart_row = static_cast<int>(i) / columns;
    const Rect place{charts_x + column * cell_width, area.y + chart_row * cell_height, cell_width, cell_height};
    if (place.y + place.height > area.y + area.height) break;
    chart_panel(screen, place, charts[i].title, charts[i].reading, *charts[i].values, charts[i].top, charts[i].top_label, charts[i].color,
                capabilities_);
  }
  // the panels below, the same width and height; then the last requests across the width
  if (layout.panels_height > 0) {
    const int third = area.width / 3;
    const int panels_x = area.x + (area.width - 3 * third) / 2;
    const int top = area.y + charts_height;
    engine_panel(screen, Rect{panels_x, top, third, layout.panels_height}, metrics, up_);
    machine_panel(screen, Rect{panels_x + third, top, third, layout.panels_height}, metrics, up_, has_gpu);
    cache_panel(screen, Rect{panels_x + 2 * third, top, third, layout.panels_height}, metrics, up_);
  }
  if (layout.requests_height > 0) {
    requests_panel(screen, Rect{area.x, area.y + charts_height + layout.panels_height, area.width, layout.requests_height}, metrics);
  }
}

void App::draw_models(Screen& screen, const Rect area) const {
  const auto* list = cards();
  const int list_width = std::clamp(area.width / 3, 24, 44);
  const Rect left{area.x, area.y, list_width, area.height};
  const Rect right{area.x + list_width, area.y, area.width - list_width, area.height};
  screen.box(left, "Models", kFrame, kTitle);
  const Rect l = left.inner();
  if (list == nullptr || list->if_array()->empty()) {
    screen.field(l.x, l.y, l.width, models_ ? "No model cards" : "Reading the models ...", kDim);
    return;
  }
  const auto& entries = *list->if_array();
  for (std::size_t i = 0; i < entries.size() && static_cast<int>(i) < l.height; ++i) {
    const std::string state = text(member(&entries[i], "state"));
    const bool selected = i == std::min(card_, entries.size() - 1U);
    const Style base = selected ? (option_ < 0 ? Style{231, kAccent, true} : kSelected) : kNormal;
    screen.fill(Rect{l.x, l.y + static_cast<int>(i), l.width, 1}, U' ', base);
    screen.text(l.x + 1, l.y + static_cast<int>(i), utf8(state_glyph(state)), selected ? base : state_style(state));
    screen.field(l.x + 3, l.y + static_cast<int>(i), l.width - 4, text(member(&entries[i], "name")), base);
  }
  const auto* card = selected_card();
  if (card == nullptr) return;
  const std::string name = text(member(card, "name"));
  screen.box(right, name, kFrame, kTitle);
  const Rect r = right.inner();
  const std::string state = text(member(card, "state"));
  row(screen, r, 0, "State", state, state_style(state));
  row(screen, r, 1, "Profile", text(member(card, "profile")));
  row(screen, r, 2, "Folder", text(member(card, "folder"), "-"));
  if (const auto* error = member(card, "error"); error != nullptr && error->if_string() != nullptr) row(screen, r, 3, "Error", *error->if_string(), Style{kRed, kDefault});
  int line = 5;
  screen.field(r.x, r.y + line++, r.width, "Options" + std::string{option_ >= 0 ? "  (Left/Right change, Enter starts with them)" : "  (o to change)"}, kTitle);
  const auto* options = member(card, "options");
  const auto edits = edited_.find(name);
  if (options != nullptr && options->if_array() != nullptr) {
    // as many as fit, from the one that keeps the focused option in view
    const int room = std::max(1, r.height - 6 - line);
    const std::size_t first = option_ >= room ? static_cast<std::size_t>(option_ - room + 1) : 0U;
    for (std::size_t i = first; i < options->if_array()->size() && line < r.height - 6; ++i, ++line) {
      const auto& option = (*options->if_array())[i];
      const std::string key = text(member(&option, "name"));
      std::string value;
      bool changed = false;
      if (edits != edited_.end() && edits->second.contains(key)) {
        auto parsed = parse_json(edits->second.at(key), kAnswerLimits);
        const auto* edited = std::get_if<JsonValue>(&parsed);
        value = edited != nullptr ? text(edited) : edits->second.at(key);
        changed = true;
      } else {
        // its value, else the default it starts with (what the start sends)
        const auto* shown_value = member(&option, "value");
        if (shown_value == nullptr || shown_value->kind() == JsonKind::null) shown_value = member(&option, "default");
        value = text(shown_value, "-");
      }
      const bool focused = option_ == static_cast<int>(i);
      const Style label_style = focused ? Style{231, kAccent, true} : kDim;
      screen.field(r.x + 1, r.y + line, std::min(26, r.width - 2), text(member(&option, "label"), key), label_style);
      if (r.width > 30) screen.field(r.x + 28, r.y + line, r.width - 29, value + (changed ? "  *" : ""), changed ? Style{kYellow, kDefault, true} : kNormal);
    }
  }
  // the calibration, as the page says it
  const auto* calibration = member(card, "calibration");
  if (calibration == nullptr || calibration->if_object() == nullptr || line >= r.height - 1) return;
  ++line;
  screen.field(r.x, r.y + line++, r.width, "Calibration", kTitle);
  const auto* running = member(calibration, "running");
  std::string said;
  const std::string cal_state = text(member(calibration, "state"));
  if (running != nullptr && running->if_object() != nullptr) {
    said = text(member(running, "stage")) == "prompt reading" ? "Measuring how this PC reads prompts ..."
                                                              : "Measuring: trial " + text(member(running, "trial")) + " of " + text(member(running, "trials")) +
                                                                    " (" + text(member(running, "stage")) + ")";
  } else if (cal_state == "fresh") {
    const auto* args = member(calibration, "args");
    std::string kept;
    if (args != nullptr && args->if_array() != nullptr) {
      for (const auto& arg : *args->if_array()) kept += (kept.empty() ? "" : " ") + text(&arg);
    }
    said = "Calibrated " + text(member(calibration, "date")).substr(0, 10) + ": " + text(member(calibration, "before")) + " -> " +
           text(member(calibration, "after")) + " tok/s" + (kept.empty() ? " (the defaults were fastest)" : " with " + kept);
  } else if (cal_state == "stale") {
    said = "Out of date (the PC, the engine or the model changed): c calibrates again";
  } else if (cal_state == "settings_changed") {
    said = "Measured at other options: c recalibrates";
  } else {
    const auto* first = member(calibration, "first_start");
    const bool before_start = first != nullptr && first->if_boolean() != nullptr && *first->if_boolean();
    said = before_start ? "Not calibrated: the first start measures this PC first (a few minutes)"
                        : "Not calibrated: c measures this PC's fastest settings (each candidate a start of the model)";
  }
  screen.field(r.x + 1, r.y + line++, r.width - 2, said, kNormal);
  if (const auto* prefill = member(calibration, "prefill"); prefill != nullptr && line < r.height) {
    std::string words = "Prompt reading: not measured yet";
    if (prefill->if_object() != nullptr) {
      const std::string p = text(member(prefill, "state"));
      words = p == "failed" ? "Prompt reading: the last measurement did not finish (the engine's defaults)"
              : p == "stale" ? "Prompt reading: out of date; the next start measures again"
                             : "Prompt reading: chunks of " + text(member(prefill, "chunk")) + " tokens, " + text(member(prefill, "tok_s")) + " tok/s";
    }
    screen.field(r.x + 1, r.y + line++, r.width - 2, words, kDim);
  }
  // the owner (2026-10-30): a calibration with an error made a report for a bug report, in the server's logs folder
  if (const auto* report = member(calibration, "report"); report != nullptr && report->if_object() != nullptr && line < r.height) {
    screen.field(r.x + 1, r.y + line++, r.width - 2,
                 "Report for a bug report: logs/calibration-reports/" + text(member(report, "file")) + " (look it over first)",
                 Style{kYellow, kDefault, false});
  }
  const auto* automatic = member(calibration, "auto");
  if (line < r.height) {
    screen.field(r.x + 1, r.y + line, r.width - 2,
                 std::string{"Recalibrate when settings change: "} + (automatic != nullptr && automatic->if_boolean() != nullptr && *automatic->if_boolean() ? "on" : "off") + " (a)",
                 kDim);
  }
}

void App::draw_downloads(Screen& screen, const Rect area) const {
  screen.box(area, "Downloads", kFrame, kTitle);
  const Rect r = area.inner();
  const JsonValue* document = downloads_ ? &*downloads_ : nullptr;
  if (document == nullptr) {
    screen.field(r.x, r.y, r.width, "Reading the downloads ...", kDim);
    return;
  }
  const auto* disk = member(document, "disk");
  gauge_row(screen, r, 0, "Disk " + text(member(disk, "root")), number(member(disk, "total_bytes")).value_or(0.0) - number(member(disk, "free_bytes")).value_or(0.0),
            number(member(disk, "total_bytes")).value_or(0.0), kCyan);
  if (const auto* active = member(document, "active"); active != nullptr && active->if_object() != nullptr) {
    const double rate = number(member(active, "rate_bps")).value_or(0.0);
    row(screen, r, 1, "Now", text(member(active, "id")) + "  " + fixed(rate / 1048576.0, 1) + " MB/s, " + text(member(active, "eta_s"), "?") + " s left",
        Style{kYellow, kDefault, true});
  }
  const auto* list = data_list(downloads_);
  if (list == nullptr) return;
  const auto& entries = *list->if_array();
  const int first = 3;
  const int rows = r.height - first - 1;
  const std::size_t shown_from = download_ >= static_cast<std::size_t>(std::max(1, rows)) ? download_ - static_cast<std::size_t>(rows) + 1U : 0U;
  for (std::size_t i = shown_from; i < entries.size() && static_cast<int>(i - shown_from) < rows; ++i) {
    const auto& entry = entries[i];
    const int y = r.y + first + static_cast<int>(i - shown_from);
    const bool selected = i == std::min(download_, entries.size() - 1U);
    const Style base = selected ? Style{231, kAccent, true} : kNormal;
    screen.fill(Rect{r.x, y, r.width, 1}, U' ', selected ? base : Style{});
    const std::string state = text(member(&entry, "state"));
    const int name_width = std::max(10, r.width / 3);
    screen.field(r.x + 1, y, name_width, text(member(&entry, "name")), base);
    const double size = number(member(&entry, "size_bytes")).value_or(0.0);
    screen.field(r.x + 2 + name_width, y, 9, size > 0.0 ? gib(size) + " GB" : "-", selected ? base : kDim);
    // what this PC already has of it ("Downloaded", "Locally provided", "Other version"), else its download state
    std::string here = on_this_pc(entry);
    Style here_style = here == "Downloaded" ? Style{kGreen, kDefault, true} : Style{kCyan, kDefault, true};
    if (here.empty() && other_version(entry)) {
      here = "Other version";
      here_style = Style{kYellow, kDefault, true};
    }
    screen.field(r.x + 12 + name_width, y, 16, here.empty() ? state : here, selected ? base : here.empty() ? state_style(state) : here_style);
    // its bar once it has bytes (a download not started has none)
    const double total = number(member(&entry, "total")).value_or(0.0);
    const double bytes = number(member(&entry, "bytes")).value_or(0.0);
    const int bar = r.width - name_width - 32;
    if (bar > 6 && total > 0.0 && bytes > 0.0) {
      screen.gauge(r.x + 29 + name_width, y, bar, bytes / total, Style{kGreen, kDefault}, Style{kBorder, kDefault});
    }
  }
  if (!entries.empty()) {
    const auto& selected = entries[std::min(download_, entries.size() - 1U)];
    const auto* error = member(&selected, "error");
    // a model already here says so (an earlier attempt's failure no longer matters), else its error, its files installed
    // with their card not made yet (Enter makes it), another version of its file here, or its note
    const bool here = !on_this_pc(selected).empty();
    const bool failed = !here && error != nullptr && error->if_string() != nullptr;
    const std::string note = here     ? "Already on this PC" + its_card(selected)
                             : failed ? *error->if_string()
                             : text(member(&selected, "state")) == "installed"
                                 ? "Its files are on this PC, but its card was not made: Enter makes it (nothing is downloaded again)"
                             : other_version(selected)
                                 ? "Another version of this file is on this PC" + its_card(selected) + "; a download brings the catalog's"
                                 : text(member(&selected, "note"));
    screen.field(r.x + 1, r.y + r.height - 1, r.width - 2, note, failed ? Style{kRed, kDefault} : kDim);
  }
}

void App::draw_settings(Screen& screen, const Rect area) const {
  screen.box(area, "Settings", kFrame, kTitle);
  const Rect r = area.inner();
  const JsonValue* document = settings_ ? &*settings_ : nullptr;
  const auto* list = member(document, "settings");
  // the API key, shown only on request
  std::string key_text;
  Style key_style = kDim;
  switch (key_state_) {
    case KeyState::held:
      key_text = show_key_ && key_ ? *key_ : "hidden (k shows it)";
      if (show_key_) key_style = Style{kYellow, kDefault, true};
      break;
    case KeyState::keyless: key_text = "none (a keyless server on this PC)"; break;
    case KeyState::unreadable:
      key_text = "could not be read (the config's token); requests go without one";
      key_style = Style{kRed, kDefault};
      break;
    case KeyState::unknown: key_text = "not known yet (the server is starting)"; break;
  }
  row(screen, r, 0, "API key", key_text, key_style);
  if (list == nullptr || list->if_array() == nullptr) {
    screen.field(r.x, r.y + 2, r.width, "Reading the settings ...", kDim);
    return;
  }
  const auto& entries = *list->if_array();
  const int first = 2;
  const int rows = r.height - first - 3;
  const std::size_t shown_from = setting_ >= static_cast<std::size_t>(std::max(1, rows)) ? setting_ - static_cast<std::size_t>(rows) + 1U : 0U;
  const int key_width = std::min(44, r.width / 2);
  for (std::size_t i = shown_from; i < entries.size() && static_cast<int>(i - shown_from) < rows; ++i) {
    const auto& entry = entries[i];
    const int y = r.y + first + static_cast<int>(i - shown_from);
    const std::string name = text(member(&entry, "key"));
    const bool selected = i == std::min(setting_, entries.size() - 1U);
    const Style base = selected ? Style{231, kAccent, true} : kNormal;
    screen.fill(Rect{r.x, y, r.width, 1}, U' ', selected ? base : Style{});
    screen.field(r.x + 1, y, key_width, name, base);
    std::string value;
    Style value_style = selected ? base : kNormal;
    if (const auto pending = pending_.find(name); pending != pending_.end()) {
      // shown as a saved value is (on/off, a list by its items, a text as itself)
      const auto parsed = parse_json(pending->second, kAnswerLimits);
      const auto* typed = std::get_if<JsonValue>(&parsed);
      value = (pending->second == "null" ? std::string{"(default)"} : typed != nullptr ? setting_text(*typed) : pending->second) + "  * unsaved";
      if (!selected) value_style = Style{kYellow, kDefault, true};
    } else if (const auto* current = member(&entry, "value"); current != nullptr && current->kind() != JsonKind::null) {
      value = setting_text(*current);
    } else {
      const auto* fallback = member(&entry, "default");
      value = fallback != nullptr && fallback->kind() != JsonKind::null ? setting_text(*fallback) + " (default)" : "(unset)";
      if (!selected) value_style = kDim;
    }
    screen.field(r.x + 2 + key_width, y, r.width - key_width - 3, value, value_style);
  }
  if (!entries.empty()) {
    const auto& selected = entries[std::min(setting_, entries.size() - 1U)];
    screen.hline(r.x, r.y + r.height - 3, r.width, kFrame);
    screen.field(r.x + 1, r.y + r.height - 2, r.width - 2, text(member(&selected, "help")), kDim);
    screen.field(r.x + 1, r.y + r.height - 1, r.width - 2,
                 pending_.empty() ? "Saved settings apply when the server starts again." : std::to_string(pending_.size()) + " unsaved change(s): w saves, u undoes",
                 pending_.empty() ? kDim : Style{kYellow, kDefault});
  }
}

void App::draw_log(Screen& screen, const Rect area) const {
  screen.box(area, "Server log", kFrame, kTitle);
  const Rect r = area.inner();
  const int total = static_cast<int>(log_.size());
  log_rows_ = std::max(1, r.height);  // (the scroll's limit: log_key)
  const int scroll = std::min(log_scroll_, std::max(0, total - log_rows_));
  const int last = std::max(0, total - scroll);
  const int first = std::max(0, last - r.height);
  for (int i = first; i < last; ++i) {
    const std::string& line = log_[static_cast<std::size_t>(i)];
    const Style style = line.find("failed") != std::string::npos || line.find("error") != std::string::npos ? Style{kRed, kDefault}
                        : line.rfind("[strata] models:", 0) == 0                                              ? Style{kCyan, kDefault}
                                                                                                              : kNormal;
    screen.field(r.x, r.y + (i - first), r.width, line, style);
  }
  if (scroll > 0) screen.text(r.x + r.width - 18, area.y, " " + std::to_string(scroll) + " lines up ", Style{kYellow, kDefault});
}

void App::draw_about(Screen& screen, const Rect area) const {
  screen.box(area, "About", kFrame, kTitle);
  const Rect r = area.inner();
  if (!about_) {
    screen.field(r.x, r.y, r.width, "Reading ...", kDim);
    return;
  }
  // every scalar of the answer as "path  value", in its order
  int line = 0;
  const auto walk = [&](const auto& self, const JsonValue& value, const std::string& path) -> void {
    if (line >= r.height) return;
    if (const auto* object = value.if_object()) {
      for (const auto& entry : *object) self(self, entry.value, path.empty() ? entry.key : path + "." + entry.key);
      return;
    }
    if (const auto* items = value.if_array()) {
      for (std::size_t i = 0; i < items->size(); ++i) self(self, (*items)[i], path + "[" + std::to_string(i) + "]");
      return;
    }
    screen.field(r.x + 1, r.y + line, std::min(40, r.width / 2), path, kDim);
    screen.field(r.x + 2 + std::min(40, r.width / 2), r.y + line, r.width - std::min(40, r.width / 2) - 3,
                 value.kind() == JsonKind::null                         ? std::string{"-"}
                 : path.ends_with("_bytes") && number(&value).has_value() ? gib(*number(&value)) + " GB"  // a size in bytes
                                                                          : text(&value),
                 kNormal);
    ++line;
  };
  walk(walk, *about_, "");
}

GateAnswer debugging_gate(const Key& key) noexcept {
  if (key.kind == Key::Kind::wheel_up || key.kind == Key::Kind::wheel_down) return GateAnswer::waiting;
  return key.kind == Key::Kind::character && (key.text == "y" || key.text == "Y") ? GateAnswer::accepted : GateAnswer::declined;
}

void draw_debugging_notice(Screen& screen, const std::string_view notice) {
  const int left = 2;
  const int width = std::max(1, screen.width() - 2 * left);
  // each paragraph's words, line by line within the width (a word longer than a line gets a line of its own, clipped)
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start <= notice.size()) {
    const auto end = std::min(notice.find("\n\n", start), notice.size());
    const std::string_view paragraph = notice.substr(start, end - start);
    if (!lines.empty()) lines.emplace_back();
    std::string line;
    std::size_t at = 0;
    while (at < paragraph.size()) {
      const auto space = std::min(paragraph.find_first_of(" \n", at), paragraph.size());
      const std::string_view word = paragraph.substr(at, space - at);
      at = space + 1U;
      if (word.empty()) continue;
      if (!line.empty() && text_width(line) + 1 + text_width(word) > width) {
        lines.push_back(std::move(line));
        line.clear();
      }
      if (!line.empty()) line += ' ';
      line += word;
    }
    if (!line.empty()) lines.push_back(std::move(line));
    start = end + 2U;
  }
  (void)screen.text(left, 1, "Debugging build", kTitle, width);
  const int keys_row = std::min(3 + static_cast<int>(lines.size()) + 1, screen.height() - 1);
  for (std::size_t i = 0; i < lines.size() && 3 + static_cast<int>(i) < keys_row; ++i) {
    (void)screen.text(left, 3 + static_cast<int>(i), lines[i], kNormal, width);
  }
  (void)screen.text(left, keys_row, "y: start the server    any other key: quit", kSelected, width);
}

}  // namespace secure_frontend::tui
