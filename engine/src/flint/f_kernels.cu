// multiengine_flint kernels (see f_kernels.cuh). Plain fp32 arithmetic on dequantised weights; the reductions are warp
// shuffles plus one shared-memory stage. The matvec, norm, argmax, top-k, dequant and plan kernels are Gemma 4's
// (g4_kernels.cu); attention, the q/k/v post-processing, the router, the activation and the combines are flint's.
#include "f_kernels.cuh"

#include <cmath>
#include <cstddef>

namespace flk {
namespace {

constexpr int kWarp = 32;

__device__ __forceinline__ float neg_inf() { return -__int_as_float(0x7f800000); }

__device__ __forceinline__ float warp_sum(float v) {
#pragma unroll
  for (int o = 16; o > 0; o >>= 1) v += __shfl_xor_sync(0xffffffffu, v, o);
  return v;
}

__device__ __forceinline__ float warp_max(float v) {
#pragma unroll
  for (int o = 16; o > 0; o >>= 1) v = fmaxf(v, __shfl_xor_sync(0xffffffffu, v, o));
  return v;
}

// Sum over the block (blockDim.x a multiple of 32, at most 1024). `red` holds 32 floats.
__device__ float block_sum(float v, float* red) {
  const int lane = threadIdx.x & 31, warp = threadIdx.x >> 5, warps = blockDim.x >> 5;
  v = warp_sum(v);
  __syncthreads();
  if (lane == 0) red[warp] = v;
  __syncthreads();
  float t = lane < warps ? red[lane] : 0.0f;
  t = warp_sum(t);
  return t;  // every thread gets the total
}

__device__ float block_max(float v, float* red) {
  const int lane = threadIdx.x & 31, warp = threadIdx.x >> 5, warps = blockDim.x >> 5;
  v = warp_max(v);
  __syncthreads();
  if (lane == 0) red[warp] = v;
  __syncthreads();
  float t = lane < warps ? red[lane] : neg_inf();
  return warp_max(t);
}

__device__ __forceinline__ float bf16_bits(unsigned short b) { return __uint_as_float(static_cast<unsigned int>(b) << 16); }

__device__ __forceinline__ float dot16_q8(const int4 v, const float4 a, const float4 b, const float4 c, const float4 d) {
  const char4 p0 = *reinterpret_cast<const char4*>(&v.x);
  const char4 p1 = *reinterpret_cast<const char4*>(&v.y);
  const char4 p2 = *reinterpret_cast<const char4*>(&v.z);
  const char4 p3 = *reinterpret_cast<const char4*>(&v.w);
  float r = 0.0f;
  r = fmaf(float(p0.x), a.x, r); r = fmaf(float(p0.y), a.y, r); r = fmaf(float(p0.z), a.z, r); r = fmaf(float(p0.w), a.w, r);
  r = fmaf(float(p1.x), b.x, r); r = fmaf(float(p1.y), b.y, r); r = fmaf(float(p1.z), b.z, r); r = fmaf(float(p1.w), b.w, r);
  r = fmaf(float(p2.x), c.x, r); r = fmaf(float(p2.y), c.y, r); r = fmaf(float(p2.z), c.z, r); r = fmaf(float(p2.w), c.w, r);
  r = fmaf(float(p3.x), d.x, r); r = fmaf(float(p3.y), d.y, r); r = fmaf(float(p3.z), d.z, r); r = fmaf(float(p3.w), d.w, r);
  return r;
}

__device__ __forceinline__ float dot8_bf16(const uint4 v, const float4 a, const float4 b) {
  float r = 0.0f;
  r = fmaf(bf16_bits(static_cast<unsigned short>(v.x & 0xffffu)), a.x, r);
  r = fmaf(bf16_bits(static_cast<unsigned short>(v.x >> 16)), a.y, r);
  r = fmaf(bf16_bits(static_cast<unsigned short>(v.y & 0xffffu)), a.z, r);
  r = fmaf(bf16_bits(static_cast<unsigned short>(v.y >> 16)), a.w, r);
  r = fmaf(bf16_bits(static_cast<unsigned short>(v.z & 0xffffu)), b.x, r);
  r = fmaf(bf16_bits(static_cast<unsigned short>(v.z >> 16)), b.y, r);
  r = fmaf(bf16_bits(static_cast<unsigned short>(v.w & 0xffffu)), b.z, r);
  r = fmaf(bf16_bits(static_cast<unsigned short>(v.w >> 16)), b.w, r);
  return r;
}

// Two rows per warp, eight warps per block (16 rows a block). acc[r] for rows row0, row0+1.
constexpr int kRowsPerWarp = 2;
constexpr int kMatvecThreads = 256;
constexpr int kRowsPerBlock = kRowsPerWarp * (kMatvecThreads / kWarp);

template <int KIND>
__device__ __forceinline__ void rows_dot(const void* __restrict__ w, const __half* __restrict__ s, const float* __restrict__ x,
                                         int cols, int row0, int nr, int lane, float acc[kRowsPerWarp]) {
  if constexpr (KIND == 0) {
    const auto* q = static_cast<const signed char*>(w);
    const int nb = cols >> 5;
    for (int c = lane * 16; c < cols; c += 512) {
      const float4* xp = reinterpret_cast<const float4*>(x + c);
      const float4 a = __ldg(xp), b = __ldg(xp + 1), cc = __ldg(xp + 2), d = __ldg(xp + 3);
#pragma unroll
      for (int r = 0; r < kRowsPerWarp; ++r) {
        if (r < nr) {
          const std::size_t row = static_cast<std::size_t>(row0 + r);
          const int4 v = *reinterpret_cast<const int4*>(q + row * cols + c);
          const float sc = __half2float(s[row * nb + (c >> 5)]);
          acc[r] = fmaf(dot16_q8(v, a, b, cc, d), sc, acc[r]);
        }
      }
    }
  } else if constexpr (KIND == 1) {
    const auto* q = static_cast<const unsigned short*>(w);
    for (int c = lane * 8; c < cols; c += 256) {
      const float4* xp = reinterpret_cast<const float4*>(x + c);
      const float4 a = __ldg(xp), b = __ldg(xp + 1);
#pragma unroll
      for (int r = 0; r < kRowsPerWarp; ++r) {
        if (r < nr) {
          const uint4 v = *reinterpret_cast<const uint4*>(q + static_cast<std::size_t>(row0 + r) * cols + c);
          acc[r] += dot8_bf16(v, a, b);
        }
      }
    }
  } else {
    const auto* q = static_cast<const float*>(w);
    for (int c = lane * 4; c < cols; c += 128) {
      const float4 a = __ldg(reinterpret_cast<const float4*>(x + c));
#pragma unroll
      for (int r = 0; r < kRowsPerWarp; ++r) {
        if (r < nr) {
          const float4 v = *reinterpret_cast<const float4*>(q + static_cast<std::size_t>(row0 + r) * cols + c);
          acc[r] += v.x * a.x + v.y * a.y + v.z * a.z + v.w * a.w;
        }
      }
    }
  }
}

template <int KIND>
__global__ void __launch_bounds__(kMatvecThreads) k_matvec(const void* __restrict__ w, const __half* __restrict__ s,
                                                           const float* __restrict__ x, float* __restrict__ y, int rows, int cols) {
  const int warp = threadIdx.x >> 5, lane = threadIdx.x & 31;
  const int row0 = blockIdx.x * kRowsPerBlock + warp * kRowsPerWarp;
  if (row0 >= rows) return;
  const int nr = min(kRowsPerWarp, rows - row0);
  const int t = blockIdx.y;
  float acc[kRowsPerWarp] = {0.0f, 0.0f};
  rows_dot<KIND>(w, s, x + static_cast<std::size_t>(t) * cols, cols, row0, nr, lane, acc);
#pragma unroll
  for (int r = 0; r < kRowsPerWarp; ++r) acc[r] = warp_sum(acc[r]);
  if (lane == 0) {
    for (int r = 0; r < nr; ++r) y[static_cast<std::size_t>(t) * rows + row0 + r] = acc[r];
  }
}

__global__ void k_rmsnorm(const float* __restrict__ x, const float* __restrict__ w, float* __restrict__ out, int n, float eps) {
  __shared__ float red[32];
  const float* xr = x + static_cast<std::size_t>(blockIdx.x) * n;
  float* o = out + static_cast<std::size_t>(blockIdx.x) * n;
  float ss = 0.0f;
  for (int i = threadIdx.x; i < n; i += blockDim.x) ss += xr[i] * xr[i];
  const float inv = rsqrtf(block_sum(ss, red) / float(n) + eps);
  for (int i = threadIdx.x; i < n; i += blockDim.x) o[i] = xr[i] * inv * w[i];
}

__global__ void k_add_rows(const float* __restrict__ x, const float* __restrict__ a, const float* __restrict__ b,
                           float* __restrict__ out, long long n) {
  const long long i = static_cast<long long>(blockIdx.x) * blockDim.x + threadIdx.x;
  if (i >= n) return;
  float v = x[i] + a[i];
  if (b) v += b[i];
  out[i] = v;
}

__global__ void k_embed_q8(const int* __restrict__ ids, const signed char* __restrict__ q, const __half* __restrict__ s, int n,
                           float* __restrict__ out) {
  const int t = blockIdx.y;
  const int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= n) return;
  const std::size_t row = static_cast<std::size_t>(ids[t]);
  out[static_cast<std::size_t>(t) * n + i] = float(q[row * n + i]) * __half2float(s[row * (n / 32) + i / 32]);
}

__global__ void k_embed_bf16(const int* __restrict__ ids, const unsigned short* __restrict__ w, int n, float* __restrict__ out) {
  const int t = blockIdx.y;
  const int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= n) return;
  out[static_cast<std::size_t>(t) * n + i] = bf16_bits(w[static_cast<std::size_t>(ids[t]) * n + i]);
}

