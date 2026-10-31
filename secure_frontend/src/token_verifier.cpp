#include "secure_frontend/token_verifier.hpp"

namespace secure_frontend {

bool TokenVerifier::matches(const std::string_view presented) const noexcept {
  return detail::fixed_window_equals(presented, token_.view(), nullptr);
}

std::function<bool(std::string_view)> make_bearer_validator(std::shared_ptr<const TokenVerifier> verifier) {
  return [verifier = std::move(verifier)](const std::string_view presented) {
    return verifier != nullptr && verifier->matches(presented);
  };
}

void InferenceKey::hold(std::optional<SecretBytes> key) {
  auto verifier = key.has_value() ? std::make_shared<const TokenVerifier>(std::move(*key)) : nullptr;
  std::lock_guard lock(mutex_);
  verifier_ = std::move(verifier);
}

bool InferenceKey::matches(const std::string_view presented) const {
  std::shared_ptr<const TokenVerifier> verifier;
  {
    std::lock_guard lock(mutex_);
    verifier = verifier_;
  }
  return verifier != nullptr && verifier->matches(presented);
}

bool InferenceKey::held() const {
  std::lock_guard lock(mutex_);
  return verifier_ != nullptr;
}

}  // namespace secure_frontend
