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

}  // namespace secure_frontend
