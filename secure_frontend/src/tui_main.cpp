// strata-tui: strata-unified-server in a terminal (spec docs/superpowers/specs/2026-10-28-server-tui-design.md; the owner:
// "a headless version of the frontend ... a TUI with the same settings and options ... monitoring the default view in a
// nice graph dashboard"). The headless twin of strata-window: the server runs as this program's child (the window's
// ServerSetup and ServerChild, its arguments passed through), its console lines go to the Log view and to
// <data>/logs/server.log (never the sign-in link), and the views drive it over loopback with its key, as the page does.
// Every way out - q, Ctrl-Break, SIGTERM/SIGHUP/SIGINT/SIGQUIT, the console closing, the terminal going away, an
// exception - puts the terminal back first, then stops the server in order (its model first).

#include "secure_frontend/config.hpp"
#include "secure_frontend/first_start.hpp"
#include "secure_frontend/tui_app.hpp"
#include "secure_frontend/tui_terminal.hpp"
#include "secure_frontend/window_shell.hpp"

#if defined(_WIN32)
#include "secure_frontend/utf8_argv.hpp"
#include "secure_frontend/win32_prelude.hpp"

#include <tlhelp32.h>
#else
#include <csignal>
#endif

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <deque>
#include <exception>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace t = secure_frontend::tui;
namespace w = secure_frontend::window;

constexpr std::uintmax_t kLogBytes = 4U << 20U;
constexpr auto kPollEvery = std::chrono::seconds(1);
constexpr auto kPollTimeout = std::chrono::seconds(5);     // a poll: the next one comes a second later anyway
constexpr auto kActionTimeout = std::chrono::seconds(15);  // a key's request (a start answers once it is under way)
constexpr auto kStopGrace = std::chrono::seconds(60);
// The console closing, a logoff or a shutdown: Windows ends this process about 5 s after the event, so the server's
// orderly stop gets 4 s before it is ended.
constexpr auto kClosingGrace = std::chrono::seconds(4);

std::atomic<bool> g_quit_signal{false};
std::atomic<bool> g_closing{false};

// What the server's reader thread and the request worker hand to the screen's thread, and back.
struct Shared final {
  std::mutex mutex;
  std::condition_variable wake;
  std::deque<std::string> lines;
  std::optional<std::string> origin;
  bool reachable{true};  // the origin is this PC's loopback, the only address loopback_request talks to
  std::optional<int> exit_code;
  std::deque<std::tuple<t::Request, int, std::string>> answers;
  std::deque<t::Request> actions;
  std::vector<t::Request> polls;
  std::optional<std::string> key;
  bool keyless{false};
  bool stopping{false};
};

#if defined(_WIN32)
HANDLE g_stopped = nullptr;  // signalled once the server is stopped and its last lines logged

BOOL WINAPI on_console_event(const DWORD event) {
  g_quit_signal.store(true);
  if (event == CTRL_CLOSE_EVENT || event == CTRL_LOGOFF_EVENT || event == CTRL_SHUTDOWN_EVENT) {
    // Windows ends the process as soon as this returns: wait (within the few seconds it gives) for the orderly stop
    g_closing.store(true);
    if (g_stopped != nullptr) (void)WaitForSingleObject(g_stopped, 10000);
  }
  return TRUE;  // Ctrl-Break (Ctrl-C is a key in raw mode): an ordinary quit
}

// strata-tui --ctrl-break <pid>: Ctrl-Break to that process's group, from its own console (the server's orderly stop).
// The program runs itself this way, with no console, while its own console closes: until its close handler returns,
// every console call of the closing console's processes waits (ServerChild's stop detaches from it first), so the
// break has to come from a process attached to no other console.
int send_ctrl_break(const std::string_view pid_text) {
  DWORD pid = 0;
  const auto parsed = std::from_chars(pid_text.data(), pid_text.data() + pid_text.size(), pid);
  if (parsed.ec != std::errc{} || parsed.ptr != pid_text.data() + pid_text.size() || pid == 0U) return 2;
  (void)FreeConsole();
  if (AttachConsole(pid) == FALSE) return 1;
  (void)SetConsoleCtrlHandler(nullptr, TRUE);  // (the break is not for this process)
  const BOOL sent = GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid);
  (void)FreeConsole();
  return sent != FALSE ? 0 : 1;
}

