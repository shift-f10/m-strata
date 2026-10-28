// engines/unified/src/kernels/ds4_index.cuh
// -----------------------------------------------------------------------------
// DeepSeek-V4.1-Flash (ds4) learned sparse-attention index. For each query the
// indexer scores every compressed KV position with a small learned head, then a
// two-level top-k (candidate blocks, then positions) selects the rows MLA will
// actually attend to. This is what makes ds4's long-context attention sparse.
//
// PORT ORIGIN: engines/maya-ref/src/ds4/ds4_kernels.cuh:85-157 (contracts) and
//              ds4_kernels.cu (bodies); called from ds4_model.cu:11556-11644.
//
// These are the heaviest, most device-tuned kernels of the family (segmented
// top-k over up to 128K positions, a tensor-core index_score path). They are
// declared faithfully here and implemented as STRUCTURED STUBS in ds4_index.cu
// with precise TODOs; the GPU box ports the verified bodies and parity-checks
// them against tools/ds4/ref.py (plan 2026-10-23 T1.4).
//
// The generic F32 router GEMV + sigmoid-router+top-k that pick EXPERTS are NOT
// here; those are u::k::matmul / u::k::router_topk (common.cuh). The top-k in
// THIS file selects attention POSITIONS, which is a ds4-specific indexer op.
// -----------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <cstdint>

#if defined(STRATA_U_CUDA)
#include <cuda_runtime.h>
#else
using cudaStream_t = void*;
#endif

#include "kernels/ds4_mla.cuh"   // kWin, kKv* formats

namespace u::ds4 {

// Per-query index sizes (ds4_kernels.cuh:115-126). Query t at position p0+t sees
// nn = (p0+t+1)/R compressed positions and cb = min(ccap, ceil(nn/blk)) candidate
// blocks. `gen` selects which stage computed n/k on the device:
//   gen 1: n=nn, k=min(kcap,nn)   gen 2: n=nn, k=cb
//   gen 3: n=cb*blk, k=min(kcap,nn)   gen 4: n=ceil(nn/blk), k=kcap
struct Rows {
    int n[kWin];
    int k[kWin];
    int gen = 0, R = 1, kcap = 0, ccap = 0, blk = 1;
    long long p0 = 0;
};

// The indexer F16 weight-projection GEMV (ds4_kernels.cuh:85-90). The indexer's
// weights_proj is plain F16 (not quantized), so it is a small dedicated GEMV
// rather than a u::k::mmvq call.
//   gemv_f16     : y[t][r] = alpha * sum_k W[r][k] x[t][k]
//   gemv_f16_rows: the same for T activations, each weight read once, bit-identical
void gemv_f16(const uint16_t* W, const float* x, int n, int k, int T, float alpha, float* y, cudaStream_t s);
void gemv_f16_rows(const uint16_t* W, const float* x, std::size_t ldx, int n, int k, int T, float alpha, float* y,
                   std::size_t ldy, cudaStream_t s);

// out[t][i] = i for i<n, rows `stride` apart (a DSpark block's rows as every
// query's selection). (ds4_kernels.cuh:292-293) PORTED faithfully in ds4_index.cu.
void iota_rows(int* out, int T, int n, int stride, cudaStream_t s);

// The indexer scores, row t (ds4_kernels.cuh:128-136):
//   out[t][i] = sum_h relu(q[t][h] . k[p]) * w[t][h]
// over the compressed positions p the query sees (p=i, or with candidates
// p=cand[t][i/blk]*blk + i%blk); positions >= lim.n[t] -> -inf. Keys k are kKvFp4p
// (or kKvBf16). The reference has a tensor-core path (DS4_INDEX_TC) with a different
// FP32 summation order.
void index_score(const float* q, const float* w, const void* k, int heads, int hd, int T, int n, const int* cand,
                 int cand_stride, int blk, const Rows& lim, float* out, long long out_stride, cudaStream_t s,
                 int kfmt = kKvFp4p, bool tc = true);

// The segmented top-k over long score rows (ds4_kernels.cuh:138-153). Picks the
// top r.k[t] of vals[t][0, r.n[t]) into out[t][0,k) in position order (stable).
struct TopkScratch {
    float* val = nullptr;     // [T][P][K]
    int* idx = nullptr;       // [T][P][K]
    int* cnt = nullptr;       // [T][P]
    uint32_t* kth = nullptr;  // [T][P]
    int T = 0, P = 0, K = 0;
};
void topk(const float* vals, long long stride, int T, const Rows& r, const int* cand, int cand_stride, int blk,
          int* out, int out_stride, cudaStream_t s, const TopkScratch* sc = nullptr);

// Level-one candidate pre-filter (ds4_kernels.cuh:154-157): block b's score is the
// max over its blk positions; the newest block is pinned; the top r.k[t] blocks ->
// out[t]. `bm` is scratch (rows bm_stride).
void cand_blocks(const float* score, long long stride, int T, const Rows& r, int blk, float* bm, long long bm_stride,
                 int* out, int out_stride, cudaStream_t s);

}  // namespace u::ds4
