// A stand-in for strata-unified-server in the window's tests (window_shell_test.cpp): it prints the server's listening
// and sign-in lines (stdout) and a log line (stderr), then waits for what stops the real server - a console Ctrl-Break
// on Windows, SIGTERM elsewhere - and exits 0 after "fake: stopping". --exit <n> ends at once with n; --ignore-stop
// keeps running through the request (the window must then kill it); --show-env <name> first prints whether that
// variable reached it ("env <name>=set" or "=unset", never its value).
#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#else
#include <csignal>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {

std::atomic<bool> g_stop{false};
bool g_ignore = false;

#if defined(_WIN32)
BOOL WINAPI on_console_event(const DWORD type) {
  if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT && type != CTRL_CLOSE_EVENT) return FALSE;
  if (!g_ignore) g_stop.store(true);
  return TRUE;
}
#else
void on_signal(int) { g_stop.store(true); }
#endif

}  // namespace

int main(int argc, char** argv) {
  int exit_now = -1;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--ignore-stop") == 0) g_ignore = true;
    if (std::strcmp(argv[i], "--exit") == 0 && i + 1 < argc) exit_now = std::atoi(argv[++i]);
    if (std::strcmp(argv[i], "--show-env") == 0 && i + 1 < argc) {
      const char* name = argv[++i];
#if defined(_WIN32)
      char value[8];
      const bool set = GetEnvironmentVariableA(name, value, sizeof value) > 0;
#else
      const char* value = std::getenv(name);
      const bool set = value != nullptr && *value != '\0';
#endif
      std::printf("env %s=%s\n", name, set ? "set" : "unset");
    }
  }
  std::fputs("[strata] models: no model started (models.start is unset); waiting for the page\n", stderr);
  std::fflush(stderr);
  std::fputs("Secure server listening on http://127.0.0.1:18999 (authenticated; Ctrl-C to stop)\n", stdout);
  std::fputs("Sign in: open http://127.0.0.1:18999/#login=Zq3_x-9Lk2Pw7_Rt0Vb4Nm8Hs1Jc6Yd5Ue2Ia9Of3Gk in a browser on this PC (one-time link, valid 10 minutes; "
             "restart for a new one)\n", stdout);
  std::fflush(stdout);
  if (exit_now >= 0) return exit_now;
#if defined(_WIN32)
  (void)SetConsoleCtrlHandler(on_console_event, TRUE);
#else
  struct sigaction action {};
  action.sa_handler = g_ignore ? SIG_IGN : on_signal;
  sigemptyset(&action.sa_mask);
  (void)sigaction(SIGTERM, &action, nullptr);
#endif
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (!g_stop.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  if (!g_stop.load()) return 9;
  std::fputs("fake: stopping\n", stdout);
  std::fflush(stdout);
  return 0;
}
