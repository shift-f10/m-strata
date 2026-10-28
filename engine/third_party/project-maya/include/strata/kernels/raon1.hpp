// include/strata/kernels/raon1.hpp - raon1 (Raon-1-mini) specific kernels, reusing the GLM kernel family for the
// rest.  See docs/superpowers/specs/2026-10-25-raon1-mini-port-blueprint.md and ik_llama.cpp PR #2733.
#pragma once

#include <cstddef>
#include <cstdint>

namespace strata::kernels {

// ---- raon1's latent cache (the fast path's --kv): a cell is kw = kv_lora + n_extra values - F32, or ggml's Q8_0 /
// Q4_0 blocks of 32 ({f16 d; 32 int8} / {f16 d; 16 bytes of 4-bit codes, value = d * (code - 8)}), as llama.cpp's
// --cache-type-k q8_0 / q4_0 store raon1's K-only MLA cache.  Types are ggml ids: 0 F32, 8 Q8_0, 2 Q4_0.
inline size_t raon1_lat_row_bytes(int type, int kw) {
    return type == 8 ? (size_t) kw / 32 * 34 : type == 2 ? (size_t) kw / 32 * 18 : (size_t) kw * 4;
}

/// T cells into the cache, row t at dst + t * raon1_lat_row_bytes(type, kw): F32 copied, Q8_0 / Q4_0 quantized with
/// ggml's reference rounding (quantize_row_q8_0_ref / quantize_row_q4_0_ref).  rows: T x kw F32, kw a multiple of 32.
void raon1_lat_put(const float* rows, int T, int kw, int type, void* dst, void* stream);

/// n cells of the cache back to F32 (T x kw, the values the decode's attention reads) - the prompt path's GEMMs.
void raon1_lat_get(const void* src, int n, int kw, int type, float* out, void* stream);

/// The fast path's raon1 Gated-MLA decode attention on a cache of `type` (raon1_mla_attention_bf16's arithmetic): the
/// cells split into up to kRaon1MlaSplits chunks a head, each chunk's online softmax into part (n_head x
/// kRaon1MlaSplits x (kv_lora + 2) floats), then per head their combination and wv_b's projection into out
/// [v_head, n_head].  kv_lora <= 512 and kw <= 768, both multiples of 32.
inline constexpr int kRaon1MlaSplits = 64;
void raon1_mla_decode(const float* q_abs, const void* cache, int type, int n_cells, const uint16_t* wv_b, int kv_lora,
                      int n_extra, int n_head, int v_head, float scale, float* part, float* out, void* stream);

/// raon1 RDA gate pre-scale on the (conv'd) V stream, per head, broadcast over head_dim:
///   erase = softplus(-beta), write = softplus(-(beta + delta)); v[i] *= exp(erase - write)
/// beta = ssm_beta . x (RAW, per head), delta = ssm_rda . x (per head).  This makes the standard delta rule
/// S' = S(I - erase k kᵀ) + write v kᵀ exact once the fused delta-net scan runs with sigmoid(beta).
/// (ik_llama.cpp llama-delta-net.cpp: the RDA branch of delta_net::build_qkv.)
void raon1_rda_vscale(const float* beta, const float* delta, float* v, int head_dim, int n_head, void* stream);

/// raon1 Gated-MLA decode attention (one query, n_cells cache cells), NoPE + absorbed.
///   q_abs  : [kw, n_head]      kw = kv_lora + n_extra  (absorbed-nope latent then the 64 extra channels)
///   kcache : [kw, n_cells]     each cell = [norm latent (kv_lora) | 64 extra]; VALUE is the first kv_lora
///   wv_b   : GGUF {kv_lora, v_head, n_head}
///   out    : [v_head, n_head]
/// score = (q_abs_h . kcache_s) * scale over kw dims; softmax over cells; ctx = sum p_s * latent_s; out = wv_b.ctx.
/// The output gate (attn_gate) is applied by the caller.  kv_lora must be a multiple of 32.
void raon1_mla_attention(const float* q_abs, const float* kcache, int n_cells, const float* wv_b, int kv_lora,
                         int n_extra, int n_head, int v_head, float scale, float* out, void* stream);

/// Build Qcur [kv_lora + n_extra, n_head] from q_b's output [qk_nope + n_extra, n_head]: nope absorbed through
/// wk_b, extra (NoPE) copied through.
void raon1_build_qcur(const float* wk_b, const float* q, float* qcur, int qk_nope, int n_extra, int kv_lora,
                      int n_head, void* stream);

/// x[i] *= sigmoid(gate[i]) — raon1's Gated-MLA output gate (attn_gate).
void raon1_gate_mul(float* x, const float* gate, int64_t n, void* stream);

/// BF16-weight variants of build_qcur / mla_attention for the fast path (attn_k_b / attn_v_b are BF16 there,
/// dequantized to F32 in the reference step path); identical math, the weight reads go through a bf16->f32 decode.
void raon1_build_qcur_bf16(const uint16_t* wk_b, const float* q, float* qcur, int qk_nope, int n_extra, int kv_lora,
                           int n_head, void* stream);
void raon1_mla_attention_bf16(const float* q_abs, const float* kcache, int n_cells, const uint16_t* wv_b, int kv_lora,
                              int n_extra, int n_head, int v_head, float scale, float* out, void* stream);

/// raon1 Raon-mHC (Helmert hyper-connections), read half.  `mixes` = hc_fn . R (gemv output, mix_dim =
/// 2*hc + (hc-1)^2 wide, NOT yet rms-divided); `scale` = [alpha_pre, alpha_post, alpha_z, eta] (4);
/// `base` = mix_dim.  Produces pre[hc], post[hc] (= magnitude*sigmoid), comb[hc*hc] (comb[d*hc + s], the Helmert
/// doubly-stochastic mixing) — all consumed by glm_hc_post, which raon1 reuses.  See strata/kernels/raon1
/// blueprint / KRAFTON llama.cpp raon1.cpp build_hc_pre.
void raon1_hc_gates(const float* R, const float* mixes, const float* scale, const float* base, float hc_eps,
                    float magnitude, int n_embd, int hc, float* pre, float* post, float* comb, void* stream);

/// raon1 mHC pre mixing: mixed[e] = sum_i pre[i] * R[i*n_embd + e] (the pre-gated stream sum fed to the mixer norm).
void raon1_hc_mix(const float* R, const float* pre, int n_embd, int hc, float* mixed, void* stream);

// ---- the prompt path's batched forms (glm_prefill.cu's raon1 mixers; one launch a sub-batch, not one a token) ---

/// T rows of the decode's rms_norm: out[t*ld_out + i] = in[t*ld_in + i] * rsqrt(mean_i(in_t^2) + eps) * w[i],
/// i < n (in place when in == out and the strides agree).
void raon1_rms_rows(const float* in, int ld_in, const float* w, float eps, int n, int T, float* out, int ld_out,
                    void* stream);

/// raon1_rda_vscale over T tokens: token t's beta at beta + t*ld_beta, delta at delta + t*ld_delta (n_head each),
/// its V stream at v + t*ld_v (n_head x head_dim).
void raon1_rda_vscale_rows(const float* beta, int ld_beta, const float* delta, int ld_delta, float* v, int ld_v,
                           int head_dim, int n_head, int T, void* stream);

/// The causal softmax over a query block's attention scores, in place: query t's head h has its scores against
/// cells [0, n_cells) in column t * n_head + h (sc + (t*n_head + h)*n_cells), and sees cells [0, first + t + 1) - the
/// softmax over those, the rest zeroed (first: the block's first query's position; qb queries).
void raon1_softmax_causal(float* sc, int n_cells, int qb, int n_head, int first, void* stream);

}  // namespace strata::kernels
