// strata-unified-engine: Qwen3.6-35B-A3B GDN kernels. See qwen35moe_gdn.cuh for the shapes, the
// per-token math, and the exact q36_engine.cu / q36ref.cu lines each kernel was ported from.
//
// Ported faithfully from upstream/src/qwen35/q36_engine.cu (k_gdn_gb :225, k_conv_seq :605,
// k_gdn_seq :672, k_gdn_norm :813 folded into the recurrence) with the clean fp32 spec in
// q36ref.cu (k_conv :200, k_gdn :212, k_gated_norm :250). The arithmetic is byte-for-byte the
// production's; only the namespace, the host-only launcher ABI (void* stream) and the trimming of
// the batch-rows / prompt-tile / verify-window fast paths (lean serve: one session, no slot
// batching, no CUDA-graph verify windows — the GPU box re-adds those for throughput) differ.
//
// NOT COMPILED ON CI/dev-host: built only behind STRATA_U_CUDA on the owner's NVIDIA box
// (design §1.3). The box compiles and parities this against the real qwen35moe weights.
#include "qwen35moe_gdn.cuh"

#include <cuda_runtime.h>

namespace u::qwen35moe {
namespace {

__device__ __forceinline__ float silu(float v) { return v / (1.0f + __expf(-v)); }
__device__ __forceinline__ float sigm(float v) { return 1.0f / (1.0f + __expf(-v)); }
__device__ __forceinline__ float softplus(float v) { return v > 20.f ? v : log1pf(__expf(v)); }

// ---- (1) gate/beta epilogue (q36_engine.cu:225 k_gdn_gb) -------------------------------------
// alpha[t,h] <- softplus(alpha + dt[h]) * ssm_a[h];  beta[t,h] <- sigmoid(beta).
__global__ void k_gdn_gate_beta(float* alpha, const float* dt, const float* ssm_a, float* beta, int HV,
                                int T) {
  const int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= HV * T) return;
  const int h = i % HV;
  alpha[i] = softplus(alpha[i] + dt[h]) * ssm_a[h];
  beta[i] = sigm(beta[i]);
}

// ---- (2) causal conv + SiLU + per-head L2-norm (q36_engine.cu:605 k_conv_seq, simple path) ----
// grid (C/128), block 128: block b owns the 128 channels of head-block b; qk = (b < qk_heads) gets
// the L2-norm. `hist` is [C][3] (the last d_conv-1=3 columns per channel). d_conv is 4 (pinned).
__global__ void __launch_bounds__(128) k_conv_seq(float* hist, const float* qkv, const float* w, float* h,
                                                  int C, int qk_heads, float eps, int T, bool write) {
  __shared__ float part[4];
  const int c = blockIdx.x * 128 + threadIdx.x;
  float v0 = hist[c * 3], v1 = hist[c * 3 + 1], v2 = hist[c * 3 + 2];
  const float w0 = w[c * 4], w1 = w[c * 4 + 1], w2 = w[c * 4 + 2], w3 = w[c * 4 + 3];
  const bool qk = (int) blockIdx.x < qk_heads;
  for (int t = 0; t < T; ++t) {
    const float x = qkv[(size_t) t * C + c];
    const float sum = v0 * w0 + v1 * w1 + v2 * w2 + x * w3;
    v0 = v1;
    v1 = v2;
    v2 = x;
    float y = silu(sum);
    if (qk) {  // L2-norm over the head's 128 channels (q and k heads only; v is left as silu)
      float sq = y * y;
      for (int o = 16; o > 0; o >>= 1) sq += __shfl_xor_sync(0xffffffffu, sq, o);
      if ((threadIdx.x & 31) == 0) part[threadIdx.x >> 5] = sq;
      __syncthreads();
      const float ss = part[0] + part[1] + part[2] + part[3];
      __syncthreads();
      y *= rsqrtf(ss + eps);
    }
    h[(size_t) t * C + c] = y;
  }
  if (!write) return;  // a verify window would let commit() move the history instead
  hist[c * 3] = v0;
  hist[c * 3 + 1] = v1;
  hist[c * 3 + 2] = v2;
}

// ---- (3) gated delta-rule + fused SiLU-gated RMS norm (q36_engine.cu:672 k_gdn_seq) -----------
// grid (HV), block (128, 4) = 512 threads. Thread (col, rg) carries RPG=32 state rows in registers
// (state layout [row][v-head][col], row_stride = HV*128). `gate` is log-space (exp'd here).
__global__ void __launch_bounds__(512) k_gdn_seq(float* state, const float* h, int C, const float* gate,
                                                 const float* beta, const float* z, const float* gamma,
                                                 float eps, float* y, int h_k, int h_v, int T, bool write) {
  constexpr int S = 128, RG = 4, RPG = 32;
  __shared__ float sk[S], sq[S];
  __shared__ float red_kv[RG][S];
  __shared__ float red_o[RG][S];
  __shared__ float wsum[S * RG / 32];
  const int head = blockIdx.x, col = threadIdx.x, rg = threadIdx.y, tid = rg * S + col;
  const int qh = head % h_k;
  const int ZV = h_v * S;
  float s[RPG];
  float* base = state + ((size_t) (rg * RPG) * h_v + head) * S + col;
  const size_t row_stride = (size_t) h_v * S;
#pragma unroll
  for (int r = 0; r < RPG; ++r) s[r] = base[r * row_stride];
  for (int t = 0; t < T; ++t) {
    const float* ht = h + (size_t) t * C;
    if (tid < S) {
      sk[tid] = ht[h_k * S + qh * S + tid];  // the k channel of this head's k-head
      sq[tid] = ht[qh * S + tid];            // the q channel
    }
    __syncthreads();
    const float g = __expf(gate[(size_t) t * h_v + head]);
    float kv = 0.0f;
#pragma unroll
    for (int r = 0; r < RPG; ++r) kv = fmaf(s[r], sk[rg * RPG + r], kv);
    red_kv[rg][col] = kv;
    __syncthreads();
    const float kv_col = red_kv[0][col] + red_kv[1][col] + red_kv[2][col] + red_kv[3][col];
    const float delta = (ht[2 * h_k * S + head * S + col] - g * kv_col) * beta[(size_t) t * h_v + head];
    float o = 0.0f;
#pragma unroll
    for (int r = 0; r < RPG; ++r) {
      s[r] = fmaf(g, s[r], sk[rg * RPG + r] * delta);  // S *= g; S += outer(k, delta)
      o = fmaf(s[r], sq[rg * RPG + r], o);             // o = S @ q
    }
    red_o[rg][col] = o;
    __syncthreads();
    float oc = 0.0f, sq_part = 0.0f;
    if (rg == 0) {
      oc = (red_o[0][col] + red_o[1][col] + red_o[2][col] + red_o[3][col]) * rsqrtf((float) S);  // /sqrt(D)
      sq_part = oc * oc;
    }
    for (int o2 = 16; o2 > 0; o2 >>= 1) sq_part += __shfl_xor_sync(0xffffffffu, sq_part, o2);
    if ((tid & 31) == 0) wsum[tid >> 5] = sq_part;
    __syncthreads();
    if (rg == 0 && y != nullptr) {  // the fused SiLU-gated RMS output norm (k_gdn_norm)
      const float ss = wsum[0] + wsum[1] + wsum[2] + wsum[3];
      const float scale = rsqrtf(ss / (float) S + eps);
      const float zz = z[(size_t) t * ZV + head * S + col];
      y[(size_t) t * ZV + head * S + col] = oc * scale * gamma[col] * (zz / (1.0f + __expf(-zz)));
    }
    __syncthreads();  // sk / sq / the reductions are overwritten by the next token
  }
  if (!write) return;
#pragma unroll
  for (int r = 0; r < RPG; ++r) base[r * row_stride] = s[r];
}

inline cudaStream_t cs(void* s) { return static_cast<cudaStream_t>(s); }

}  // namespace

void gdn_gate_beta(float* alpha, const float* dt, const float* ssm_a, float* beta, int HV, int T,
                   void* stream) {
  const int n = HV * T;
  k_gdn_gate_beta<<<(unsigned) ((n + 255) / 256), 256, 0, cs(stream)>>>(alpha, dt, ssm_a, beta, HV, T);
}

void gdn_conv(float* hist, const float* qkv, const float* conv_w, float* out, int C, int qk_heads,
              float eps, int T, bool write, void* stream) {
  // C % 128 == 0 (ssm_conv_channels = 128*(2*HK+HV)); one block per 128-channel head.
  k_conv_seq<<<(unsigned) (C / 128), 128, 0, cs(stream)>>>(hist, qkv, conv_w, out, C, qk_heads, eps, T, write);
}

void gdn_recurrence(float* S, const float* h, int C, const float* gate, const float* beta,
                    const float* z, const float* gamma, float eps, float* y, int HK, int HV, int T,
                    bool write, void* stream) {
  k_gdn_seq<<<(unsigned) HV, dim3(128, 4), 0, cs(stream)>>>(S, h, C, gate, beta, z, gamma, eps, y, HK, HV, T,
                                                            write);
}

}  // namespace u::qwen35moe
