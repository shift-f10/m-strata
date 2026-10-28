// GUI-first start-up (docs/superpowers/specs/2026-10-27-gui-first-install-design.md §5a): the email alerts' SMTP
// dialogue over a scripted server - implicit TLS (465) and STARTTLS (587) as IONOS offers them, AUTH PLAIN else
// LOGIN, the message's headers and dot-stuffing - and its refusals: no AUTH before TLS, a failed login named with the
// server's own code, and nothing in a header that could add one.
#include "secure_frontend/alert_routes.hpp"
#include "secure_frontend/email_alerts.hpp"
#include "secure_frontend/first_start.hpp"
#include "secure_frontend/smtp_client.hpp"
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

// A server that answers each command line with the next scripted reply (one entry may hold several lines).
class ScriptedServer final : public secure_frontend::SmtpTransport {
 public:
  explicit ScriptedServer(std::deque<std::string> replies, const bool tls = false) : replies_(std::move(replies)), tls_(tls) {}
  bool write(const std::string_view bytes) override {
    written_ += bytes;
    if (in_data_) {
      if (written_.size() >= 5U && written_.compare(written_.size() - 5U, 5U, "\r\n.\r\n") == 0) in_data_ = false;
      return true;
    }
    if (bytes == "DATA\r\n") in_data_after_reply_ = true;
    return true;
  }
  std::optional<std::string> read_line() override {
    if (pending_.empty()) {
      if (replies_.empty()) return std::nullopt;
      std::string reply = replies_.front();
      replies_.pop_front();
      for (std::size_t start = 0; start < reply.size();) {
        const auto end = reply.find('\n', start);
        pending_.push_back(reply.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) break;
        start = end + 1U;
      }
      if (in_data_after_reply_) {
        in_data_ = true;
        in_data_after_reply_ = false;
      }
    }
    std::string line = pending_.front();
    pending_.pop_front();
    return line;
  }
  bool start_tls() override {
    tls_ = true;
    tls_at_ = written_.size();
    return true;
  }
  [[nodiscard]] bool tls() const { return tls_; }
  [[nodiscard]] const std::string& written() const { return written_; }
  [[nodiscard]] std::size_t tls_at() const { return tls_at_; }

 private:
  std::deque<std::string> replies_;
  std::deque<std::string> pending_;
  std::string written_;
  bool tls_{false};
  bool in_data_{false};
  bool in_data_after_reply_{false};
  std::size_t tls_at_{0};
};

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

// The queue's transport: forwards to a server the test keeps, so its log outlives the connection.
class Forward final : public secure_frontend::SmtpTransport {
 public:
  explicit Forward(std::shared_ptr<ScriptedServer> server) : server_(std::move(server)) {}
  bool write(const std::string_view bytes) override { return server_->write(bytes); }
  std::optional<std::string> read_line() override { return server_->read_line(); }
  bool start_tls() override { return server_->start_tls(); }

 private:
  std::shared_ptr<ScriptedServer> server_;
};

std::deque<std::string> a_good_session() {
  return {"220 hi", "250-host\n250 AUTH PLAIN", "235 ok", "250 ok", "250 ok", "354 go", "250 queued", "221 bye"};
}