// One block per (token, head), 64 threads: thread i owns elements i and i + 64 (NeoX's pairs).
__global__ void k_qkv_post(QkvPost p) {
  __shared__ float red[32];
  constexpr int kHeads = 32, kKv = 4, half = kHead / 2;
  const int t = blockIdx.x, h = blockIdx.y, i = threadIdx.x;
  const int pos = (p.d_pos != nullptr ? *p.d_pos : p.pos0) + t;
  const float* src;
  const float* w = nullptr;
  int kind;  // 0 q, 1 k, 2 v
  int hh;
  if (h < kHeads) {
    kind = 0; hh = h;
    src = p.q_raw + (static_cast<std::size_t>(t) * kHeads + hh) * kHead;
    w = p.q_norm;
  } else if (h < kHeads + kKv) {
    kind = 1; hh = h - kHeads;
    src = p.k_raw + (static_cast<std::size_t>(t) * kKv + hh) * kHead;
    w = p.k_norm;
  } else {
    kind = 2; hh = h - kHeads - kKv;
    src = p.v_raw + (static_cast<std::size_t>(t) * kKv + hh) * kHead;
  }
  float a = src[i], b = src[i + half];
  if (kind != 2) {
    const float inv = rsqrtf(block_sum(a * a + b * b, red) / float(kHead) + p.eps);
    a *= inv * w[i];
    b *= inv * w[i + half];
    if (p.rope) {
      const float theta = float(pos) * powf(p.theta_scale, float(i));
      float sn, cs;
      sincosf(theta, &sn, &cs);
      const float a2 = a * cs - b * sn, b2 = a * sn + b * cs;
      a = a2;
      b = b2;
    }
  }
  if (kind == 0) {
    const std::size_t o = (static_cast<std::size_t>(t) * kHeads + hh) * kHead;
    p.q_out[o + i] = a;
    p.q_out[o + i + half] = b;
    if (p.q16) {
      p.q16[o + i] = __float2half(a);
      p.q16[o + i + half] = __float2half(b);
    }
  } else {
    const int slot = p.ring ? pos % p.ring : pos;
    __half* c = (kind == 1 ? p.k_cache : p.v_cache) + (static_cast<std::size_t>(hh) * p.cap + slot) * kHead;
    c[i] = __float2half(a);
    c[i + half] = __float2half(b);
  }
}

