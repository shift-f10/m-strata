#pragma once
// strata-prepare: SHA-256 (FIPS 180-4) for the hashes upstream's pack tools record (a tensor's source bytes in
// conversions.json, native_experts.txt in experts.bin.src.json), as lowercase hex.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace strata_prepare {

class Sha256 {
 public:
  Sha256();
  void update(const void* data, std::size_t size);
  [[nodiscard]] std::string hex();   // finishes the hash

 private:
  void block(const unsigned char* p);
  std::array<std::uint32_t, 8> h_{};
  std::array<unsigned char, 64> buffer_{};
  std::size_t used_ = 0;
  std::uint64_t total_ = 0;
};

}  // namespace strata_prepare