// The server child's process id: this process's child running `program` (ServerChild keeps its own to itself).
std::optional<DWORD> child_running(const std::filesystem::path& program) {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return std::nullopt;
  std::optional<DWORD> found;
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof entry;
  const std::wstring name = program.filename().wstring();
  for (BOOL more = Process32FirstW(snapshot, &entry); more != FALSE && !found; more = Process32NextW(snapshot, &entry)) {
    if (entry.th32ParentProcessID == GetCurrentProcessId() &&
        CompareStringOrdinal(entry.szExeFile, -1, name.c_str(), -1, TRUE) == CSTR_EQUAL) {
      found = entry.th32ProcessID;
    }
  }
  (void)CloseHandle(snapshot);
  return found;
}

// The console closing: Ctrl-Break to the server by `strata-tui --ctrl-break`, then up to `grace` for it to end in
// order. False when it did not (ServerChild's stop then ends it).
bool stop_by_helper(const std::filesystem::path& program, const w::ServerChild& server, const std::chrono::milliseconds grace) {
  const auto pid = child_running(program);
  if (!pid) return false;
  std::wstring self(32768, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size()));
  if (length == 0U || length >= self.size()) return false;
  self.resize(length);
  std::wstring command = L"\"" + self + L"\" --ctrl-break " + std::to_wstring(*pid);
  STARTUPINFOW startup{};
  startup.cb = sizeof startup;
  PROCESS_INFORMATION started{};
  if (CreateProcessW(self.c_str(), command.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, nullptr, &startup, &started) ==
      FALSE) {
    return false;
  }
  (void)WaitForSingleObject(started.hProcess, 2000);
  (void)CloseHandle(started.hThread);
  (void)CloseHandle(started.hProcess);
  const auto until = std::chrono::steady_clock::now() + grace;
  while (server.running() && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  return !server.running();
}
#else
void on_signal(int) { g_quit_signal.store(true); }
#endif

