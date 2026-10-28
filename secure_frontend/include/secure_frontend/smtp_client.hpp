#pragma once

// GUI-first start-up (docs/superpowers/specs/2026-10-27-gui-first-install-design.md §5a): the email alerts' SMTP
// dialogue (RFC 5321) - built only with SECURE_FRONTEND_EMAIL_ALERTS. It runs over an SmtpTransport (smtp_tls.hpp:
// TLS from the first byte, or plain until STARTTLS) and never sends AUTH, or anything after it, outside TLS: with
// SmtpSecurity::starttls a server that offers no STARTTLS ends the dialogue before AUTH. AUTH PLAIN when offered,
// else LOGIN. The password goes only into the AUTH lines; the results carry the server's reply, never a secret.

#include "secure_frontend/frontend_config.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace secure_frontend {

class SmtpTransport {
 public:
  virtual ~SmtpTransport() = default;
  [[nodiscard]] virtual bool write(std::string_view bytes) = 0;
  // One reply line without its CRLF; nullopt when the connection closed, failed or timed out.
  [[nodiscard]] virtual std::optional<std::string> read_line() = 0;
  // STARTTLS: the TLS handshake on the open connection (the certificate and host name checked).
  [[nodiscard]] virtual bool start_tls() = 0;
  // The connection's next bytes, whatever a line read left buffered first (web search's HTTP over the same
  // connections, web_search.hpp); nullopt when it closed, failed or timed out. A transport without it has none.
  [[nodiscard]] virtual std::optional<std::string> read_chunk() { return std::nullopt; }
};

struct SmtpMessage final {
  std::string from;
  std::vector<std::string> to;
  std::string subject;  // one line: line breaks become spaces
  std::string body;     // plain text, UTF-8
};

struct SmtpResult final {
  bool sent{false};
  std::string stage;  // where it ended: greeting, ehlo, starttls, tls, auth, mail, rcpt, data, message, done
  int code{0};        // the server's last reply code (0: none)
  std::string reply;  // the server's last reply line, printable ASCII, at most 200 characters
};

// The whole dialogue: greeting, EHLO `client_name`, STARTTLS (starttls mode), AUTH, MAIL, RCPT, DATA, the message,
// QUIT. `date` is the Date header's text, `message_id` the Message-ID's (with its <>).
[[nodiscard]] SmtpResult send_mail(SmtpTransport& transport, SmtpSecurity security, std::string_view client_name,
                                   std::string_view username, std::string_view password, const SmtpMessage& message,
                                   std::string_view date, std::string_view message_id);

// The message as DATA sends it: the headers, a blank line, the body with CRLF line ends and dot-stuffing (no final
// "."). A header value's line breaks become spaces.
[[nodiscard]] std::string render_message(const SmtpMessage& message, std::string_view date, std::string_view message_id);

[[nodiscard]] std::string base64_encode(std::string_view bytes);

}  // namespace secure_frontend
