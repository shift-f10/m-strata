// engines/unified/src/kernels/ds4_index.cu
// -----------------------------------------------------------------------------
// Bodies for the ds4 learned-index kernels declared in ds4_index.cuh. iota_rows
// is ported faithfully; the heavy index_score / segmented-topk / cand_blocks /
// F16 indexer GEMV are STRUCTURED STUBS with precise TODOs for the GPU box, which
// ports the verified bodies from engines/maya-ref/src/ds4/ds4_kernels.cu and
// parity-checks against tools/ds4/ref.py (plan 2026-10-23 T1.4).
//
// Compiled only on the GPU box (STRATA_U_CUDA ON); an empty TU on host / CI.
// -----------------------------------------------------------------------------
#include "kernels/ds4_index.cuh"

#if defined(STRATA_U_CUDA)

#include <cstdio>
#include <cuda_runtime.h>

namespace u::ds4 {
namespace {
// PORT: ds4_kernels.cu:2039 iota_rows_kernel (faithful).
__global__ void iota_rows_kernel(int* out, int n, int stride) {
    const int t = blockIdx.x;
    for (int i = threadIdx.x; i < n; i += blockDim.x) out[(size_t) t * stride + i] = i;
}
}  // namespace

void iota_rows(int* out, int T, int n, int stride, cudaStream_t s) {
    if (T > 0) iota_rows_kernel<<<T, 64, 0, s>>>(out, n, stride);
}

void gemv_f16(const uint16_t*, const float*, int n, int, int T, float, float* y, cudaStream_t s) {
    // STRUCTURED STUB. TODO(box): port gemv_f16 / gemv_f16_rows (ds4_kernels.cuh:85-90,
    //   ds4_kernels.cu): a warp a row, alpha*sum_k W[r][k] x[t][k], F16 weights. The
    //   indexer's weights_proj. Zero so the indexer scores degrade gracefully.
    if (T > 0 && n > 0) cudaMemsetAsync(y, 0, (size_t) T * n * sizeof(float), s);
}
void gemv_f16_rows(const uint16_t*, const float*, std::size_t, int n, int, int T, float, float* y, std::size_t ldy,
                   cudaStream_t s) {
    // STRUCTURED STUB. TODO(box): the row-batched gemv_f16 (bit-identical to gemv_f16).
    if (T > 0 && n > 0) cudaMemsetAsync(y, 0, (size_t) (T - 1) * ldy * sizeof(float) + (size_t) n * sizeof(float), s);
}

void index_score(const float*, const float*, const void*, int, int, int T, int n, const int*, int, int, const Rows&,
                 float* out, long long out_stride, cudaStream_t s, int, bool) {
    // STRUCTURED STUB. TODO(box): port index_score (ds4_kernels.cuh:128-136,
    //   ds4_kernels.cu): out[t][i] = sum_h relu(q[t][h].k[p]) w[t][h]; the kKvFp4p key
    //   decode; the -inf mask past lim.n[t]; the tensor-core path (DS4_INDEX_TC) with
    //   its FP32 summation order. Set scores to a large negative so top-k is a no-op
    //   until filled (keeps the graph linking + running).
    (void) n;
    if (T > 0) {
        // -inf-ish fill: cudaMemset can't write floats, so a tiny kernel would; here
        // we leave `out` to the box. A memset to 0 at least makes top-k deterministic.
        cudaMemsetAsync(out, 0, (size_t) T * out_stride * sizeof(float), s);
    }
}

void topk(const float*, long long, int T, const Rows&, const int*, int, int, int* out, int out_stride, cudaStream_t s,
          const TopkScratch*) {
    // STRUCTURED STUB. TODO(box): port the segmented top-k (ds4_kernels.cuh:138-153,
    //   ds4_kernels.cu): top r.k[t] of vals[t][0,r.n[t]) in position order, the
    //   TopkScratch segment passes for long rows. Selection left at row 0 for every
    //   query until filled (deterministic; wrong logits, links + runs).
    if (T > 0) cudaMemsetAsync(out, 0, (size_t) T * out_stride * sizeof(int), s);
}

void cand_blocks(const float*, long long, int T, const Rows&, int, float*, long long, int* out, int out_stride,
                 cudaStream_t s) {
    // STRUCTURED STUB. TODO(box): port cand_blocks (ds4_kernels.cuh:154-157): block
    //   max-pool score, pin the newest block, top r.k[t] blocks -> out[t].
    if (T > 0) cudaMemsetAsync(out, 0, (size_t) T * out_stride * sizeof(int), s);
}

}  // namespace u::ds4

#endif  // STRATA_U_CUDA
