#include "secure_frontend/secret_bytes.hpp"

#include <array>
#include <cstring>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#else
#include <string.h>
#endif

namespace secure_frontend {

void secure_wipe(void* const data, const std::size_t size) noexcept {
  if (data == nullptr || size == 0U) return;
#if defined(_WIN32)
  SecureZeroMemory(data, size);
#elif defined(__GLIBC__)
  explicit_bzero(data, size);
#else
  volatile unsigned char* bytes = static_cast<volatile unsigned char*>(data);
  for (std::size_t index = 0; index < size; ++index) bytes[index] = 0U;
#endif
}

SecretBytes::SecretBytes(SecretBytes&& other) noexcept : size_(other.size_) {
  std::memcpy(bytes_.data(), other.bytes_.data(), other.size_);
  other.clear();
}

SecretBytes& SecretBytes::operator=(SecretBytes&& other) noexcept {
  if (this != &other) {
    clear();
    std::memcpy(bytes_.data(), other.bytes_.data(), other.size_);
    size_ = other.size_;
    other.clear();
  }
  return *this;
}

SecretBytes::~SecretBytes() { clear(); }

bool SecretBytes::assign(const std::string_view bytes) noexcept {
  clear();
  return append(bytes);
}

bool SecretBytes::append(const std::string_view bytes) noexcept {
  if (bytes.size() > kCapacity - size_) return false;
  if (!bytes.empty()) std::memcpy(bytes_.data() + size_, bytes.data(), bytes.size());
  size_ += bytes.size();
  return true;
}

void SecretBytes::clear() noexcept {
  secure_wipe(bytes_.data(), bytes_.size());
  size_ = 0U;
}

std::string_view SecretBytes::view() const noexcept {
  return std::string_view{reinterpret_cast<const char*>(bytes_.data()), size_};
}

namespace detail {
bool fixed_window_equals(const std::string_view presented, const std::string_view stored,
                         std::size_t* const bytes_compared) noexcept {
  if (bytes_compared != nullptr) *bytes_compared = 0U;
  if (presented.size() > kTokenWindowBytes || stored.size() > kTokenWindowBytes) return false;
  std::array<unsigned char, kTokenWindowBytes> left{};
  std::array<unsigned char, kTokenWindowBytes> right{};
  if (!presented.empty()) std::memcpy(left.data(), presented.data(), presented.size());
  if (!stored.empty()) std::memcpy(right.data(), stored.data(), stored.size());
  std::size_t difference = presented.size() ^ stored.size();
  std::size_t compared = 0U;
  for (std::size_t index = 0U; index < kTokenWindowBytes; ++index) {
    difference |= static_cast<std::size_t>(left[index] ^ right[index]);
    ++compared;
  }
  secure_wipe(left.data(), left.size());
  secure_wipe(right.data(), right.size());
  if (bytes_compared != nullptr) *bytes_compared = compared;
  return difference == 0U && !stored.empty();
}
}  // namespace detail

}  // namespace secure_frontend
