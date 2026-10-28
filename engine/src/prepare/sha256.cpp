#include "sha256.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace strata_prepare {
namespace {

constexpr std::array<std::uint32_t, 64> kK{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

constexpr std::uint32_t rotr(const std::uint32_t x, const unsigned n) { return (x >> n) | (x << (32U - n)); }

}  // namespace

Sha256::Sha256() {
  h_ = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU, 0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
}

void Sha256::block(const unsigned char* p) {
  std::array<std::uint32_t, 64> w{};
  for (std::size_t i = 0; i < 16; ++i) {
    w[i] = (static_cast<std::uint32_t>(p[4 * i]) << 24U) | (static_cast<std::uint32_t>(p[4 * i + 1]) << 16U) |
           (static_cast<std::uint32_t>(p[4 * i + 2]) << 8U) | static_cast<std::uint32_t>(p[4 * i + 3]);
  }
  for (std::size_t i = 16; i < 64; ++i) {
    const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3U);
    const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10U);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
  for (std::size_t i = 0; i < 64; ++i) {
    const std::uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kK[i] + w[i];
    const std::uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  h_[0] += a;
  h_[1] += b;
  h_[2] += c;
  h_[3] += d;
  h_[4] += e;
  h_[5] += f;
  h_[6] += g;
  h_[7] += h;
}

void Sha256::update(const void* data, std::size_t size) {
  const auto* p = static_cast<const unsigned char*>(data);
  total_ += size;
  while (size > 0U) {
    const std::size_t take = std::min<std::size_t>(64U - used_, size);
    std::memcpy(buffer_.data() + used_, p, take);
    used_ += take;
    p += take;
    size -= take;
    if (used_ == 64U) {
      block(buffer_.data());
      used_ = 0;
    }
  }
}

std::string Sha256::hex() {
  const std::uint64_t bits = total_ * 8U;
  const unsigned char one = 0x80U;
  update(&one, 1);
  const unsigned char zero = 0U;
  while (used_ != 56U) update(&zero, 1);
  unsigned char length[8];
  for (std::size_t i = 0; i < 8; ++i) length[i] = static_cast<unsigned char>(bits >> (56U - 8U * i));
  update(length, 8);
  std::string out;
  char hex[9];
  for (const auto word : h_) {
    std::snprintf(hex, sizeof(hex), "%08x", word);
    out += hex;
  }
  return out;
}

}  // namespace strata_prepare
