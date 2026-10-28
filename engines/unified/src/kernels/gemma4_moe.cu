// strata-unified-engine: Gemma-4-26B-A4B ZERO-HOST-SYNC on-device MoE expert dispatch (see
// gemma4_moe.cuh). The kernels are a byte-faithful port of engines/gemma4/src/g4_kernels.cu (the
// read-only 26/26-verified parity oracle): the Q8_0 dot dequantises the weights against the f32
// activation (NOT the unified MMVQ Q8_1 path), the BF16 dot widens the stored uint16 to f32, and the
// GeGLU / combine use the oracle's exact fma order and gelu_tanh constants. The host side ports
// g4_engine.cu's repack_q8 / expert_layout / place_experts / rebalance.
#include "gemma4_moe.cuh"

#include <cuda_runtime.h>

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace u::gemma4 {
namespace {

// ================================ device kernels (g4_kernels.cu) ================================

constexpr int kWarp = 32;
constexpr int kRowsPerWarp = 2;
constexpr int kMatvecThreads = 256;
constexpr int kRowsPerBlock = kRowsPerWarp * (kMatvecThreads / kWarp);  // 16

unsigned blocks(long long n, int per) { return static_cast<unsigned>((n + per - 1) / per); }

__device__ __forceinline__ float warp_sum(float v) {
#pragma unroll
  for (int o = 16; o > 0; o >>= 1) v += __shfl_xor_sync(0xffffffffu, v, o);
  return v;
}

__device__ __forceinline__ float bf16_bits(unsigned short b) {
  return __uint_as_float(static_cast<unsigned int>(b) << 16);
}

// 16 Q8_0 int8 weights (one int4 load) dotted against 16 f32 activations; the fma order is the
// oracle's (g4_kernels.cu:41).
__device__ __forceinline__ float dot16_q8(const int4 v, const float4 a, const float4 b, const float4 c,
                                          const float4 d) {
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

// 8 BF16 weights (one uint4 load) dotted against 8 f32 activations (g4_kernels.cu:54).
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

// Two rows per warp. acc[r] for rows row0, row0+1 (g4_kernels.cu:72).
template <int KIND>
__device__ __forceinline__ void rows_dot(const void* __restrict__ w, const __half* __restrict__ s,
                                         const float* __restrict__ x, int cols, int row0, int nr, int lane,
                                         float acc[kRowsPerWarp]) {
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
  } else {
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
  }
}

// grid(rows_blocks, n_pick), 256 threads; block (row0, k) reads table[picks[k]] (g4_kernels.cu:136).
template <int KIND>
__global__ void __launch_bounds__(kMatvecThreads)
    k_matvec_experts(int which, const ExpertPtr* __restrict__ table, const int* __restrict__ picks,
                     const float* __restrict__ x, int x_stride, float* __restrict__ y, int rows, int cols) {
  const int warp = threadIdx.x >> 5, lane = threadIdx.x & 31;
  const int row0 = blockIdx.x * kRowsPerBlock + warp * kRowsPerWarp;
  if (row0 >= rows) return;
  const int nr = min(kRowsPerWarp, rows - row0);
  const int k = blockIdx.y;
  const ExpertPtr e = table[picks[k]];
  const void* w = which == 0 ? e.gu_w : e.dn_w;
  const __half* s = which == 0 ? e.gu_s : e.dn_s;
  float acc[kRowsPerWarp] = {0.0f, 0.0f};
  rows_dot<KIND>(w, s, x + static_cast<std::size_t>(k) * x_stride, cols, row0, nr, lane, acc);
#pragma unroll
  for (int r = 0; r < kRowsPerWarp; ++r) acc[r] = warp_sum(acc[r]);
  if (lane == 0) {
    for (int r = 0; r < nr; ++r) y[static_cast<std::size_t>(k) * rows + row0 + r] = acc[r];
  }
}

__device__ __forceinline__ float gelu_tanh(float x) {
  return 0.5f * x * (1.0f + tanhf(0.79788456080286535587989211986876f * x * (1.0f + 0.044715f * x * x)));
}

__global__ void k_geglu(const float* __restrict__ g, int gs, const float* __restrict__ u, int us,
                        float* __restrict__ m, int n) {
  const int r = blockIdx.y;
  const int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= n) return;
  m[static_cast<std::size_t>(r) * n + i] =
      gelu_tanh(g[static_cast<std::size_t>(r) * gs + i]) * u[static_cast<std::size_t>(r) * us + i];
}

__global__ void k_moe_combine(const float* __restrict__ y, const float* __restrict__ w, const int* __restrict__ picks,
                              const float* __restrict__ ds, int n_pick, int n, float* __restrict__ moe) {
  const int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= n) return;
  float acc = 0.0f;
  for (int k = 0; k < n_pick; ++k) acc = fmaf(y[static_cast<std::size_t>(k) * n + i], w[k] * ds[picks[k]], acc);
  moe[i] = acc;
}