constexpr int kSplit = 128;  // keys per decode-attention block

// Gemma 4's split-K decode attention at flint's shape: grid (4 KV heads, splits), 256 threads; the 8 query heads of KV
// head blockIdx.x share its keys. Scores times `scale`. `dpos` non-null: the keys are [max(0, pos - window + 1), pos]
// for the device word pos (lo and hi unused).
__global__ void __launch_bounds__(256) k_attn_part(const float* __restrict__ q, const __half* __restrict__ kc,
                                                   const __half* __restrict__ vc, int cap, int ring, int lo, int hi,
                                                   float* __restrict__ scratch, int splits, const int* __restrict__ dpos,
                                                   int window, float scale) {
  constexpr int HD = kHead, G = kGroup;
  __shared__ float qs[G][HD];
  __shared__ float sc[G][kSplit];
  __shared__ float stat[G][2];
  if (dpos != nullptr) {
    hi = *dpos;
    lo = window > 0 ? max(hi - window + 1, 0) : 0;
  }
  const int g = blockIdx.x, split = blockIdx.y;
  const int warp = threadIdx.x >> 5, lane = threadIdx.x & 31;
  for (int i = threadIdx.x; i < G * HD; i += blockDim.x) qs[i / HD][i % HD] = q[(static_cast<std::size_t>(g) * G) * HD + i];
  __syncthreads();
  const int k0 = lo + split * kSplit;
  const int nk = min(kSplit, hi + 1 - k0);
  const __half* kbase = kc + static_cast<std::size_t>(g) * cap * HD;
  const __half* vbase = vc + static_cast<std::size_t>(g) * cap * HD;
  for (int kk = warp; kk < nk; kk += 8) {
    const int p = k0 + kk;
    const int slot = ring ? p % ring : p;
    const __half2* kr = reinterpret_cast<const __half2*>(kbase + static_cast<std::size_t>(slot) * HD);
    float acc[G];
#pragma unroll
    for (int gi = 0; gi < G; ++gi) acc[gi] = 0.0f;
#pragma unroll
    for (int j = 0; j < HD / 64; ++j) {
      const float2 kf = __half22float2(kr[j * 32 + lane]);
      const int d = j * 64 + lane * 2;
#pragma unroll
      for (int gi = 0; gi < G; ++gi) acc[gi] = fmaf(qs[gi][d], kf.x, fmaf(qs[gi][d + 1], kf.y, acc[gi]));
    }
#pragma unroll
    for (int gi = 0; gi < G; ++gi) {
      const float v = warp_sum(acc[gi]);
      if (lane == 0) sc[gi][kk] = v * scale;
    }
  }
  __syncthreads();
  if (warp < G) {
    float m = neg_inf();
    for (int kk = lane; kk < nk; kk += 32) m = fmaxf(m, sc[warp][kk]);
    m = warp_max(m);
    float l = 0.0f;
    for (int kk = lane; kk < nk; kk += 32) {
      const float e = expf(sc[warp][kk] - m);
      sc[warp][kk] = e;
      l += e;
    }
    l = warp_sum(l);
    if (lane == 0) {
      stat[warp][0] = m;
      stat[warp][1] = l;
    }
  }
  __syncthreads();
  for (int t = threadIdx.x; t < HD / 2; t += blockDim.x) {
    float2 o[G];
#pragma unroll
    for (int gi = 0; gi < G; ++gi) o[gi] = make_float2(0.0f, 0.0f);
    for (int kk = 0; kk < nk; ++kk) {
      const int p = k0 + kk;
      const int slot = ring ? p % ring : p;
      const float2 v = __half22float2(reinterpret_cast<const __half2*>(vbase + static_cast<std::size_t>(slot) * HD)[t]);
#pragma unroll
      for (int gi = 0; gi < G; ++gi) {
        o[gi].x = fmaf(sc[gi][kk], v.x, o[gi].x);
        o[gi].y = fmaf(sc[gi][kk], v.y, o[gi].y);
      }
    }
#pragma unroll
    for (int gi = 0; gi < G; ++gi) {
      float* dst = scratch + (static_cast<std::size_t>(g * G + gi) * splits + split) * (HD + 2);
      dst[2 * t] = o[gi].x;
      dst[2 * t + 1] = o[gi].y;
    }
  }
  if (threadIdx.x < G) {
    float* dst = scratch + (static_cast<std::size_t>(g * G + threadIdx.x) * splits + split) * (HD + 2);
    dst[HD] = stat[threadIdx.x][0];
    dst[HD + 1] = stat[threadIdx.x][1];
  }
}

