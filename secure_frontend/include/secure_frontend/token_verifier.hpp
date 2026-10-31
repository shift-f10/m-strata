#pragma once

#include "secure_frontend/secret_bytes.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
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

// The owner's external client (2026-10-31, gap L4: "a serving credential can't stop models or rotate keys"): the
// inference-only key the server accepts beside its own (ApiRouterConfig::inference_validator), held here so that a new
// one, or none, applies to the next request at once. Thread-safe; compared as TokenVerifier compares.
class InferenceKey final {
 public:
  void hold(std::optional<SecretBytes> key);
  [[nodiscard]] bool matches(std::string_view presented) const;
  [[nodiscard]] bool held() const;

 private:
  mutable std::mutex mutex_;
  std::shared_ptr<const TokenVerifier> verifier_;
};

}  // namespace secure_frontend
