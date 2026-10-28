#pragma once

#include "secure_frontend/secret_bytes.hpp"

#include <functional>
#include <memory>
#include <string_view>
#include <utility>

namespace secure_frontend {

// The one place the server's token lives after it is read (CFG-06: never in
// FrontendConfig). Comparison is detail::fixed_window_equals.
class TokenVerifier final {
 public:
  explicit TokenVerifier(SecretBytes token) noexcept : token_(std::move(token)) {}
  [[nodiscard]] bool matches(std::string_view presented) const noexcept;

 private:
  SecretBytes token_;
};

// A BearerTokenValidator for a RequestRouter (ApiRouterConfig::validator) that shares the verifier;
// the token is never copied into the closure.
[[nodiscard]] std::function<bool(std::string_view)> make_bearer_validator(std::shared_ptr<const TokenVerifier> verifier);

}  // namespace secure_frontend
