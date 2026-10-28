// strata-unified-engine: Gemma-4-26B-A4B attention kernels (arch "gemma4") — the pieces that
// are genuinely gemma4-specific and have no equivalent in the shared set (kernels/common.cuh).
//
// PORTED FROM the 26/26-verified reference engine engines/gemma4/src/g4_kernels.cu (read-only
// parity oracle). Re-expressed onto the unified engine's host-only launcher convention: every
// launcher takes `void* stream` (a cudaStream_t) so this header stays includable from a host TU
// without <cuda_runtime.h>, and the half-precision KV caches are carried as uint16_t* (the raw
// f16 bits) rather than __half* for the same reason — the .cu reinterpret_casts them. This matches
// how the shared bf16 path is typed in common.cuh (bf16_gemv takes uint16_t*).
//
// WHY THESE ARE NOT SHARED (and the generic ones they parallel in u::k::):
//   qkv_post  — fuses, per (token,head): a per-head RMS norm over head_dim (q/k weighted by
//               q_norm/k_norm, v unweighted), the DUAL RoPE (sliding layers: NEOX base 1e4, no
//               freq factors; full layers: the PROPORTIONAL rope, base 1e6 divided by the
//               rope_freqs factors), and the f16 KV-cache write (ring slot = pos%ring on sliding
//               layers, linear slot = pos on full layers). The rope math mirrors u::k::rope_neox_base
//               but the shared launcher neither folds in the per-head norm + cache write nor takes
//               the full-layer freq_factors, so the fused kernel stays gemma4's. g4_kernels.cu
//               k_qkv_post (217-271), launcher qkv_post (712-717).
//   attn_decode — split-K online-softmax decode attention over the f16 ring/linear caches,
//               TEMPLATED on the two gemma4 head widths HD∈{256 sliding, 512 full} and group sizes
//               G∈{2 sliding, 8 full}, with the sliding-window lower bound and ring-modulo slot
//               addressing, and V-FROM-K on full layers (the caller passes v_cache == the same
//               projection as k, normed-but-not-roped). This parallels u::k::attn_decode_split +
//               u::k::attn_merge (the same running-max/running-sum merge), but the shared pair is
//               f32 q/k/v with a mask array and generic GQA, not f16 ring caches at fixed 256/512
//               widths, so it does not fit. g4_kernels.cu k_attn_part / k_attn_merge (273-376),
//               launcher attn_decode / attn_decode_splits (719-735).
//   attn_softmax — the windowed/full masked softmax for the OPTIONAL batched-prefill fast-path
//               (see gemma4_graph.cu: the lean baseline consumes the prompt token-by-token through
//               the decode path above, so this is only needed when the box later adds the batched
//               prefill that pairs it with u::k::gemm_prefill_* for the score/output GEMMs). The
//               shared u::k::softmax_rows is a plain row softmax with no window/ring validity mask.
//               g4_kernels.cu k_attn_softmax (378-410), launcher attn_softmax (737-739).
//
// All arithmetic is f32 (the reference keeps attention in f32 for llama.cpp parity; the caches are
// f16 only for VRAM). sm_75+ (Turing), matching the shared set. Attention scale is 1.0 (gemma4
// folds 1/sqrt(head_dim) into the loaded weights, per the reference), so no scale argument here.
#pragma once

#include <cstdint>

namespace u::gemma4 {

// q/k norm (weighted) + v rms (unweighted), dual-RoPE, then the f16 cache write. One block per
// (token, head) over the q heads then the k heads then the v heads (grid.y = n_head + 2*n_kv),
// hd/2 threads (thread i owns elements i and i+hd/2). Mirrors g4k::QkvPost (g4_kernels.cuh:63-85).
struct QkvPost {
  const float* q_raw;        // [T][n_head*hd]
  const float* k_raw;        // [T][n_kv*hd]
  const float* v_raw;        // [T][n_kv*hd]  (== k_raw on full layers: V-from-K)
  const float* q_norm;       // [hd] weighted RMS for q
  const float* k_norm;       // [hd] weighted RMS for k
  const float* freq_factors; // [hd/2] proportional-rope factors (full layers) or nullptr (sliding)
  float rope_base;           // 1e4 sliding / 1e6 full
  int hd;                    // 256 sliding / 512 full
  int n_head;                // 16
  int n_kv;                  // 8 sliding / 2 full
  int pos0;                  // position of token 0 of this call (host; used when pos0_dev is null)
  const int* pos0_dev;       // optional DEVICE position of token 0 (decode/graph); null => use pos0.
                             // Lets a captured CUDA graph advance the position without a host relaunch.
  int T;                     // tokens this call (1 on the decode path)
  int ring;                  // sliding: ring rows (slot = pos%ring); 0 => full (slot = pos)
  int cap;                   // rows per KV head in the cache
  float eps;                 // RMS epsilon
  float* q_out;              // [T][n_head*hd] roped, normed queries (attention input)
  std::uint16_t* q16;        // optional f16 copy of q_out (batched-prefill path); nullptr on decode
  std::uint16_t* k_cache;    // [n_kv][cap][hd] f16
  std::uint16_t* v_cache;    // [n_kv][cap][hd] f16
  float theta_scale;         // set by the launcher from rope_base and hd (leave 0)
};
void qkv_post(const QkvPost& p, void* stream);

// Keys per decode-attention split block (the reference's kSplit = 128).
[[nodiscard]] int attn_decode_splits(int keys);

// Decode attention for one query position read from `pos_dev` (DEVICE int), scale 1.0. Keys span
// [max(0, pos-window+1), pos] (window 0 => all keys up to pos); slot = p (full) or p%ring (sliding).
// The query position lives on the device so a captured CUDA graph can advance it with no host relaunch;
// the kernel clamps each split to the valid key count at replay time. `n_split` is the FIXED number of
// split blocks to launch (grid.y) — the caller sizes it for the largest position the graph will reach;
// extra splits past the current position write neutral partials that the merge ignores (bitwise no-op).
// out is [n_head][hd]. `scratch` holds the per-head split partials and needs n_head * max_splits *
// (hd + 2) floats. Only the two gemma4 (hd, G) pairs are instantiated: (512, 8) full and (256, 2) sliding.
void attn_decode(const float* q, const std::uint16_t* k_cache, const std::uint16_t* v_cache, int hd,
                 int n_head, int n_kv, int cap, int ring, int window, const int* pos_dev, int n_split,
                 float* out, float* scratch, int max_splits, void* stream);

// Batched-prefill masked softmax (optional fast-path; see the header note). `s` is [G][Ts][L]
// scores of the queries at qpos0+t (t<Ts) against L key slots; `p` receives the masked softmax as
// f16. Slot validity: full (ring 0) => slot index, valid when <= qpos; sliding => the latest
// position <= last_pos congruent to the slot mod ring, valid within the window. Invalid keys => 0.
void attn_softmax(const float* s, std::uint16_t* p, int G, int Ts, int L, int qpos0, int ring,
                  int window, int last_pos, void* stream);

}  // namespace u::gemma4