// The splits merged; the head's sink joins the maximum and the denominator (llama.cpp's soft_max with sinks), with no
// value of its own.
__global__ void k_attn_merge(const float* __restrict__ scratch, int splits, int n_split, const float* __restrict__ sinks,
                             float* __restrict__ out) {
  constexpr int HD = kHead;
  const int h = blockIdx.x;
  const float* base = scratch + static_cast<std::size_t>(h) * splits * (HD + 2);
  float M = sinks != nullptr ? sinks[h] : neg_inf();
  for (int s = 0; s < n_split; ++s) M = fmaxf(M, base[static_cast<std::size_t>(s) * (HD + 2) + HD]);
  float L = sinks != nullptr ? expf(sinks[h] - M) : 0.0f;
  for (int s = 0; s < n_split; ++s) {
    const float* b = base + static_cast<std::size_t>(s) * (HD + 2);
    L += expf(b[HD] - M) * b[HD + 1];
  }
  for (int d = threadIdx.x; d < HD; d += blockDim.x) {
    float o = 0.0f;
    for (int s = 0; s < n_split; ++s) {
      const float* b = base + static_cast<std::size_t>(s) * (HD + 2);
      o += expf(b[HD] - M) * b[d];
    }
    out[static_cast<std::size_t>(h) * HD + d] = o / L;
  }
}

__global__ void k_attn_softmax(const float* __restrict__ s, __half* __restrict__ p, int Ts, int L, int qpos0, int ring,
                               int window, int last_pos, const float* __restrict__ sinks, float scale) {
  __shared__ float red[32];
  const int row = blockIdx.x;
  const int gi = row / Ts, t = row % Ts;
  const int qp = qpos0 + t;
  const float* sr = s + static_cast<std::size_t>(row) * L;
  __half* pr = p + static_cast<std::size_t>(row) * L;
  auto valid = [&](int j) {
    int kp = j;
    if (ring) kp = last_pos - (((last_pos - j) % ring) + ring) % ring;
    return kp >= 0 && kp <= qp && (window == 0 || qp - kp < window);
  };
  const float sink = sinks != nullptr ? sinks[gi] : neg_inf();
  float m = sink;
  for (int j = threadIdx.x; j < L; j += blockDim.x)
    if (valid(j)) m = fmaxf(m, sr[j] * scale);
  m = block_max(m, red);
  float l = 0.0f;
  for (int j = threadIdx.x; j < L; j += blockDim.x)
    if (valid(j)) l += expf(sr[j] * scale - m);
  l = block_sum(l, red);
  if (sinks != nullptr) l += expf(sink - m);
  const float inv = 1.0f / l;
  for (int j = threadIdx.x; j < L; j += blockDim.x) pr[j] = __float2half(valid(j) ? expf(sr[j] * scale - m) * inv : 0.0f);
}

