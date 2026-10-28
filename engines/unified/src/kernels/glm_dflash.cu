// engines/unified/src/kernels/glm_dflash.cu - the glm5-next NextN/DFlash draft glue. See glm_dflash.cuh
// for the block structure and the TODOs the GPU box completes.
//
// Only the one GLM-specific NextN glue kernel is ported concrete here (the eh_proj input assembly);
// the NextN block's attention + FFN reuse the trunk kernels + u::ExpertTier, and the verify/accept/
// DFlash orchestration is the documented box TODO in glm_dflash.cuh.
//
// Ported from the reference's NextN input stage (engines/maya-ref/src/core/glm_model.cu L3628,
// L264-274 for the tensor set). namespace u::glm.
#include "glm_dflash.cuh"

#include <cuda_runtime.h>

namespace u::glm {
namespace {

constexpr int DF_THREADS = 256;

// Two independent rms norms packed into one [2*n_embd] row: lanes [0,n_embd) normalize `emb` with
// `enorm_w`, lanes [n_embd,2*n_embd) normalize `h_final` with `hnorm_w`. Each half reduces its own
// sum of squares in shared memory (ggml_rms_norm: mean over the feature axis, then the weight).
__global__ void glm_nextn_eh_input_kernel(const float* __restrict__ emb, const float* __restrict__ h_final,
                                          const float* __restrict__ enorm_w, const float* __restrict__ hnorm_w,
                                          int n_embd, float eps, float* __restrict__ out) {
    __shared__ float ss_emb;
    __shared__ float ss_h;
    __shared__ float red[DF_THREADS];
    const int tid = threadIdx.x;

    // sum of squares of emb
    float local = 0.0f;
    for (int e = tid; e < n_embd; e += DF_THREADS) local += emb[e] * emb[e];
    red[tid] = local;
    __syncthreads();
    for (int span = DF_THREADS / 2; span > 0; span >>= 1) {
        if (tid < span) red[tid] += red[tid + span];
        __syncthreads();
    }
    if (tid == 0) ss_emb = red[0];
    __syncthreads();

    // sum of squares of h_final
    local = 0.0f;
    for (int e = tid; e < n_embd; e += DF_THREADS) local += h_final[e] * h_final[e];
    red[tid] = local;
    __syncthreads();
    for (int span = DF_THREADS / 2; span > 0; span >>= 1) {
        if (tid < span) red[tid] += red[tid + span];
        __syncthreads();
    }
    if (tid == 0) ss_h = red[0];
    __syncthreads();

    const float inv_emb = rsqrtf(ss_emb / (float) n_embd + eps);
    const float inv_h = rsqrtf(ss_h / (float) n_embd + eps);
    for (int e = tid; e < n_embd; e += DF_THREADS) {
        out[e] = emb[e] * inv_emb * enorm_w[e];
        out[n_embd + e] = h_final[e] * inv_h * hnorm_w[e];
    }
}

}  // namespace

void glm_nextn_eh_input(const float* emb, const float* h_final, const float* enorm_w, const float* hnorm_w,
                        int n_embd, float eps, float* out, void* stream) {
    if (n_embd <= 0) return;
    glm_nextn_eh_input_kernel<<<1, DF_THREADS, 0, (cudaStream_t) stream>>>(emb, h_final, enorm_w, hnorm_w,
                                                                           n_embd, eps, out);
}

}  // namespace u::glm
