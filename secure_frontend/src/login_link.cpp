#include "secure_frontend/login_link.hpp"

#include "secure_frontend/secret_bytes.hpp"
#include "secure_frontend/token_generator.hpp"

#include <cstddef>
#include <variant>

namespace secure_frontend {

std::optional<std::string> LoginLinks::issue(const Clock::time_point now) {
  auto generated = generate_token();   // 32 random bytes from the system's source, base64url (43 characters)
  auto* bytes = std::get_if<SecretBytes>(&generated);
  if (bytes == nullptr) return std::nullopt;
  std::string code{bytes->view()};
  const std::scoped_lock lock(mutex_);
  code_ = code;
  expires_ = now + kLifetime;
  wrong_ = 0;
  live_ = true;
  return code;
}

bool LoginLinks::claim(const std::string_view code, const Clock::time_point now) {
  const std::scoped_lock lock(mutex_);
  if (!live_) return false;
  if (now >= expires_) {
    live_ = false;
    return false;
  }
  // Constant time over the stored code's length: every byte is compared whatever the first difference.
  unsigned difference = code.size() == code_.size() ? 0U : 1U;
  for (std::size_t i = 0; i < code_.size(); ++i) {
    const unsigned char presented = i < code.size() ? static_cast<unsigned char>(code[i]) : 0U;
    difference |= presented ^ static_cast<unsigned char>(code_[i]);
  }
  if (difference != 0U) {
    if (++wrong_ >= kMaximumAttempts) live_ = false;
    return false;
  }
  live_ = false;   // used once
  return true;
}

std::string login_link_url(const std::string_view authority, const std::string_view code) {
  return "http://" + std::string{authority} + "/#login=" + std::string{code};
}

}  // namespace secure_frontend
