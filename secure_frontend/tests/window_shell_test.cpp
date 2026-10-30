// strata-window's portable half (spec docs/superpowers/specs/2026-10-28-server-window-design.md, Part 2; the owner:
// "instead of a console application, we should make it into a window application ... minimized into the tray that
// has the start and stop models menu"): what the window reads from the server's console and keeps of it, where its
// web view may go, the tray's model list and requests, the server it starts and its key, and the server child.
#include "secure_frontend/first_start.hpp"
#include "secure_frontend/frontend_config.hpp"
#include "secure_frontend/secret_source.hpp"
#include "secure_frontend/token_reader.hpp"
#include "secure_frontend/window_shell.hpp"
#include "support/test_support.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <exception>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#else
#include <sys/stat.h>
#endif

namespace {

namespace w = secure_frontend::window;

int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

// LoginLinks' code: 32 random bytes, base64url (43 characters; '-' and '_' among them)
constexpr std::string_view kCode = "Zq3_x-9Lk2Pw7_Rt0Vb4Nm8Hs1Jc6Yd5Ue2Ia9Of3Gk";
const std::string kSignIn = "Sign in: open http://127.0.0.1:8080/#login=" + std::string{kCode} +
                            " in a browser on this PC (one-time link, valid 10 minutes; restart for a new one)";

// The server prints its one-time link to its console only; the window opens it and never writes it to its log.
void the_sign_in_link_is_read_and_never_logged() {
  expect(w::sign_in_url(kSignIn) == std::optional<std::string>{"http://127.0.0.1:8080/#login=" + std::string{kCode}}, "the link");
  expect(w::sign_in_url("Sign in: open http://[::1]:8080/#login=" + std::string{kCode} + " in a browser on this PC").has_value() &&
             w::sign_in_url("Sign in: open http://localhost:9000/#login=" + std::string{kCode} + " in a browser on this PC").has_value(),
         "IPv6 loopback and localhost");
  // the page's rule for a code (app.js): 16 to 128 of A-Z a-z 0-9 _ -
  expect(!w::sign_in_url("Sign in: open http://127.0.0.1:8080/#login=AB12-CD34 in a browser on this PC").has_value() &&
             !w::sign_in_url("Sign in: open http://127.0.0.1:8080/#login=" + std::string(129, 'a') + " in a browser").has_value(),
         "a code shorter or longer than the page takes");
  for (const char* other : {"Sign in: open http://192.0.2.10:8080/#login=ABCD-EFGH in a browser on this PC",
                            "Sign in: open https://127.0.0.1:8080/#login=ABCD-EFGH in a browser on this PC",
                            "Sign in: open http://127.0.0.1:8080/ in a browser on this PC",
                            "Sign in: open http://127.0.0.1:8080/x#login=ABCD-EFGH in a browser on this PC",
                            "Sign in: open http://127.0.0.1:8080/#login=AB\"CD in a browser on this PC",
                            "Sign in: open javascript:alert(1)#login=ABCD in a browser on this PC",
                            "[strata] Sign in: open http://127.0.0.1:8080/#login=ABCD-EFGH in a browser on this PC"}) {
    expect(!w::sign_in_url(other).has_value(), std::string{"not the server's link: "} + other);
  }
  expect(!w::loggable(kSignIn), "the sign-in line is not logged");
  expect(!w::loggable("anything with http://127.0.0.1:8080/#login=" + std::string{kCode} + " in it"), "nor any line holding a login code");
  expect(w::loggable("[strata] models: no model started (models.start is unset); waiting for the page"), "other lines are");
}

// The window opens the page on the Monitor the first time (the owner: "the open window shows the Monitor
// statistics"), through the one-time link or, with none, the page's own sign-in; later loads (a restart) leave the
// view to the page.
void the_window_opens_on_the_monitor_first() {
  const std::string link = "http://127.0.0.1:8080/#login=" + std::string{kCode};
  expect(w::page_address(link, "http://127.0.0.1:8080", true) == link + "&view=monitor", "the link, on the Monitor");
  expect(w::page_address(link, "http://127.0.0.1:8080", false) == link, "a restart's link as it is");
  expect(w::page_address(std::nullopt, "http://127.0.0.1:8080", true) == "http://127.0.0.1:8080/#view=monitor",
         "no link: the page's sign-in, on the Monitor");
  expect(w::page_address(std::nullopt, "http://127.0.0.1:8080", false) == "http://127.0.0.1:8080/", "no link later");
  expect(w::navigation_for(link + "&view=monitor", "http://127.0.0.1:8080") == w::Navigation::stay, "it stays in the window");
}

// The page's origin from the listening line; a wildcard bind is opened on this PC's own address.
void the_listening_line_names_the_pages_origin() {
  const auto origin = [](const std::string& authority) {
    return w::listening_origin("Secure server listening on http://" + authority + " (authenticated; Ctrl-C to stop)");
  };
  expect(origin("127.0.0.1:8080") == std::optional<std::string>{"http://127.0.0.1:8080"}, "loopback");
  expect(origin("0.0.0.0:8080") == std::optional<std::string>{"http://127.0.0.1:8080"}, "0.0.0.0 is opened on 127.0.0.1");
  expect(origin("[::]:8080") == std::optional<std::string>{"http://[::1]:8080"}, ":: is opened on ::1");
  expect(origin("192.0.2.10:8080") == std::optional<std::string>{"http://192.0.2.10:8080"}, "a named address stays");
  expect(w::listening_origin("Secure server listening on http://127.0.0.1:8080").has_value(), "the line's end");
  expect(!w::listening_origin("strata: listening 127.0.0.1:8080, workers 8").has_value(), "not the listening line");
  expect(!origin("127.0.0.1:8080/x\"y").has_value(), "an authority only");
}

// The web view stays on the page's origin; other web addresses go to the default browser; anything else is refused.
void navigation_stays_on_the_pages_origin() {
  const std::string origin = "http://127.0.0.1:8080";
  for (const char* stay : {"http://127.0.0.1:8080/", "http://127.0.0.1:8080/#login=ABCD-EFGH", "http://127.0.0.1:8080/ui/app.js",
                           "HTTP://127.0.0.1:8080/", "http://127.0.0.1:8080", "http://127.0.0.1:8080?x=1"}) {
    expect(w::navigation_for(stay, origin) == w::Navigation::stay, std::string{"stays: "} + stay);
  }
  for (const char* browser : {"http://127.0.0.1:8081/", "http://localhost:8080/", "https://example.org/docs",
                              "https://127.0.0.1:8080/"}) {
    expect(w::navigation_for(browser, origin) == w::Navigation::browser, std::string{"the default browser: "} + browser);
  }
  for (const char* refuse : {"file:///C:/Windows/win.ini", "javascript:alert(1)", "about:blank", "ms-settings:privacy",
                             "http://127.0.0.1:8080@example.com/", "data:text/html,<p>x</p>", ""}) {
    expect(w::navigation_for(refuse, origin) == w::Navigation::refuse, std::string{"refused: "} + refuse);
  }
}

// The tray lists the launcher's cards with their states; a start names the card only (its remembered options).
void the_tray_lists_the_launchers_cards() {
  const auto models = w::tray_models(
      R"({"object":"list","running":"Qwen3.8","state":"running","error":null,"data":[)"
      R"({"name":"Qwen3.8","profile":"qwen","state":"running","error":null,"page_card":true,"options":[],"calibration":null,"reasoning_effort":true},)"
      R"({"name":"GLM 5.3","profile":"glm53","state":"stopped","error":null,"page_card":false,"options":[{"key":"x"}],"calibration":null,"reasoning_effort":false}]})");
  expect(models.has_value() && models->models.size() == 2U && models->models[0].name == "Qwen3.8" &&
             models->models[0].state == "running" && models->models[1].name == "GLM 5.3" && models->models[1].state == "stopped" &&
             models->running == std::optional<std::string>{"Qwen3.8"} && models->state == "running",
         "the cards, in order, with their states");
  const auto none = w::tray_models(R"({"object":"list","running":null,"state":"stopped","error":null,"data":[]})");
  expect(none.has_value() && none->models.empty() && !none->running.has_value() && none->state == "stopped", "no card yet");
  expect(!w::tray_models(R"({"object":"list","data":[{"id":"Qwen3.8","object":"model"}]})").has_value(), "/v1/models is not it");
  expect(!w::tray_models("not json").has_value(), "not JSON");
  expect(w::start_body("Qwen \"3.8\"") == R"({"name":"Qwen \"3.8\""})", "the start names the card");
}

