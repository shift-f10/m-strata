// engines/unified/src/kernels/glm_dsa.cuh - the glm5-next DSA layer's learned k-pool indexer and
// absorbed (NoPE-)MLA attention, the GLM-specific sparse/latent-attention mixer for the unified engine.
//
// PORTED FROM (reviewed, verbatim math, renamed into u::glm):
//   engines/maya-ref/include/strata/kernels/glm_dsa.hpp   (the math + layout contracts)
//   engines/maya-ref/src/kernels/cuda/glm_dsa.cu          (the four kernels)
//   engines/maya-ref/src/core/glm_model.cu                (q_absorb_kernel, L427)
// Reference spec: engines/maya-ref/ref/glm.py::dsa_layer, transcribed from
//   engines/maya-ref/ref/upstream-llama.cpp/glm5-next.cpp L767-1005.
//
// The 11 full-attention layers (of 45) cache ONLY the compressed latent (kv_lora per token): q is
// absorbed through wk_b (score = (wk_b_h^T q_h) . latent) and v is absorbed through wv_b AFTER the
// softmax, so no per-head K or V ever exists. The learned k-pool indexer decides which cache cells
// each query sees. The four pieces, per query token t:
//
//   pool   pooled[e, p] = sum_m softmax_m(ig[e, member_m] + ape[e, m]) * ik[e, member_m]
//          THE SOFTMAX RUNS SEPARATELY PER KEY-DIM ELEMENT; pool p's members are p*kpool..p*kpool+kpool-1.
//   score  score[t, p] = sum_h relu(iq_h . pooled_p) * weights[h, t], weights prescaled by
//          1/sqrt(key_dim * n_head) by the graph.
//   select pool p is visible to t only when its LAST member is <= t; the top_k/kpool visible pools
//          with the highest scores are expanded to their kpool cells (descending score order), the
//          incomplete tail (cells n_vis*kpool .. t) is appended when select_tail, the row padded -1.
//          The rank formula is stable descending, ties by lower index.
//   attend scores_s = (q_abs_h . latent_cell_s) / sqrt(qk_nope); softmax over the VALID cells
//          (padding excluded = the graph's -inf additive mask); ctx = sum_s prob_s * latent_s;
//          out_h = wv_b_h^T ctx.
//
// LAYOUT CONTRACTS (f32, ggml order - features fastest):
//   ik, ig, pooled : (key_dim, T | n_pools)     e + key_dim*t (or *p)
//   ape            : (key_dim, kpool)           e + key_dim*m
//   iq             : (key_dim, idx_heads, T)    e + key_dim*h + key_dim*idx_heads*t
//   weights        : (idx_heads, T)             h + idx_heads*t
//   score          : (n_pools, T)               p + n_pools*t
//   cells          : (n_sel, T)                 s + n_sel*t, -1 = padding
//   latents        : (kv_lora, n_cache)         e + kv_lora*c
//   q_abs          : (kv_lora, n_head, T)       e + kv_lora*h + kv_lora*n_head*t
//   wv_b           : GGUF {kv_lora, v_head, n_head} -> c + kv_lora*v + kv_lora*v_head*h
//   out            : (v_head, n_head, T)        e + v_head*h + v_head*n_head*t
#pragma once

#include <cstdint>

namespace u::glm {

/// LayerNorm per column over `dim` (ggml_norm): y = (x - mean)/sqrt(var + eps) * w + b, in place
/// allowed (out may alias x). The DSA indexer's key norm (indexer.k_norm) is a LayerNorm with a bias,
/// not an RMSNorm - the shared common.cuh exposes only rms_norm, so this GLM-lineage helper (the
/// reference's layer_norm_kernel) lives with the DSA kernels.
void glm_layer_norm(const float* x, const float* w, const float* b, float* out, int dim, int T, float eps,
                    void* stream);

/// q absorbed through wk_b: q_abs[c, h, t] = sum_e wk_b[e, c, h] * q[e, h, t]. wk_b is the GGUF
/// attn_k_b tensor laid out (qk_nope, kv_lora, n_head). Ported from glm_model.cu q_absorb_kernel.
void glm_q_absorb(const float* wk_b, const float* q, float* q_abs, int qk_nope, int kv_lora, int n_head,
                  int T, void* stream);

/// pooled from ik, ig, ape (see above). n_pools is the number of COMPLETED pools; for the T=1 serve
/// path the graph calls this once, for the single pool that just completed, into its slot.
void glm_dsa_pool(const float* ik, const float* ig, const float* ape, int key_dim, int kpool, int n_pools,
                  float* pooled, void* stream);

/// score[t, p] over every (query, completed pool) pair; visibility is the select kernel's business.
void glm_dsa_score(const float* iq, const float* pooled, const float* weights, int key_dim, int idx_heads,
                   int n_tokens, int n_pools, float* score, void* stream);

/// The selection: cells[s + n_sel*t], kpool cells per selected pool then the tail, -1 padding.
/// `select_tail` is 0/1; n_sel must be kpool*top_pools + (select_tail ? kpool-1 : 0). `pos[t]` is
/// each query's GLOBAL cache position (a T=1 streaming call passes its cache slot).
void glm_dsa_select(const float* score, int n_pools, int kpool, int top_pools, int select_tail, int T,
                    int n_sel, const int* pos, int* cells, void* stream);

/// The absorbed attention. `cells` rows of n_sel with -1 padding; latents is the whole cache
/// (n_cache >= every referenced cell). out is (v_head, n_head, T).
void glm_mla_attention(const float* q_abs, const float* latents, const int* cells, const float* wv_b,
                       int kv_lora, int n_head, int v_head, int qk_nope, int T, int n_sel, float* out,
                       void* stream);

}  // namespace u::glm
