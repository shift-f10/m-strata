// strata-unified-engine: the GGUF block layouts the CPU i-quant dots read (ggml-common.h; internal to
// u_cpu_iq.cpp / u_cpu_iq_avx2.cpp).
#pragma once

#include "u_cpu_iq.hpp"

#include <cstdint>

namespace u::cpu::detail {

constexpr int kQK = 256;

struct BlockIq2s {
  std::uint16_t d;
  std::uint8_t qs[kQK / 4];
  std::uint8_t qh[kQK / 32];
  std::uint8_t scales[kQK / 32];
};
struct BlockIq3xxs {
  std::uint16_t d;
  std::uint8_t qs[3 * kQK / 8];
};
struct BlockIq3s {
  std::uint16_t d;
  std::uint8_t qs[kQK / 4];
  std::uint8_t qh[kQK / 32];
  std::uint8_t signs[kQK / 8];
  std::uint8_t scales[kQK / 64];
};
struct BlockIq4xs {
  std::uint16_t d;
  std::uint16_t scales_h;
  std::uint8_t scales_l[kQK / 64];
  std::uint8_t qs[kQK / 2];
};
struct BlockIq4nl {
  std::uint16_t d;
  std::uint8_t qs[16];
};
struct BlockQ20 {  // Strata Q2_0: 64 values, code c in {0..3} -> d * (c - 1); value n at qs[n/4] >> 2*(n%4)
  std::uint16_t d;
  std::uint8_t qs[16];
};
static_assert(sizeof(BlockIq2s) == 82 && sizeof(BlockIq3xxs) == 98 && sizeof(BlockIq3s) == 110 &&
                  sizeof(BlockIq4xs) == 136 && sizeof(BlockIq4nl) == 18 && sizeof(BlockQ20) == 18 &&
                  sizeof(BlockQ8K) == 292 && sizeof(BlockQ80) == 34,
              "GGUF block sizes");

}  // namespace u::cpu::detail