// counts[picks[i]] += 1 for i < n_pick (the per-token routing tally; mirrors the atomicAdd
// g4k::k_router_topk folds into its counts output, kept separate so u::k::router_topk is untouched).
__global__ void k_count_picks(const int* __restrict__ picks, int n_pick, int* __restrict__ counts) {
  const int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i < n_pick) atomicAdd(&counts[picks[i]], 1);
}

// ---- BATCHED PREFILL device kernels (byte-faithful ports of g4_kernels.cu) ----

// Wide dequantisation of one planar expert matrix: one thread per 16 Q8_0 values (int4 load, a 16-value
// run never crosses a 32-value block) or 8 BF16 values (uint4). Byte-for-byte g4_kernels.cu dequant_chunk
// (543-572); the Q8_0 value is float(int8)*__half2float(scale), identical to the decode dot's per-value
// arithmetic, so the bucketed cublasSgemm reproduces the decode MoE's result.
template <int KIND>
__device__ __forceinline__ void dequant_chunk(const void* __restrict__ w, const __half* __restrict__ s,
                                             float* __restrict__ out, long long chunk, int cols) {
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
    for (int k = 0; k < 4; ++k)
      o[k] = make_float4(float(p[k].x) * d, float(p[k].y) * d, float(p[k].z) * d, float(p[k].w) * d);
  } else {
    const long long i = chunk * 8;
    const uint4 v = *reinterpret_cast<const uint4*>(static_cast<const unsigned short*>(w) + i);
    float4* o = reinterpret_cast<float4*>(out + i);
    o[0] = make_float4(bf16_bits(static_cast<unsigned short>(v.x & 0xffffu)), bf16_bits(static_cast<unsigned short>(v.x >> 16)),
                       bf16_bits(static_cast<unsigned short>(v.y & 0xffffu)), bf16_bits(static_cast<unsigned short>(v.y >> 16)));
    o[1] = make_float4(bf16_bits(static_cast<unsigned short>(v.z & 0xffffu)), bf16_bits(static_cast<unsigned short>(v.z >> 16)),
                       bf16_bits(static_cast<unsigned short>(v.w & 0xffffu)), bf16_bits(static_cast<unsigned short>(v.w >> 16)));
  }
}

template <int KIND>
__global__ void k_dequant_expert(const ExpertPtr* __restrict__ table, int e, int which, float* __restrict__ out,
                                long long chunks, int cols) {
  const long long c = static_cast<long long>(blockIdx.x) * blockDim.x + threadIdx.x;
  if (c >= chunks) return;
  const ExpertPtr p = table[e];
  dequant_chunk<KIND>(which == 0 ? p.gu_w : p.dn_w, which == 0 ? p.gu_s : p.dn_s, out, c, cols);
}

// out[r*n + i] = x[(order[r]/n_used)*n + i]: gather each gathered row's source token. g4k::k_gather_rows.
__global__ void k_gather_rows(const float* __restrict__ x, const int* __restrict__ order, int n, int n_used,
                             float* __restrict__ out) {
  const int r = blockIdx.y;
  const int src = order[r] / n_used;
  for (int i = blockIdx.x * blockDim.x + threadIdx.x; i < n; i += gridDim.x * blockDim.x)
    out[static_cast<std::size_t>(r) * n + i] = x[static_cast<std::size_t>(src) * n + i];
}

// moe[t*n+i] = sum_{k<n_used} y[inv[t*n_used+k]*n+i] * w[t*n_used+k] * ds[picks[t*n_used+k]].
// g4k::k_scatter_combine (the fma order matches k_moe_combine; the per-expert down scale is folded here).
__global__ void k_scatter_combine(const float* __restrict__ y, const int* __restrict__ inv,
                                 const float* __restrict__ w, const int* __restrict__ picks,
                                 const float* __restrict__ ds, int n, int n_used, float* __restrict__ moe) {
  const int t = blockIdx.y;
  for (int i = blockIdx.x * blockDim.x + threadIdx.x; i < n; i += gridDim.x * blockDim.x) {
    float acc = 0.0f;
    for (int k = 0; k < n_used; ++k) {
      const int a = t * n_used + k;
      acc = fmaf(y[static_cast<std::size_t>(inv[a]) * n + i], w[a] * ds[picks[a]], acc);
    }
    moe[static_cast<std::size_t>(t) * n + i] = acc;
  }
}