// The tray's requests carry the key in the Authorization header only; the answers are read by their length.
void requests_carry_the_key_only_in_its_header() {
  const std::string key = "strata-secret-key-dev-placeholder";
  expect(w::request_text("POST", "/v1/strata/models/start", "127.0.0.1:8080", &key, R"({"name":"a"})") ==
             "POST /v1/strata/models/start HTTP/1.1\r\nHost: 127.0.0.1:8080\r\nAuthorization: Bearer " + key +
                 "\r\nContent-Type: application/json\r\nContent-Length: 12\r\nConnection: close\r\n\r\n{\"name\":\"a\"}",
         "a start");
  expect(w::request_text("GET", "/v1/strata/models", "127.0.0.1:8080", nullptr, "") ==
             "GET /v1/strata/models HTTP/1.1\r\nHost: 127.0.0.1:8080\r\nConnection: close\r\n\r\n",
         "the list, keyless");
  expect(w::request_text("POST", "/v1/strata/models/stop", "[::1]:8080", &key, "").find("\r\nContent-Length: 0\r\n") != std::string::npos,
         "a stop with no body");
  const auto answer = w::parse_answer("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}");
  expect(answer.has_value() && answer->status == 200 && answer->body == "{}", "an answer");
  expect(!w::parse_answer("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n{}").has_value(), "a short body");
  expect(!w::parse_answer("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\n{}\r\n0\r\n\r\n").has_value(), "a chunked one");
}