// §5a: the password is kept in the secret folder (DPAPI on Windows) and read back; the queue sends only the events
// chosen, at most one per kind a minute, and a send without a password says so.
void alerts_queue_and_the_password() {
  const auto secret = secure_frontend_test::make_scratch_directory();
  expect(secure_frontend::store_smtp_password(secret, "line\nbreak").has_value(), "a password with a line break is refused");
  expect(!secure_frontend::store_smtp_password(secret, "short pass").has_value(), "a mailbox password is stored");
  const auto read = secure_frontend::read_smtp_password(secret);
  expect(read.has_value() && read->view() == "short pass", "and read back");
#if defined(_WIN32)
  expect(secure_frontend_test::read_file(secret / "smtp-password").find("short pass") == std::string::npos, "never in the clear on disk");
#endif

  std::vector<std::shared_ptr<ScriptedServer>> servers;
  secure_frontend::EmailAlertsConfig config;
  config.enabled = true;
  config.host = "smtp.ionos.com";
  config.username = "alerts@example.com";
  config.from = "alerts@example.com";
  config.to = {"owner@example.com"};
  config.events = {"download_finished"};
  secure_frontend::AlertSettings settings{.config = config, .secret_dir = secret, .server_name = "testbox"};
  settings.connect = [&servers](std::string*) -> std::unique_ptr<secure_frontend::SmtpTransport> {
    servers.push_back(std::make_shared<ScriptedServer>(a_good_session(), true));
    return std::make_unique<Forward>(servers.back());
  };
  settings.log = [](std::string_view) {};
  secure_frontend::EmailAlerts alerts(std::move(settings));
  expect(alerts.raise("download_finished", "flint-Q8_0 is downloaded", "Its card is in Models."), "a chosen event is queued");
  expect(alerts.wait_idle(std::chrono::seconds(10)), "and sent");
  expect(servers.size() == 1U && has(servers[0]->written(), "Subject: Strata on testbox: flint-Q8_0 is downloaded\r\n") &&
             has(servers[0]->written(), "Its card is in Models.\r\n"),
         "the mail names the server and the event");
  expect(!alerts.raise("download_finished", "again", "x"), "one per kind a minute");
  expect(!alerts.raise("engine_stopped", "x", "y"), "an event not chosen is not sent");
  const auto test = alerts.send_test();
  expect(test.sent && servers.size() == 2U && has(servers[1]->written(), "Subject: Strata on testbox: test email"), "a test email");
  alerts.shutdown();

  const auto empty = secure_frontend_test::make_scratch_directory();
  secure_frontend::AlertSettings no_password{.config = config, .secret_dir = empty, .server_name = "testbox"};
  no_password.log = [](std::string_view) {};
  secure_frontend::EmailAlerts without(std::move(no_password));
  const auto refused = without.send_test();
  expect(!refused.sent && refused.stage == "password", "no password: said so, nothing sent");
  config.enabled = false;
  secure_frontend::AlertSettings off{.config = config, .secret_dir = secret, .server_name = "testbox"};
  off.log = [](std::string_view) {};
  secure_frontend::EmailAlerts disabled(std::move(off));
  expect(!disabled.raise("download_finished", "x", "y"), "alerts off: nothing queued");
}

// §5a: the page's alert settings over the routes - read from the config file, saved to it (validated; applied at the
// next start), the password kept in the secret folder and never shown, a test email through the saved settings -
// for this PC's clients only.
void alert_routes_read_save_and_test() {
  const auto root = secure_frontend_test::make_scratch_directory();
  const auto data = secure_frontend::data_folder_layout(root / "data");
  const auto engine = root / "engine.exe";
  secure_frontend_test::write_file(engine, "engine");
  expect(std::holds_alternative<secure_frontend::FirstStartResult>(secure_frontend::ensure_data_folder(data, engine)), "a data folder");
  std::vector<std::shared_ptr<ScriptedServer>> servers;
  secure_frontend::AlertRouteSettings settings{.config_file = data.config_file, .secret_dir = data.secret, .server_name = "testbox"};
  settings.connect = [&servers](const secure_frontend::EmailAlertsConfig&, std::string*) -> std::unique_ptr<secure_frontend::SmtpTransport> {
    servers.push_back(std::make_shared<ScriptedServer>(a_good_session(), true));
    return std::make_unique<Forward>(servers.back());
  };
  settings.log = [](std::string_view) {};
  secure_frontend::AlertRoutes routes(std::move(settings));
  const auto post = [&routes](const secure_frontend::RouteId route, const std::string& body, const bool loopback = true) {
    return routes.answer(route, body, secure_frontend::RequestContext{.loopback_peer = loopback});
  };
  const auto first = post(secure_frontend::RouteId::strata_alerts, "{}");
  expect(first.status == 200 && has(first.body, "\"built\":true") && has(first.body, "\"enabled\":false") &&
             has(first.body, "\"password_set\":false"),
         "the settings, off at first: " + first.body);
  expect(post(secure_frontend::RouteId::strata_alerts, "{}", false).status == 403, "this PC's clients only");

  const std::string ionos = R"({"enabled":true,"host":"smtp.ionos.com","port":465,"security":"ssl","username":"alerts@example.com",)"
                            R"("from":"alerts@example.com","to":["owner@example.com"],"events":["download_finished","server_started"]})";
  const auto saved = post(secure_frontend::RouteId::strata_alerts_settings, ionos);
  expect(saved.status == 200 && has(saved.body, "\"restart_needed\":true"), "saved: " + saved.body);
  const auto text = secure_frontend_test::read_file(data.config_file);
  expect(has(text, "\"host\": \"smtp.ionos.com\"") && has(text, "\"listener\""), "into the config file, the rest kept: " + text);
  const auto bad = post(secure_frontend::RouteId::strata_alerts_settings, R"({"enabled":true,"host":"smtp.ionos.com","port":465,"security":"ssl",)"
                                                                         R"("username":"a@example.com","from":"no address","to":["b@example.com"],"events":[]})");
  expect(bad.status == 422 && has(bad.body, "\"code\":\"settings_refused\"") && has(bad.body, "alerts.email.from"),
         "a bad address is refused by name: " + bad.body);
  expect(has(secure_frontend_test::read_file(data.config_file), "\"from\": \"alerts@example.com\""), "and nothing is written");

  expect(post(secure_frontend::RouteId::strata_alerts_password, R"({"password":"mailbox pass"})").status == 200, "the password");
  const auto after = post(secure_frontend::RouteId::strata_alerts, "{}");
  expect(has(after.body, "\"password_set\":true") && !has(after.body, "mailbox pass") && has(after.body, "\"host\":\"smtp.ionos.com\""),
         "is set, never shown: " + after.body);
  const auto tested = post(secure_frontend::RouteId::strata_alerts_test, "{}");
  expect(tested.status == 200 && has(tested.body, "\"sent\":true") && servers.size() == 1U &&
             has(servers[0]->written(), "AUTH PLAIN ") && has(servers[0]->written(), "Subject: Strata on testbox: test email"),
         "a test email through the saved settings: " + tested.body);
}