// ================================ host helpers (g4_engine.cu) ================================

constexpr std::size_t kAlign = 256;
std::size_t align_up(std::size_t n) { return (n + kAlign - 1) / kAlign * kAlign; }

// Bytes of one weight row of `cols` values.
std::size_t row_bytes(WKind k, std::size_t cols) {
  return k == WKind::q8 ? cols / 32 * 34 : cols * 2;  // Q8_0: nb blocks of 34; BF16: 2 bytes/value
}

// GGUF Q8_0 rows ([fp16 d][32 x int8] blocks) into the engine's int8 plane and fp16 scale plane
// (g4_engine.cu:33). Byte-for-byte the oracle so the device dot is bit-identical.
void repack_q8(const std::uint8_t* src, std::size_t rows, std::size_t cols, std::int8_t* q, std::uint16_t* s) {
  const std::size_t nb = cols / 32;
  for (std::size_t r = 0; r < rows; ++r) {
    for (std::size_t b = 0; b < nb; ++b) {
      const std::uint8_t* blk = src + (r * nb + b) * 34;
      std::memcpy(&s[r * nb + b], blk, 2);
      std::memcpy(&q[r * cols + b * 32], blk + 2, 32);
    }
  }
}

[[nodiscard]] bool kind_of(int ggml_type, WKind& out) {
  if (ggml_type == 8) { out = WKind::q8; return true; }   // GGML_TYPE_Q8_0
  if (ggml_type == 30) { out = WKind::bf16; return true; }  // GGML_TYPE_BF16
  return false;                                             // F32 / k-quant experts: a box extension
}

}  // namespace

// ================================ kernel launchers ================================

void matvec_experts(WKind kind, int which, int rows, int cols, const ExpertPtr* table, const int* picks,
                    int n_pick, const float* x, int x_stride, float* y, void* stream) {
  const dim3 grid(blocks(rows, kRowsPerBlock), static_cast<unsigned>(n_pick));
  const auto st = static_cast<cudaStream_t>(stream);
  if (kind == WKind::q8) {
    k_matvec_experts<0><<<grid, kMatvecThreads, 0, st>>>(which, table, picks, x, x_stride, y, rows, cols);
  } else {
    k_matvec_experts<1><<<grid, kMatvecThreads, 0, st>>>(which, table, picks, x, x_stride, y, rows, cols);
  }
}

void geglu_experts(const float* g, int g_stride, const float* u, int u_stride, float* m, int rows, int n,
                   void* stream) {
  k_geglu<<<dim3(blocks(n, 256), static_cast<unsigned>(rows)), 256, 0, static_cast<cudaStream_t>(stream)>>>(
      g, g_stride, u, u_stride, m, n);
}

void moe_combine(const float* y, const float* weights, const int* picks, const float* down_scale, int n_pick,
                 int n, float* moe, void* stream) {
  k_moe_combine<<<blocks(n, 256), 256, 0, static_cast<cudaStream_t>(stream)>>>(y, weights, picks, down_scale,
                                                                               n_pick, n, moe);
}

void count_picks(const int* picks, int n_pick, int* counts, void* stream) {
  if (n_pick <= 0) return;
  // Grid over n_pick (not <<<1, n_pick>>>): the batched-prefill chunk tallies T*n_exp_used picks at once,
  // which can exceed the 1024 thread/block limit. Decode (n_pick = 8) still runs in a single block with
  // the same atomicAdds, so its result is unchanged.
  k_count_picks<<<blocks(n_pick, 256), 256, 0, static_cast<cudaStream_t>(stream)>>>(picks, n_pick, counts);
}

void gather_rows(const float* x, const int* order, int count, int n, int n_used, float* out, void* stream) {
  if (count <= 0) return;
  k_gather_rows<<<dim3(blocks(n, 256), static_cast<unsigned>(count)), 256, 0, static_cast<cudaStream_t>(stream)>>>(
      x, order, n, n_used, out);
}

void dequant_expert(WKind kind, int which, int rows, int cols, const ExpertPtr* table, int e, float* out,
                    void* stream) {
  const long long total = static_cast<long long>(rows) * cols;
  const auto st = static_cast<cudaStream_t>(stream);
  if (kind == WKind::q8) {
    const long long n = total / 16;  // one thread per 16 Q8_0 values
    k_dequant_expert<0><<<blocks(n, 256), 256, 0, st>>>(table, e, which, out, n, cols);
  } else {
    const long long n = total / 8;  // one thread per 8 BF16 values
    k_dequant_expert<1><<<blocks(n, 256), 256, 0, st>>>(table, e, which, out, n, cols);
  }
}

