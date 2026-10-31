#include "secure_frontend/notifier_hub.hpp"

#include "secure_frontend/deadline.hpp"
#include "secure_frontend/engine_stdio_process.hpp"
#include "secure_frontend/json.hpp"

#include <algorithm>
#include <condition_variable>
#include <ctime>
#include <cstdio>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
#include <utility>
#include <variant>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#else
#include <unistd.h>
#endif

namespace secure_frontend {
namespace {

constexpr std::size_t kMaxAnswerLine = 64U * 1024U;
constexpr std::size_t kMaxShownChars = 400U;
constexpr std::chrono::milliseconds kIdlePoll{250};
constexpr std::chrono::seconds kWriteLimit{5};
constexpr JsonLimits kAnswerLimits{.max_bytes = kMaxAnswerLine, .max_depth = 2U, .max_container_entries = 8U, .max_string_bytes = 8192U};

std::string json_text(const std::string_view text) {
  std::string out;
  append_json_string(&out, text);
  return out;
}

// A plugin's words as the log and the tab show them: printable ASCII, at most 400 characters.
std::string shown(const std::string_view text) {
  std::string out{text.substr(0, kMaxShownChars)};
  std::replace_if(out.begin(), out.end(), [](const char c) { return c < ' ' || c >= 0x7f; }, '?');
  return text.size() > kMaxShownChars ? out + " [cut]" : out;
}

const JsonValue* member(const JsonValue& object, const std::string_view key) {
  const auto* members = object.if_object();
  if (members == nullptr) return nullptr;
  for (const auto& entry : *members) {
    if (entry.key == key) return &entry.value;
  }
  return nullptr;
}

struct Answer final {
  std::optional<std::string> event;  // an event's answer names it; the test's does not
  bool ok{false};
  std::string reply;
};
// {"ok": bool, "reply": text}, or {"event", "ok", "reply"}; any other line is no answer.
std::optional<Answer> answer_of(const std::string_view line) {
  const auto parsed = parse_json(line, kAnswerLimits);
  const auto* root = std::get_if<JsonValue>(&parsed);
  if (root == nullptr) return std::nullopt;
  const auto* ok = member(*root, "ok");
  const auto* reply = member(*root, "reply");
  if (ok == nullptr || ok->if_boolean() == nullptr || reply == nullptr || reply->if_string() == nullptr) return std::nullopt;
  Answer answer{.event = std::nullopt, .ok = *ok->if_boolean(), .reply = shown(*reply->if_string())};
  if (const auto* event = member(*root, "event"); event != nullptr && event->if_string() != nullptr) answer.event = shown(*event->if_string());
  return answer;
}

}  // namespace

struct NotifierHub::Running final {
  struct Item final {
    std::string line;
    std::shared_ptr<std::promise<NotifierTest>> test;  // the tab's test (null: an event)
  };
  NotifierPlugin plugin;
  std::unique_ptr<EngineStdioProcess> process;  // the worker's (the constructor's before it starts)
  std::mutex mutex;
  std::condition_variable changed;
  std::deque<Item> queue;
  bool up{false};        // its program runs (an event for it is queued, not dropped)
  bool busy{false};      // an item is being written, or its answer read
  bool stopping{false};
  std::thread worker;
  std::mutex join_mutex;  // stop() and shutdown() each join the worker once
};

namespace {

std::unique_ptr<EngineStdioProcess> start_program(const NotifierPlugin& plugin, const NotifierHubSettings& settings) {
  // in its own folder, with no new privileges (Linux); its stderr to the log only as the tool hub sends it there
  EngineCommand command{.executable = plugin.program,
                        .arguments = plugin.arguments,
                        .environment = plugin.environment,
                        .working_directory = plugin.working_directory,
                        .no_new_privileges = true,
                        .insert_serve = false,
                        .max_read_line_bytes = kMaxAnswerLine};
  if (settings.log_stderr && settings.log) {
    command.stderr_lines = [log = settings.log, prefix = "[plugin " + plugin.id + "] "](const std::string_view line) {
      log(prefix + std::string{line.substr(0, kMaxShownChars)} + (line.size() > kMaxShownChars ? " [cut]" : ""));
    };
  }
  auto process = std::make_unique<EngineStdioProcess>();
  if (process->start(command)) return nullptr;
  return process;
}

}  // namespace

NotifierHub::NotifierHub(NotifierHubSettings settings) : settings_(std::move(settings)) {
  if (!settings_.now) settings_.now = rfc3339_now;
  // every program starts here, before the server raises its first event; each plugin's thread takes it from there
  for (auto& plugin : settings_.plugins) {
    auto running = std::make_unique<Running>();
    running->plugin = plugin;
    running->process = start_program(plugin, settings_);
    running->up = running->process != nullptr;
    say(running->up ? "[strata] plugins: " + plugin.id + " is running"
                    : "[strata] plugins: " + plugin.id + "'s program did not start; it is tried again in a minute");
    plugins_.push_back(std::move(running));
  }
  for (auto& running : plugins_) running->worker = std::thread([this, plugin = running.get()] { run(*plugin); });
}

NotifierHub::~NotifierHub() { shutdown(); }

void NotifierHub::say(const std::string& line) const {
  if (settings_.log) settings_.log(line);
}

void NotifierHub::raise(const std::string_view event, const std::string& subject, const std::string& body) {
  const std::string line = "{\"event\":" + json_text(event) + ",\"subject\":" + json_text(subject) + ",\"body\":" + json_text(body) +
                           ",\"server\":" + json_text(settings_.server_name) + ",\"time\":" + json_text(settings_.now()) + "}";
  for (auto& plugin : plugins_) {
    const auto& wanted = plugin->plugin.events;
    if (wanted && std::find(wanted->begin(), wanted->end(), event) == wanted->end()) continue;
    std::string dropped;
    {
      const std::lock_guard lock(plugin->mutex);
      if (plugin->stopping) continue;
      if (!plugin->up) dropped = " is not running; ";
      else if (plugin->queue.size() >= kMaxQueued) dropped = "'s queue is full; ";
      else plugin->queue.push_back(Running::Item{line, nullptr});
    }
    if (dropped.empty()) plugin->changed.notify_all();
    else say("[strata] plugins: " + plugin->plugin.id + dropped + std::string{event} + " was not passed on");
  }
}

NotifierTest NotifierHub::test(const std::string_view id) {
  const auto found = std::find_if(plugins_.begin(), plugins_.end(), [id](const auto& plugin) { return plugin->plugin.id == id; });
  if (found == plugins_.end()) return {false, "no notifier plugin " + std::string{id} + " runs in this server"};
  Running& plugin = **found;
  auto promise = std::make_shared<std::promise<NotifierTest>>();
  auto answered = promise->get_future();
  {
    const std::lock_guard lock(plugin.mutex);
    if (plugin.stopping || !plugin.up) return {false, std::string{id} + "'s program is not running now"};
    if (plugin.queue.size() >= kMaxQueued) return {false, std::string{id} + "'s queue is full; try again"};
    plugin.queue.push_back(Running::Item{"{\"test\":true,\"server\":" + json_text(settings_.server_name) + "}", promise});
  }
  plugin.changed.notify_all();
  // the worker answers within the limit, once the items before it are written
  if (answered.wait_for(settings_.test_timeout + std::chrono::seconds(kWriteLimit) * 2) != std::future_status::ready) {
    return {false, "no answer: " + std::string{id} + " is still busy"};
  }
  return answered.get();
}

void NotifierHub::run(Running& plugin) {
  const std::string& id = plugin.plugin.id;
  std::optional<std::chrono::steady_clock::time_point> down_since;
  if (plugin.process == nullptr) down_since = std::chrono::steady_clock::now();
  const auto set_up = [&plugin](const bool up) {
    const std::lock_guard lock(plugin.mutex);
    plugin.up = up;
  };
  const auto log_answer = [this, &id](const Answer& answer) {
    say("[strata] plugins: " + id + ": " + *answer.event + (answer.ok ? ": " : " failed: ") + answer.reply);
  };
  for (;;) {
    {
      const std::lock_guard lock(plugin.mutex);
      if (plugin.stopping) break;
    }
    // down: started again after the pause; meanwhile a test waiting is told so
    if (plugin.process == nullptr) {
      if (std::chrono::steady_clock::now() - *down_since >= settings_.restart_pause) {
        plugin.process = start_program(plugin.plugin, settings_);
        if (plugin.process != nullptr) {
          down_since.reset();
          set_up(true);
          say("[strata] plugins: " + id + " is running");
          continue;
        }
        down_since = std::chrono::steady_clock::now();
        say("[strata] plugins: " + id + "'s program did not start; it is tried again in a minute");
      }
      std::unique_lock lock(plugin.mutex);
      plugin.changed.wait_for(lock, kIdlePoll, [&plugin] { return plugin.stopping || !plugin.queue.empty(); });
      while (!plugin.queue.empty()) {
        auto item = std::move(plugin.queue.front());
        plugin.queue.pop_front();
        if (item.test) item.test->set_value({false, id + "'s program is not running now"});
      }
      lock.unlock();
      plugin.changed.notify_all();
      continue;
    }
    std::optional<Running::Item> item;
    {
      const std::lock_guard lock(plugin.mutex);
      if (!plugin.queue.empty()) {
        item = std::move(plugin.queue.front());
        plugin.queue.pop_front();
        plugin.busy = true;
      }
    }
    if (item) {
      std::string error;
      const bool written = plugin.process->write_line(item->line, MonotonicClock::now() + kWriteLimit, error);
      if (item->test) {
        NotifierTest result{false, "no answer within " + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(settings_.test_timeout).count()) +
                                       " seconds"};
        if (!written) result.reply = id + "'s program did not take the test";
        const auto deadline = std::chrono::steady_clock::now() + settings_.test_timeout;
        while (written) {
          const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
          if (left <= std::chrono::milliseconds::zero()) break;
          // in short waits, so a server that is stopping is not held up for the test's whole limit
          const auto read = plugin.process->read_line(std::min(left, kIdlePoll));
          if (read.kind == EngineReadKind::timeout) {
            const std::lock_guard lock(plugin.mutex);
            if (!plugin.stopping) continue;
            result.reply = "the server is stopping";
            break;
          }
          if (read.kind != EngineReadKind::line) {
            result.reply = id + "'s program ended before it answered";
            break;
          }
          const auto answer = answer_of(read.line);
          if (!answer) continue;
          if (answer->event) {
            log_answer(*answer);
            continue;
          }
          result = {answer->ok, answer->reply};
          break;
        }
        item->test->set_value(std::move(result));
      }
      {
        const std::lock_guard lock(plugin.mutex);
        plugin.busy = false;
      }
      plugin.changed.notify_all();
    } else {
      // idle: an answer to an event, or the program's end
      const auto read = plugin.process->read_line(kIdlePoll);
      if (read.kind == EngineReadKind::line) {
        if (const auto answer = answer_of(read.line); answer && answer->event) log_answer(*answer);
      }
    }
    if (!plugin.process->running()) {
      set_up(false);
      plugin.process.reset();
      down_since = std::chrono::steady_clock::now();
      say("[strata] plugins: " + id + "'s program ended; it starts again in a minute");
    }
  }
  if (plugin.process != nullptr) {
    plugin.process->close_stdin();
    (void)plugin.process->terminate(std::chrono::seconds(2));
  }
  const std::lock_guard lock(plugin.mutex);
  plugin.up = false;
  for (auto& item : plugin.queue) {
    if (item.test) item.test->set_value({false, "the server is stopping"});
  }
  plugin.queue.clear();
  plugin.changed.notify_all();
}

