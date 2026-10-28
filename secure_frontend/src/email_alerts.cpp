#include "secure_frontend/email_alerts.hpp"

#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/smtp_tls.hpp"
#include "secure_frontend/token_generator.hpp"
#include "secure_frontend/token_store.hpp"

#include <algorithm>
#include <ctime>
#include <iostream>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#include <dpapi.h>
#endif

namespace secure_frontend {
namespace {

#if defined(_WIN32)
constexpr std::string_view kDpapiMagic = "STRATA-SMTP-DPAPI1\n";
#endif
constexpr std::size_t kMaxPasswordBytes = 256;
constexpr std::size_t kMaxQueued = 16;

bool password_ok(const std::string_view password) {
  return !password.empty() && password.size() <= kMaxPasswordBytes &&
         std::none_of(password.begin(), password.end(), [](const char c) { return c == '\r' || c == '\n' || c == '\0'; });
}

std::string message_id() {
  auto random = generate_token();
  std::string id = "<";
  if (const auto* bytes = std::get_if<SecretBytes>(&random)) {
    static constexpr char kHex[] = "0123456789abcdef";
    for (const char c : bytes->view().substr(0, 12)) {
      id.push_back(kHex[static_cast<unsigned char>(c) >> 4U]);
      id.push_back(kHex[static_cast<unsigned char>(c) & 15U]);
    }
  }
  return id + "@strata.local>";
}

}  // namespace

std::optional<std::string> store_smtp_password(const std::filesystem::path& secret_dir, const std::string_view password) {
  if (!password_ok(password)) return std::string{"the password must be 1-256 characters without line breaks"};
  auto root = open_root(secret_dir, RootPolicy::existing);
  const auto file = parse_relative_path(kSmtpPasswordFile);
  if (!std::holds_alternative<CanonicalRoot>(root) || !std::holds_alternative<RelativePath>(file)) {
    return std::string{"the server's secret folder could not be opened"};
  }
  std::string content;
#if defined(_WIN32)
  DATA_BLOB input{static_cast<DWORD>(password.size()), reinterpret_cast<BYTE*>(const_cast<char*>(password.data()))};
  DATA_BLOB output{};
  if (CryptProtectData(&input, L"strata smtp password", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output) == 0) {
    return std::string{"the password could not be protected"};
  }
  content.assign(kDpapiMagic);
  content.append(reinterpret_cast<const char*>(output.pbData), output.cbData);
  SecureZeroMemory(output.pbData, output.cbData);
  LocalFree(output.pbData);
#else
  content.assign(password);
#endif
  const auto failure = write_private_file(std::get<CanonicalRoot>(root), std::get<RelativePath>(file), content, TokenStoreMode::replace);
  std::fill(content.begin(), content.end(), '\0');
  if (failure) return std::string{"the password could not be written to the secret folder"};
  return std::nullopt;
}

std::optional<SecretBytes> read_smtp_password(const std::filesystem::path& secret_dir) {
  auto root = open_root(secret_dir, RootPolicy::existing);
  const auto file = parse_relative_path(kSmtpPasswordFile);
  if (!std::holds_alternative<CanonicalRoot>(root) || !std::holds_alternative<RelativePath>(file)) return std::nullopt;
  auto opened = open_beneath(std::get<CanonicalRoot>(root), std::get<RelativePath>(file));
  if (!std::holds_alternative<OpenedFile>(opened)) return std::nullopt;
  auto read = std::get<OpenedFile>(opened).read_all(4096);
  if (!std::holds_alternative<std::string>(read)) return std::nullopt;
  std::string raw = std::get<std::string>(std::move(read));
  SecretBytes password;
  bool ok = false;
#if defined(_WIN32)
  if (raw.rfind(kDpapiMagic, 0) == 0U) {
    DATA_BLOB input{static_cast<DWORD>(raw.size() - kDpapiMagic.size()),
                    reinterpret_cast<BYTE*>(raw.data() + kDpapiMagic.size())};
    DATA_BLOB output{};
    if (CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output) != 0) {
      const std::string_view plain{reinterpret_cast<const char*>(output.pbData), output.cbData};
      ok = password_ok(plain) && password.assign(plain);
      SecureZeroMemory(output.pbData, output.cbData);
      LocalFree(output.pbData);
    }
  }
#else
  ok = password_ok(raw) && password.assign(raw);
#endif
  std::fill(raw.begin(), raw.end(), '\0');
  if (!ok) return std::nullopt;
  return password;
}

