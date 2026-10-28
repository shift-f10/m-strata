// strata-unified-engine: Gemma-4-26B-A4B attention kernels (see gemma4_attn.cuh).
//
// A faithful port of the attention device code from the 26/26-verified reference engine
// engines/gemma4/src/g4_kernels.cu (read-only parity oracle). The device math is reproduced
// bit-for-bit; only the host launchers changed shape: `void* stream` (cast to cudaStream_t) and
// uint16_t* f16 caches (reinterpret_cast to __half*). Per-piece provenance is cited at each kernel.
#include "gemma4_attn.cuh"

#include <cuda_fp16.h>
#include <cuda_runtime.h>

#include <cmath>
#include <cstddef>

namespace u::gemma4 {
namespace {

[[maybe_unused]] constexpr int kWarp = 32;

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
// g4_kernels.cu block_sum (28-37).
__device__ float block_sum(float v, float* red) {
  const int lane = threadIdx.x & 31, warp = threadIdx.x >> 5, warps = blockDim.x >> 5;
  v = warp_sum(v);
  __syncthreads();
  if (lane == 0) red[warp] = v;
  __syncthreads();
  float t = lane < warps ? red[lane] : 0.0f;
  t = warp_sum(t);
  return t;
}

// One block per (token, head), hd/2 threads: thread i owns elements i and i + hd/2.
// Port of g4_kernels.cu k_qkv_post (217-271). q/k weighted RMS + v unweighted RMS, dual-RoPE
// (full layers divide theta by freq_factors — the proportional rope), then the f16 cache write.
__global__ void k_qkv_post_impl(QkvPost p) {
  __shared__ float red[32];
  const int t = blockIdx.x, h = blockIdx.y;
  const int half = p.hd / 2, i = threadIdx.x;
  const int pos = (p.pos0_dev ? *p.pos0_dev : p.pos0) + t;  // DEVICE pos for the captured decode graph
  const float* src;
  const float* w = nullptr;
  bool rope = true;
  int kind;  // 0 q, 1 k, 2 v
  int hh;
  if (h < p.n_head) {
    kind = 0; hh = h;
    src = p.q_raw + (static_cast<std::size_t>(t) * p.n_head + hh) * p.hd;
    w = p.q_norm;
  } else if (h < p.n_head + p.n_kv) {
    kind = 1; hh = h - p.n_head;
    src = p.k_raw + (static_cast<std::size_t>(t) * p.n_kv + hh) * p.hd;
    w = p.k_norm;
  } else {
    kind = 2; hh = h - p.n_head - p.n_kv;
    src = p.v_raw + (static_cast<std::size_t>(t) * p.n_kv + hh) * p.hd;
    rope = false;
  }
  float a = src[i], b = src[i + half];
  const float inv = rsqrtf(block_sum(a * a + b * b, red) / float(p.hd) + p.eps);
  a *= inv;
  b *= inv;
  if (w) {
    a *= w[i];
    b *= w[i + half];
  }
  if (rope) {
    float theta = float(pos) * powf(p.theta_scale, float(i));
    if (p.freq_factors) theta /= p.freq_factors[i];
    float sn, cs;
    sincosf(theta, &sn, &cs);
    const float a2 = a * cs - b * sn, b2 = a * sn + b * cs;
    a = a2;
    b = b2;
  }
  if (kind == 0) {
    const std::size_t o = (static_cast<std::size_t>(t) * p.n_head + hh) * p.hd;
    p.q_out[o + i] = a;
    p.q_out[o + i + half] = b;
    if (p.q16) {
      reinterpret_cast<__half*>(p.q16)[o + i] = __float2half(a);
      reinterpret_cast<__half*>(p.q16)[o + i + half] = __float2half(b);
    }
  } else {
    const int slot = p.ring ? pos % p.ring : pos;
    __half* c = reinterpret_cast<__half*>(kind == 1 ? p.k_cache : p.v_cache) +
                (static_cast<std::size_t>(hh) * p.cap + slot) * p.hd;
    c[i] = __float2half(a);
    c[i + half] = __float2half(b);
  }
}

constexpr int kSplit = 128;  // keys per decode-attention block (g4_kernels.cu:273)

// grid (n_kv, splits), 256 threads; G query heads share KV head blockIdx.x. Port of
// g4_kernels.cu k_attn_part (276-355): per split, the dot products over its key slice, the
// online-softmax stats, and the unnormalized value accumulation into `scratch`.
template <int HD, int G>
__global__ void __launch_bounds__(256) k_attn_part(const float* __restrict__ q, const __half* __restrict__ kc,
                                                   const __half* __restrict__ vc, int cap, int ring, int window,
                                                   const int* __restrict__ pos_dev, float* __restrict__ scratch,
                                                   int splits) {
  __shared__ float qs[G][HD];
  __shared__ float sc[G][kSplit];
  __shared__ float stat[G][2];
  // The query position is read on the DEVICE so one captured graph serves every decode step; lo/hi are
  // recomputed here each replay and the key loop below clamps to [lo, hi], so a split past the current
  // position does no work and writes neutral (m=-inf, l=0, o=0) partials the merge discards.
  const int pos = *pos_dev;
  const int lo = window > 0 ? (pos - window + 1 > 0 ? pos - window + 1 : 0) : 0;
  const int hi = pos;
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
      if (lane == 0) sc[gi][kk] = v;
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

// Port of g4_kernels.cu k_attn_merge (357-376): combine the split partials by the online-softmax
// rescale exp(m_i - M). Identical in spirit to u::k::attn_merge.
template <int HD>
__global__ void k_attn_merge(const float* __restrict__ scratch, int splits, int n_split, float* __restrict__ out) {
  const int h = blockIdx.x;
  const float* base = scratch + static_cast<std::size_t>(h) * splits * (HD + 2);
  float M = neg_inf();
  for (int s = 0; s < n_split; ++s) M = fmaxf(M, base[static_cast<std::size_t>(s) * (HD + 2) + HD]);
  float L = 0.0f;
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

// Port of g4_kernels.cu k_attn_softmax (378-410): the windowed/full masked softmax for the
// batched-prefill fast-path.
__global__ void k_attn_softmax(const float* __restrict__ s, __half* __restrict__ p, int Ts, int L, int qpos0, int ring,
                               int window, int last_pos) {
  __shared__ float red[32];
  const int row = blockIdx.x;
  const int t = row % Ts;
  const int qp = qpos0 + t;
  const float* sr = s + static_cast<std::size_t>(row) * L;
  __half* pr = p + static_cast<std::size_t>(row) * L;
  auto valid = [&](int j) {
    int kp = j;
    if (ring) kp = last_pos - (((last_pos - j) % ring) + ring) % ring;
    return kp >= 0 && kp <= qp && (window == 0 || qp - kp < window);
  };
  float m = neg_inf();
  for (int j = threadIdx.x; j < L; j += blockDim.x)
    if (valid(j)) m = fmaxf(m, sr[j]);
  {
    const int lane = threadIdx.x & 31, warp = threadIdx.x >> 5, warps = blockDim.x >> 5;
    m = warp_max(m);
    __syncthreads();
    if (lane == 0) red[warp] = m;
    __syncthreads();
    float t2 = lane < warps ? red[lane] : neg_inf();
    m = warp_max(t2);
  }
  float l = 0.0f;
  for (int j = threadIdx.x; j < L; j += blockDim.x)
    if (valid(j)) l += expf(sr[j] - m);
  l = block_sum(l, red);
  const float inv = 1.0f / l;
  for (int j = threadIdx.x; j < L; j += blockDim.x) pr[j] = __float2half(valid(j) ? expf(sr[j] - m) * inv : 0.0f);
}

}  // namespace

void qkv_post(const QkvPost& in, void* stream) {
  QkvPost p = in;
  // host powf, as ggml-cuda's rope launcher (g4_kernels.cu qkv_post, 712-717).
  p.theta_scale = powf(p.rope_base, -2.0f / float(p.hd));
  k_qkv_post_impl<<<dim3(static_cast<unsigned>(p.T), static_cast<unsigned>(p.n_head + 2 * p.n_kv)),
                    static_cast<unsigned>(p.hd / 2), 0, static_cast<cudaStream_t>(stream)>>>(p);
}

int attn_decode_splits(int keys) { return (keys + kSplit - 1) / kSplit; }

void attn_decode(const float* q, const std::uint16_t* k_cache, const std::uint16_t* v_cache, int hd, int n_head,
                 int n_kv, int cap, int ring, int window, const int* pos_dev, int n_split, float* out,
                 float* scratch, int max_splits, void* stream) {
  const auto st = static_cast<cudaStream_t>(stream);
  const __half* kc = reinterpret_cast<const __half*>(k_cache);
  const __half* vc = reinterpret_cast<const __half*>(v_cache);
  if (n_split < 1) n_split = 1;
  if (n_split > max_splits) n_split = max_splits;  // scratch is sized for max_splits
  // FIXED grid (n_split blocks) so the launch is identical every decode step and a CUDA graph can be
  // captured once; k_attn_part reads the live position from pos_dev and clamps each split. The merge
  // over n_split partials is bitwise-identical whether or not trailing splits were empty.
  const dim3 grid(static_cast<unsigned>(n_kv), static_cast<unsigned>(n_split));
  const int G = n_head / n_kv;
  if (hd == 512 && G == 8) {
    k_attn_part<512, 8><<<grid, 256, 0, st>>>(q, kc, vc, cap, ring, window, pos_dev, scratch, max_splits);
    k_attn_merge<512><<<static_cast<unsigned>(n_head), 256, 0, st>>>(scratch, max_splits, n_split, out);
  } else if (hd == 256 && G == 2) {
    k_attn_part<256, 2><<<grid, 256, 0, st>>>(q, kc, vc, cap, ring, window, pos_dev, scratch, max_splits);
    k_attn_merge<256><<<static_cast<unsigned>(n_head), 256, 0, st>>>(scratch, max_splits, n_split, out);
  }
  // TODO(box): only the two gemma4 (hd,G) pairs are instantiated. If a future gemma4 variant
  // changes the head widths, add the matching template instantiation (g4_kernels.cu:728-734).
}

void attn_softmax(const float* s, std::uint16_t* p, int G, int Ts, int L, int qpos0, int ring, int window,
                  int last_pos, void* stream) {
  k_attn_softmax<<<static_cast<unsigned>(G * Ts), 256, 0, static_cast<cudaStream_t>(stream)>>>(
      s, reinterpret_cast<__half*>(p), Ts, L, qpos0, ring, window, last_pos);
}

}  // namespace u::gemma4
