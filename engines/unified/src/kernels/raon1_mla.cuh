// strata-unified-engine: Raon-1-mini Gated-MLA — the latent/MLA attention used on 7 of the 25
// layers. Raon's near-twin of GLM-5.3-Flash's absorbed MLA
// (engines/maya-ref/src/kernels/cuda/glm_dsa.cu :: glm_mla_attention_kernel), with three Raon
// differences:
//   1. NoPE — there is NO rotary split. GLM's qk = qk_nope + qk_rope; Raon's qk is ENTIRELY nope
//      (qk_nope = 192), so no rope kernel is called anywhere in this family (u_geometry pins
//      RopeKind::nope) and the score scale is rsqrt(qk_nope) over the full 192.
//   2. DENSE causal attention, not GLM's DSA sparse k-pool selection. GLM selects n_sel cells via
//      the indexer and buffers n_sel probs in shared memory; Raon (table in the design doc: "Gated
//      MLA", no indexer column) attends over ALL visible cells [0, pos], so this kernel uses an
//      ONLINE (streaming) softmax — running max / running denom / running ctx accumulator — and
//      needs no O(context) score buffer. That keeps it correct at Raon's 262144 context.
//   3. Raon dims: qk_nope 192, v_head 128, kv_lora 512, q_lora 576 (query down-proj rank).
//
// GLM ANALOGY (piece by piece):
//   mla_absorb_q  <- the q-absorption GLM does before glm_mla_attention: fold the per-head K
//                    up-projection wk_b into the query so scores are a dot in the kv_lora LATENT
//                    space (q_abs is kv_lora-wide per head). GLM precomputes q_abs; we expose it
//                    as its own launcher so the graph can reuse the shared matmul where possible.
//   mla_attention <- glm_mla_attention_kernel, rewritten dense + online-softmax: per (query,head)
//                    score_s = (q_abs_h . latent_s) * rsqrt(qk_nope) over visible cells, softmax,
//                    ctx = sum prob_s*latent_s (kv_lora wide), out[v,h] = sum_c wv_b[c,v,h]*ctx[c].
//   mla_out_gate  <- the "Gated" in Gated-MLA: a sigmoid output gate on the attention result
//                    before o_proj (cf. GLM KDA's out_gate and qwen35moe's full-attn sigmoid gate).
//
// All arithmetic f32 (GLM's MLA/indexer run F32 for parity; Raon inherits it). The box verifies.
//
// OPEN TODOs for the GPU box (confirm against the real Raon-1-mini; do not treat as settled):
//   - The exact absorption: GLM absorbs K's up-proj (wk_b) into q and V's up-proj (wv_b) out of
//     ctx. Confirm Raon's kv_a/kv_b tensor split and that the latent cached per token is the
//     kv_lora-wide post-norm compressed KV (deepseek2/GLM convention).
//   - Whether Raon's MLA is truly dense or carries a lightweight selector. The design doc's table
//     shows no indexer for Raon; if the GGUF ships indexer tensors, port glm_dsa's pool/score/
//     select ahead of mla_attention and pass the selected cells instead of [0,pos].
//   - The gate source and shape for mla_out_gate: is it a per-(v_head,head) projection gated by
//     sigmoid, a per-head scalar, or absent? Written here as a per-element sigmoid gate (GLM KDA
//     shape); confirm and adjust. If Raon has no MLA output gate, the graph skips this launcher.
//   - qk_nope = 192 as the score-scale denominator (NoPE: the whole head). Confirm the scale.
#pragma once

#include <cstdint>

namespace u::raon {

// Absorb the per-head K up-projection wk_b into the projected query, giving q_abs: kv_lora floats
// per (head, token). wk_b is [kv_lora, qk_nope, n_head] (ggml order, kv_lora fastest).
// q is [qk_nope, n_head, T]. Mirrors GLM's pre-attention q-absorption.
void mla_absorb_q(const float* q, const float* wk_b, int kv_lora, int qk_nope, int n_head, int T,
                  float* q_abs, void* stream);

// Dense causal latent attention with online softmax. `latents` is the per-token KV latent cache
// (kv_lora floats per cell), cells 0..(pos_base+T-1) valid. For query token t, `pos` gives its
// global cache position (visibility is cells [0, pos[t]]). wv_b is [kv_lora, v_head, n_head].
// Writes out: v_head floats per (head, token). Mirrors glm_mla_attention_kernel, dense.
void mla_attention(const float* q_abs, const float* latents, const int* pos, const float* wv_b,
                   int kv_lora, int n_head, int v_head, int qk_nope, int T, float* out, void* stream);

// The Gated-MLA output gate: gated[v,h,t] = out[v,h,t] * sigmoid(gate[v,h,t]). `gate` has the same
// (v_head, n_head, T) layout as `out`. TODO(box): confirm the gate's shape/source (see header).
void mla_out_gate(const float* out, const float* gate, int v_head, int n_head, int T, float* gated,
                  void* stream);

}  // namespace u::raon
