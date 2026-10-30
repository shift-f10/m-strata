#pragma once

// A scripted SMTP server for the mail tests (smtp_client_test, email_notifier_test): it answers each command line with
// the next scripted reply (one entry may hold several lines) and keeps what it was sent; Forward hands a connection to
// one the test keeps, so its record outlives the connection.

#include "secure_frontend/smtp_client.hpp"

#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace test_support {

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

class Forward final : public secure_frontend::SmtpTransport {
 public:
  explicit Forward(std::shared_ptr<ScriptedServer> server) : server_(std::move(server)) {}
  bool write(const std::string_view bytes) override { return server_->write(bytes); }
  std::optional<std::string> read_line() override { return server_->read_line(); }
  bool start_tls() override { return server_->start_tls(); }

 private:
  std::shared_ptr<ScriptedServer> server_;
};

// A whole good session over implicit TLS: greeting, EHLO, AUTH, MAIL, RCPT, DATA, the message, QUIT.
inline std::deque<std::string> a_good_session() {
  return {"220 hi", "250-host\n250 AUTH PLAIN", "235 ok", "250 ok", "250 ok", "354 go", "250 queued", "221 bye"};
}

}  // namespace test_support
