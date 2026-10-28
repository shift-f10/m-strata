// strata-unified-engine: Gemma-4-26B-A4B small glue kernels (arch "gemma4") — the two tiny
// pieces the shared set (kernels/common.cuh) does not carry. Host-includable (void* stream).
//
// PORTED FROM engines/gemma4/src/g4_kernels.cu (the read-only 26/26-verified parity oracle).
//
//   softcap   — the final-logit soft-cap x = cap * tanh(x / cap) (cap = 30 for gemma4). The
//               attention path folds 1/sqrt(hd) into the weights and uses no attn soft-cap, so
//               this is only the HEAD soft-cap. No shared equivalent. g4_kernels.cu k_softcap
//               (486-489), launcher softcap (754).
//   embed_q8  — the token-embedding gather for a Q8_0 embedding, scaled by sqrt(n_embd) (Gemma
//               scales embeddings by sqrt(d_model)). Unlike the reference (which repacked Q8_0
//               into separate int8/scale planes), this reads the GGUF Q8_0 blocks DIRECTLY
//               (34 bytes = fp16 scale + 32 int8 per 32 elems), the same on-disk layout
//               u::k::mmvq reads for the tied head — so the unified engine keeps ONE Q8_0 layout
//               and never repacks. g4_kernels.cu k_embed_q8 (207-214), launcher embed_q8
//               (707-710); the reference's repack lived host-side in g4_engine.cu repack_q8
//               (33-42) and is intentionally dropped here.
#pragma once

#include <cstdint>

namespace u::gemma4 {

// x[i] = cap * tanh(x[i] / cap), in place over n elements.
void softcap(float* x, int n, float cap, void* stream);

// out[t*n + i] = dequant(Q8_0 row ids[t])[i] * scale, for t < T, i < n. `q8_blocks` is the GGUF
// Q8_0 token_embd tensor (row-major [vocab, n], each row n/32 blocks of 34 bytes). `scale` is the
// Gemma embedding scale (sqrt(n_embd)). n must be a multiple of 32.
void embed_q8(const int* ids, int T, const void* q8_blocks, int n, float scale, float* out, void* stream);

// FUSED layer-tail kernels (one block per row), ported from engines/gemma4/src/g4_kernels.cu to cut
// the per-token kernel-launch count of forward_one on Windows/WDDM. One launch each, replacing the
// composed rms_norm/add/scale sequences the decode path used before (same math, fewer launches).
//
//   add_rmsnorm — out = x + rms(y)*w  (the post-attention sandwich; g4 k_add_rmsnorm 173-181).
//   ffn_end     — out = (x1 + rms(rms(dense)*w1 + rms(moe)*w2)*w3) * layer_scale  (the double-sandwich
//                 layer tail; g4 k_ffn_end 183-205). `rows` rows of `n`; rms is x*rsqrt(mean(x^2)+eps).
void add_rmsnorm(const float* x, const float* y, const float* w, float* out, int rows, int n, float eps, void* stream);
void ffn_end(const float* x1, const float* dense, const float* moe, const float* w1, const float* w2, const float* w3,
             float layer_scale, float* out, int rows, int n, float eps, void* stream);

// DEVICE query-position cell for the captured decode graph. pos_set writes it (before the first replay);
// pos_inc increments it by one (enqueued at the tail of the graph, so each replay advances the position
// on the device with no host round-trip). Both are single-element ops on the stream.
void pos_set(int* pos_dev, int value, void* stream);
void pos_inc(int* pos_dev, void* stream);

}  // namespace u::gemma4
