#include "secure_frontend/email_notifier.hpp"

#include "secure_frontend/json.hpp"
#include "secure_frontend/token_generator.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <ctime>
#include <set>
#include <utility>
#include <variant>

namespace secure_frontend {
namespace {

constexpr JsonLimits kLineLimits{.max_bytes = 64U * 1024U, .max_depth = 2U, .max_container_entries = 8U, .max_string_bytes = 16U * 1024U};
constexpr std::size_t kMaxRecipients = 5U;

bool printable(const std::string_view text) {
  return !text.empty() && text.size() <= 254U && std::all_of(text.begin(), text.end(), [](const char c) { return c > ' ' && c < 0x7f; });
}

bool dns_name(const std::string_view host) {
  return printable(host) && host.size() <= 253U && host.front() != '-' && host.front() != '.' && host.back() != '.' &&
         host.find("..") == std::string_view::npos && std::all_of(host.begin(), host.end(), [](const char c) {
           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
         });
}

const JsonValue* member(const JsonValue& object, const std::string_view key) {
  const auto* members = object.if_object();
  if (members == nullptr) return nullptr;
  for (const auto& entry : *members) {
    if (entry.key == key) return &entry.value;
  }
  return nullptr;
}

std::string text_of(const JsonValue* value) {
  return value != nullptr && value->if_string() != nullptr ? *value->if_string() : std::string{};
}

// The server's name as a line names it: printable ASCII, at most 64 ("this PC" when it names none).
std::string server_name_of(const JsonValue& line) {
  std::string name = text_of(member(line, "server")).substr(0, 64U);
  std::replace_if(name.begin(), name.end(), [](const char c) { return c < ' ' || c >= 0x7f; }, '?');
  return name.empty() ? std::string{"this PC"} : name;
}

std::string answer(const std::optional<std::string>& event, const bool ok, const std::string_view reply) {
  std::string out = "{";
  if (event) {
    out += "\"event\":";
    append_json_string(&out, *event);
    out += ",";
  }
  out += std::string{"\"ok\":"} + (ok ? "true" : "false") + ",\"reply\":";
  append_json_string(&out, reply);
  return out + "}";
}

std::string reply_of(const SmtpResult& result) { return result.sent ? "sent (" + result.reply + ")" : result.stage + ": " + result.reply; }

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

EmailNotifierSettings parse_email_arguments(const std::vector<std::string>& arguments) {
  EmailNotifierSettings settings;
  const auto fail = [&settings](std::string why) {
    if (settings.problem.empty()) settings.problem = std::move(why);
  };
  std::set<std::string, std::less<>> given;
  for (std::size_t i = 0; i < arguments.size(); ++i) {
    const std::string& flag = arguments[i];
    if (i + 1U >= arguments.size()) {
      fail(flag + " needs a value");
      break;
    }
    const std::string& value = arguments[++i];
    given.insert(flag);
    if (flag == "--host") {
      if (dns_name(value)) settings.host = value;
      else fail("host must be the mail server's DNS name (letters, digits, dots and dashes)");
    } else if (flag == "--port") {
      unsigned port = 0;
      const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), port);
      if (error == std::errc{} && end == value.data() + value.size() && port >= 1U && port <= 65535U) settings.port = static_cast<std::uint16_t>(port);
      else fail("port must be 1-65535");
    } else if (flag == "--security") {
      if (value == "ssl") settings.security = SmtpSecurity::ssl;
      else if (value == "starttls") settings.security = SmtpSecurity::starttls;
      else fail("security must be ssl or starttls");
    } else if (flag == "--username") {
      if (printable(value)) settings.username = value;
      else fail("username must be 1-254 printable characters without spaces");
    } else if (flag == "--from") {
      if (is_valid_email_address(value)) settings.from = value;
      else fail("from must be an email address");
    } else if (flag == "--to") {
      std::vector<std::string> to;
      bool ok = true;
      for (std::size_t start = 0; start <= value.size();) {
        const auto comma = value.find(',', start);
        const std::string one = value.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        ok = ok && is_valid_email_address(one);
        to.push_back(one);
        if (comma == std::string::npos) break;
        start = comma + 1U;
      }
      if (ok && !to.empty() && to.size() <= kMaxRecipients) settings.to = std::move(to);
      else fail("to must be 1-5 email addresses, comma-separated");
    } else {
      fail("unknown argument " + flag);
    }
  }
  for (const std::string_view needed : {"--host", "--username", "--from", "--to"}) {
    if (!given.contains(needed)) fail(std::string{needed.substr(2U)} + " is needed");
  }
  return settings;
}

EmailNotifier::EmailNotifier(EmailNotifierSettings settings, SecretBytes password, Connect connect, const std::chrono::seconds per_kind)
    : settings_(std::move(settings)), password_(std::move(password)), connect_(std::move(connect)), per_kind_(per_kind) {}

SmtpResult EmailNotifier::send(const std::string& server, const std::string& subject, const std::string& body) {
  if (!settings_.problem.empty()) return SmtpResult{false, "settings", 0, settings_.problem};
  if (password_.empty()) return SmtpResult{false, "password", 0, "no SMTP password is set (the email plugin's password setting)"};
  std::string error;
  auto transport = connect_ ? connect_(settings_, &error) : nullptr;
  if (transport == nullptr) return SmtpResult{false, "connect", 0, error.empty() ? std::string{"no connection"} : error};
  const SmtpMessage message{settings_.from, settings_.to, "Strata on " + server + ": " + subject,
                            body + "\n\n-- \nSent by the Strata server on " + server + "."};
  return send_mail(*transport, settings_.security, "strata.local", settings_.username, password_.view(), message, mail_date_now(),
                   message_id());
}

std::optional<std::string> EmailNotifier::handle(const std::string_view line) {
  const auto parsed = parse_json(line, kLineLimits);
  const auto* root = std::get_if<JsonValue>(&parsed);
  if (root == nullptr || root->if_object() == nullptr) return std::nullopt;
  const std::string server = server_name_of(*root);
  if (const auto* test = member(*root, "test"); test != nullptr && test->if_boolean() != nullptr && *test->if_boolean()) {
    const auto result = send(server, "test email", "This is a test email from Strata. Its alerts will look like this.");
    return answer(std::nullopt, result.sent, reply_of(result));
  }
  const auto* event = member(*root, "event");
  if (event == nullptr || event->if_string() == nullptr) return std::nullopt;
  const std::string name = event->if_string()->substr(0, 64U);
  if (std::find(kAlertEvents.begin(), kAlertEvents.end(), name) == kAlertEvents.end()) {
    return answer(name, false, "not sent: not an event the email plugin knows");
  }
  const auto now = std::chrono::steady_clock::now();
  if (const auto last = last_sent_.find(name); last != last_sent_.end() && now - last->second < per_kind_) {
    return answer(name, false, "not sent: one of its kind went out within a minute");
  }
  if (settings_.problem.empty() && !password_.empty()) last_sent_[name] = now;
  const auto result = send(server, text_of(member(*root, "subject")).substr(0, 200U), text_of(member(*root, "body")).substr(0, 8192U));
  return answer(name, result.sent, reply_of(result));
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

}  // namespace secure_frontend
