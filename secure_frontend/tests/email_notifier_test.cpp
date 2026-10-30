// Email alerts as a notifier plugin (plugin spec 2026-10-30, section 10; the owner's decision 6): strata-email's core
// over a scripted SMTP server - its arguments, an event line mailed (at most one per kind a minute) and answered, the
// tab's test answered, and nothing sent without a password or with wrong settings, each said in the answer.
#include "secure_frontend/email_notifier.hpp"
#include "support/scripted_smtp.hpp"
#include "support/test_support.hpp"

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

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

const std::vector<std::string> kIonos{"--host", "smtp.ionos.com", "--port", "465", "--security", "ssl", "--username", "alerts@example.com",
                                      "--from", "alerts@example.com", "--to", "owner@example.com,second@example.com"};

secure_frontend::SecretBytes password_of(const std::string& text) {
  secure_frontend::SecretBytes password;
  (void)password.assign(text);
  return password;
}

// A notifier whose every connection is a good session the test keeps (one RCPT reply for each of kIonos's two addresses).
struct Mailbox final {
  std::vector<std::shared_ptr<test_support::ScriptedServer>> servers;
  secure_frontend::EmailNotifier::Connect connect() {
    return [this](const secure_frontend::EmailNotifierSettings&, std::string*) -> std::unique_ptr<secure_frontend::SmtpTransport> {
      servers.push_back(std::make_shared<test_support::ScriptedServer>(
          std::deque<std::string>{"220 hi", "250-host\n250 AUTH PLAIN", "235 ok", "250 ok", "250 ok", "250 ok", "354 go", "250 queued", "221 bye"},
          true));
      return std::make_unique<test_support::Forward>(servers.back());
    };
  }
};

void the_arguments_are_read_and_a_wrong_one_named() {
  const auto good = secure_frontend::parse_email_arguments(kIonos);
  expect(good.problem.empty() && good.host == "smtp.ionos.com" && good.port == 465U && good.security == secure_frontend::SmtpSecurity::ssl &&
             good.username == "alerts@example.com" && good.from == "alerts@example.com" &&
             good.to == std::vector<std::string>{"owner@example.com", "second@example.com"},
         "the IONOS settings are read: " + good.problem);
  const auto starttls = secure_frontend::parse_email_arguments({"--host", "smtp.example.org", "--port", "587", "--security", "starttls",
                                                                "--username", "u", "--from", "a@example.org", "--to", "b@example.org"});
  expect(starttls.problem.empty() && starttls.port == 587U && starttls.security == secure_frontend::SmtpSecurity::starttls, "STARTTLS on 587");
  const auto named = [](std::vector<std::string> arguments, const std::size_t at, const std::string& value, const std::string& name) {
    arguments[at] = value;
    const auto read = secure_frontend::parse_email_arguments(arguments);
    expect(has(read.problem, name), "a wrong " + name + " is named: " + read.problem);
  };
  named(kIonos, 1U, "", "host");
  named(kIonos, 1U, "smtp ionos com", "host");
  named(kIonos, 3U, "0", "port");
  named(kIonos, 3U, "465x", "port");
  named(kIonos, 5U, "plain", "security");
  named(kIonos, 9U, "not an address", "from");
  named(kIonos, 11U, "owner@example.com,,x", "to");
  named(kIonos, 11U, "a@example.com,b@example.com,c@example.com,d@example.com,e@example.com,f@example.com", "to");
  const auto missing = secure_frontend::parse_email_arguments({"--host", "smtp.ionos.com"});
  expect(!missing.problem.empty(), "missing settings are named: " + missing.problem);
  const auto unknown = secure_frontend::parse_email_arguments({"--hots", "x"});
  expect(has(unknown.problem, "--hots"), "an unknown argument is named: " + unknown.problem);
}

