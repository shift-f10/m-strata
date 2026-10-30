// GUI-first start-up (docs/superpowers/specs/2026-10-27-gui-first-install-design.md §5a): the email alerts' SMTP
// dialogue over a scripted server - implicit TLS (465) and STARTTLS (587) as IONOS offers them, AUTH PLAIN else
// LOGIN, the message's headers and dot-stuffing - and its refusals: no AUTH before TLS, a failed login named with the
// server's own code, and nothing in a header that could add one.
#include "secure_frontend/smtp_client.hpp"
#include "support/scripted_smtp.hpp"
#include "support/test_support.hpp"

#include <chrono>
#include <deque>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

using test_support::ScriptedServer;

const secure_frontend::SmtpMessage kMessage{"alerts@example.com", {"owner@example.com", "second@example.com"},
                                            "Strata: download finished", "flint-Q8_0 is downloaded.\n.hidden line\nend"};

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

void implicit_tls_sends_with_auth_plain() {
  ScriptedServer server({"220 smtp.ionos.com ESMTP", "250-smtp.ionos.com\n250-AUTH LOGIN PLAIN\n250 SIZE 140000000", "235 ok",
                         "250 ok", "250 ok", "250 ok", "354 go", "250 queued", "221 bye"},
                        true);
  const auto result = secure_frontend::send_mail(server, secure_frontend::SmtpSecurity::ssl, "strata.local", "alerts@example.com",
                                                 "secret-pass", kMessage, "Tue, 27 Oct 2026 18:00:00 +0000", "<1@strata.local>");
  expect(result.sent, "sent: " + result.stage + " " + std::to_string(result.code) + " " + result.reply);
  const std::string& w = server.written();
  expect(has(w, "EHLO strata.local\r\n") && has(w, "AUTH PLAIN AGFsZXJ0c0BleGFtcGxlLmNvbQBzZWNyZXQtcGFzcw==\r\n"),
         "EHLO, then AUTH PLAIN with \\0user\\0password: " + w);
  expect(has(w, "MAIL FROM:<alerts@example.com>\r\n") && has(w, "RCPT TO:<owner@example.com>\r\n") &&
             has(w, "RCPT TO:<second@example.com>\r\n") && has(w, "DATA\r\n"),
         "the envelope");
  expect(has(w, "From: alerts@example.com\r\n") && has(w, "To: owner@example.com, second@example.com\r\n") &&
             has(w, "Subject: Strata: download finished\r\n") && has(w, "Date: Tue, 27 Oct 2026 18:00:00 +0000\r\n") &&
             has(w, "Message-ID: <1@strata.local>\r\n") && has(w, "Content-Type: text/plain; charset=utf-8\r\n"),
         "the headers");
  expect(has(w, "\r\n..hidden line\r\n") && has(w, "\r\nend\r\n.\r\nQUIT\r\n"), "dot-stuffing, CRLF lines, the end, QUIT");
}

void starttls_upgrades_before_auth_login() {
  ScriptedServer server({"220-smtp.ionos.de\n220 ready", "250-smtp.ionos.de\n250-STARTTLS\n250 SIZE 1", "220 go ahead",
                         "250-smtp.ionos.de\n250 AUTH LOGIN", "334 VXNlcm5hbWU6", "334 UGFzc3dvcmQ6", "235 ok", "250 ok", "250 ok",
                         "250 ok", "354 go", "250 queued", "221 bye"});
  const auto result = secure_frontend::send_mail(server, secure_frontend::SmtpSecurity::starttls, "strata.local", "alerts@example.com",
                                                 "secret-pass", kMessage, "Tue, 27 Oct 2026 18:00:00 +0000", "<2@strata.local>");
  expect(result.sent, "sent over STARTTLS: " + result.stage + " " + result.reply);
  const std::string& w = server.written();
  expect(server.tls() && w.find("STARTTLS\r\n") < server.tls_at() && w.find("AUTH LOGIN") > server.tls_at(),
         "TLS starts before AUTH");
  expect(w.find("EHLO strata.local\r\n", server.tls_at()) != std::string::npos, "EHLO again inside TLS");
  expect(has(w, "AUTH LOGIN\r\nYWxlcnRzQGV4YW1wbGUuY29t\r\nc2VjcmV0LXBhc3M=\r\n"), "AUTH LOGIN with base64 user and password");
}

void refusals_are_named_and_never_leak() {
  // a server without STARTTLS: nothing is sent in the clear, the password above all
  ScriptedServer plain({"220 hi", "250-host\n250 AUTH PLAIN"});
  const auto clear = secure_frontend::send_mail(plain, secure_frontend::SmtpSecurity::starttls, "strata.local", "u@example.com",
                                                "secret-pass", kMessage, "d", "<3@x>");
  expect(!clear.sent && clear.stage == "starttls" && !has(plain.written(), "AUTH") && !has(plain.written(), "secret"),
         "no STARTTLS: stopped before AUTH: " + clear.stage);
  // a wrong password: the server's code and reply, the password nowhere
  ScriptedServer wrong({"220 hi", "250-host\n250 AUTH PLAIN", "535 5.7.8 Authentication credentials invalid"}, true);
  const auto denied = secure_frontend::send_mail(wrong, secure_frontend::SmtpSecurity::ssl, "strata.local", "u@example.com",
                                                 "secret-pass", kMessage, "d", "<4@x>");
  expect(!denied.sent && denied.stage == "auth" && denied.code == 535 && has(denied.reply, "credentials invalid") &&
             !has(denied.reply, "secret"),
         "a refused login says so: " + denied.reply);
  // a closed connection
  ScriptedServer gone({}, true);
  const auto none = secure_frontend::send_mail(gone, secure_frontend::SmtpSecurity::ssl, "strata.local", "u@example.com", "p", kMessage,
                                               "d", "<5@x>");
  expect(!none.sent && none.stage == "greeting", "no greeting");
  // a header can never gain a line
  const auto text = secure_frontend::render_message(
      {"a@example.com", {"b@example.com"}, "hi\r\nBcc: evil@example.com", "body"}, "d", "<6@x>");
  expect(!has(text, "\r\nBcc:") && has(text, "Subject: hi  Bcc: evil@example.com\r\n"), "a subject's line breaks become spaces: " + text);
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"implicit_tls_sends_with_auth_plain", implicit_tls_sends_with_auth_plain},
      {"starttls_upgrades_before_auth_login", starttls_upgrades_before_auth_login},
      {"refusals_are_named_and_never_leak", refusals_are_named_and_never_leak},
  }, failures);
}