void scatter_combine(const float* y, const int* inv, const float* weights, const int* picks,
                     const float* down_scale, int T, int n, int n_used, float* moe, void* stream) {
  if (T <= 0) return;
  k_scatter_combine<<<dim3(blocks(n, 256), static_cast<unsigned>(T)), 256, 0, static_cast<cudaStream_t>(stream)>>>(
      y, inv, weights, picks, down_scale, n, n_used, moe);
}

// ================================ Gemma4Experts ================================

Gemma4Experts::~Gemma4Experts() { close(); }

ExpertPtr Gemma4Experts::expert_ptr(const Layer& L, const std::uint8_t* blob) const {
  ExpertPtr p{};
  p.gu_w = blob + L.gu_w;
  p.dn_w = blob + L.dn_w;
  p.gu_s = L.gu_kind == WKind::q8 ? reinterpret_cast<const __half*>(blob + L.gu_s) : nullptr;
  p.dn_s = L.dn_kind == WKind::q8 ? reinterpret_cast<const __half*>(blob + L.dn_s) : nullptr;
  return p;
}

bool Gemma4Experts::open(int n_layers, int n_expert, int n_embd, int n_ff_exp, int n_exp_used, std::string& err) {
  if (opened_) { err = "gemma4 experts: already open"; return false; }
  if (n_layers <= 0 || n_expert <= 0 || n_embd <= 0 || n_ff_exp <= 0 || n_exp_used <= 0) {
    err = "gemma4 experts: bad geometry (layers/experts/embd/ff/used must be > 0)";
    return false;
  }
  if (n_embd % 32 != 0 || n_ff_exp % 32 != 0) {
    err = "gemma4 experts: n_embd and n_ff_exp must be multiples of 32 (Q8_0 blocks)";
    return false;
  }
  n_layers_ = n_layers;
  n_expert_ = n_expert;
  n_embd_ = n_embd;
  n_ff_exp_ = n_ff_exp;
  n_exp_used_ = n_exp_used;
  gu_rows_ = 2 * n_ff_exp;  // fused gate rows then up rows
  layers_.clear();
  layers_.resize(static_cast<std::size_t>(n_layers));

  const std::size_t ne = static_cast<std::size_t>(n_layers_) * static_cast<std::size_t>(n_expert_);
  if (cudaMalloc(reinterpret_cast<void**>(&counts_dev_), ne * sizeof(int)) != cudaSuccess) {
    counts_dev_ = nullptr;
    err = "gemma4 experts: cudaMalloc for the routing counts failed";
    return false;
  }
  if (cudaMemset(counts_dev_, 0, ne * sizeof(int)) != cudaSuccess) {
    err = "gemma4 experts: cudaMemset for the routing counts failed";
    return false;
  }
  counts_host_.assign(ne, 0);

  const std::size_t gu = static_cast<std::size_t>(n_exp_used_) * static_cast<std::size_t>(gu_rows_);
  const std::size_t mx = static_cast<std::size_t>(n_exp_used_) * static_cast<std::size_t>(n_ff_exp_);
  const std::size_t yg = static_cast<std::size_t>(n_exp_used_) * static_cast<std::size_t>(n_embd_);
  const bool ok = cudaMalloc(reinterpret_cast<void**>(&gu_), gu * sizeof(float)) == cudaSuccess &&
                  cudaMalloc(reinterpret_cast<void**>(&mx_), mx * sizeof(float)) == cudaSuccess &&
                  cudaMalloc(reinterpret_cast<void**>(&yg_), yg * sizeof(float)) == cudaSuccess;
  if (!ok) {
    err = "gemma4 experts: cudaMalloc for the decode scratch (gu/mx/yg) failed";
    close();
    return false;
  }
  opened_ = true;
  return true;
}