// An event line is mailed - the subject and body as the server wrote them, the server's name in the subject, a footer -
// and answered; another of its kind within the minute is not sent, and the answer says why; another kind is.
void an_event_is_mailed_once_a_minute_per_kind() {
  Mailbox mailbox;
  secure_frontend::EmailNotifier notifier(secure_frontend::parse_email_arguments(kIonos), password_of("mailbox pass"), mailbox.connect());
  const auto sent = notifier.handle(R"({"event":"download_finished","subject":"flint-Q8_0 is downloaded","body":"Its card is in Models.",)"
                                    R"("server":"testbox","time":"2026-10-30T12:00:00Z"})");
  expect(sent.has_value() && has(*sent, R"("event":"download_finished")") && has(*sent, R"("ok":true)"), "answered: " + sent.value_or("(none)"));
  expect(mailbox.servers.size() == 1U && has(mailbox.servers[0]->written(), "Subject: Strata on testbox: flint-Q8_0 is downloaded\r\n") &&
             has(mailbox.servers[0]->written(), "Its card is in Models.\r\n") &&
             has(mailbox.servers[0]->written(), "Sent by the Strata server on testbox.") &&
             has(mailbox.servers[0]->written(), "RCPT TO:<second@example.com>") && has(mailbox.servers[0]->written(), "AUTH PLAIN "),
         "the mail names the server and the event, to every address");
  const auto again = notifier.handle(R"({"event":"download_finished","subject":"again","body":"x","server":"testbox","time":"t"})");
  expect(again.has_value() && has(*again, R"("ok":false)") && has(*again, "within a minute") && mailbox.servers.size() == 1U,
         "one per kind a minute: " + again.value_or("(none)"));
  const auto other = notifier.handle(R"({"event":"engine_stopped","subject":"the engine stopped","body":"y","server":"testbox","time":"t"})");
  expect(other.has_value() && has(*other, R"("ok":true)") && mailbox.servers.size() == 2U, "another kind is sent");
  const auto unknown = notifier.handle(R"({"event":"rm_rf","subject":"s","body":"b","server":"testbox","time":"t"})");
  expect(unknown.has_value() && has(*unknown, R"("ok":false)") && mailbox.servers.size() == 2U, "an event it does not know is not sent");
  expect(!notifier.handle("not json").has_value() && !notifier.handle(R"({"hello":1})").has_value(), "other lines are not answered");
}

// The tab's test: a test mail now, answered by {"ok", "reply"} with the mail server's own reply.
void the_test_is_answered() {
  Mailbox mailbox;
  secure_frontend::EmailNotifier notifier(secure_frontend::parse_email_arguments(kIonos), password_of("mailbox pass"), mailbox.connect());
  for (int i = 0; i < 2; ++i) {
    const auto answer = notifier.handle(R"({"test":true,"server":"testbox"})");
    expect(answer.has_value() && answer->rfind(R"({"ok":true,"reply":)", 0) == 0U, "the test is sent: " + answer.value_or("(none)"));
  }
  expect(mailbox.servers.size() == 2U && has(mailbox.servers[1]->written(), "Subject: Strata on testbox: test email\r\n"),
         "each test is a mail (no per-kind limit for a test)");
}

// Nothing is sent without a password or with settings that are wrong, and the answer says why (never the password).
void without_a_password_or_with_wrong_settings_nothing_is_sent() {
  Mailbox mailbox;
  secure_frontend::EmailNotifier no_password(secure_frontend::parse_email_arguments(kIonos), {}, mailbox.connect());
  const auto test = no_password.handle(R"({"test":true,"server":"testbox"})");
  expect(test.has_value() && has(*test, R"("ok":false)") && has(*test, "password") && mailbox.servers.empty(),
         "no password: said so, nothing sent: " + test.value_or("(none)"));
  auto wrong_arguments = kIonos;
  wrong_arguments[9] = "nobody";
  secure_frontend::EmailNotifier wrong(secure_frontend::parse_email_arguments(wrong_arguments), password_of("mailbox pass"), mailbox.connect());
  const auto event = wrong.handle(R"({"event":"server_started","subject":"s","body":"b","server":"testbox","time":"t"})");
  expect(event.has_value() && has(*event, R"("ok":false)") && has(*event, "from") && !has(*event, "mailbox pass") && mailbox.servers.empty(),
         "wrong settings: named, nothing sent: " + event.value_or("(none)"));
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"the_arguments_are_read_and_a_wrong_one_named", the_arguments_are_read_and_a_wrong_one_named},
      {"an_event_is_mailed_once_a_minute_per_kind", an_event_is_mailed_once_a_minute_per_kind},
      {"the_test_is_answered", the_test_is_answered},
      {"without_a_password_or_with_wrong_settings_nothing_is_sent", without_a_password_or_with_wrong_settings_nothing_is_sent},
  }, failures);
}