// One warp per token: 256 logits, eight per lane; sigmoid probabilities, the top 6 by probability (ties by the lower id).
__global__ void k_router_topk(const float* __restrict__ logits, int T, int* __restrict__ picks, float* __restrict__ weights,
                              int* __restrict__ counts, float w_scale) {
  const int t = blockIdx.x * (blockDim.x / 32) + (threadIdx.x >> 5);
  const int lane = threadIdx.x & 31;
  if (t >= T) return;
  constexpr int kPer = kRouted / 32;
  float p[kPer];
#pragma unroll
  for (int j = 0; j < kPer; ++j) p[j] = 1.0f / (1.0f + expf(-logits[static_cast<std::size_t>(t) * kRouted + lane + 32 * j]));
  bool taken[kPer];
#pragma unroll
  for (int j = 0; j < kPer; ++j) taken[j] = false;
  float chosen[kPicks];
  int ids[kPicks];
  float total = 0.0f;
  for (int k = 0; k < kPicks; ++k) {
    float best = -1.0f;
    int bi = 1 << 30;
#pragma unroll
    for (int j = 0; j < kPer; ++j) {
      const int id = lane + 32 * j;
      if (!taken[j] && (p[j] > best || (p[j] == best && id < bi))) {
        best = p[j];
        bi = id;
      }
    }
#pragma unroll
    for (int o = 16; o > 0; o >>= 1) {
      const float ob = __shfl_xor_sync(0xffffffffu, best, o);
      const int oi = __shfl_xor_sync(0xffffffffu, bi, o);
      if (ob > best || (ob == best && oi < bi)) {
        best = ob;
        bi = oi;
      }
    }
    if ((bi & 31) == lane) taken[bi >> 5] = true;
    chosen[k] = best;
    ids[k] = bi;
    total += best;
  }
  total = fmaxf(total, 6.103515625e-5f);
  if (lane < kPicks) {
    picks[static_cast<std::size_t>(t) * kPicks + lane] = ids[lane];
    weights[static_cast<std::size_t>(t) * kPicks + lane] = chosen[lane] / total * w_scale;
    if (counts) atomicAdd(&counts[ids[lane]], 1);
  }
}

__global__ void k_swiglu(const float* __restrict__ g, int gs, const float* __restrict__ u, int us, float* __restrict__ m, int n) {
  const int r = blockIdx.y;
  const int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= n) return;
  const float a = g[static_cast<std::size_t>(r) * gs + i];
  m[static_cast<std::size_t>(r) * n + i] = a / (1.0f + expf(-a)) * u[static_cast<std::size_t>(r) * us + i];
}

__global__ void k_moe_combine(const float* __restrict__ y, const float* __restrict__ w, int n_pick, int n, float* __restrict__ moe) {
  const int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= n) return;
  float acc = 0.0f;
  for (int k = 0; k < n_pick; ++k) acc = fmaf(y[static_cast<std::size_t>(k) * n + i], w[k], acc);
  moe[i] = acc;
}

__global__ void k_scatter_combine(const float* __restrict__ y, const int* __restrict__ inv, const float* __restrict__ w, int n,
                                  float* __restrict__ moe) {
  const int t = blockIdx.y;
  for (int i = blockIdx.x * blockDim.x + threadIdx.x; i < n; i += gridDim.x * blockDim.x) {
    float acc = 0.0f;
#pragma unroll
    for (int k = 0; k < kPicks; ++k) {
      const int a = t * kPicks + k;
      acc = fmaf(y[static_cast<std::size_t>(inv[a]) * n + i], w[a], acc);
    }
    moe[static_cast<std::size_t>(t) * n + i] = acc;
  }
}

__global__ void k_argmax_part(const float* __restrict__ x, int n, float* __restrict__ bv, int* __restrict__ bi) {
  __shared__ float sv[32];
  __shared__ int si[32];
  float best = neg_inf();
  int idx = 0x7fffffff;
  for (int i = blockIdx.x * blockDim.x + threadIdx.x; i < n; i += gridDim.x * blockDim.x) {
    const float v = x[i];
    if (v > best || (v == best && i < idx)) {
      best = v;
      idx = i;
    }
  }
#pragma unroll
  for (int o = 16; o > 0; o >>= 1) {
    const float ov = __shfl_xor_sync(0xffffffffu, best, o);
    const int oi = __shfl_xor_sync(0xffffffffu, idx, o);
    if (ov > best || (ov == best && oi < idx)) {
      best = ov;
      idx = oi;
    }
  }
  const int lane = threadIdx.x & 31, warp = threadIdx.x >> 5;
  if (lane == 0) {
    sv[warp] = best;
    si[warp] = idx;
  }
  __syncthreads();
  if (threadIdx.x == 0) {
    for (int w = 1; w < static_cast<int>(blockDim.x >> 5); ++w) {
      if (sv[w] > best || (sv[w] == best && si[w] < idx)) {
        best = sv[w];
        idx = si[w];
      }
    }
    bv[blockIdx.x] = best;
    bi[blockIdx.x] = idx;
  }
}

__global__ void k_argmax_final(const float* __restrict__ bv, const int* __restrict__ bi, int parts, int* __restrict__ out) {
  if (threadIdx.x != 0) return;
  float best = neg_inf();
  int idx = 0x7fffffff;
  for (int k = 0; k < parts; ++k) {
    if (bv[k] > best || (bv[k] == best && bi[k] < idx)) {
      best = bv[k];
      idx = bi[k];
    }
  }
  out[0] = isfinite(best) ? idx : -2;  // -2: the logits are not finite (the serve loop ends with "nonfinite")
}