bool Gemma4Experts::load_layer(int layer, const std::uint8_t* gu_src, int gu_type, const std::uint8_t* dn_src,
                               int dn_type, const float* down_scale, std::string& err) {
  if (!opened_) { err = "gemma4 experts: load_layer before open"; return false; }
  if (layer < 0 || layer >= n_layers_) { err = "gemma4 experts: load_layer bad layer"; return false; }
  if (gu_src == nullptr || dn_src == nullptr || down_scale == nullptr) {
    err = "gemma4 experts: load_layer null source";
    return false;
  }
  Layer& L = layers_[static_cast<std::size_t>(layer)];
  if (!kind_of(gu_type, L.gu_kind) || !kind_of(dn_type, L.dn_kind)) {
    err = "gemma4 experts: expert tensor is not Q8_0 or BF16 (F32/k-quant experts are a box extension)";
    return false;
  }

  // Blob layout: [gate_up weights][gate_up scales][down weights][down scales], each 256-aligned; a
  // BF16 matrix has no scale plane (g4_engine.cu expert_layout).
  const std::size_t gu_n = static_cast<std::size_t>(gu_rows_) * static_cast<std::size_t>(n_embd_);
  const std::size_t dn_n = static_cast<std::size_t>(n_embd_) * static_cast<std::size_t>(n_ff_exp_);
  std::size_t at = 0;
  L.gu_w = at;
  at = align_up(at + gu_n * (L.gu_kind == WKind::q8 ? 1u : 2u));
  L.gu_s = at;
  if (L.gu_kind == WKind::q8) at = align_up(at + gu_n / 32 * 2);
  L.dn_w = at;
  at = align_up(at + dn_n * (L.dn_kind == WKind::q8 ? 1u : 2u));
  L.dn_s = at;
  if (L.dn_kind == WKind::q8) at = align_up(at + dn_n / 32 * 2);
  L.blob_bytes = at;

  void* host = nullptr;
  const std::size_t arena = L.blob_bytes * static_cast<std::size_t>(n_expert_);
  if (cudaHostAlloc(&host, arena, cudaHostAllocMapped | cudaHostAllocPortable) != cudaSuccess) {
    err = "gemma4 experts: cudaHostAlloc for a layer's mapped expert arena failed";
    return false;
  }
  L.host = static_cast<std::uint8_t*>(host);
  void* alias = nullptr;
  if (cudaHostGetDevicePointer(&alias, host, 0) != cudaSuccess) {
    err = "gemma4 experts: cudaHostGetDevicePointer failed (the device lacks mapped-host support)";
    return false;
  }
  L.alias = static_cast<std::uint8_t*>(alias);

  // Per-expert source stride in the mmap: the fused gate_up tensor and the down tensor are
  // [n_expert][rows, cols] contiguous.
  const std::size_t gu_per = row_bytes(L.gu_kind, static_cast<std::size_t>(n_embd_)) * static_cast<std::size_t>(gu_rows_);
  const std::size_t dn_per = row_bytes(L.dn_kind, static_cast<std::size_t>(n_ff_exp_)) * static_cast<std::size_t>(n_embd_);
  for (int e = 0; e < n_expert_; ++e) {
    std::uint8_t* blob = L.host + L.blob_bytes * static_cast<std::size_t>(e);
    const std::uint8_t* gu_e = gu_src + gu_per * static_cast<std::size_t>(e);
    const std::uint8_t* dn_e = dn_src + dn_per * static_cast<std::size_t>(e);
    if (L.gu_kind == WKind::q8) {
      repack_q8(gu_e, static_cast<std::size_t>(gu_rows_), static_cast<std::size_t>(n_embd_),
                reinterpret_cast<std::int8_t*>(blob + L.gu_w), reinterpret_cast<std::uint16_t*>(blob + L.gu_s));
    } else {
      std::memcpy(blob + L.gu_w, gu_e, gu_per);
    }
    if (L.dn_kind == WKind::q8) {
      repack_q8(dn_e, static_cast<std::size_t>(n_embd_), static_cast<std::size_t>(n_ff_exp_),
                reinterpret_cast<std::int8_t*>(blob + L.dn_w), reinterpret_cast<std::uint16_t*>(blob + L.dn_s));
    } else {
      std::memcpy(blob + L.dn_w, dn_e, dn_per);
    }
  }
  L.down_scale.assign(static_cast<std::size_t>(n_expert_), 0.0f);
  std::memcpy(L.down_scale.data(), down_scale, static_cast<std::size_t>(n_expert_) * sizeof(float));
  return true;
}

