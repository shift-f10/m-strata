// engines/unified/src/kernels/ds4_dspark.cuh
// -----------------------------------------------------------------------------
// DeepSeek-V4.1-Flash (ds4) DSpark speculative drafter: the small device kernels
// its draft heads need. DSpark runs the checkpoint's mtp.* blocks (a block of
// [seed, noise, noise, ...]) to propose several tokens at once, each with a
// confidence, which the trunk then verifies. It reuses the MLA layer path for the
// blocks themselves; what is DSpark-specific is the Markov bias head on the vocab,
// the confidence head, and the small reductions below.
//
// PORT ORIGIN: engines/maya-ref/src/ds4/ds4_kernels.cuh:290-306 (contracts),
//              ds4_kernels.cu (bodies); driven from ds4_model.cu:8946-9307
//              (seed_drafter / draft_ / verify / rollback).
//
// SCOPE (lean): the SINGLE-GPU greedy drafter. The reference's tensor-parallel
// "markov share" across a head group and the FULL sampled speculative-acceptance
// rule (speculative sampling with a residual draw) are represented as structured
// stubs / a greedy fallback with precise TODOs; the GPU box ports the verified
// forms (plan 2026-10-23 T1.4-T1.5). The actual draft/verify control flow lives
// in ds4_graph.cu; this file holds only the drafter's device kernels. The sampler
// itself is the shared u::k::sampler.
// -----------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <cstdint>

#if defined(STRATA_U_CUDA)
#include <cuda_runtime.h>
#else
using cudaStream_t = void*;
#endif

namespace u::ds4 {

// The DSpark Markov bias on a single logits row (ds4_kernels.cuh:294-295):
//   logits[v] += E[*tok] . H[v]   for v < V   (E BF16 [V][R], H F16 [V][R])
// The previous token's low-rank embedding biases the next position's logits.
// PORT: ds4_kernels.cu markov_kernel (a warp a vocab row). Faithful in ds4_dspark.cu.
void markov_add(float* logits, const uint16_t* E, const uint16_t* H, const int* tok, int V, int R, cudaStream_t s);

// The DSpark confidence head (ds4_kernels.cuh:304-306):
//   out[t] = sigmoid(w . [x[t] | E[tok[t]]])   (x rows dim floats, w F32 [dim+R])
// A draft is kept while its confidence stays above the request's floor.
// PORT: ds4_kernels.cu confidence kernel. Faithful in ds4_dspark.cu.
void confidence(const float* x, const uint16_t* E, const int* tok, const float* w, int T, int dim, int R, float* out,
                cudaStream_t s);

// Tensor-parallel Markov head over a head group (ds4_kernels.cuh:296-303). On the
// lean single-GPU path these collapse to markov_add + the shared argmax sampler, so
// they are STRUCTURED STUBS here. TODO(box): port markov_share / argmax_share /
// markov_pick for multi-GPU head groups from ds4_kernels.cu.
void markov_share(float* logits, const uint16_t* H, const uint16_t* erow, int vn, int R, cudaStream_t s);
void argmax_share(const float* row, int vn, int v0, unsigned* slot, unsigned seq, cudaStream_t s);
void markov_pick(const unsigned* slots, int nm, unsigned seq, int* tok, const uint16_t* E, uint16_t* erow, int R,
                 cudaStream_t s);

}  // namespace u::ds4