// Wide dequantisation: one thread per 16 values (Q8_0) or 8 values (BF16); F32 copies 4 at a time.
template <int KIND>
__device__ __forceinline__ void dequant_chunk(const void* __restrict__ w, const __half* __restrict__ s, float* __restrict__ out,
                                              long long chunk, int cols) {
  if constexpr (KIND == 0) {
    const long long i = chunk * 16;
    const long long row = i / cols;
    const int c = static_cast<int>(i % cols);
    const int4 v = *reinterpret_cast<const int4*>(static_cast<const signed char*>(w) + i);
    const float d = __half2float(s[row * (cols / 32) + c / 32]);
    const char4 p[4] = {*reinterpret_cast<const char4*>(&v.x), *reinterpret_cast<const char4*>(&v.y),
                        *reinterpret_cast<const char4*>(&v.z), *reinterpret_cast<const char4*>(&v.w)};
    float4* o = reinterpret_cast<float4*>(out + i);
#pragma unroll
    for (int k = 0; k < 4; ++k) o[k] = make_float4(float(p[k].x) * d, float(p[k].y) * d, float(p[k].z) * d, float(p[k].w) * d);
  } else if constexpr (KIND == 1) {
    const long long i = chunk * 8;
    const uint4 v = *reinterpret_cast<const uint4*>(static_cast<const unsigned short*>(w) + i);
    float4* o = reinterpret_cast<float4*>(out + i);
    o[0] = make_float4(bf16_bits(static_cast<unsigned short>(v.x & 0xffffu)), bf16_bits(static_cast<unsigned short>(v.x >> 16)),
                       bf16_bits(static_cast<unsigned short>(v.y & 0xffffu)), bf16_bits(static_cast<unsigned short>(v.y >> 16)));
    o[1] = make_float4(bf16_bits(static_cast<unsigned short>(v.z & 0xffffu)), bf16_bits(static_cast<unsigned short>(v.z >> 16)),
                       bf16_bits(static_cast<unsigned short>(v.w & 0xffffu)), bf16_bits(static_cast<unsigned short>(v.w >> 16)));
  } else {
    const long long i = chunk * 4;
    *reinterpret_cast<float4*>(out + i) = *reinterpret_cast<const float4*>(static_cast<const float*>(w) + i);
  }
}

template <int KIND>
constexpr int chunk_values() { return KIND == 0 ? 16 : KIND == 1 ? 8 : 4; }

template <int KIND>
__global__ void k_dequant(const void* __restrict__ w, const __half* __restrict__ s, float* __restrict__ out, long long chunks, int cols) {
  const long long c = static_cast<long long>(blockIdx.x) * blockDim.x + threadIdx.x;
  if (c < chunks) dequant_chunk<KIND>(w, s, out, c, cols);
}

__global__ void k_to_half(const float* __restrict__ x, __half* __restrict__ out, long long n) {
  const long long i = static_cast<long long>(blockIdx.x) * blockDim.x + threadIdx.x;
  if (i < n) out[i] = __float2half(x[i]);
}

// The sampler's candidates: each block ranks 1,024 entries in shared memory (bitonic sort, highest first, ties by the
// lower id) and keeps its first `keep`; passes repeat until one block is left.
constexpr int kTopkBlock = 1024;

__device__ __forceinline__ bool ranks_before(float av, int ai, float bv, int bi) { return av > bv || (av == bv && ai < bi); }

__global__ void __launch_bounds__(kTopkBlock) k_topk_pass(const float* __restrict__ in_v, const int* __restrict__ in_i, int count,
                                                         float* __restrict__ out_v, int* __restrict__ out_i, int keep,
                                                         int* __restrict__ nonfinite) {
  __shared__ float sv[kTopkBlock];
  __shared__ int si[kTopkBlock];
  const int t = threadIdx.x;
  const int g = blockIdx.x * kTopkBlock + t;
  float v = neg_inf();
  int id = 0x7fffffff;
  if (g < count) {
    v = in_v[g];
    id = in_i ? in_i[g] : g;
    if (in_i == nullptr && !isfinite(v)) {
      if (nonfinite) *nonfinite = 1;
      if (isnan(v)) v = neg_inf();
    }
  }
  sv[t] = v;
  si[t] = id;
  __syncthreads();
  for (int k = 2; k <= kTopkBlock; k <<= 1) {
    for (int j = k >> 1; j > 0; j >>= 1) {
      const int p = t ^ j;
      if (p > t) {
        const bool first_half = (t & k) == 0;  // this run ends highest first
        const bool swap = first_half ? ranks_before(sv[p], si[p], sv[t], si[t]) : ranks_before(sv[t], si[t], sv[p], si[p]);
        if (swap) {
          const float tv = sv[t];
          const int ti = si[t];
          sv[t] = sv[p];
          si[t] = si[p];
          sv[p] = tv;
          si[p] = ti;
        }
      }
      __syncthreads();
    }
  }
  if (t < keep) {
    out_v[static_cast<std::size_t>(blockIdx.x) * keep + t] = sv[t];
    out_i[static_cast<std::size_t>(blockIdx.x) * keep + t] = si[t];
  }
}

__global__ void k_set_u64(unsigned long long* dst, int index, unsigned long long value) { dst[index] = value; }