bool Gemma4Experts::finalize(std::int64_t vram_budget_bytes, std::string& err) {
  if (!opened_) { err = "gemma4 experts: finalize before open"; return false; }
  for (const auto& L : layers_)
    if (L.host == nullptr) { err = "gemma4 experts: finalize before every layer was loaded"; return false; }

  // The device ffn_down_exps.scale (every layer's [n_expert], one array).
  const std::size_t ne = static_cast<std::size_t>(n_layers_) * static_cast<std::size_t>(n_expert_);
  if (cudaMalloc(reinterpret_cast<void**>(&down_scale_dev_), ne * sizeof(float)) != cudaSuccess) {
    down_scale_dev_ = nullptr;
    err = "gemma4 experts: cudaMalloc for the device down scales failed";
    return false;
  }
  for (int l = 0; l < n_layers_; ++l) {
    if (cudaMemcpy(down_scale_dev_ + static_cast<std::size_t>(l) * static_cast<std::size_t>(n_expert_),
                   layers_[static_cast<std::size_t>(l)].down_scale.data(),
                   static_cast<std::size_t>(n_expert_) * sizeof(float), cudaMemcpyHostToDevice) != cudaSuccess) {
      err = "gemma4 experts: down-scale upload failed";
      return false;
    }
  }

  // VRAM resident slots: S experts per layer within the budget (per_slot = the sum of the layers'
  // blob sizes, since residency is S experts in EVERY layer). Best-effort: a short alloc => S = 0.
  std::size_t per_slot = 0;
  for (const auto& L : layers_) per_slot += L.blob_bytes;
  int S = 0;
  if (vram_budget_bytes > 0 && per_slot > 0) {
    std::size_t s = static_cast<std::size_t>(vram_budget_bytes) / per_slot;
    if (s > static_cast<std::size_t>(n_expert_)) s = static_cast<std::size_t>(n_expert_);
    S = static_cast<int>(s);
  }
  if (S > 0) {
    bool slots_ok = true;
    for (auto& L : layers_) {
      if (cudaMalloc(reinterpret_cast<void**>(&L.slots), L.blob_bytes * static_cast<std::size_t>(S)) != cudaSuccess) {
        L.slots = nullptr;
        slots_ok = false;
        break;
      }
    }
    if (!slots_ok) {  // free any that did allocate and run fully mapped-host
      for (auto& L : layers_)
        if (L.slots != nullptr) { cudaFree(L.slots); L.slots = nullptr; }
      cudaGetLastError();  // clear the sticky OOM
      S = 0;
    }
  }
  n_slots_ = S;

  for (auto& L : layers_) {
    L.slot_of.assign(static_cast<std::size_t>(n_expert_), -1);
    L.score.assign(static_cast<std::size_t>(n_expert_), 0.0);
    L.table_host.resize(static_cast<std::size_t>(n_expert_));
    if (S > 0) L.expert_in.assign(static_cast<std::size_t>(S), -1);
    for (int e = 0; e < n_expert_; ++e) {
      if (S > 0 && e < S) {  // resident: fill slot e from the mapped arena (blocking, at load)
        std::uint8_t* slot = L.slots + L.blob_bytes * static_cast<std::size_t>(e);
        if (cudaMemcpy(slot, L.host + L.blob_bytes * static_cast<std::size_t>(e), L.blob_bytes,
                       cudaMemcpyHostToDevice) != cudaSuccess) {
          err = "gemma4 experts: initial slot fill failed";
          return false;
        }
        L.slot_of[static_cast<std::size_t>(e)] = e;
        L.expert_in[static_cast<std::size_t>(e)] = e;
        L.table_host[static_cast<std::size_t>(e)] = expert_ptr(L, slot);
      } else {  // streamed from the mapped host arena over PCIe
        L.table_host[static_cast<std::size_t>(e)] = expert_ptr(L, L.alias + L.blob_bytes * static_cast<std::size_t>(e));
      }
    }
    if (cudaMalloc(reinterpret_cast<void**>(&L.table), sizeof(ExpertPtr) * static_cast<std::size_t>(n_expert_)) !=
        cudaSuccess) {
      err = "gemma4 experts: cudaMalloc for a layer's device table failed";
      return false;
    }
    if (cudaMemcpy(L.table, L.table_host.data(), sizeof(ExpertPtr) * static_cast<std::size_t>(n_expert_),
                   cudaMemcpyHostToDevice) != cudaSuccess) {
      err = "gemma4 experts: device table upload failed";
      return false;
    }
  }
  return true;
}

