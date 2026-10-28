#include "secure_frontend/smtp_client.hpp"

#include <algorithm>
#include <cctype>

namespace secure_frontend {
namespace {

// A header value on one line: CR and LF (and every other control) become spaces.
std::string one_line(const std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) out.push_back(static_cast<unsigned char>(c) < 0x20U || c == 0x7f ? ' ' : c);
  return out;
}

std::string printable(const std::string_view text) {
  std::string out;
  for (const char c : text.substr(0, 200)) out.push_back(c >= 0x20 && c < 0x7f ? c : '?');
  return out;
}

struct Reply final {
  int code{0};
  std::string last;                 // the last line, printable
  std::vector<std::string> lines;   // every line's text after the code and its separator
};

// One reply: lines "ddd-..." continue it, "ddd ..." (or "ddd") ends it.
std::optional<Reply> read_reply(SmtpTransport& transport) {
  Reply reply;
  for (int lines = 0; lines < 100; ++lines) {
    const auto line = transport.read_line();
    if (!line || line->size() < 3U || !std::isdigit(static_cast<unsigned char>((*line)[0])) ||
        !std::isdigit(static_cast<unsigned char>((*line)[1])) || !std::isdigit(static_cast<unsigned char>((*line)[2]))) {
      return std::nullopt;
    }
    const int code = std::stoi(line->substr(0, 3));
    if (reply.code != 0 && code != reply.code) return std::nullopt;
    reply.code = code;
    reply.last = printable(*line);
    reply.lines.push_back(line->size() > 4U ? line->substr(4) : std::string{});
    if (line->size() == 3U || (*line)[3] == ' ') return reply;
    if ((*line)[3] != '-') return std::nullopt;
  }
  return std::nullopt;
}

bool has_extension(const Reply& ehlo, const std::string_view name, const std::string_view parameter = {}) {
  for (const auto& line : ehlo.lines) {
    std::string upper;
    for (const char c : line) upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    if (upper.rfind(name, 0) != 0U) continue;
    if (parameter.empty()) return upper.size() == name.size() || upper[name.size()] == ' ';
    if (upper.find(std::string{" "} + std::string{parameter}) != std::string::npos) return true;
  }
  return false;
}

}  // namespace

std::string base64_encode(const std::string_view bytes) {
  static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((bytes.size() + 2U) / 3U * 4U);
  for (std::size_t i = 0; i < bytes.size(); i += 3U) {
    const auto b0 = static_cast<unsigned char>(bytes[i]);
    const auto b1 = i + 1U < bytes.size() ? static_cast<unsigned char>(bytes[i + 1U]) : 0U;
    const auto b2 = i + 2U < bytes.size() ? static_cast<unsigned char>(bytes[i + 2U]) : 0U;
    out.push_back(kAlphabet[b0 >> 2U]);
    out.push_back(kAlphabet[((b0 & 0x03U) << 4U) | (b1 >> 4U)]);
    out.push_back(i + 1U < bytes.size() ? kAlphabet[((b1 & 0x0fU) << 2U) | (b2 >> 6U)] : '=');
    out.push_back(i + 2U < bytes.size() ? kAlphabet[b2 & 0x3fU] : '=');
  }
  return out;
}

std::string render_message(const SmtpMessage& message, const std::string_view date, const std::string_view message_id) {
  std::string to;
  for (std::size_t i = 0; i < message.to.size(); ++i) to += (i == 0U ? "" : ", ") + one_line(message.to[i]);
  std::string out = "From: " + one_line(message.from) + "\r\nTo: " + to + "\r\nSubject: " + one_line(message.subject) +
                    "\r\nDate: " + one_line(date) + "\r\nMessage-ID: " + one_line(message_id) +
                    "\r\nMIME-Version: 1.0\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Transfer-Encoding: 8bit\r\n\r\n";
  // the body: CRLF line ends, a line starting with '.' gets one more (RFC 5321 4.5.2)
  std::size_t start = 0;
  while (start <= message.body.size()) {
    auto end = message.body.find('\n', start);
    std::string line = message.body.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty() && line.front() == '.') line.insert(line.begin(), '.');
    out += line + "\r\n";
    if (end == std::string::npos) break;
    start = end + 1U;
  }
  return out;
}

SmtpResult send_mail(SmtpTransport& transport, const SmtpSecurity security, const std::string_view client_name,
                     const std::string_view username, const std::string_view password, const SmtpMessage& message,
                     const std::string_view date, const std::string_view message_id) {
  SmtpResult result;
  // One command and its reply; false (the result filled) unless the reply's code is one of `ok`.
  const auto step = [&](const std::string_view stage, const std::string& line, std::initializer_list<int> ok) -> std::optional<Reply> {
    result.stage = std::string{stage};
    if (!line.empty() && !transport.write(line + "\r\n")) return std::nullopt;
    auto reply = read_reply(transport);
    if (!reply) return std::nullopt;
    result.code = reply->code;
    result.reply = reply->last;
    if (std::find(ok.begin(), ok.end(), reply->code) == ok.end()) return std::nullopt;
    return reply;
  };
  if (!step("greeting", "", {220})) return result;
  auto ehlo = step("ehlo", "EHLO " + std::string{client_name}, {250});
  if (!ehlo) return result;
  if (security == SmtpSecurity::starttls) {
    result.stage = "starttls";
    if (!has_extension(*ehlo, "STARTTLS")) {
      result.reply = "the server offers no STARTTLS; nothing was sent in the clear";
      (void)transport.write("QUIT\r\n");
      return result;
    }
    if (!step("starttls", "STARTTLS", {220})) return result;
    result.stage = "tls";
    if (!transport.start_tls()) {
      result.reply = "the TLS handshake failed (the certificate, its host name or TLS 1.2)";
      return result;
    }
    ehlo = step("ehlo", "EHLO " + std::string{client_name}, {250});
    if (!ehlo) return result;
  }
  if (has_extension(*ehlo, "AUTH", "PLAIN")) {
    std::string plain;
    plain.push_back('\0');
    plain.append(username);
    plain.push_back('\0');
    plain.append(password);
    if (!step("auth", "AUTH PLAIN " + base64_encode(plain), {235})) return result;
  } else {
    if (!step("auth", "AUTH LOGIN", {334}) || !step("auth", base64_encode(username), {334}) ||
        !step("auth", base64_encode(password), {235})) {
      return result;
    }
  }
  if (!step("mail", "MAIL FROM:<" + one_line(message.from) + ">", {250})) return result;
  for (const auto& to : message.to) {
    if (!step("rcpt", "RCPT TO:<" + one_line(to) + ">", {250, 251})) return result;
  }
  if (!step("data", "DATA", {354})) return result;
  if (!step("message", render_message(message, date, message_id) + ".", {250})) return result;
  result.sent = true;
  result.stage = "done";
  (void)transport.write("QUIT\r\n");
  (void)read_reply(transport);
  return result;
}

}  // namespace secure_frontend
