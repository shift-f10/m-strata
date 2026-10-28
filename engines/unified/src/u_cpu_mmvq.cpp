// strata-unified-engine: the CPU mirror of the GPU MMVQ i-quant x Q8_1 dots (see u_cpu_mmvq.hpp). Each dot is
// a line-for-line host translation of kernels/u_iq.cu's vec_dot_iqX_q8_1, with the CUDA SIMD intrinsics
// (__dp4a / __vsub4 / __vcmpne4 / __byte_perm / get_int_b*) rewritten as scalar byte ops, against a host copy
// of the same codebook grids. Result: a CPU-computed expert equals the GPU grouped kernel's (same Q8_1
// activation, same integer math), up to the order the per-block float terms are summed.
#include "u_cpu_mmvq.hpp"

#include "u_cpu_iq.hpp"          // fp16_to_f32 / f32_to_fp16
#include "u_cpu_iq_detail.hpp"   // the GGUF weight block layouts

#include <algorithm>
#include <cmath>
#include <cstring>

#include "u_cpu_iq_tables.inc"   // k_iq2s_grid[1024] u64, k_iq3xxs_grid[256] u32, k_iq3s_grid[512] u32, k_kvalues_iq4nl[16]

namespace u::cpu {
namespace {

using detail::BlockIq2s;
using detail::BlockIq3xxs;
using detail::BlockIq3s;
using detail::BlockIq4xs;
using detail::BlockIq4nl;
using detail::BlockQ20;

// ---- scalar hosts of the CUDA intrinsics the vec_dots use ----
inline int popc8(std::uint8_t v) {
  v = (std::uint8_t)(v - ((v >> 1) & 0x55));
  v = (std::uint8_t)((v & 0x33) + ((v >> 2) & 0x33));
  return (v + (v >> 4)) & 0x0f;
}
inline int dp4a(std::uint32_t a, std::uint32_t b, int c) {
  for (int k = 0; k < 4; ++k) c += (int)(std::int8_t)(a >> (8 * k)) * (int)(std::int8_t)(b >> (8 * k));
  return c;
}
inline std::uint32_t vsub4(std::uint32_t a, std::uint32_t b) {  // per-byte a - b (mod 256)
  std::uint32_t r = 0;
  for (int k = 0; k < 4; ++k) r |= (std::uint32_t)(std::uint8_t)((a >> (8 * k)) - (b >> (8 * k))) << (8 * k);
  return r;
}
inline std::uint32_t vcmpne4(std::uint32_t a, std::uint32_t b) {  // per-byte 0xFF if != else 0x00
  std::uint32_t r = 0;
  for (int k = 0; k < 4; ++k)
    if ((std::uint8_t)(a >> (8 * k)) != (std::uint8_t)(b >> (8 * k))) r |= (std::uint32_t)0xFFu << (8 * k);
  return r;
}
inline std::uint32_t byte_perm(std::uint32_t a, std::uint32_t b, std::uint32_t s) {  // 3-bit select from 8 bytes
  const std::uint8_t src[8] = {(std::uint8_t)a,        (std::uint8_t)(a >> 8),  (std::uint8_t)(a >> 16),
                               (std::uint8_t)(a >> 24), (std::uint8_t)b,         (std::uint8_t)(b >> 8),
                               (std::uint8_t)(b >> 16), (std::uint8_t)(b >> 24)};
  std::uint32_t r = 0;
  for (int i = 0; i < 4; ++i) r |= (std::uint32_t)src[(s >> (4 * i)) & 0x7] << (8 * i);
  return r;
}
inline std::uint32_t unpack_ksigns(std::uint8_t v) {
  const std::uint32_t p = (std::uint32_t)(popc8(v) & 1);
  return (std::uint32_t)(std::uint8_t)(v ^ (p << 7)) * 0x01010101u;
}
inline int get_int_b2(const void* x, int i) {
  const std::uint16_t* p = (const std::uint16_t*)x;
  return (int)((std::uint32_t)p[2 * i] | ((std::uint32_t)p[2 * i + 1] << 16));
}
inline int get_int_b4(const void* x, int i) {
  int v;
  std::memcpy(&v, (const std::uint8_t*)x + 4 * (std::size_t)i, 4);
  return v;
}
inline float d8(const BlockQ81* y) { return fp16_to_f32(y->d); }  // __low2float(ds)

struct I2 {
  std::uint32_t x, y;
};
inline I2 table16(std::uint32_t q4, const std::int8_t* table) {  // llama.cpp get_int_from_table_16
  const std::uint32_t* t = (const std::uint32_t*)table;
  std::uint32_t tmp[2];
  const std::uint32_t lhsi = 0x32103210u | ((q4 & 0x88888888u) >> 1);
  for (std::uint32_t i = 0; i < 2; ++i) {
    const std::uint32_t shift = 16 * i;
    const std::uint32_t low = byte_perm(t[0], t[1], q4 >> shift);
    const std::uint32_t high = byte_perm(t[2], t[3], q4 >> shift);
    tmp[i] = byte_perm(low, high, lhsi >> shift);
  }
  return {byte_perm(tmp[0], tmp[1], 0x6420u), byte_perm(tmp[0], tmp[1], 0x7531u)};
}

// ---- the six dots, one (block kbx, sub-call iqs) each, mirroring kernels/u_iq.cu ----
float dot_iq2_s(const std::uint8_t* vbq, const BlockQ81* y, int kbx, int iqs) {
  const BlockIq2s* bq2 = (const BlockIq2s*)vbq + kbx;
  const int qs_packed = get_int_b2(bq2->qs, iqs / 2);
  const std::uint8_t* qs = (const std::uint8_t*)&qs_packed;
  const int qh = bq2->qh[iqs / 2];
  const int signs_packed_32 = get_int_b2(bq2->qs, 256 / 32 + iqs / 2);
  const std::uint8_t* sg = (const std::uint8_t*)&signs_packed_32;
  const int ls0 = bq2->scales[iqs / 2] & 0x0F, ls1 = bq2->scales[iqs / 2] >> 4;
  int sumi0 = 0, sumi1 = 0;
  for (int l0 = 0; l0 < 8; l0 += 2) {
    const std::uint64_t g = k_iq2s_grid[qs[l0 / 2] | ((qh << (8 - l0)) & 0x300)];
    const std::uint32_t s0 = vcmpne4(((sg[l0 / 2] & 0x03) << 7) | ((sg[l0 / 2] & 0x0C) << 21), 0);
    const std::uint32_t s1 = vcmpne4(((sg[l0 / 2] & 0x30) << 3) | ((sg[l0 / 2] & 0xC0) << 17), 0);
    const std::uint32_t gl = vsub4((std::uint32_t)g ^ s0, s0);
    const std::uint32_t gh = vsub4((std::uint32_t)(g >> 32) ^ s1, s1);
    const std::uint32_t u0 = (std::uint32_t)get_int_b4(y[iqs / 2].qs, l0 + 0);
    const std::uint32_t u1 = (std::uint32_t)get_int_b4(y[iqs / 2].qs, l0 + 1);
    if (l0 < 4) { sumi0 = dp4a(gl, u0, sumi0); sumi0 = dp4a(gh, u1, sumi0); }
    else        { sumi1 = dp4a(gl, u0, sumi1); sumi1 = dp4a(gh, u1, sumi1); }
  }
  const int sumi = (sumi0 * ls0 + sumi1 * ls1 + (sumi0 + sumi1) / 2) / 4;
  return fp16_to_f32(bq2->d) * d8(&y[iqs / 2]) * (float)sumi;
}

float dot_iq3_xxs(const std::uint8_t* vbq, const BlockQ81* y, int kbx, int iqs) {
  const BlockIq3xxs* bq3 = (const BlockIq3xxs*)vbq + kbx;
  const int q3lo = get_int_b2(bq3->qs, iqs), q3hi = get_int_b2(bq3->qs, iqs + 1);
  std::uint8_t q3[8];
  std::memcpy(q3, &q3lo, 4);
  std::memcpy(q3 + 4, &q3hi, 4);
  const std::uint32_t aux32 = (std::uint32_t)get_int_b2(bq3->qs, 256 / 16 + iqs / 2);
  int sumi = 0;
  for (int l0 = 0; l0 < 8; l0 += 2) {
    const std::uint32_t gx = k_iq3xxs_grid[q3[l0 + 0]], gy = k_iq3xxs_grid[q3[l0 + 1]];
    const std::uint32_t signs = unpack_ksigns((std::uint8_t)(aux32 >> (7 * l0 / 2)));
    const std::uint32_t s0 = vcmpne4(signs & 0x08040201, 0);
    const std::uint32_t gl = vsub4(gx ^ s0, s0);
    const std::uint32_t u0 = (std::uint32_t)get_int_b4(y[iqs / 2].qs, l0 + 0);
    const std::uint32_t s1 = vcmpne4(signs & 0x80402010, 0);
    const std::uint32_t gh = vsub4(gy ^ s1, s1);
    const std::uint32_t u1 = (std::uint32_t)get_int_b4(y[iqs / 2].qs, l0 + 1);
    sumi = dp4a(gl, u0, sumi);
    sumi = dp4a(gh, u1, sumi);
  }
  const int ls = (int)(aux32 >> 28);
  sumi = (ls * sumi + sumi / 2) / 2;
  return fp16_to_f32(bq3->d) * d8(&y[iqs / 2]) * (float)sumi;
}

float dot_iq3_s(const std::uint8_t* vbq, const BlockQ81* y, int kbx, int iqs) {
  const BlockIq3s* bq3 = (const BlockIq3s*)vbq + kbx;
  const int qlo = get_int_b2(bq3->qs, iqs + 0), qhi = get_int_b2(bq3->qs, iqs + 1);
  std::uint8_t qs[8];
  std::memcpy(qs, &qlo, 4);
  std::memcpy(qs + 4, &qhi, 4);
  const int qh = bq3->qh[iqs / 2];
  const int signs_packed_32 = get_int_b2(bq3->signs, iqs / 2);
  const std::uint8_t* sg = (const std::uint8_t*)&signs_packed_32;
  int sumi = 0;
  for (int l0 = 0; l0 < 8; l0 += 2) {
    const std::uint32_t gx = k_iq3s_grid[qs[l0 + 0] | ((qh << (8 - l0)) & 0x100)];
    const std::uint32_t gy = k_iq3s_grid[qs[l0 + 1] | ((qh << (7 - l0)) & 0x100)];
    const std::uint32_t s0 = vcmpne4(((sg[l0 / 2] & 0x03) << 7) | ((sg[l0 / 2] & 0x0C) << 21), 0);
    const std::uint32_t s1 = vcmpne4(((sg[l0 / 2] & 0x30) << 3) | ((sg[l0 / 2] & 0xC0) << 17), 0);
    const std::uint32_t gl = vsub4(gx ^ s0, s0);
    const std::uint32_t gh = vsub4(gy ^ s1, s1);
    sumi = dp4a(gl, (std::uint32_t)get_int_b4(y[iqs / 2].qs, l0 + 0), sumi);
    sumi = dp4a(gh, (std::uint32_t)get_int_b4(y[iqs / 2].qs, l0 + 1), sumi);
  }
  sumi *= 1 + 2 * ((bq3->scales[iqs / 4] >> ((iqs << 1) & 0x04)) & 0x0F);
  return fp16_to_f32(bq3->d) * d8(&y[iqs / 2]) * (float)sumi;
}

float dot_iq4_nl(const std::uint8_t* vbq, const BlockQ81* y, int kbx, int iqs) {
  const BlockIq4nl* bq4 = (const BlockIq4nl*)vbq + kbx;
  int sumi = 0;
  for (int l = 0; l < 2; ++l) {
    const std::uint32_t aux_q4 = (std::uint32_t)get_int_b2(bq4->qs, iqs + l);
    const I2 v = table16(aux_q4, k_kvalues_iq4nl);
    sumi = dp4a(v.x, (std::uint32_t)get_int_b4(y->qs, iqs + l + 0), sumi);
    sumi = dp4a(v.y, (std::uint32_t)get_int_b4(y->qs, iqs + l + 4), sumi);
  }
  return fp16_to_f32(bq4->d) * d8(y) * (float)sumi;
}

float dot_iq4_xs(const std::uint8_t* vbq, const BlockQ81* y, int kbx, int iqs) {
  const BlockIq4xs* bq4 = (const BlockIq4xs*)vbq + kbx;
  int sumi = 0;
  for (int j = 0; j < 4; ++j) {
    const std::uint32_t aux_q4 = (std::uint32_t)get_int_b4(bq4->qs, iqs + j);
    const I2 v = table16(aux_q4, k_kvalues_iq4nl);
    sumi = dp4a(v.x, (std::uint32_t)get_int_b4(y[iqs / 4].qs, j + 0), sumi);
    sumi = dp4a(v.y, (std::uint32_t)get_int_b4(y[iqs / 4].qs, j + 4), sumi);
  }
  const int ls = ((bq4->scales_l[iqs / 8] >> (iqs & 0x04)) & 0x0F) | (((bq4->scales_h >> (iqs / 2)) & 0x03) << 4);
  sumi *= ls - 32;
  return fp16_to_f32(bq4->d) * d8(&y[iqs / 4]) * (float)sumi;
}

float dot_q2_0(const std::uint8_t* vbq, const BlockQ81* y, int kbx, int iqs) {
  const BlockQ20* bq = (const BlockQ20*)vbq + kbx;
  const float d2 = fp16_to_f32(bq->d);
  const std::int16_t* qs = (const std::int16_t*)bq->qs + iqs * 4;
  const BlockQ81* bq8 = y + iqs;
  int sumi = 0;
  for (int j = 0; j < 4; ++j) {
    const std::uint32_t q = (std::uint32_t)(std::uint16_t)qs[j];
    const std::uint32_t u = (std::uint32_t)get_int_b4(bq8->qs, j * 2 + 0);
    const std::uint32_t v = (std::uint32_t)get_int_b4(bq8->qs, j * 2 + 1);
    const std::uint32_t qe = byte_perm(0x020100FFu, 0x020100FFu, q >> 0);
    const std::uint32_t qo = byte_perm(0x020100FFu, 0x020100FFu, q >> 2);
    const std::uint32_t qx = byte_perm(qe, qo, 0x5140u);
    const std::uint32_t qy = byte_perm(qe, qo, 0x7362u);
    sumi = dp4a(u, qx, sumi);
    sumi = dp4a(v, qy, sumi);
  }
  return d2 * d8(bq8) * (float)sumi;
}

struct Spec {
  int qk, ipb, step;
  float (*dot)(const std::uint8_t*, const BlockQ81*, int, int);
};
bool spec_of(int type, Spec& s) {
  switch (type) {
    case 22: s = {256, 8, 2, dot_iq2_s}; return true;
    case 18: s = {256, 8, 2, dot_iq3_xxs}; return true;
    case 21: s = {256, 8, 2, dot_iq3_s}; return true;
    case 23: s = {256, 8, 4, dot_iq4_xs}; return true;
    case 20: s = {32, 2, 2, dot_iq4_nl}; return true;
    case 42: s = {64, 2, 1, dot_q2_0}; return true;
    default: return false;
  }
}

}  // namespace

bool mmvq_type_supported(int type) {
  Spec s;
  return spec_of(type, s);
}

void quantize_q81(const float* x, BlockQ81* y, int n) {
  for (int b = 0; b * 32 < n; ++b) {
    float amax = 0.0f, sum = 0.0f;
    for (int i = 0; i < 32; ++i) {
      const float xi = x[b * 32 + i];
      amax = std::max(amax, std::fabs(xi));
      sum += xi;
    }
    const float d = std::min(amax / 127.0f, 65504.0f);
    for (int i = 0; i < 32; ++i)
      y[b].qs[i] = amax == 0.0f ? (std::int8_t)0
                                : (std::int8_t)std::max(-127.0f, std::min(127.0f, std::round(x[b * 32 + i] / d)));
    y[b].d = f32_to_fp16(d);
    y[b].s = f32_to_fp16(std::max(-65504.0f, std::min(65504.0f, sum)));
  }
}

float mmvq_row_dot(int type, const std::uint8_t* w, const BlockQ81* x, int n_in) {
  Spec s;
  if (!spec_of(type, s)) return 0.0f;
  const int nb = n_in / s.qk;
  const int xpb = s.qk / 32;  // Q8_1 blocks per weight block
  float acc = 0.0f;
  for (int kbx = 0; kbx < nb; ++kbx)
    for (int p = 0; p < s.ipb; ++p) acc += s.dot(w, x + (std::int64_t)kbx * xpb, kbx, s.step * p);
  return acc;
}

void mmvq_rows(int type, const std::uint8_t* w, std::int64_t row_bytes, const BlockQ81* x, float* y, int n_in,
               int n_out) {
  for (int r = 0; r < n_out; ++r) y[r] = mmvq_row_dot(type, w + (std::int64_t)r * row_bytes, x, n_in);
}

}  // namespace u::cpu