bool Gemma4Experts::run_moe(int layer, const int* picks_dev, const float* wts_dev, const float* e_in, float* moe,
                            void* stream, std::string& err) {
  if (!opened_) { err = "gemma4 experts: run_moe before open"; return false; }
  if (layer < 0 || layer >= n_layers_) { err = "gemma4 experts: run_moe bad layer"; return false; }
  const Layer& L = layers_[static_cast<std::size_t>(layer)];
  if (L.table == nullptr) { err = "gemma4 experts: run_moe before finalize"; return false; }

  // gate|up (fused) -> GeGLU -> down -> weighted combine with the per-expert down scale, all on the
  // DEVICE indexed by the device picks (g4_engine.cu:545-548). No host sync.
  matvec_experts(L.gu_kind, 0, gu_rows_, n_embd_, L.table, picks_dev, n_exp_used_, e_in, 0, gu_, stream);
  geglu_experts(gu_, gu_rows_, gu_ + n_ff_exp_, gu_rows_, mx_, n_exp_used_, n_ff_exp_, stream);
  matvec_experts(L.dn_kind, 1, n_embd_, n_ff_exp_, L.table, picks_dev, n_exp_used_, mx_, n_ff_exp_, yg_, stream);
  moe_combine(yg_, wts_dev, picks_dev, down_scale_dev_ + static_cast<std::size_t>(layer) * static_cast<std::size_t>(n_expert_),
              n_exp_used_, n_embd_, moe, stream);
  count_picks(picks_dev, n_exp_used_, counts_dev_ + static_cast<std::size_t>(layer) * static_cast<std::size_t>(n_expert_),
              stream);
  if (cudaGetLastError() != cudaSuccess) { err = "gemma4 experts: MoE kernel launch failed"; return false; }
  return true;
}

std::int64_t Gemma4Experts::rebalance(int max_swaps, void* stream, std::string& err, double count_scale) {
  if (n_slots_ == 0 || n_slots_ >= n_expert_) return 0;  // nothing resident, or everything resident
  const auto st = static_cast<cudaStream_t>(stream);
  const std::size_t ne = static_cast<std::size_t>(n_layers_) * static_cast<std::size_t>(n_expert_);
  // The ONE sync: read the routing counts and reset them.
  if (cudaMemcpyAsync(counts_host_.data(), counts_dev_, ne * sizeof(int), cudaMemcpyDeviceToHost, st) != cudaSuccess ||
      cudaMemsetAsync(counts_dev_, 0, ne * sizeof(int), st) != cudaSuccess ||
      cudaStreamSynchronize(st) != cudaSuccess) {
    err = "gemma4 experts: rebalance counts copy failed";
    return -1;
  }
  for (int l = 0; l < n_layers_; ++l) {
    std::vector<double>& score = layers_[static_cast<std::size_t>(l)].score;
    for (int e = 0; e < n_expert_; ++e)
      score[static_cast<std::size_t>(e)] =
          score[static_cast<std::size_t>(e)] * 0.85 +
          count_scale *
              static_cast<double>(counts_host_[static_cast<std::size_t>(l) * static_cast<std::size_t>(n_expert_) +
                                               static_cast<std::size_t>(e)]);
  }

  // Instrumentation (no behaviour change): tally this interval's routed-expert HITS (resident slot)
  // vs MISSES (mapped host, read over PCIe) from the counts just copied + the CURRENT residency
  // (before the swaps below change it). PCIe decode bytes ~= misses * blob_bytes.
  for (int l = 0; l < n_layers_; ++l) {
    const Layer& L = layers_[static_cast<std::size_t>(l)];
    const int* row = counts_host_.data() + static_cast<std::size_t>(l) * static_cast<std::size_t>(n_expert_);
    for (int e = 0; e < n_expert_; ++e) {
      if (row[e] <= 0) continue;
      if (L.slot_of[static_cast<std::size_t>(e)] >= 0)
        stat_hits_ += static_cast<std::uint64_t>(row[e]);
      else
        stat_misses_ += static_cast<std::uint64_t>(row[e]);
    }
  }

  int swaps = 0;
  bool more = true;
  while (more && swaps < max_swaps) {
    bool any = false;
    for (auto& L : layers_) {
      if (swaps >= max_swaps) break;
      int hot = -1, cold_slot = -1;
      for (int e = 0; e < n_expert_; ++e) {
        if (L.slot_of[static_cast<std::size_t>(e)] < 0 &&
            (hot < 0 || L.score[static_cast<std::size_t>(e)] > L.score[static_cast<std::size_t>(hot)]))
          hot = e;
      }
      for (int s = 0; s < n_slots_; ++s) {
        const int e = L.expert_in[static_cast<std::size_t>(s)];
        if (cold_slot < 0 ||
            L.score[static_cast<std::size_t>(e)] <
                L.score[static_cast<std::size_t>(L.expert_in[static_cast<std::size_t>(cold_slot)])])
          cold_slot = s;
      }
      if (hot < 0 || cold_slot < 0) continue;
      const int cold = L.expert_in[static_cast<std::size_t>(cold_slot)];
      if (L.score[static_cast<std::size_t>(hot)] <= L.score[static_cast<std::size_t>(cold)] * 1.25 + 0.5) continue;
      std::uint8_t* slot = L.slots + L.blob_bytes * static_cast<std::size_t>(cold_slot);
      if (cudaMemcpyAsync(slot, L.host + L.blob_bytes * static_cast<std::size_t>(hot), L.blob_bytes,
                          cudaMemcpyHostToDevice, st) != cudaSuccess) {
        err = "gemma4 experts: rebalance slot refill failed";
        return -1;
      }
      L.table_host[static_cast<std::size_t>(hot)] = expert_ptr(L, slot);
      L.table_host[static_cast<std::size_t>(cold)] = expert_ptr(L, L.alias + L.blob_bytes * static_cast<std::size_t>(cold));
      L.slot_of[static_cast<std::size_t>(hot)] = cold_slot;
      L.slot_of[static_cast<std::size_t>(cold)] = -1;
      L.expert_in[static_cast<std::size_t>(cold_slot)] = hot;
      if (cudaMemcpyAsync(L.table + hot, &L.table_host[static_cast<std::size_t>(hot)], sizeof(ExpertPtr),
                          cudaMemcpyHostToDevice, st) != cudaSuccess ||
          cudaMemcpyAsync(L.table + cold, &L.table_host[static_cast<std::size_t>(cold)], sizeof(ExpertPtr),
                          cudaMemcpyHostToDevice, st) != cudaSuccess) {
        err = "gemma4 experts: rebalance table patch failed";
        return -1;
      }
      ++swaps;
      ++stat_fills_;  // one slot refill = one expert load (+ one eviction)
      any = true;
    }
    more = any;
  }
  if (swaps > 0 && cudaStreamSynchronize(st) != cudaSuccess) {
    err = "gemma4 experts: rebalance sync failed";
    return -1;
  }
  return swaps;
}

