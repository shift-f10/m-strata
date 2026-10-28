#pragma once

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace secure_frontend {

// GUI-first start-up (docs/superpowers/specs/2026-10-27-gui-first-install-design.md §3, D1): the one-time login link.
// The server prints http://127.0.0.1:<port>/#login=<code> at every start; the page posts the code to
// POST /v1/strata/login-link (loopback clients only) and receives the API key once. The code is 32 random bytes
// (43 base64url characters), valid kLifetime, used at most once, and spent after kMaximumAttempts wrong tries; a new
// code replaces the one before it. Thread-safe.
class LoginLinks final {
 public:
  using Clock = std::chrono::steady_clock;
  static constexpr std::chrono::minutes kLifetime{10};
  static constexpr int kMaximumAttempts = 10;

  // A fresh code (nullopt when the system's random source fails).
  [[nodiscard]] std::optional<std::string> issue(Clock::time_point now);
  // true exactly once, for the current code before it expires; compared in constant time.
  [[nodiscard]] bool claim(std::string_view code, Clock::time_point now);

 private:
  std::mutex mutex_;
  std::string code_;
  Clock::time_point expires_{};
  int wrong_{0};
  bool live_{false};
};

// The link for `authority` (host:port): the code follows '#', so a browser never sends it in a request line, a
// Referer or a log.
[[nodiscard]] std::string login_link_url(std::string_view authority, std::string_view code);

}  // namespace secure_frontend