// Owner 2026-10-27 ("I don't see a section for the email notification" on a page opened from another PC): the routes
// let in the clients the server's rule lets in (it passes model_files_allowed, as for the model-file routes); the
// password is still never shown. Without a rule, this PC's clients only.
void alert_routes_let_in_the_clients_the_rule_lists() {
  const auto root = secure_frontend_test::make_scratch_directory();
  const auto data = secure_frontend::data_folder_layout(root / "data");
  const auto engine = root / "engine.exe";
  secure_frontend_test::write_file(engine, "engine");
  expect(std::holds_alternative<secure_frontend::FirstStartResult>(secure_frontend::ensure_data_folder(data, engine)), "a data folder");
  secure_frontend::AlertRouteSettings settings{.config_file = data.config_file, .secret_dir = data.secret, .server_name = "testbox"};
  settings.log = [](std::string_view) {};
  settings.client_allowed = [](const secure_frontend::RequestContext& context) {
    return context.loopback_peer || (context.peer[0] == 4U && context.peer[1] == 172U && context.peer[2] == 16U && context.peer[3] == 1U);
  };
  secure_frontend::AlertRoutes routes(std::move(settings));
  const auto from = [&routes](const secure_frontend::RouteId route, const std::string& body, const unsigned char subnet) {
    secure_frontend::RequestContext context{.loopback_peer = false};
    context.peer[0] = 4U;
    context.peer[1] = 172U;
    context.peer[2] = 16U;
    context.peer[3] = subnet;
    context.peer[4] = 7U;
    return routes.answer(route, body, context);
  };
  const auto listed = from(secure_frontend::RouteId::strata_alerts, "{}", 1U);
  expect(listed.status == 200 && has(listed.body, "\"built\":true"), "a listed client sees the settings: " + listed.body);
  expect(from(secure_frontend::RouteId::strata_alerts_password, R"({"password":"mailbox pass"})", 1U).status == 200, "and sets the password");
  const auto after = from(secure_frontend::RouteId::strata_alerts, "{}", 1U);
  expect(has(after.body, "\"password_set\":true") && !has(after.body, "mailbox pass"), "which is never shown: " + after.body);
  expect(from(secure_frontend::RouteId::strata_alerts, "{}", 3U).status == 403, "a client the rule does not list is refused");
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"implicit_tls_sends_with_auth_plain", implicit_tls_sends_with_auth_plain},
      {"starttls_upgrades_before_auth_login", starttls_upgrades_before_auth_login},
      {"refusals_are_named_and_never_leak", refusals_are_named_and_never_leak},
      {"alerts_queue_and_the_password", alerts_queue_and_the_password},
      {"alert_routes_read_save_and_test", alert_routes_read_save_and_test},
      {"alert_routes_let_in_the_clients_the_rule_lists", alert_routes_let_in_the_clients_the_rule_lists},
  }, failures);
}