bool NotifierHub::wait_idle(const std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (auto& plugin : plugins_) {
    std::unique_lock lock(plugin->mutex);
    if (!plugin->changed.wait_until(lock, deadline, [&plugin] { return plugin->queue.empty() && !plugin->busy; })) return false;
  }
  return true;
}

bool NotifierHub::stop(const std::string_view id) {
  const auto found = std::find_if(plugins_.begin(), plugins_.end(), [id](const auto& plugin) { return plugin->plugin.id == id; });
  if (found == plugins_.end()) return false;
  Running& plugin = **found;
  {
    const std::lock_guard lock(plugin.mutex);
    plugin.stopping = true;
  }
  plugin.changed.notify_all();
  const std::lock_guard joining(plugin.join_mutex);  // its worker ends its program and answers a test that waits
  if (plugin.worker.joinable()) plugin.worker.join();
  return true;
}

void NotifierHub::shutdown() {
  for (auto& plugin : plugins_) {
    {
      const std::lock_guard lock(plugin->mutex);
      plugin->stopping = true;
    }
    plugin->changed.notify_all();
  }
  for (auto& plugin : plugins_) {
    const std::lock_guard joining(plugin->join_mutex);
    if (plugin->worker.joinable()) plugin->worker.join();
  }
}

std::string server_display_name() {
  std::string name;
#if defined(_WIN32)
  wchar_t buffer[MAX_COMPUTERNAME_LENGTH + 1]{};
  DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
  if (GetComputerNameW(buffer, &size) != 0) {
    for (DWORD i = 0; i < size; ++i) name.push_back(buffer[i] > 0x20 && buffer[i] < 0x7f ? static_cast<char>(buffer[i]) : '?');
  }
#else
  char buffer[256]{};
  if (gethostname(buffer, sizeof(buffer) - 1U) == 0) {
    for (const char* c = buffer; *c != '\0'; ++c) name.push_back(*c > 0x20 && *c < 0x7f ? *c : '?');
  }
#endif
  if (name.size() > 64U) name.resize(64U);
  return name.empty() ? std::string{"this PC"} : name;
}

std::string rfc3339_now() {
  const std::time_t now = std::time(nullptr);
  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &now);
#else
  gmtime_r(&now, &utc);
#endif
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ", utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour,
                utc.tm_min, utc.tm_sec);
  return buffer;
}

}  // namespace secure_frontend
