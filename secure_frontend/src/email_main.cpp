// strata-email (docs/superpowers/specs/2026-10-30-engine-family-plugins-design.md, section 10): the email plugin's
// program, a notifier. Built only with SECURE_FRONTEND_EMAIL_ALERTS.
//
//   strata-email --host <dns name> --port <1-65535> --security ssl|starttls --username <name> --from <address>
//                --to <address>[,<address>...]        (SMTP_PASSWORD: the mailbox's password)
//
// stdin: the notifier host's lines, one JSON object each; stdout: one answer line for each line it acts on (email_notifier.hpp);
// stderr: nothing of a mail, a password or an address.
#include "secure_frontend/crt_environment.hpp"
#include "secure_frontend/email_notifier.hpp"
#include "secure_frontend/smtp_tls.hpp"

#include <iostream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace {

constexpr std::size_t kMaxLineBytes = 64U * 1024U;

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
  (void)_setmode(_fileno(stdin), _O_BINARY);
  (void)_setmode(_fileno(stdout), _O_BINARY);
#endif
  const std::vector<std::string> arguments(argv + 1, argv + argc);
  auto settings = secure_frontend::parse_email_arguments(arguments);
  secure_frontend::SecretBytes password;
  if (const char* given = secure_frontend::crt_getenv("SMTP_PASSWORD"); given != nullptr && *given != '\0') (void)password.assign(given);
  secure_frontend::EmailNotifier notifier(
      std::move(settings), std::move(password), [](const secure_frontend::EmailNotifierSettings& to, std::string* error) {
        return secure_frontend::connect_smtp(to.host, to.port, to.security, std::chrono::seconds(30), error);
      });
  std::string line;
  while (std::getline(std::cin, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.size() > kMaxLineBytes) continue;  // (never a line of the host's)
    if (const auto answer = notifier.handle(line)) std::cout << *answer << '\n' << std::flush;
  }
  return 0;
}
