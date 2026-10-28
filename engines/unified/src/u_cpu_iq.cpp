// strata-unified-engine: CPU row dot products for the qwen4exp routed-expert quants (see u_cpu_iq.hpp).
// The scalar forms are ggml's *_generic dot products (MIT, The ggml authors; llama.cpp @ 4615f346
// ggml/src/ggml-cpu/quants.c) and its reference quantizers (ggml/src/ggml-quants.c), transcribed; Q2_0 is ours.
#include "u_cpu_iq.hpp"
#include "u_cpu_iq_detail.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif

namespace u::cpu {

namespace {

#include "u_cpu_iq_tables.inc"

using namespace detail;

int nearest_int(float f) { return (int)std::lrintf(f); }

float dot_iq2_s(const BlockIq2s* x, const BlockQ8K* y, int nb) {
  float sumf = 0.0f;
  for (int i = 0; i < nb; ++i) {
    const float d = fp16_to_f32(x[i].d) * y[i].d;
    const std::int8_t* q8 = y[i].qs;
    const std::uint8_t* qs = x[i].qs;
    const std::uint8_t* qh = x[i].qh;
    const std::uint8_t* signs = qs + kQK / 8;
    int bsum = 0;
    for (int ib32 = 0; ib32 < kQK / 32; ++ib32) {
      const int ls1 = 1 + 2 * (x[i].scales[ib32] & 0xf);
      const int ls2 = 1 + 2 * (x[i].scales[ib32] >> 4);
      int sumi1 = 0, sumi2 = 0;
      for (int l = 0; l < 2; ++l) {
        const auto* grid = reinterpret_cast<const std::uint8_t*>(k_iq2s_grid + (qs[l] | ((qh[ib32] << (8 - 2 * l)) & 0x300)));
        for (int j = 0; j < 8; ++j) sumi1 += q8[j] * grid[j] * ((signs[l] & k_kmask_iq2xs[j]) ? -1 : 1);
        q8 += 8;
      }
      for (int l = 2; l < 4; ++l) {
        const auto* grid = reinterpret_cast<const std::uint8_t*>(k_iq2s_grid + (qs[l] | ((qh[ib32] << (8 - 2 * l)) & 0x300)));
        for (int j = 0; j < 8; ++j) sumi2 += q8[j] * grid[j] * ((signs[l] & k_kmask_iq2xs[j]) ? -1 : 1);
        q8 += 8;
      }
      bsum += ls1 * sumi1 + ls2 * sumi2;
      qs += 4;
      signs += 4;
    }
    sumf += d * (float)bsum;
  }
  return 0.125f * sumf;
}

float dot_iq3_xxs(const BlockIq3xxs* x, const BlockQ8K* y, int nb) {
  float sumf = 0.0f;
  for (int i = 0; i < nb; ++i) {
    const float d = fp16_to_f32(x[i].d) * y[i].d;
    const std::uint8_t* q3 = x[i].qs;
    const std::uint8_t* gas = x[i].qs + kQK / 4;
    const std::int8_t* q8 = y[i].qs;
    std::int32_t bsum = 0;
    for (int ib32 = 0; ib32 < kQK / 32; ++ib32) {
      std::uint32_t aux32 = 0;
      std::memcpy(&aux32, gas, sizeof(aux32));
      gas += sizeof(aux32);
      const std::uint32_t ls = 2 * (aux32 >> 28) + 1;
      std::int32_t sumi = 0;
      for (int l = 0; l < 4; ++l) {
        const auto* grid1 = reinterpret_cast<const std::uint8_t*>(k_iq3xxs_grid + q3[2 * l + 0]);
        const auto* grid2 = reinterpret_cast<const std::uint8_t*>(k_iq3xxs_grid + q3[2 * l + 1]);
        const std::uint8_t signs = k_ksigns_iq2xs[(aux32 >> 7 * l) & 127];
        for (int j = 0; j < 4; ++j) {
          sumi += grid1[j] * q8[j + 0] * ((signs & k_kmask_iq2xs[j + 0]) ? -1 : 1);
          sumi += grid2[j] * q8[j + 4] * ((signs & k_kmask_iq2xs[j + 4]) ? -1 : 1);
        }
        q8 += 8;
      }
      q3 += 8;
      bsum += sumi * (std::int32_t)ls;
    }
    sumf += d * (float)bsum;
  }
  return 0.25f * sumf;
}

float dot_iq3_s(const BlockIq3s* x, const BlockQ8K* y, int nb) {
  float sumf = 0.0f;
  for (int i = 0; i < nb; ++i) {
    const float d = fp16_to_f32(x[i].d) * y[i].d;
    const std::uint8_t* qs = x[i].qs;
    const std::uint8_t* qh = x[i].qh;
    const std::uint8_t* signs = x[i].signs;
    const std::int8_t* q8 = y[i].qs;
    std::int32_t bsum = 0;
    for (int ib32 = 0; ib32 < kQK / 32; ib32 += 2) {
      const std::int32_t ls1 = 2 * (x[i].scales[ib32 / 2] & 0xf) + 1;
      const std::int32_t ls2 = 2 * (x[i].scales[ib32 / 2] >> 4) + 1;
      for (int half = 0; half < 2; ++half) {
        std::int32_t sumi = 0;
        for (int l = 0; l < 4; ++l) {
          const auto* grid1 =
              reinterpret_cast<const std::uint8_t*>(k_iq3s_grid + (qs[2 * l + 0] | ((qh[ib32 + half] << (8 - 2 * l)) & 256)));
          const auto* grid2 =
              reinterpret_cast<const std::uint8_t*>(k_iq3s_grid + (qs[2 * l + 1] | ((qh[ib32 + half] << (7 - 2 * l)) & 256)));
          for (int j = 0; j < 4; ++j) {
            sumi += grid1[j] * q8[j + 0] * ((signs[l] & k_kmask_iq2xs[j + 0]) ? -1 : 1);
            sumi += grid2[j] * q8[j + 4] * ((signs[l] & k_kmask_iq2xs[j + 4]) ? -1 : 1);
          }
          q8 += 8;
        }
        qs += 8;
        signs += 4;
        bsum += sumi * (half == 0 ? ls1 : ls2);
      }
    }
    sumf += d * (float)bsum;
  }
  return sumf;
}

float dot_iq4_xs(const BlockIq4xs* x, const BlockQ8K* y, int nb) {
  float sumf = 0.0f;
  for (int ibl = 0; ibl < nb; ++ibl) {
    const float d4d8 = fp16_to_f32(x[ibl].d) * y[ibl].d;
    std::uint16_t h = x[ibl].scales_h;
    const std::uint8_t* qs = x[ibl].qs;
    const std::int8_t* q8 = y[ibl].qs;
    for (int ib = 0; ib < kQK / 32; ib += 2) {
      const std::uint8_t ls1 = (std::uint8_t)((x[ibl].scales_l[ib / 2] & 0xf) | ((h << 4) & 0x30));
      const std::uint8_t ls2 = (std::uint8_t)((x[ibl].scales_l[ib / 2] >> 4) | ((h << 2) & 0x30));
      h = (std::uint16_t)(h >> 4);
      const float d1 = d4d8 * (float)(ls1 - 32);
      const float d2 = d4d8 * (float)(ls2 - 32);
      int sumi1 = 0, sumi2 = 0;
      for (int j = 0; j < 16; ++j) {
        sumi1 += q8[j + 0] * k_kvalues_iq4nl[qs[j] & 0xf];
        sumi2 += q8[j + 16] * k_kvalues_iq4nl[qs[j] >> 4];
      }
      sumf += d1 * (float)(sumi1 + sumi2);
      qs += 16;
      q8 += 32;
      sumi1 = sumi2 = 0;
      for (int j = 0; j < 16; ++j) {
        sumi1 += q8[j + 0] * k_kvalues_iq4nl[qs[j] & 0xf];
        sumi2 += q8[j + 16] * k_kvalues_iq4nl[qs[j] >> 4];
      }
      sumf += d2 * (float)(sumi1 + sumi2);
      qs += 16;
      q8 += 32;
    }
  }
  return sumf;
}

float dot_iq4_nl(const BlockIq4nl* x, const BlockQ80* y, int nb) {
  float sumf = 0.0f;
  for (int ib = 0; ib < nb; ++ib) {
    const float d = fp16_to_f32(y[ib].d) * fp16_to_f32(x[ib].d);
    int sumi1 = 0, sumi2 = 0;
    for (int j = 0; j < 16; ++j) {
      sumi1 += y[ib].qs[j + 0] * k_kvalues_iq4nl[x[ib].qs[j] & 0xf];
      sumi2 += y[ib].qs[j + 16] * k_kvalues_iq4nl[x[ib].qs[j] >> 4];
    }
    sumf += d * (float)(sumi1 + sumi2);
  }
  return sumf;
}

// Q2_0 (64 values a block) . Q8_0 (32 a block): two activation blocks per weight block.
float dot_q2_0(const BlockQ20* x, const BlockQ80* y, int nb) {
  float sumf = 0.0f;
  for (int ib = 0; ib < nb; ++ib) {
    const float dx = fp16_to_f32(x[ib].d);
    for (int h = 0; h < 2; ++h) {
      const BlockQ80& yb = y[2 * ib + h];
      int sumi = 0;
      for (int j = 0; j < 32; ++j) {
        const int n = 32 * h + j;
        const int c = (x[ib].qs[n / 4] >> (2 * (n % 4))) & 3;
        sumi += (c - 1) * yb.qs[j];
      }
      sumf += dx * fp16_to_f32(yb.d) * (float)sumi;
    }
  }
  return sumf;
}

}  // namespace

float fp16_to_f32(std::uint16_t h) noexcept {
  const std::uint32_t sign = (std::uint32_t)(h & 0x8000u) << 16;
  std::uint32_t exp = (h >> 10) & 0x1f, man = h & 0x3ffu;
  std::uint32_t bits;
  if (exp == 0) {
    if (man == 0) {
      bits = sign;
    } else {  // subnormal: normalize
      int e = -1;
      do {
        ++e;
        man <<= 1;
      } while ((man & 0x400u) == 0);
      bits = sign | ((std::uint32_t)(127 - 15 - e) << 23) | ((man & 0x3ffu) << 13);
    }
  } else if (exp == 31) {
    bits = sign | 0x7f800000u | (man << 13);
  } else {
    bits = sign | ((exp + 127 - 15) << 23) | (man << 13);
  }
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

std::uint16_t f32_to_fp16(float f) noexcept {  // round to nearest even
  std::uint32_t x;
  std::memcpy(&x, &f, 4);
  const std::uint32_t sign = (x >> 16) & 0x8000u;
  const std::uint32_t absx = x & 0x7fffffffu;
  if (absx >= 0x7f800000u) return (std::uint16_t)(sign | 0x7c00u | (absx > 0x7f800000u ? 0x200u : 0u));
  if (absx >= 0x477ff000u) return (std::uint16_t)(sign | 0x7c00u);  // overflow -> inf
  if (absx < 0x38800000u) {                                       // subnormal half (or zero)
    if (absx < 0x33000000u) return (std::uint16_t)sign;
    const std::uint32_t e = absx >> 23, m = (absx & 0x7fffffu) | 0x800000u;
    const std::uint32_t shift = 126 - e;  // 14..24
    std::uint32_t v = m >> shift;
    const std::uint32_t rem = m & ((1u << shift) - 1), halfway = 1u << (shift - 1);
    if (rem > halfway || (rem == halfway && (v & 1u))) ++v;
    return (std::uint16_t)(sign | v);
  }
  std::uint32_t v = ((absx >> 13) - (112u << 10));
  const std::uint32_t rem = absx & 0x1fffu;
  if (rem > 0x1000u || (rem == 0x1000u && (v & 1u))) ++v;
  return (std::uint16_t)(sign | v);
}

bool iq_type_supported(int type) noexcept {
  return type == 18 || type == 20 || type == 21 || type == 22 || type == 23 || type == 42;
}
bool iq_type_q8k(int type) noexcept { return type == 18 || type == 21 || type == 22 || type == 23; }

std::int64_t row_bytes(int type, int n) noexcept {
  switch (type) {
    case 18: return (std::int64_t)(n / 256) * 98;
    case 20: return (std::int64_t)(n / 32) * 18;
    case 21: return (std::int64_t)(n / 256) * 110;
    case 22: return (std::int64_t)(n / 256) * 82;
    case 23: return (std::int64_t)(n / 256) * 136;
    case 42: return (std::int64_t)(n / 64) * 18;
    default: return 0;
  }
}

bool have_avx2() noexcept {
  static const bool v = [] {
#if defined(_MSC_VER)
    int r[4] = {0, 0, 0, 0};
    __cpuidex(r, 7, 0);
    const bool avx2 = (r[1] & (1 << 5)) != 0;
    __cpuid(r, 1);
    const bool fma = (r[2] & (1 << 12)) != 0, osx = (r[2] & (1 << 27)) != 0;
    return avx2 && fma && osx;
#else
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (!__get_cpuid_count(7, 0, &a, &b, &c, &d)) return false;
    const bool avx2 = (b & (1u << 5)) != 0;
    if (!__get_cpuid(1, &a, &b, &c, &d)) return false;
    return avx2 && (c & (1u << 12)) != 0 && (c & (1u << 27)) != 0;
#endif
  }();
  return v;
}

void quantize_q8k(const float* x, BlockQ8K* y, int n) {
  for (int i = 0; i < n / kQK; ++i, x += kQK) {
    float max = 0.0f, amax = 0.0f;
    for (int j = 0; j < kQK; ++j) {
      const float ax = std::fabs(x[j]);
      if (ax > amax) {
        amax = ax;
        max = x[j];
      }
    }
    if (amax == 0.0f) {
      y[i].d = 0.0f;
      std::memset(y[i].qs, 0, sizeof y[i].qs);
      std::memset(y[i].bsums, 0, sizeof y[i].bsums);
      continue;
    }
    const float iscale = -127.0f / max;
    for (int j = 0; j < kQK; ++j) y[i].qs[j] = (std::int8_t)std::min(127, nearest_int(iscale * x[j]));
    for (int j = 0; j < kQK / 16; ++j) {
      int sum = 0;
      for (int ii = 0; ii < 16; ++ii) sum += y[i].qs[j * 16 + ii];
      y[i].bsums[j] = (std::int16_t)sum;
    }
    y[i].d = 1.0f / iscale;
  }
}

void quantize_q80(const float* x, BlockQ80* y, int n) {
  for (int i = 0; i < n / 32; ++i) {
    float amax = 0.0f;
    for (int j = 0; j < 32; ++j) amax = std::max(amax, std::fabs(x[i * 32 + j]));
    const float d = amax / 127.0f;
    const float id = d != 0.0f ? 1.0f / d : 0.0f;
    y[i].d = f32_to_fp16(d);
    for (int j = 0; j < 32; ++j) y[i].qs[j] = (std::int8_t)std::roundf(x[i * 32 + j] * id);
  }
}

float dot_ref(int type, const void* row, const void* xq, int n) {
  switch (type) {
    case 22: return dot_iq2_s(static_cast<const BlockIq2s*>(row), static_cast<const BlockQ8K*>(xq), n / kQK);
    case 18: return dot_iq3_xxs(static_cast<const BlockIq3xxs*>(row), static_cast<const BlockQ8K*>(xq), n / kQK);
    case 21: return dot_iq3_s(static_cast<const BlockIq3s*>(row), static_cast<const BlockQ8K*>(xq), n / kQK);
    case 23: return dot_iq4_xs(static_cast<const BlockIq4xs*>(row), static_cast<const BlockQ8K*>(xq), n / kQK);
    case 20: return dot_iq4_nl(static_cast<const BlockIq4nl*>(row), static_cast<const BlockQ80*>(xq), n / 32);
    case 42: return dot_q2_0(static_cast<const BlockQ20*>(row), static_cast<const BlockQ80*>(xq), n / 64);
    default: return 0.0f;
  }
}

float dot(int type, const void* row, const void* xq, int n) {
  return have_avx2() ? detail::dot_avx2(type, row, xq, n) : dot_ref(type, row, xq, n);
}

}  // namespace u::cpu
