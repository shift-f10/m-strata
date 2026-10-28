// engines/unified/src/kernels/glm_ffn.cu - the glm5-next clamped SwiGLU. See glm_ffn.cuh.
//
// PORTED FROM engines/maya-ref/src/kernels/cuda/glm_ffn.cu (verbatim), namespace strata::kernels -> u::glm.
#include "glm_ffn.cuh"

#include <cuda_runtime.h>

namespace u::glm {
namespace {

constexpr int GF_THREADS = 256;

__global__ void glm_swiglu_clamp_kernel(const float* __restrict__ gate, const float* __restrict__ up,
                                        float limit, long long n, float* __restrict__ out) {
    const long long i = (long long) blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    const float g = fminf(gate[i], limit);
    const float u = fminf(fmaxf(up[i], -limit), limit);
    // precise expf, not __expf: the fixture's gates reach |g| ~ 30, where the fast intrinsic's
    // argument-scaled ulp error lands a few e-5 off on silu - tighter than the parity tolerance.
    out[i] = (g / (1.0f + expf(-g))) * u;
}

}  // namespace

void glm_swiglu_clamp(const float* gate, const float* up, float limit, int64_t n, float* out, void* stream) {
    if (n <= 0) return;
    const long long blocks = ((long long) n + GF_THREADS - 1) / GF_THREADS;
    glm_swiglu_clamp_kernel<<<(unsigned) blocks, GF_THREADS, 0, (cudaStream_t) stream>>>(gate, up, limit, n, out);
}

}  // namespace u::glm
