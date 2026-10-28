#pragma once

// GUI-first start-up (docs/superpowers/specs/2026-10-27-gui-first-install-design.md §5a): the email alerts - built
// only with SECURE_FRONTEND_EMAIL_ALERTS. An alert names an event (kAlertEvents) and a card or model, never a path, a
// prompt or a key; it is queued and sent from a background thread (a slow or failing mail server never delays a
// request), only for the events alerts.email.events names, at most one per kind a minute, at most 16 waiting, with no
// retry. The SMTP password is kept in roots.secret (smtp-password): Windows DPAPI for this user, elsewhere 0600.

#include "secure_frontend/frontend_config.hpp"
#include "secure_frontend/secret_bytes.hpp"
#include "secure_frontend/smtp_client.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace secure_frontend {

inline constexpr std::string_view kSmtpPasswordFile = "smtp-password";

// nullopt: stored; else a fixed reason. 1-256 bytes without line breaks or NUL.
[[nodiscard]] std::optional<std::string> store_smtp_password(const std::filesystem::path& secret_dir, std::string_view password);
[[nodiscard]] std::optional<SecretBytes> read_smtp_password(const std::filesystem::path& secret_dir);

struct AlertSettings final {
  EmailAlertsConfig config;
  std::filesystem::path secret_dir;  // roots.secret
  std::string server_name;           // what the mails call this server (the PC's name)
  // One connection to the mail server (production: connect_smtp with the config's host, port and security).
  std::function<std::unique_ptr<SmtpTransport>(std::string* error)> connect{};
  std::function<void(std::string_view)> log{};
  std::chrono::seconds per_kind{60};
};

class EmailAlerts final {
 public:
  explicit EmailAlerts(AlertSettings settings);
  ~EmailAlerts();
  EmailAlerts(const EmailAlerts&) = delete;
  EmailAlerts& operator=(const EmailAlerts&) = delete;

  // Queued (true) or dropped (false: off, an event not chosen, its kind sent within per_kind, or 16 waiting).
  bool raise(std::string_view event, const std::string& subject, const std::string& body);
  // The page's "Send a test email", now: the server's answer (never a secret).
  [[nodiscard]] SmtpResult send_test();
  // Waits for the queue to empty (tests), up to `timeout`.
  [[nodiscard]] bool wait_idle(std::chrono::milliseconds timeout);
  void shutdown();

 private:
  struct Alert final {
    std::string subject;
    std::string body;
  };
  SmtpResult send(const Alert& alert);
  void run();
  void log(std::string_view line) const;

  AlertSettings settings_;
  std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<Alert> queue_;
  std::map<std::string, std::chrono::steady_clock::time_point, std::less<>> last_sent_;
  bool sending_{false};
  bool stopping_{false};
  std::thread worker_;
};

// RFC 5322's date for now (UTC): "Tue, 27 Oct 2026 18:00:00 +0000".
[[nodiscard]] std::string mail_date_now();

}  // namespace secure_frontend
