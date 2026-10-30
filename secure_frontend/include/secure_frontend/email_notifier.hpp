#pragma once

// Email alerts as a notifier plugin (docs/superpowers/specs/2026-10-30-engine-family-plugins-design.md, section 10; the
// owner's decision 6): the core of strata-email, the email plugin's program, moved out of the server (it was EmailAlerts,
// spec 2026-10-27 §5a). It reads the notifier host's lines - one JSON line per event, and the Plugins tab's test - and
// sends each as a mail through one SMTP server over TLS (smtp_client.hpp, smtp_tls.hpp), at most one per kind a minute,
// with no retry. Every line it acts on is answered by one line: a test's {"ok", "reply"}, an event's {"event", "ok",
// "reply"}. Its settings come as arguments (--host, --port, --security, --username, --from, --to), the password as
// SMTP_PASSWORD; a mail names the event and the server, never a path, a prompt or a key.

#include "secure_frontend/frontend_config.hpp"
#include "secure_frontend/secret_bytes.hpp"
#include "secure_frontend/smtp_client.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace secure_frontend {

struct EmailNotifierSettings final {
  std::string host;                 // the SMTP server, a DNS name
  std::uint16_t port{465};
  SmtpSecurity security{SmtpSecurity::ssl};
  std::string username;             // IONOS: the full mailbox address
  std::string from;
  std::vector<std::string> to;      // 1-5 addresses (one argument, comma-separated: a text list as the host writes it)
  std::string problem;              // what is wrong with the arguments (empty: nothing); every line is answered with it
};
// strata-email's arguments; the first wrong or missing one is named in `problem`.
[[nodiscard]] EmailNotifierSettings parse_email_arguments(const std::vector<std::string>& arguments);

class EmailNotifier final {
 public:
  // One connection to the mail server (production: connect_smtp with the settings' host, port and security).
  using Connect = std::function<std::unique_ptr<SmtpTransport>(const EmailNotifierSettings& settings, std::string* error)>;
  EmailNotifier(EmailNotifierSettings settings, SecretBytes password, Connect connect = {},
                std::chrono::seconds per_kind = std::chrono::seconds(60));

  // One line from the host: the answer line to write back, or nullopt for a line that is neither an event nor the test.
  [[nodiscard]] std::optional<std::string> handle(std::string_view line);

 private:
  [[nodiscard]] SmtpResult send(const std::string& server, const std::string& subject, const std::string& body);

  EmailNotifierSettings settings_;
  SecretBytes password_;
  Connect connect_;
  std::chrono::seconds per_kind_;
  std::map<std::string, std::chrono::steady_clock::time_point, std::less<>> last_sent_;
};

// RFC 5322's date for now (UTC): "Tue, 27 Oct 2026 18:00:00 +0000".
[[nodiscard]] std::string mail_date_now();

}  // namespace secure_frontend
