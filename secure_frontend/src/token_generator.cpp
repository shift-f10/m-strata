#include "secure_frontend/token_generator.hpp"

#include <array>
#include <cerrno>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#include <bcrypt.h>
#else
#include <sys/random.h>
#endif

namespace secure_frontend {

bool fill_random(unsigned char* const buffer, const std::size_t size) noexcept {
#if defined(_WIN32)
  return BCryptGenRandom(nullptr, buffer, static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
#else
  std::size_t filled = 0U;
  while (filled < size) {
    const ssize_t count = ::getrandom(buffer + filled, size - filled, 0);
    if (count < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    filled += static_cast<std::size_t>(count);
  }
  return true;
#endif
}

bool base64url_encode(const std::string_view bytes, SecretBytes& out) noexcept {
  static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  out.clear();
  std::array<char, 4> quad{};
  const auto at = [&bytes](const std::size_t i) { return static_cast<unsigned>(static_cast<unsigned char>(bytes[i])); };
  std::size_t i = 0U;
  bool ok = true;
  for (; ok && i + 3U <= bytes.size(); i += 3U) {
    const unsigned v = (at(i) << 16U) | (at(i + 1U) << 8U) | at(i + 2U);
    quad = {kAlphabet[(v >> 18U) & 63U], kAlphabet[(v >> 12U) & 63U], kAlphabet[(v >> 6U) & 63U], kAlphabet[v & 63U]};
    ok = out.append(std::string_view{quad.data(), 4U});
  }
  const std::size_t rest = bytes.size() - i;
  if (ok && rest == 1U) {
    const unsigned v = at(i) << 16U;
    quad = {kAlphabet[(v >> 18U) & 63U], kAlphabet[(v >> 12U) & 63U], 0, 0};
    ok = out.append(std::string_view{quad.data(), 2U});
  } else if (ok && rest == 2U) {
    const unsigned v = (at(i) << 16U) | (at(i + 1U) << 8U);
    quad = {kAlphabet[(v >> 18U) & 63U], kAlphabet[(v >> 12U) & 63U], kAlphabet[(v >> 6U) & 63U], 0};
    ok = out.append(std::string_view{quad.data(), 3U});
  }
  secure_wipe(quad.data(), quad.size());
  if (!ok) out.clear();
  return ok;
}

std::variant<SecretBytes, TokenGenerationError> generate_token() {
  std::array<unsigned char, kGeneratedTokenRandomBytes> raw{};
  const bool filled = fill_random(raw.data(), raw.size());
  SecretBytes token;
  const bool encoded =
      filled && base64url_encode(std::string_view{reinterpret_cast<const char*>(raw.data()), raw.size()}, token);
  secure_wipe(raw.data(), raw.size());
  if (!encoded) return TokenGenerationError{"the operating system random number generator is unavailable"};
  return token;
}

}  // namespace secure_frontend
