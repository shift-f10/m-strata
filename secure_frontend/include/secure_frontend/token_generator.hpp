#pragma once

#include "secure_frontend/secret_bytes.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <variant>

namespace secure_frontend {

inline constexpr std::size_t kGeneratedTokenRandomBytes = 32U;
inline constexpr std::size_t kGeneratedTokenCharacters = 43U;

struct TokenGenerationError final {
  std::string message;
};

// Linux: getrandom(buf, n, 0), looping on EINTR and short reads; any other
// failure (ENOSYS included) fails closed -- there is no /dev/urandom fallback.
// Windows: BCryptGenRandom(nullptr, ..., BCRYPT_USE_SYSTEM_PREFERRED_RNG).
[[nodiscard]] bool fill_random(unsigned char* buffer, std::size_t size) noexcept;
// RFC 4648 §5 alphabet, no padding. False if the output would exceed SecretBytes.
[[nodiscard]] bool base64url_encode(std::string_view bytes, SecretBytes& out) noexcept;
// 32 CSPRNG bytes, base64url: 43 ASCII characters, no prefix.
[[nodiscard]] std::variant<SecretBytes, TokenGenerationError> generate_token();

}  // namespace secure_frontend
