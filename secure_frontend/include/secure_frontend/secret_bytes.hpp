#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace secure_frontend {

inline constexpr std::size_t kTokenWindowBytes = 256U;

// Overwrites memory in a way the optimizer may not remove.
void secure_wipe(void* data, std::size_t size) noexcept;

// Holds a token or other secret inline (never on the heap, so no allocator
// copy survives), move-only, wiped on destruction and on move-from.
class SecretBytes final {
 public:
  static constexpr std::size_t kCapacity = kTokenWindowBytes;

  SecretBytes() noexcept = default;
  SecretBytes(const SecretBytes&) = delete;
  SecretBytes& operator=(const SecretBytes&) = delete;
  SecretBytes(SecretBytes&& other) noexcept;
  SecretBytes& operator=(SecretBytes&& other) noexcept;
  ~SecretBytes();

  [[nodiscard]] bool assign(std::string_view bytes) noexcept;
  [[nodiscard]] bool append(std::string_view bytes) noexcept;
  void clear() noexcept;
  [[nodiscard]] std::string_view view() const noexcept;
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0U; }
  // Test hook: the inline storage, so a test can prove it holds only zeros.
  [[nodiscard]] const unsigned char* storage_for_tests() const noexcept { return bytes_.data(); }

 private:
  std::array<unsigned char, kCapacity> bytes_{};
  std::size_t size_{0U};
};

namespace detail {
// Copies both values into zero-padded 256-byte windows and XORs all 256
// bytes, folding in the length difference, so the work done is the same for
// every input up to the public maximum. A value longer than 256 bytes is
// rejected before comparing. An empty stored value never matches.
// *bytes_compared (test instrumentation) receives the number of bytes compared.
[[nodiscard]] bool fixed_window_equals(std::string_view presented, std::string_view stored,
                                       std::size_t* bytes_compared) noexcept;
}  // namespace detail

}  // namespace secure_frontend