std::int64_t Gemma4Experts::resident_bytes() const {
  std::int64_t total = 0;
  for (const auto& L : layers_)
    total += static_cast<std::int64_t>(n_slots_) * static_cast<std::int64_t>(L.blob_bytes);
  return total;
}

// ---- read-only accessors for the batched-prefill moe_batch (gemma4_graph.cu) ----

const ExpertPtr* Gemma4Experts::layer_table(int layer) const {
  if (layer < 0 || layer >= n_layers_) return nullptr;
  return layers_[static_cast<std::size_t>(layer)].table;
}

WKind Gemma4Experts::layer_gu_kind(int layer) const {
  if (layer < 0 || layer >= n_layers_) return WKind::q8;
  return layers_[static_cast<std::size_t>(layer)].gu_kind;
}

WKind Gemma4Experts::layer_dn_kind(int layer) const {
  if (layer < 0 || layer >= n_layers_) return WKind::q8;
  return layers_[static_cast<std::size_t>(layer)].dn_kind;
}

const float* Gemma4Experts::layer_down_scale_dev(int layer) const {
  if (down_scale_dev_ == nullptr || layer < 0 || layer >= n_layers_) return nullptr;
  return down_scale_dev_ + static_cast<std::size_t>(layer) * static_cast<std::size_t>(n_expert_);
}

int* Gemma4Experts::layer_counts_dev(int layer) const {
  if (counts_dev_ == nullptr || layer < 0 || layer >= n_layers_) return nullptr;
  return counts_dev_ + static_cast<std::size_t>(layer) * static_cast<std::size_t>(n_expert_);
}

void Gemma4Experts::close() {
  for (auto& L : layers_) {
    if (L.table != nullptr) cudaFree(L.table);
    if (L.slots != nullptr) cudaFree(L.slots);
    if (L.host != nullptr) cudaFreeHost(L.host);
    L.table = nullptr;
    L.slots = nullptr;
    L.host = nullptr;
    L.alias = nullptr;
  }
  layers_.clear();
  if (counts_dev_ != nullptr) cudaFree(counts_dev_);
  if (down_scale_dev_ != nullptr) cudaFree(down_scale_dev_);
  if (gu_ != nullptr) cudaFree(gu_);
  if (mx_ != nullptr) cudaFree(mx_);
  if (yg_ != nullptr) cudaFree(yg_);
  counts_dev_ = nullptr;
  down_scale_dev_ = nullptr;
  gu_ = mx_ = yg_ = nullptr;
  counts_host_.clear();
  n_slots_ = 0;
  opened_ = false;
}

}  // namespace u::gemma4
