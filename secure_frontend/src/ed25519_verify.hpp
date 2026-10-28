#pragma once
// SP7 M2 Task 1 (spec D-2): dependency-free RFC 8032 Ed25519 verification.
// VERIFY PATH ONLY — this library never signs (the plan's binding seam: "No
// signing in C++"). Internal header (src/): consumers are the release
// manifest library (Task 2), the update verb (Task 3) and the tests.
//
// Implemented to RFC 8032 §5.1.7 exactly: point decoding (reject y >= p),
// the canonical S < L check (rejects the S + k*L malleations of §8.4), and
// the verification equation [S]B = R + [k]A checked strictly, as the RFC's
// section-6 reference does - no cofactor multiplication. Unit tests pin the
// §7.1 official vectors plus forged-signature / tampered-message /
// swapped-pubkey / non-canonical-S / bad-encoding negatives.
#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace secure_frontend {

// True iff `signature` is a valid Ed25519 signature by `public_key` over
// `message`. Every parse failure (out-of-range field values, S >= L,
// non-decodable points) returns false; nothing distinguishes parse failures
// from equation failures.
[[nodiscard]] bool ed25519_verify(const std::array<std::uint8_t, 32>& public_key, std::span<const std::uint8_t> message,
                                  const std::array<std::uint8_t, 64>& signature) noexcept;

[[nodiscard]] inline bool ed25519_verify(const std::array<std::uint8_t, 32>& public_key, std::string_view message,
                                         const std::array<std::uint8_t, 64>& signature) noexcept {
  return ed25519_verify(public_key,
                        std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(message.data()), message.size()},
                        signature);
}

}  // namespace secure_frontend
