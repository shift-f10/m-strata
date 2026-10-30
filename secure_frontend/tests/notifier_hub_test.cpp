// Notifier plugins (the owner, 2026-10-30, decisions 6 and 7; plugin spec section 10): the notifier host tells each
// notifier plugin the events it wants as one JSON line each, answers the tab's test with the plugin's own line (or says
// why there was none), logs a plugin's answers to events, drops an event while a program is down and starts it again
// after the pause - over real child processes (the fake notifier program).
#include "secure_frontend/crt_environment.hpp"
#include "secure_frontend/notifier_hub.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

using secure_frontend::NotifierHub;
using secure_frontend::NotifierPlugin;

std::map<std::string, std::string, std::less<>> base_environment() {
  std::map<std::string, std::string, std::less<>> environment;
#if defined(_WIN32)
  for (const char* name : {"SystemRoot", "PATH"}) {
#else
  for (const char* name : {"PATH"}) {
#endif
    if (const char* value = secure_frontend::crt_getenv(name)) environment[name] = value;
  }
  return environment;
}

NotifierPlugin fake(const std::string& id, const std::filesystem::path& record, std::vector<std::string> more = {}) {
  NotifierPlugin plugin{.id = id, .program = FAKE_NOTIFIER_PATH, .arguments = {"--record", record.string()}, .environment = base_environment()};
  plugin.arguments.insert(plugin.arguments.end(), more.begin(), more.end());
  return plugin;
}

std::vector<std::string> lines_of(const std::filesystem::path& file) {
  std::vector<std::string> lines;
  std::istringstream in(secure_frontend_test::read_file(file));
  for (std::string line; std::getline(in, line);) lines.push_back(line);
  return lines;
}

// Waits up to five seconds for `file` to hold `count` lines.
std::vector<std::string> wait_for_lines(const std::filesystem::path& file, const std::size_t count) {
  for (int i = 0; i < 50; ++i) {
    std::error_code error;
    if (std::filesystem::exists(file, error) && lines_of(file).size() >= count) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  std::error_code error;
  return std::filesystem::exists(file, error) ? lines_of(file) : std::vector<std::string>{};
}

struct Log final {
  std::mutex mutex;
  std::vector<std::string> lines;
  std::function<void(std::string_view)> sink() {
    return [this](const std::string_view line) {
      const std::lock_guard lock(mutex);
      lines.emplace_back(line);
    };
  }
  bool contains(const std::string& part) {
    const std::lock_guard lock(mutex);
    return std::any_of(lines.begin(), lines.end(), [&part](const std::string& line) { return has(line, part); });
  }
};

// Each plugin is told the events it wants - all, or those its `events` setting names - as one JSON line each, in order,
// with the subject, body, the server's name and the time.
void events_reach_each_notifier_that_wants_them() {
  const auto folder = secure_frontend_test::make_scratch_directory();
  auto chosen = fake("chosen", folder / "chosen.txt");
  chosen.events = std::vector<std::string>{"server_started"};
  NotifierHub hub({.plugins = {fake("all", folder / "all.txt"), chosen}, .server_name = "testbox",
                   .now = [] { return std::string{"2026-10-30T12:00:00Z"}; }});
  hub.raise("server_started", "the server started", "It listens on 127.0.0.1:8080.");
  hub.raise("download_finished", "flint-Q8_0 is downloaded", "Its card is in Models.");
  expect(hub.wait_idle(std::chrono::seconds(10)), "the queues empty");
  const auto all = wait_for_lines(folder / "all.txt", 2U);
  expect(all.size() == 2U &&
             all[0] == R"({"event":"server_started","subject":"the server started","body":"It listens on 127.0.0.1:8080.","server":"testbox","time":"2026-10-30T12:00:00Z"})" &&
             has(all[1], R"("event":"download_finished")"),
         "a plugin without an events setting is told of both, in order: " + (all.empty() ? std::string{} : all[0]));
  const auto some = wait_for_lines(folder / "chosen.txt", 1U);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  expect(lines_of(folder / "chosen.txt").size() == 1U && !some.empty() && has(some[0], R"("event":"server_started")"),
         "one whose events setting names server_started only is told of it alone");
}

// The tab's test gets the plugin's own answer (here naming its arguments, its working folder and its environment); one
// that never answers is said so after the time limit; an id the hub does not run is refused.
void the_test_is_answered_or_said_why_not() {
  const auto folder = secure_frontend_test::make_scratch_directory();
  const auto home = secure_frontend_test::make_scratch_directory();
  auto talker = fake("talker", folder / "talker.txt", {"--flag", "x"});
  talker.working_directory = home;
  talker.environment["FAKE_NOTIFIER_TOKEN"] = "t0ken";
  NotifierHub hub({.plugins = {talker, fake("silent", folder / "silent.txt", {"--no-test-answer"})}, .server_name = "testbox",
                   .test_timeout = std::chrono::seconds(2)});
  const auto answered = hub.test("talker");
  expect(answered.ok && has(answered.reply, "--flag x") && has(answered.reply, "token: t0ken"), "the test is answered: " + answered.reply);
  const auto cwd_at = answered.reply.find("cwd: ");
  std::error_code same;
  if (cwd_at != std::string::npos) {
    const auto end = answered.reply.find(';', cwd_at);
    expect(std::filesystem::equivalent(answered.reply.substr(cwd_at + 5U, end - cwd_at - 5U), home, same), "it runs in its own folder");
  }
  const auto lines = wait_for_lines(folder / "talker.txt", 1U);
  expect(!lines.empty() && lines[0] == R"({"test":true,"server":"testbox"})", "the test line: " + (lines.empty() ? std::string{} : lines[0]));
  const auto started = std::chrono::steady_clock::now();
  const auto silent = hub.test("silent");
  expect(!silent.ok && has(silent.reply, "no answer") && std::chrono::steady_clock::now() - started < std::chrono::seconds(10),
         "a silent plugin: no answer, said after the limit: " + silent.reply);
  const auto ghost = hub.test("ghost");
  expect(!ghost.ok && has(ghost.reply, "ghost"), "an id it does not run: " + ghost.reply);
}

// A plugin's answer to an event is logged; one that does not answer events is not waited for.
void event_answers_are_logged() {
  const auto folder = secure_frontend_test::make_scratch_directory();
  Log log;
  NotifierHub hub({.plugins = {fake("mailer", folder / "mailer.txt", {"--answer-events"}), fake("quiet", folder / "quiet.txt")},
                   .server_name = "testbox", .log = log.sink()});
  hub.raise("engine_stopped", "the engine stopped", "Qwen stopped unexpectedly.");
  bool logged = false;
  for (int i = 0; i < 50 && !logged; ++i) {
    logged = log.contains("[strata] plugins: mailer: engine_stopped: noted");
    if (!logged) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  expect(logged, "the answer is in the log");
  expect(wait_for_lines(folder / "quiet.txt", 1U).size() == 1U, "the quiet one was told too");
  expect(log.contains("[strata] plugins: mailer is running") && log.contains("[strata] plugins: quiet is running"), "each start is logged");
}

// A program that ends is started again after the pause; an event raised meanwhile is dropped, and the log says so.
void a_program_that_ends_is_started_again() {
  const auto folder = secure_frontend_test::make_scratch_directory();
  Log log;
  NotifierHub hub({.plugins = {fake("brief", folder / "brief.txt", {"--exit-after", "1"})}, .server_name = "testbox", .log = log.sink(),
                   .restart_pause = std::chrono::seconds(2)});
  hub.raise("server_started", "one", "1");
  expect(wait_for_lines(folder / "brief.txt", 1U).size() == 1U, "the first event reached it");
  bool ended = false;
  for (int i = 0; i < 50 && !ended; ++i) {
    ended = log.contains("[strata] plugins: brief's program ended");
    if (!ended) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  expect(ended, "its end is logged");
  hub.raise("download_finished", "two", "2");
  expect(log.contains("[strata] plugins: brief is not running; download_finished was not passed on"), "an event meanwhile is dropped, said");
  std::this_thread::sleep_for(std::chrono::milliseconds(2600));
  hub.raise("calibration_finished", "three", "3");
  const auto lines = wait_for_lines(folder / "brief.txt", 2U);
  expect(lines.size() == 2U && has(lines[1], "calibration_finished"), "after the pause it runs again and is told");
}

// Stopping the server does not wait for a test still waiting on a silent plugin: the test is told why.
void stopping_does_not_wait_for_a_test() {
  const auto folder = secure_frontend_test::make_scratch_directory();
  NotifierHub hub({.plugins = {fake("silent", folder / "silent.txt", {"--no-test-answer"})}, .server_name = "testbox",
                   .test_timeout = std::chrono::seconds(60)});
  secure_frontend::NotifierTest result;
  std::thread waiting([&hub, &result] { result = hub.test("silent"); });
  (void)wait_for_lines(folder / "silent.txt", 1U);
  const auto started = std::chrono::steady_clock::now();
  hub.shutdown();
  waiting.join();
  expect(std::chrono::steady_clock::now() - started < std::chrono::seconds(5), "the stop is not held up by the test");
  expect(!result.ok && has(result.reply, "stopping"), "the test says why: " + result.reply);
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"events_reach_each_notifier_that_wants_them", events_reach_each_notifier_that_wants_them},
      {"the_test_is_answered_or_said_why_not", the_test_is_answered_or_said_why_not},
      {"event_answers_are_logged", event_answers_are_logged},
      {"a_program_that_ends_is_started_again", a_program_that_ends_is_started_again},
      {"stopping_does_not_wait_for_a_test", stopping_does_not_wait_for_a_test},
  }, failures);
}
