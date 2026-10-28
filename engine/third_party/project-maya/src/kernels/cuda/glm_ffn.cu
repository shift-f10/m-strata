// src/kernels/cuda/glm_ffn.cu - the glm5-next clamped SwiGLU.  See glm_ffn.hpp for the math.
#include "strata/kernels/glm_ffn.hpp"

#include <cuda_runtime.h>

namespace strata::kernels {
namespace {

constexpr int GF_THREADS = 256;

__device__ __forceinline__ float dsigmoid(float x) { return 1.0f / (1.0f + __expf(-x)); }

__global__ void glm_swiglu_clamp_kernel(const float* __restrict__ gate, const float* __restrict__ up,
                                        float limit, long long n, float* __restrict__ out) {
    const long long i = (long long) blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    const float g = fminf(gate[i], limit);
    const float u = fminf(fmaxf(up[i], -limit), limit);
    // precise expf, not __expf: the fixture's gates reach |g| ~ 30, where the fast intrinsic's
    // argument-scaled ulp error lands a few e-5 off on silu - the parity harness's tolerance is
    // tighter than that, and so is the model
    out[i] = (g / (1.0f + expf(-g))) * u;
}

// raon1's SiTU-GLU, as KRAFTON's llama.cpp (raon-1-mini-preview) llm_graph_context::build_situ computes it:
//   situ(x, b) = ggml_softcap(x, 1/b, b) * sigmoid(x) = b*tanh(x/b) * sigmoid(x)   (the gate branch)
//   out = situ(gate, beta_gate) * beta_up*tanh(up/beta_up)   -- the up branch is ONLY softcapped, no sigmoid
//   (beta_gate = situ_beta, beta_up = situ_linear_beta; a non-positive beta_up leaves up as is)
// precise tanhf/expf (not the fast intrinsics), matching the clamped-swiglu precision note above.
__device__ __forceinline__ float situ_act(float x, float beta) {
    return beta * tanhf(x / beta) * (1.0f / (1.0f + expf(-x)));
}

__global__ void glm_situ_glu_kernel(const float* __restrict__ gate, const float* __restrict__ up,
                                    float beta_gate, float beta_up, long long n, float* __restrict__ out) {
    const long long i = (long long) blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    out[i] = situ_act(gate[i], beta_gate) * (beta_up > 0.0f ? beta_up * tanhf(up[i] / beta_up) : up[i]);
}

}  // namespace

void glm_swiglu_clamp(const float* gate, const float* up, float limit, int64_t n, float* out, void* stream) {
    if (n <= 0) return;
    const long long blocks = ((long long) n + GF_THREADS - 1) / GF_THREADS;
    glm_swiglu_clamp_kernel<<<(unsigned) blocks, GF_THREADS, 0, (cudaStream_t) stream>>>(gate, up, limit, n, out);
}

void glm_situ_glu(const float* gate, const float* up, float beta_gate, float beta_up, int64_t n, float* out,
                  void* stream) {
    if (n <= 0) return;
    const long long blocks = ((long long) n + GF_THREADS - 1) / GF_THREADS;
    glm_situ_glu_kernel<<<(unsigned) blocks, GF_THREADS, 0, (cudaStream_t) stream>>>(gate, up, beta_gate, beta_up, n,
                                                                                     out);
}

}  // namespace strata::kernels