// "http://host:port" names this PC's loopback (127.0.0.0/8, ::1).
bool loopback_origin(const std::string_view origin) {
  if (!origin.starts_with("http://")) return false;
  const auto authority = origin.substr(7);
  const auto colon = authority.rfind(':');
  if (colon == std::string_view::npos) return false;
  auto host = authority.substr(0, colon);
  if (host.size() >= 2U && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2U);
  const auto address = secure_frontend::parse_listener_address(host);
  return address.has_value() && address->scope() == secure_frontend::AddressScope::loopback;
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
  const auto converted = secure_frontend::utf8_command_line(argc, argv);
  if (!converted) {
    std::cerr << "strata-tui: the command line could not be read\n";
    return 2;
  }
  std::vector<std::string> arguments(converted->begin() + (converted->empty() ? 0 : 1), converted->end());
  if (arguments.size() == 2U && arguments[0] == "--ctrl-break") return send_ctrl_break(arguments[1]);
  g_stopped = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  (void)SetConsoleCtrlHandler(on_console_event, TRUE);
#else
  std::vector<std::string> arguments;
  for (int i = 1; i < argc; ++i) arguments.emplace_back(argv[i]);
  // (Ctrl-C and Ctrl-\ are keys in raw mode; these come from kill, a closed terminal or a dropped SSH session)
  for (const int quit_signal : {SIGTERM, SIGHUP, SIGINT, SIGQUIT}) std::signal(quit_signal, on_signal);
#endif
  if (!arguments.empty() && (arguments[0] == "--help" || arguments[0] == "-h")) {
    std::cout << "strata-tui: the M-Strata server in this terminal. It starts strata-unified-server (beside it) with these\n"
                 "arguments, shows its Monitor, Models, Downloads, Settings, Log and About, and stops it when you quit (q).\n";
    return 0;
  }
  auto made = w::server_setup(arguments, secure_frontend::executable_directory());
  if (const auto* problem = std::get_if<std::string>(&made)) {
    std::cerr << "strata-tui: " << *problem << '\n';
    return 2;
  }
  const auto setup = std::get<w::ServerSetup>(std::move(made));
  t::Terminal terminal;
  if (const auto problem = terminal.open()) {
    std::cerr << "strata-tui: " << *problem << '\n';
    return 1;
  }
  const t::Capabilities capabilities = terminal.capabilities();
  // make --debugging (owner 2026-10-29): a debugging build says what it logs and starts the server only on y; any other
  // key (or a quit signal, or a closed terminal) ends it before anything starts
  if (w::asks_before_starting()) {
    t::KeyDecoder decoder;
    std::optional<t::TerminalSize> drawn;
    t::GateAnswer answer = t::GateAnswer::waiting;
    while (answer == t::GateAnswer::waiting) {
      if (g_quit_signal.load() || terminal.gone()) {
        answer = t::GateAnswer::declined;
        break;
      }
      const auto size = terminal.size();
      if (!drawn || !(*drawn == size)) {
        t::Screen screen(size.width, size.height);
        t::draw_debugging_notice(screen, w::kDebuggingNotice);
        terminal.write(t::render(screen, nullptr, capabilities));
        drawn = size;
      }
      const std::string typed = terminal.read(std::chrono::milliseconds(100));
      auto keys = decoder.feed(typed);
      if (typed.empty() && decoder.holding()) {
        for (auto& key : decoder.flush()) keys.push_back(std::move(key));
      }
      for (const auto& key : keys) {
        answer = t::debugging_gate(key);
        if (answer != t::GateAnswer::waiting) break;
      }
    }
    if (answer != t::GateAnswer::accepted) {
      terminal.close();
      std::cout << "strata-tui: not started (a debugging build starts the server only on y).\n";
#if defined(_WIN32)
      if (g_stopped != nullptr) (void)SetEvent(g_stopped);
#endif
      return 0;
    }
  }
  t::App app(capabilities, std::nullopt);
  Shared shared;
  std::optional<w::RotatingLog> log;  // opened at the first line once the logs folder is there (a first start makes it)

  w::ServerChild server;
  const auto failed = server.start(
      setup.server, setup.arguments, setup.environment,
      [&shared, &setup](std::string line) {
        try {
          if (!w::loggable(line)) return;  // the one-time sign-in link: never shown, never logged
          std::lock_guard lock(shared.mutex);
          if (const auto origin = w::listening_origin(line)) {
            shared.origin = *origin;
            shared.reachable = loopback_origin(*origin);
            const auto key = w::server_key(setup);
            shared.key = key ? key->key : std::nullopt;
            shared.keyless = key.has_value() && key->keyless;
          }
          shared.lines.push_back(std::move(line));
          shared.wake.notify_all();
        } catch (...) {
          // (a line lost: this is the server child's reader thread, which must go on)
        }
      },
      [&shared](const int code) {
        std::lock_guard lock(shared.mutex);
        shared.exit_code = code;
        shared.wake.notify_all();
      });
  app.server_state(failed ? *failed : std::string{"starting"}, false);

  // The server's lines to its log file and the Log view; whether there were any.
  const auto drain = [&shared, &setup, &log, &app] {
    std::deque<std::string> lines;
    {
      std::lock_guard lock(shared.mutex);
      lines.swap(shared.lines);
    }
    for (auto& line : lines) {
      if (!log && setup.data) {
        std::error_code error;
        if (std::filesystem::is_directory(setup.data->logs, error)) log.emplace(setup.data->logs / "server.log", kLogBytes);
      }
      if (log) log->write(line);
      app.log_line(std::move(line));
    }
    return !lines.empty();
  };

  // the requests: the views' polls every second, the keys' actions at once; none is started once quitting began
  std::thread worker([&shared] {
    auto next_poll = std::chrono::steady_clock::now();
    for (;;) {
      std::vector<t::Request> todo;
      bool polling = false;
      std::optional<std::string> origin;
      std::optional<std::string> key;
      {
        std::unique_lock lock(shared.mutex);
        shared.wake.wait_until(lock, next_poll, [&shared] { return shared.stopping || !shared.actions.empty(); });
        if (shared.stopping) return;
        if (!shared.actions.empty()) {
          todo.assign(shared.actions.begin(), shared.actions.end());
          shared.actions.clear();
        } else if (std::chrono::steady_clock::now() >= next_poll) {
          todo = shared.polls;
          polling = true;
          next_poll = std::chrono::steady_clock::now() + kPollEvery;
        }
        origin = shared.origin;
        key = shared.key;
      }
      if (!origin) {
        // no server to ask yet (it has not said where it listens): a key's request is answered as not answered, so it
        // is said rather than lost
        if (!polling && !todo.empty()) {
          std::lock_guard lock(shared.mutex);
          for (const auto& request : todo) shared.answers.emplace_back(request, 0, std::string{});
          shared.wake.notify_all();
        }
        continue;
      }
      for (const auto& request : todo) {
        {
          std::lock_guard lock(shared.mutex);
          if (shared.stopping) return;
        }
        int status = 0;
        std::string body;
        try {
          const auto answer = w::loopback_request(*origin, request.method, request.path, key ? &*key : nullptr, request.body,
                                                  polling ? kPollTimeout : kActionTimeout);
          if (answer) {
            status = answer->status;
            body = answer->body;
          }
        } catch (...) {
          // (no answer)
        }
        std::lock_guard lock(shared.mutex);
        shared.answers.emplace_back(request, status, std::move(body));
        shared.wake.notify_all();
      }
    }
  });

  std::optional<std::string> failure;  // what ended the loop when it was not a quit
  try {
    t::KeyDecoder decoder;
    std::optional<t::Screen> shown;
    t::TerminalSize size = terminal.size();
    auto drawn_at = std::chrono::steady_clock::now() - std::chrono::hours(1);
    bool dirty = true;
    bool quit = false;
    while (!quit) {
      const std::string typed = terminal.read(std::chrono::milliseconds(50));
      auto keys = decoder.feed(typed);
      if (typed.empty() && decoder.holding()) {
        for (auto& key : decoder.flush()) keys.push_back(std::move(key));
      }
      for (const auto& key : keys) {
        const auto outcome = app.key(key);
        dirty = true;
        if (outcome.quit) quit = true;
        if (!outcome.requests.empty()) {
          std::lock_guard lock(shared.mutex);
          for (const auto& request : outcome.requests) shared.actions.push_back(request);
          shared.wake.notify_all();
        }
      }
      if (g_quit_signal.load() || terminal.gone()) quit = true;
      if (drain()) dirty = true;
      std::deque<std::tuple<t::Request, int, std::string>> answers;  // (parsed after the lock: the reader and the worker wait on it)
      {
        std::lock_guard lock(shared.mutex);
        if (shared.origin) {
          app.server_state(*shared.origin, true, shared.reachable);
          app.set_key(shared.key, shared.keyless);
        }
        if (shared.exit_code) app.server_state("stopped (exit " + std::to_string(*shared.exit_code) + ")", false);
        answers.swap(shared.answers);
      }
      for (auto& [request, status, body] : answers) {
        app.answer(request, status, body);
        dirty = true;
      }
      {
        std::lock_guard lock(shared.mutex);
        shared.polls = app.polls();
      }
      const std::string said = app.status();
      app.tick(std::time(nullptr));  // (an action's words leave the status line after ten seconds)
      if (app.status() != said) dirty = true;
      const auto now_size = terminal.size();
      if (!(now_size == size)) {
        size = now_size;
        shown.reset();
        dirty = true;
      }
      const auto now = std::chrono::steady_clock::now();
      if (dirty || now - drawn_at >= kPollEvery) {
        t::Screen screen(size.width, size.height);
        app.draw(screen, std::time(nullptr));
        terminal.write(t::render(screen, shown ? &*shown : nullptr, capabilities));
        shown = std::move(screen);
        drawn_at = now;
        dirty = false;
      }
    }
  } catch (const std::exception& error) {
    failure = error.what();
  } catch (...) {
    failure = "an unexpected error";
  }

  // Quitting. No request is started from here on. The terminal is put back first: on Windows, stopping the server
  // attaches this process to the server's own console for a moment (Ctrl-Break reaches the server only from there),
  // which must not happen to a console still in raw mode on the alternate screen. Then the server stops in order (its
  // model first), its last lines are logged, and the request worker (whose request in flight ends with the server) is
  // joined.
  {
    std::lock_guard lock(shared.mutex);
    shared.stopping = true;
    shared.wake.notify_all();
  }
  terminal.close();
  if (failure) std::cerr << "strata-tui: " << *failure << '\n';
  bool orderly = true;
  if (server.running()) {
    std::cout << "strata-tui: stopping the server (its model first) ..." << std::endl;
    const auto grace = g_closing.load() ? std::chrono::milliseconds(kClosingGrace) : std::chrono::milliseconds(kStopGrace);
    auto left = grace;
#if defined(_WIN32)
    // The break comes from a helper (see send_ctrl_break) on every way out, so this process never leaves its own console:
    // ServerChild's stop detaches to reach the server's, which closes a console this process alone holds (a Windows
    // Terminal tab running strata-tui) and loses the lines below - or, while the console closes, waits. stop() then only
    // reaps the server, or, when the helper could not start or the server did not end in what is left of the grace,
    // asks and ends it (while the console closes, Windows may end this process first, and with it, through the job,
    // the server).
    const auto began = std::chrono::steady_clock::now();
    (void)stop_by_helper(setup.server, server, grace);
    const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began);
    left = std::max(std::chrono::milliseconds(1000), grace - spent);
#endif
    orderly = server.stop(left);
  }
  try {
    (void)drain();
  } catch (...) {
    // (a log that cannot be written is no reason to stay)
  }
#if defined(_WIN32)
  if (g_stopped != nullptr) (void)SetEvent(g_stopped);
#endif
  worker.join();
  std::cout << (orderly ? "strata-tui: the server stopped.\n" : "strata-tui: the server did not stop in time and was ended.\n");
  return failure ? 1 : 0;
}