std::string mail_date_now() {
  const std::time_t now = std::time(nullptr);
  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &now);
#else
  gmtime_r(&now, &utc);
#endif
  static constexpr const char* kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static constexpr const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%s, %02d %s %04d %02d:%02d:%02d +0000", kDays[utc.tm_wday % 7], utc.tm_mday,
                kMonths[utc.tm_mon % 12], utc.tm_year + 1900, utc.tm_hour, utc.tm_min, utc.tm_sec);
  return buffer;
}

EmailAlerts::EmailAlerts(AlertSettings settings) : settings_(std::move(settings)) {
  if (!settings_.connect) {
    const EmailAlertsConfig config = settings_.config;
    settings_.connect = [config](std::string* error) {
      return connect_smtp(config.host, config.port, config.security, std::chrono::seconds(30), error);
    };
  }
  worker_ = std::thread([this] { run(); });
}

EmailAlerts::~EmailAlerts() { shutdown(); }

void EmailAlerts::log(const std::string_view line) const {
  if (settings_.log) {
    settings_.log(line);
    return;
  }
  std::cerr << line << '\n' << std::flush;
}

bool EmailAlerts::raise(const std::string_view event, const std::string& subject, const std::string& body) {
  const auto& config = settings_.config;
  if (!config.enabled || std::find(config.events.begin(), config.events.end(), event) == config.events.end()) return false;
  const auto now = std::chrono::steady_clock::now();
  {
    std::lock_guard lock(mutex_);
    if (stopping_ || queue_.size() >= kMaxQueued) return false;
    if (const auto last = last_sent_.find(event); last != last_sent_.end() && now - last->second < settings_.per_kind) {
      log("[strata] alerts: " + std::string{event} + " not sent (one of its kind went out within a minute)");
      return false;
    }
    last_sent_[std::string{event}] = now;
    queue_.push_back(Alert{subject, body});
  }
  changed_.notify_all();
  return true;
}

SmtpResult EmailAlerts::send(const Alert& alert) {
  const auto& config = settings_.config;
  auto password = read_smtp_password(settings_.secret_dir);
  if (!password) return SmtpResult{false, "password", 0, "no SMTP password is set (the page's alert settings)"};
  std::string error;
  auto transport = settings_.connect(&error);
  if (transport == nullptr) return SmtpResult{false, "connect", 0, error};
  const SmtpMessage message{config.from, config.to, "Strata on " + settings_.server_name + ": " + alert.subject,
                            alert.body + "\n\n-- \nSent by the Strata server on " + settings_.server_name + "."};
  return send_mail(*transport, config.security, "strata.local", config.username, password->view(), message, mail_date_now(),
                   message_id());
}

SmtpResult EmailAlerts::send_test() {
  if (settings_.config.host.empty() || settings_.config.to.empty()) {
    return SmtpResult{false, "settings", 0, "the mail server and at least one address are needed"};
  }
  return send(Alert{"test email", "This is a test email from Strata. Its alerts will look like this."});
}

void EmailAlerts::run() {
  for (;;) {
    Alert alert;
    {
      std::unique_lock lock(mutex_);
      changed_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (stopping_) return;
      alert = std::move(queue_.front());
      queue_.pop_front();
      sending_ = true;
    }
    const auto result = send(alert);
    log(result.sent ? "[strata] alerts: sent \"" + alert.subject + "\""
                    : "[strata] alerts: \"" + alert.subject + "\" not sent (" + result.stage + ": " + result.reply + ")");
    {
      std::lock_guard lock(mutex_);
      sending_ = false;
    }
    changed_.notify_all();
  }
}

bool EmailAlerts::wait_idle(const std::chrono::milliseconds timeout) {
  std::unique_lock lock(mutex_);
  return changed_.wait_for(lock, timeout, [this] { return queue_.empty() && !sending_; });
}

void EmailAlerts::shutdown() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  changed_.notify_all();
  if (worker_.joinable()) worker_.join();
}

}  // namespace secure_frontend
