// engines/unified/src/kernels/qwen4exp_qsa.cuh - Qwen3.8-Flash-Next QSA (Qwen Sparse Attention), the
// family-specific mixer for the 12 full-attention layers (every 4th: 3,7,...,47).
//
// PORTED FROM (reviewed, verbatim math, renamed into the unified engine's u::qwen4exp namespace):
//   upstream/src/kernels/cuda/native_qsa.cu            (QSA RMS norms + the output gate)
//   upstream/src/kernels/cuda/native_qsa_indexer.cu    (the indexer: pooled-key append, norm, RoPE)
//   upstream/src/kernels/cuda/qsa_select.cu            (block scores + top-k cell selection)
//   upstream/src/kernels/cuda/qsa_decode_attn.cu       (sparse flash attention over selected cells)
//   upstream/src/kernels/cuda/qsa_prompt_attn.cu       (the prefill variant)
//   upstream/src/kernels/cuda/native_rope.cu           (NEOX RoPE on the first n_rot of head_dim)
// Reference spec: upstream docs/semantics.md (QSA), ref/qsa.py.
//
// WHAT QSA IS. A GQA attention (24 query heads, 2 KV heads, head_dim 256) whose per-query key set is
// SPARSE: a lightweight indexer (4 heads, key_dim 128) scores pooled blocks of the past (one pooled
// key per idx_block=4 cells) and selects the top idx_top_k(=2048) cells; the full attention then runs
// only over the selected cells. NEOX RoPE rotates the first n_rot(=64) of head_dim (and of the
// indexer key). This is the model's long-context mechanism and the main QSA port cost.
//
// THE GRAPH does q/k/v projections (via u::k::mmvq / BF16) and the residual/HC; these kernels do the
// QSA-specific pieces. LAYOUT: f32, ggml order. hd = head_dim(256), H = n_head(24), Hkv = n_head_kv(2),
// n_rot = 64; indexer: idH = idx_q_heads(4), idD = idx_key_dim(128), block = idx_block(4).
//
// NOTE ON REUSE. The non-sparse fallback (attend over the whole context) is exactly the shared
// u::k::attn_decode / attn_decode_split+merge (common.cuh). QSA differs only by the selected-cell
// gather; the box can validate QSA against that dense path first (set idx_top_k >= context so the
// selection is the identity), which is the recommended parity ladder.
#pragma once

#include <cstdint>

namespace u::qwen4exp {

struct QsaShapes final {
  int head_dim = 256;     // hd
  int n_head = 24;        // H (query heads)
  int n_head_kv = 2;      // Hkv (GQA key/value heads)
  int n_rot = 64;         // rotated slice of head_dim and of the indexer key
  int idx_q_heads = 4;    // idH
  int idx_key_dim = 128;  // idD
  int idx_block = 4;      // pooled key per `block` cells
  int idx_top_k = 2048;   // selection budget (cells)
};

// NEOX RoPE on the first n_rot of each head's head_dim, in place, over q and k (per token position
// `pos`). `x` is (hd, heads, T). This wraps the shared u::k::rope_neox_base for the partial-rotary
// NEOX layout qwen uses; kept in the QSA module because the indexer key needs the same rotation on a
// different width (idD). Ported from upstream native_rope.cu (NEOX partial). The graph may call
// u::k::rope_neox_base directly instead — identical math (common.cuh L149-152).
void qsa_rope_neox(float* x, int heads, int hd, int n_rot, double freq_base, const int* pos, int T,
                   void* stream);

// Append this step's pooled indexer keys to the pooled-key cache and RoPE them. The indexer pools the
// past into one key per `block` cells; `raw` is the indexer k-projection (idD, idH, T), `pos` the
// token positions. Updates `pooled_cache` (idD, idH, n_blocks) in place. Ported from upstream
// native_qsa_indexer.cu append_first/append_blocks/append_tail (L186-254).
void qsa_indexer_append(const float* raw, const float* idx_k_norm, float eps, const int* pos,
                        const QsaShapes& s, int T, float* pooled_cache, int n_cells, void* stream);

// Score every pooled block against the indexer query and select the top idx_top_k cells per query.
// `idx_q` is (idD, idH, T) (RoPE'd); `pooled_cache` (idD, idH, n_blocks). Writes, per token, the
// selected cell ids `sel` (idx_top_k) and their count `sel_n`. Ported from upstream qsa_select.cu
// block_scores_kernel (L34) + block_topk_kernel (L63) — the generic (non tensor-core) path; the
// WMMA/TC fast paths (block_scores_tc_kernel L201, block_scores_wmma_kernel L355) are a box speed
// extension, not needed for parity.
void qsa_select_cells(const float* idx_q, const float* pooled_cache, const QsaShapes& s, int n_cells,
                      int T, int* sel, int* sel_n, void* stream);

// Sparse flash attention over the SELECTED cells only. `q` is (hd, H, T); `k_cache`,`v_cache` are the
// per-QSA-layer KV cache (hd, Hkv, n_cells); `sel`/`sel_n` from qsa_select_cells. Online-softmax, GQA
// (query head h reads KV head h / (H/Hkv)), scale 1/sqrt(hd). Writes `o` (hd, H, T). Faithful
// transcription of upstream qsa_decode_attn.cu attn_chunk_body (L125) + attn_merge (decode); the
// prefill path is qsa_prompt_attn.cu. For the first parity gate, pass sel = all cells to reduce this
// to the dense u::k::attn_decode (common.cuh L167-169).
void qsa_attention(const float* q, const float* k_cache, const float* v_cache, const int* sel,
                   const int* sel_n, const QsaShapes& s, int n_cells, int T, float scale,
                   float* o, void* stream);

// Append this step's k,v into the per-layer KV cache at the token positions (hd, Hkv, n_cells).
// Ported from upstream kv_stream.cu / the QSA KV append. (The unified engine keeps an f32 KV here;
// the upstream f16/q4/q8 KV-quant ring — kv_q4.cu/kv_q8.cu — is a box memory extension.)
void qsa_kv_append(const float* k, const float* v, const int* pos, const QsaShapes& s, int T,
                   float* k_cache, float* v_cache, void* stream);

// The QSA output gate: y = o * sigmoid(gate_proj) per head (the "gated" attention output;
// native_qsa.cu gate kernel L94). `o`,`gate` are (hd, H, T). Then the graph's ssm/attn_out GEMV.
void qsa_out_gate(const float* o, const float* gate, const QsaShapes& s, int T, float* y, void* stream);

}  // namespace u::qwen4exp