__global__ void k_plan_decode(const int* __restrict__ picks, int n_pick, const unsigned long long* __restrict__ ptrtab,
                              unsigned long long* __restrict__ grp_ptr, int* __restrict__ grp_start, int* __restrict__ n_groups,
                              int* __restrict__ ent_dst, int* __restrict__ ent_tok) {
  const int k = static_cast<int>(threadIdx.x);
  if (k < n_pick) {
    grp_ptr[k] = ptrtab[picks[k]];
    grp_start[k] = k;
    ent_dst[k] = k;
    ent_tok[k] = 0;
  }
  if (k == 0) {
    grp_start[n_pick] = n_pick;
    *n_groups = n_pick;
  }
}

unsigned blocks(long long n, int per) { return static_cast<unsigned>((n + per - 1) / per); }

// the split-K pair at `n_split` splits; the keys from (lo, pos) or, when dpos is set, from the device word
void attn_decode_launch(const float* q, const __half* k_cache, const __half* v_cache, int cap, int ring, int window, int lo, int pos,
                        const int* dpos, int n_split, const float* sinks, float scale, float* out, float* scratch, int max_splits,
                        cudaStream_t st) {
  if (n_split < 1 || n_split > max_splits) return;  // the engine sizes scratch for its context
  const dim3 grid(4u, static_cast<unsigned>(n_split));
  k_attn_part<<<grid, 256, 0, st>>>(q, k_cache, v_cache, cap, ring, lo, pos, scratch, max_splits, dpos, window, scale);
  k_attn_merge<<<32u, 128, 0, st>>>(scratch, max_splits, n_split, sinks, out);
}

}  // namespace

void matvec(const Mat& m, const float* x, float* y, int T, cudaStream_t st) {
  const dim3 grid(blocks(m.rows, kRowsPerBlock), static_cast<unsigned>(T));
  switch (m.kind) {
    case WKind::q8: k_matvec<0><<<grid, kMatvecThreads, 0, st>>>(m.w, m.s, x, y, m.rows, m.cols); break;
    case WKind::bf16: k_matvec<1><<<grid, kMatvecThreads, 0, st>>>(m.w, m.s, x, y, m.rows, m.cols); break;
    case WKind::f32: k_matvec<2><<<grid, kMatvecThreads, 0, st>>>(m.w, m.s, x, y, m.rows, m.cols); break;
  }
}

void rmsnorm(const float* x, const float* w, float* out, int rows, int n, float eps, cudaStream_t st) {
  k_rmsnorm<<<static_cast<unsigned>(rows), 256, 0, st>>>(x, w, out, n, eps);
}

void add_rows(const float* x, const float* a, const float* b, float* out, int rows, int n, cudaStream_t st) {
  const long long total = static_cast<long long>(rows) * n;
  k_add_rows<<<blocks(total, 256), 256, 0, st>>>(x, a, b, out, total);
}

void embed(const int* ids, int T, const Mat& table, float* out, cudaStream_t st) {
  const dim3 grid(blocks(table.cols, 256), static_cast<unsigned>(T));
  if (table.kind == WKind::q8) {
    k_embed_q8<<<grid, 256, 0, st>>>(ids, static_cast<const signed char*>(table.w), table.s, table.cols, out);
  } else if (table.kind == WKind::bf16) {
    k_embed_bf16<<<grid, 256, 0, st>>>(ids, static_cast<const unsigned short*>(table.w), table.cols, out);
  }
}

void qkv_post(const QkvPost& in, cudaStream_t st) {
  QkvPost p = in;
  p.theta_scale = powf(p.rope_base, -2.0f / float(kHead));  // host powf, as ggml-cuda's rope launcher
  k_qkv_post<<<dim3(static_cast<unsigned>(p.T), 40u), static_cast<unsigned>(kHead / 2), 0, st>>>(p);
}

int attn_decode_splits(int keys) { return (keys + kSplit - 1) / kSplit; }

int attn_decode_keys(int pos, int window) {
  const int lo = window > 0 ? (pos - window + 1 > 0 ? pos - window + 1 : 0) : 0;
  return pos - lo + 1;
}

void attn_decode(const float* q, const __half* k_cache, const __half* v_cache, int cap, int ring, int window, int pos,
                 const float* sinks, float scale, float* out, float* scratch, int max_splits, cudaStream_t st) {
  const int keys = attn_decode_keys(pos, window);
  attn_decode_launch(q, k_cache, v_cache, cap, ring, window, pos - keys + 1, pos, nullptr, attn_decode_splits(keys), sinks, scale, out,
                     scratch, max_splits, st);
}

void attn_decode_dev(const float* q, const __half* k_cache, const __half* v_cache, int cap, int ring, int window, const int* d_pos,
                     int n_split, const float* sinks, float scale, float* out, float* scratch, int max_splits, cudaStream_t st) {
  attn_decode_launch(q, k_cache, v_cache, cap, ring, window, 0, 0, d_pos, n_split, sinks, scale, out, scratch, max_splits, st);
}

