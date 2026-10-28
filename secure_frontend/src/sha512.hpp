#pragma once
// SP7 M2 Task 1 (spec D-2): in-library self-contained SHA-512 for the Ed25519
// verifier (RFC 8032 hashes with SHA-512). Internal header (src/), like the
// ed25519 verifier beside it: the only consumers are the ed25519 library and
// its tests. Same dependency-free rule as secure_frontend_sha256.
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace secure_frontend {

using Sha512Digest = std::array<std::uint8_t, 64>;

// FIPS 180-4 SHA-512, streaming, portable C++ (no intrinsics, no dependency).
class Sha512 final {
 public:
  Sha512() noexcept;
  void update(std::span<const std::uint8_t> bytes) noexcept;
  void update(std::string_view bytes) noexcept;
  // Pads, returns the digest, and resets to the initial state.
  [[nodiscard]] Sha512Digest finish() noexcept;
  [[nodiscard]] std::uint64_t bytes_hashed() const noexcept { return length_low_bytes_; }

 private:
  void compress(const std::uint8_t* block) noexcept;
  void reset() noexcept;
  std::array<std::uint64_t, 8> state_{};
  std::array<std::uint8_t, 128> buffer_{};
  std::size_t buffered_{0};
  // 128-bit message length in bytes (FIPS 180-4 §4.2 pads with a 128-bit bit count).
  std::uint64_t length_low_bytes_{0};
  std::uint64_t length_high_bytes_{0};
};

[[nodiscard]] Sha512Digest sha512(std::string_view bytes) noexcept;

[[nodiscard]] std::string to_hex(const Sha512Digest& digest);

}  // namespace secure_frontend