// The server's console in <data>/logs/server.log: two bounded files, the older one replaced.
void the_log_rotates_at_its_bound() {
  const auto root = secure_frontend_test::make_scratch_directory();
  const auto file = root / "server.log";
  {
    w::RotatingLog log(file, 64);
    for (int i = 1; i <= 7; ++i) log.write("line-" + std::to_string(i) + " 0123456789ab");  // 20 bytes with the newline
  }
  expect(secure_frontend_test::read_file(file.string() + ".1") == "line-4 0123456789ab\nline-5 0123456789ab\nline-6 0123456789ab\n",
         "the older file: the lines before the last rotation");
  expect(secure_frontend_test::read_file(file) == "line-7 0123456789ab\n", "the current file");
  expect(!std::filesystem::exists(file.string() + ".2"), "two files at most");
#if !defined(_WIN32)
  struct stat info {};
  expect(::stat(file.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600, "owner-only");
#endif
  // a data folder whose name the system's ANSI code page cannot spell (path::string() throws for it on Windows)
  const auto named = root / std::filesystem::path{u8"logs-日本"};
  std::filesystem::create_directories(named);
  auto rotated = named / "server.log";
  rotated += ".1";
  try {
    w::RotatingLog log(named / "server.log", 64);
    for (int i = 1; i <= 4; ++i) log.write("line-" + std::to_string(i) + " 0123456789ab");
    expect(std::filesystem::exists(rotated), "it rotates under any folder name");
  } catch (const std::exception&) {
    expect(false, "it rotates under any folder name (no exception)");
  }
}

// The window's Log (the owner: "in the window app version we need to add a log capability, the console have it but
// this one do not so we can troubleshoot"): the server's newest lines in memory, bounded by lines and by bytes; a full
// log drops its oldest tenth at once, so a view shows every line again only now and then.
void the_log_view_keeps_the_newest_lines_in_its_bound() {
  w::LogLines log(20, 1024U * 1024U);
  for (int i = 1; i <= 20; ++i) log.add("line-" + std::to_string(i));
  expect(log.lines().size() == 20U && log.lines().front() == "line-1" && log.lines().back() == "line-20" && log.first() == 0U &&
             log.end() == 20U,
         "twenty lines, the newest last");
  log.add("line-21");
  expect(log.lines().size() == 18U && log.lines().front() == "line-4" && log.lines().back() == "line-21" && log.first() == 3U &&
             log.end() == 21U,
         "one line over: the oldest tenth dropped at once");
  expect(log.last(2) == std::vector<std::string>{"line-20", "line-21"} && log.last(100).size() == 18U &&
             log.last(100).front() == "line-4",
         "the last lines, the newest last (the page shown when the server ends)");
  w::LogLines bytes(1000, 100);
  for (int i = 0; i < 10; ++i) bytes.add("0123456789");
  expect(bytes.lines().size() == 10U && bytes.bytes() == 100U, "a hundred bytes kept");
  bytes.add("abcdefghij");
  expect(bytes.lines().size() == 9U && bytes.bytes() == 90U && bytes.lines().back() == "abcdefghij", "over its bytes: the oldest tenth dropped");
  w::LogLines cut(10, 100);
  cut.add(std::string(250, 'x'));
  expect(cut.lines().size() == 1U && cut.lines().back() == std::string(100, 'x'), "a line longer than the bound is cut to it");
  cut.add("y");
  expect(cut.lines().size() == 1U && cut.lines().back() == "y" && cut.bytes() == 1U, "the newest line always stays");
  w::LogLines character(10, 5);
  character.add("abcd\xC3\xA9");
  expect(character.lines().size() == 1U && character.lines().back() == "abcd", "never cut inside a character");
}

// A view (a Windows edit control, a GTK text view) shows valid UTF-8 without control characters, and never the
// sign-in link; it adds only the lines it lacks, or shows them all again once lines it shows were dropped.
void the_log_view_holds_text_a_view_can_show() {
  w::LogLines log(100, 64U * 1024U);
  constexpr char kControls[] = "a\0b\x1b[1mc\rd\x7f" "e\tf";
  log.add(std::string_view{kControls, sizeof kControls - 1U});
  log.add("bad \xFF" " byte");
  log.add("cut \xE2\x82");
  log.add("caf\xC3\xA9 \xE2\x82\xAC");
  log.add(kSignIn);
  log.add("anything with http://127.0.0.1:8080/#login=" + std::string{kCode} + " in it");
  expect(log.lines() == std::deque<std::string>{"a b [1mc d e\tf", "bad \xEF\xBF\xBD byte", "cut \xEF\xBF\xBD", "caf\xC3\xA9 \xE2\x82\xAC"},
         "control characters spaces (a tab stays), a broken sequence U+FFFD, no sign-in link");
  w::LogLines two(100, 1024);
  expect(two.text().empty(), "an empty log's text");
  two.add("one");
  two.add("two");
  expect(two.text() == "one\ntwo\n" && two.text("\r\n") == "one\r\ntwo\r\n", "Copy's text: every line ends");
  w::LogLines view(20, 1024);
  expect(view.newer_than(0, 0) == std::optional<std::size_t>{0U}, "an empty log, an empty view");
  for (int i = 1; i <= 5; ++i) view.add("line-" + std::to_string(i));
  expect(view.newer_than(0, 3) == std::optional<std::size_t>{3U}, "a view of three lines adds the two after them");
  expect(view.newer_than(0, 5) == std::optional<std::size_t>{5U}, "a view of every line adds none");
  for (int i = 6; i <= 21; ++i) view.add("line-" + std::to_string(i));
  expect(!view.newer_than(0, 5).has_value(), "lines it shows were dropped: every line again");
  expect(view.newer_than(view.first(), view.end()) == std::optional<std::size_t>{view.lines().size()}, "up to date");
  expect(!view.newer_than(view.first(), view.end() + 1U).has_value(), "a view of lines never kept: every line again");
}

// Save writes what Copy copies, owner-only as server.log; a failure is a message without the path.
void the_log_view_saves_what_it_shows() {
  const auto root = secure_frontend_test::make_scratch_directory();
  w::LogLines log(100, 64U * 1024U);
  log.add("[strata] models: Qwen3.8: a calibration reply failed (timed out)");
  log.add("engine generation 2 ended: exit");
  const auto file = root / "saved.txt";
  expect(!log.save(file).has_value() && secure_frontend_test::read_file(file) == log.text() && !log.text().empty(), "the lines, as Copy has them");
#if !defined(_WIN32)
  struct stat info {};
  expect(::stat(file.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600, "owner-only");
#endif
  const auto failed = log.save(root / "no-such-folder" / "saved.txt");
  expect(failed.has_value() && failed->find("no-such-folder") == std::string::npos, "a message without the path");
  std::tm when{};
  when.tm_year = 2026 - 1900;
  when.tm_mon = 9;
  when.tm_mday = 29;
  when.tm_hour = 14;
  when.tm_min = 3;
  when.tm_sec = 12;
  expect(w::saved_log_name(when) == "m-strata-log-2026-10-29-140312.txt", "Save's name: the time it was saved");
}

// The window starts the server beside it with its own arguments; the data folder is the one that start uses.
void the_window_starts_the_server_beside_it() {
  const std::filesystem::path install = secure_frontend_test::make_scratch_directory();
#if defined(_WIN32)
  const auto server = install / "strata-unified-server.exe";
#else
  const auto server = install / "strata-unified-server";
#endif
  const auto data = install / "elsewhere";
  const auto setup = w::server_setup({"--data-dir", data.string(), "--port", "9000"}, install);
  expect(std::holds_alternative<w::ServerSetup>(setup), "a data folder start");
  if (const auto* found = std::get_if<w::ServerSetup>(&setup)) {
    expect(found->server == server, "the server beside the window");
    expect(found->arguments == std::vector<std::string>{"--data-dir", data.string(), "--port", "9000"}, "its arguments unchanged");
    expect(found->data.has_value() && found->data->root == data && found->data->logs == data / "logs", "that data folder");
  }
  const auto plain = w::server_setup({}, install);
  expect(std::holds_alternative<w::ServerSetup>(plain) && std::get<w::ServerSetup>(plain).data.has_value() &&
             std::get<w::ServerSetup>(plain).data->root == install / "data",
         "no arguments: <install>/data");
  const auto config = w::server_setup({"--config", (install / "c.json").string()}, install);
  expect(std::holds_alternative<w::ServerSetup>(config) && !std::get<w::ServerSetup>(config).data.has_value(), "--config: no data folder");
  for (const std::vector<std::string>& refused : std::vector<std::vector<std::string>>{{"--bogus"}, {"--help"}, {"--check"}, {"init"}}) {
    expect(std::holds_alternative<std::string>(w::server_setup(refused, install)), "refused: " + refused[0]);
  }
}

bool variable_set(const char* name) {
#if defined(_WIN32)
  char value[8];
  return GetEnvironmentVariableA(name, value, sizeof value) > 0;
#else
  const char* value = std::getenv(name);
  return value != nullptr && *value != '\0';
#endif
}

void set_variable(const char* name, const char* value) {
#if defined(_WIN32)
  (void)_putenv_s(name, value);
#else
  (void)setenv(name, value, 1);
#endif
}

// An install as the server leaves it after its first start: the engine beside it and a data folder (the window reads
// the key of a server that started, which needed both).
std::filesystem::path started_install() {
  const auto install = secure_frontend_test::make_scratch_directory();
  std::filesystem::create_directories(secure_frontend::bundled_engine(install).parent_path());
  secure_frontend_test::write_file(secure_frontend::bundled_engine(install), "", 0700);
  expect(std::holds_alternative<secure_frontend::FirstStartResult>(secure_frontend::ensure_data_folder(
             secure_frontend::data_folder_layout(install / "data"), secure_frontend::bundled_engine(install))),
         "a first start's data folder");
  return install;
}

// The key the running server checks, read as `strata key show` reads it: the data folder's key file, or the
// --token-env variable of a development start.
void the_servers_key_is_read_as_key_show_reads_it() {
  const auto install = started_install();
  const auto folder = secure_frontend::data_folder_layout(install / "data");
  const auto setup = std::get<w::ServerSetup>(w::server_setup({}, install));
  const auto key = w::server_key(setup);
  // the store's own read (DPAPI on Windows: the file's bytes are not the key)
  const auto source = secure_frontend::make_protected_file_secret_source("token");
  std::string stored;
  if (const auto* found = std::get_if<secure_frontend::SecretSource>(&source)) {
    const auto read = secure_frontend::read_token(*found, folder.secret);
    if (const auto* bytes = std::get_if<secure_frontend::SecretBytes>(&read)) stored = std::string{bytes->view()};
  }
  expect(!stored.empty() && key.has_value() && !key->keyless && key->key == std::optional<std::string>{stored}, "the key file's key");
  set_variable("STRATA_WINDOW_TEST_KEY", "strata-secret-key-dev-placeholder");
  const auto dev = std::get<w::ServerSetup>(w::server_setup({"--token-env", "STRATA_WINDOW_TEST_KEY"}, install));
  const auto dev_key = w::server_key(dev);
  expect(dev_key.has_value() && dev_key->key == std::optional<std::string>{"strata-secret-key-dev-placeholder"},
         "a --token-env start's variable");
}

// The server child as the window runs it, with the fake server (fake_server_child_main.cpp).
struct Watched final {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<std::string> lines;
  std::optional<int> exit_code;
  bool saw(const std::string_view text) {
    for (const auto& line : lines) if (line.find(text) != std::string::npos) return true;
    return false;
  }
};
std::optional<std::string> start_fake(w::ServerChild& child, Watched& watched, std::vector<std::string> arguments) {
  const auto& extra = secure_frontend_test::extra_arguments();
  if (extra.empty()) return std::string{"no fake server given"};
  return child.start(extra[0], arguments, w::current_environment(),
                     [&watched](std::string line) {
                       std::lock_guard lock(watched.mutex);
                       watched.lines.push_back(std::move(line));
                       watched.changed.notify_all();
                     },
                     [&watched](const int code) {
                       std::lock_guard lock(watched.mutex);
                       watched.exit_code = code;
                       watched.changed.notify_all();
                     });
}

void the_server_child_stops_in_order() {
  Watched watched;
  w::ServerChild child;
  expect(!start_fake(child, watched, {"--show-group"}).has_value(), "started");
  {
    std::unique_lock lock(watched.mutex);
    expect(watched.changed.wait_for(lock, std::chrono::seconds(20), [&] { return watched.saw("#login=" + std::string{kCode}); }),
           "its stdout reaches the window");
    expect(watched.saw("waiting for the page"), "and its stderr");
#if !defined(_WIN32)
    // its own process group: a terminal's hangup, Ctrl-C or Ctrl-Z reaches the window or the TUI, which stops it in
    // order, never the server itself mid-generation
    expect(watched.saw("group own"), "the server leads its own process group");
#endif
  }
  expect(child.running(), "running");
  expect(child.stop(std::chrono::seconds(15)), "it stopped when asked (Ctrl-Break / SIGTERM), not killed");
  std::unique_lock lock(watched.mutex);
  expect(watched.changed.wait_for(lock, std::chrono::seconds(5), [&] { return watched.exit_code.has_value(); }) &&
             watched.exit_code == 0 && watched.saw("fake: stopping"),
         "its own orderly exit, reported");
  expect(!child.running(), "not running after");
}

void a_server_that_ignores_the_stop_is_killed() {
  Watched watched;
  w::ServerChild child;
  expect(!start_fake(child, watched, {"--ignore-stop"}).has_value(), "started");
  {
    std::unique_lock lock(watched.mutex);
    (void)watched.changed.wait_for(lock, std::chrono::seconds(20), [&] { return watched.saw("#login="); });
  }
  const auto began = std::chrono::steady_clock::now();
  expect(!child.stop(std::chrono::milliseconds(1500)), "killed after the grace period");
  expect(std::chrono::steady_clock::now() - began < std::chrono::seconds(10), "promptly");
  std::unique_lock lock(watched.mutex);
  expect(watched.changed.wait_for(lock, std::chrono::seconds(5), [&] { return watched.exit_code.has_value(); }) &&
             watched.exit_code != 0 && !watched.saw("fake: stopping"),
         "its end reported");
  expect(!child.running(), "not running after");
}

void a_server_that_exits_is_reported() {
  Watched watched;
  w::ServerChild child;
  expect(!start_fake(child, watched, {"--exit", "3"}).has_value(), "started");
  std::unique_lock lock(watched.mutex);
  expect(watched.changed.wait_for(lock, std::chrono::seconds(20), [&] { return watched.exit_code.has_value(); }) && watched.exit_code == 3,
         "its exit code");
  expect(watched.saw("Secure server listening"), "its lines before it");
  lock.unlock();
  expect(child.stop(std::chrono::seconds(1)), "a stop after it ended is a no-op");
}

void a_missing_server_is_a_message() {
  w::ServerChild child;
  const auto missing = secure_frontend_test::make_scratch_directory() / "strata-unified-server-missing";
  const auto failed = child.start(missing, {}, w::current_environment(), [](std::string) {}, [](int) {});
  expect(failed.has_value() && failed->find("missing") == std::string::npos, "a message without the path");
  expect(!child.running(), "nothing runs");
}

// Reading a --token-env key removes the variable from the reader (token_reader.cpp scrubs it), so every start of the
// server - a restart from the tray too - gets the environment the window started with, not the window's current one.
void every_start_gets_the_windows_first_environment() {
  const auto install = started_install();
  set_variable("STRATA_WINDOW_RESTART_KEY", "strata-secret-key-dev-placeholder");
  const auto setup = std::get<w::ServerSetup>(w::server_setup({"--token-env", "STRATA_WINDOW_RESTART_KEY"}, install));
  const auto key = w::server_key(setup);
  expect(key.has_value() && key->key.has_value(), "the key read");
  expect(!variable_set("STRATA_WINDOW_RESTART_KEY"), "reading it removed it from this process (the scrub)");
  const auto& extra = secure_frontend_test::extra_arguments();
  if (extra.empty()) {
    expect(false, "no fake server given");
    return;
  }
  Watched watched;
  w::ServerChild child;
  expect(!child.start(extra[0], {"--show-env", "STRATA_WINDOW_RESTART_KEY"}, setup.environment,
                      [&watched](std::string line) {
                        std::lock_guard lock(watched.mutex);
                        watched.lines.push_back(std::move(line));
                        watched.changed.notify_all();
                      },
                      [&watched](const int code) {
                        std::lock_guard lock(watched.mutex);
                        watched.exit_code = code;
                        watched.changed.notify_all();
                      }).has_value(),
         "started");
  {
    std::unique_lock lock(watched.mutex);
    expect(watched.changed.wait_for(lock, std::chrono::seconds(20), [&] { return watched.saw("env STRATA_WINDOW_RESTART_KEY="); }) &&
               watched.saw("env STRATA_WINDOW_RESTART_KEY=set"),
           "the restarted server still gets the --token-env variable");
  }
  expect(child.stop(std::chrono::seconds(15)), "stopped");
}

}  // namespace

// The owner (2026-10-29): a build made with --debugging opens with a pop-up (the window) or a screen to accept with y
// (the TUI) before it starts the server, saying what that build logs and what to take care of; any other build starts
// as before. One text for both.
void a_debugging_build_says_what_it_logs_before_it_starts() {
  const std::string_view notice = w::kDebuggingNotice;
  for (const char* words : {"debugging build", "one line per request", "the engine's own output", "your prompts",
                            "the paths of your model files", "keep the server's log private", "before you share it",
                            "without --debugging", "Settings > Troubleshooting"}) {
    expect(notice.find(words) != std::string_view::npos, std::string{"the notice says: "} + words);
  }
  expect(notice.find('\t') == std::string_view::npos && notice.find('\r') == std::string_view::npos,
         "plain text, paragraphs split by blank lines");
  expect(w::asks_before_starting() == secure_frontend::debugging_build(), "only a debugging build asks");
}

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"the_sign_in_link_is_read_and_never_logged", the_sign_in_link_is_read_and_never_logged},
      {"a_debugging_build_says_what_it_logs_before_it_starts", a_debugging_build_says_what_it_logs_before_it_starts},
      {"the_listening_line_names_the_pages_origin", the_listening_line_names_the_pages_origin},
      {"the_window_opens_on_the_monitor_first", the_window_opens_on_the_monitor_first},
      {"navigation_stays_on_the_pages_origin", navigation_stays_on_the_pages_origin},
      {"the_tray_lists_the_launchers_cards", the_tray_lists_the_launchers_cards},
      {"requests_carry_the_key_only_in_its_header", requests_carry_the_key_only_in_its_header},
      {"the_log_rotates_at_its_bound", the_log_rotates_at_its_bound},
      {"the_log_view_keeps_the_newest_lines_in_its_bound", the_log_view_keeps_the_newest_lines_in_its_bound},
      {"the_log_view_holds_text_a_view_can_show", the_log_view_holds_text_a_view_can_show},
      {"the_log_view_saves_what_it_shows", the_log_view_saves_what_it_shows},
      {"the_window_starts_the_server_beside_it", the_window_starts_the_server_beside_it},
      {"the_servers_key_is_read_as_key_show_reads_it", the_servers_key_is_read_as_key_show_reads_it},
      {"the_server_child_stops_in_order", the_server_child_stops_in_order},
      {"a_server_that_ignores_the_stop_is_killed", a_server_that_ignores_the_stop_is_killed},
      {"a_server_that_exits_is_reported", a_server_that_exits_is_reported},
      {"a_missing_server_is_a_message", a_missing_server_is_a_message},
      {"every_start_gets_the_windows_first_environment", every_start_gets_the_windows_first_environment},
  }, failures);
}