void attn_softmax(const float* s, __half* p, int G, int Ts, int L, int qpos0, int ring, int window, int last_pos, const float* sinks,
                  float scale, cudaStream_t st) {
  k_attn_softmax<<<static_cast<unsigned>(G * Ts), 256, 0, st>>>(s, p, Ts, L, qpos0, ring, window, last_pos, sinks, scale);
}

void router_topk(const float* logits, int T, int* picks, float* weights, int* counts, float w_scale, cudaStream_t st) {
  k_router_topk<<<blocks(T, 4), 128, 0, st>>>(logits, T, picks, weights, counts, w_scale);
}

void swiglu(const float* g, int g_stride, const float* u, int u_stride, float* m, int rows, int n, cudaStream_t st) {
  k_swiglu<<<dim3(blocks(n, 256), static_cast<unsigned>(rows)), 256, 0, st>>>(g, g_stride, u, u_stride, m, n);
}

void moe_combine(const float* y, const float* weights, int n_pick, int n, float* moe, cudaStream_t st) {
  k_moe_combine<<<blocks(n, 256), 256, 0, st>>>(y, weights, n_pick, n, moe);
}

void scatter_combine(const float* y, const int* inv, const float* weights, int T, int n, float* moe, cudaStream_t st) {
  k_scatter_combine<<<dim3(blocks(n, 256), static_cast<unsigned>(T)), 256, 0, st>>>(y, inv, weights, n, moe);
}

void argmax(const float* x, int n, int* out, void* scratch, cudaStream_t st) {
  auto* bv = static_cast<float*>(scratch);
  auto* bi = reinterpret_cast<int*>(bv + 1024);
  const int parts = 256;
  k_argmax_part<<<parts, 256, 0, st>>>(x, n, bv, bi);
  k_argmax_final<<<1, 32, 0, st>>>(bv, bi, parts, out);
}

void dequant(const Mat& m, float* out, cudaStream_t st) {
  const long long total = static_cast<long long>(m.rows) * m.cols;
  switch (m.kind) {
    case WKind::q8: {
      const long long n = total / chunk_values<0>();
      k_dequant<0><<<blocks(n, 256), 256, 0, st>>>(m.w, m.s, out, n, m.cols);
      break;
    }
    case WKind::bf16: {
      const long long n = total / chunk_values<1>();
      k_dequant<1><<<blocks(n, 256), 256, 0, st>>>(m.w, m.s, out, n, m.cols);
      break;
    }
    case WKind::f32: {
      const long long n = total / chunk_values<2>();
      k_dequant<2><<<blocks(n, 256), 256, 0, st>>>(m.w, m.s, out, n, m.cols);
      break;
    }
  }
}

void to_half(const float* x, __half* out, long long n, cudaStream_t st) { k_to_half<<<blocks(n, 256), 256, 0, st>>>(x, out, n); }

std::size_t topk_scratch_bytes(int n) {
  const std::size_t first = blocks(n, kTopkBlock) * static_cast<std::size_t>(kTopkMax);
  return 2 * first * (sizeof(float) + sizeof(int)) + 256;
}

void topk(const float* x, int n, int k, float* vals, int* ids, int* nonfinite, void* scratch, cudaStream_t st) {
  if (n <= 0 || k <= 0 || k > kTopkMax) return;
  const std::size_t first = blocks(n, kTopkBlock) * static_cast<std::size_t>(kTopkMax);
  auto* base = static_cast<unsigned char*>(scratch);
  float* v[2] = {reinterpret_cast<float*>(base), reinterpret_cast<float*>(base + first * sizeof(float))};
  int* i[2] = {reinterpret_cast<int*>(base + 2 * first * sizeof(float)), reinterpret_cast<int*>(base + 2 * first * sizeof(float) + first * sizeof(int))};
  int count = n;
  unsigned nb = blocks(count, kTopkBlock);
  k_topk_pass<<<nb, kTopkBlock, 0, st>>>(x, nullptr, count, v[0], i[0], kTopkMax, nonfinite);
  int cur = 0;
  while (nb > 1) {
    count = static_cast<int>(nb) * kTopkMax;
    nb = blocks(count, kTopkBlock);
    k_topk_pass<<<nb, kTopkBlock, 0, st>>>(v[cur], i[cur], count, v[1 - cur], i[1 - cur], kTopkMax, nullptr);
    cur = 1 - cur;
  }
  cudaMemcpyAsync(vals, v[cur], sizeof(float) * static_cast<std::size_t>(k), cudaMemcpyDeviceToDevice, st);
  cudaMemcpyAsync(ids, i[cur], sizeof(int) * static_cast<std::size_t>(k), cudaMemcpyDeviceToDevice, st);
}

void set_u64(unsigned long long* dst, int index, unsigned long long value, cudaStream_t st) { k_set_u64<<<1, 1, 0, st>>>(dst, index, value); }

void plan_decode(const int* picks, int n_pick, const unsigned long long* ptrtab, unsigned long long* grp_ptr, int* grp_start,
                 int* n_groups, int* ent_dst, int* ent_tok, cudaStream_t st) {
  k_plan_decode<<<1, 32, 0, st>>>(picks, n_pick, ptrtab, grp_ptr, grp_start, n_groups, ent_dst, ent_tok);
}

}  // namespace flk
