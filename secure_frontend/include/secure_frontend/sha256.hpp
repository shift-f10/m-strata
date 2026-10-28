#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace secure_frontend {

using Sha256Digest = std::array<std::uint8_t, 32>;

// Files are hashed in reads of this size (spec §4.2).
inline constexpr std::size_t kSha256ReadChunkBytes = std::size_t{8} * 1024U * 1024U;

// FIPS 180-4 SHA-256, streaming, portable C++ (no intrinsics, no dependency).
class Sha256 final {
 public:
  Sha256() noexcept;
  void update(std::span<const std::uint8_t> bytes) noexcept;
  void update(std::string_view bytes) noexcept;
  // Pads, returns the digest, and resets to the initial state.
  [[nodiscard]] Sha256Digest finish() noexcept;
  [[nodiscard]] std::uint64_t bytes_hashed() const noexcept { return length_bytes_; }

 private:
  friend void sha256_add_length_for_test(Sha256& hasher, std::uint64_t bytes) noexcept;
  void compress(const std::uint8_t* block) noexcept;
  void reset() noexcept;
  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buffered_{0};
  std::uint64_t length_bytes_{0};
};

// The 8-byte big-endian bit count that ends the padding (FIPS 180-4 §5.1.1).
// Valid for messages below 2^61 bytes.
[[nodiscard]] std::array<std::uint8_t, 8> sha256_length_field(std::uint64_t message_bytes) noexcept;

[[nodiscard]] Sha256Digest sha256(std::string_view bytes) noexcept;
// 64 lowercase hex characters.
[[nodiscard]] std::string to_hex(const Sha256Digest& digest);
// Exactly 64 characters of [0-9a-f].
[[nodiscard]] bool is_lower_hex_digest(std::string_view text) noexcept;

#if defined(SECURE_FRONTEND_TEST_HOOKS)
// Spec §10.2 length-only hook: advances the length counter by `bytes` without
// hashing anything. Only at a block boundary and for whole blocks; otherwise a no-op.
void sha256_add_length_for_test(Sha256& hasher, std::uint64_t bytes) noexcept;
#endif

}  // namespace secure_frontend
