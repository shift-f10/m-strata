// engines/unified/src/kernels/glm_dflash.cuh - the glm5-next speculative-draft path: the model's own
// NextN (MTP) block and the optional DFlash2 block-diffusion drafter, for the unified engine's serve
// loop. GLM-specific; Raon-1-mini reuses this path (design §3.2).
//
// SCOPE OF THIS PORT: the NextN block's one GLM-specific glue kernel (the eh_proj input assembly) is
// ported concrete below. The block's attention + FFN reuse the trunk kernels (glm_kda / glm_dsa /
// glm_ffn + u::ExpertTier), and the verify-window / acceptance / DFlash orchestration is graph logic
// that the GPU box must finish - it lives in the reference across several large files and is the one
// piece this port leaves as a structured TODO (the task's "structure + TODO where deep", rather than
// a silent guess):
//
//   NextN block + draft head : engines/maya-ref/src/core/glm_mtp.cu (one-GPU)      [768 lines]
//                              engines/maya-ref/src/core/glm_mtp_rows.cu (batched)
//   pipelined spec decode     : engines/maya-ref/src/core/glm_fast_path.cu::decode_spec
//   verify window             : engines/maya-ref/src/core/glm_window.cu
//   DFlash2 dense drafter     : engines/maya-ref/src/core/glm_dflash.cu            [2050 lines]
//                              engines/maya-ref/include/strata/core/glm_dflash.hpp (the interface)
//
// THE NEXTN BLOCK, exactly (ref: glm_model.cu L3628, "without hyper-connections, read through eh_proj
// from [enorm(embedding of the next token), hnorm(final hidden state)]"; tensors blk.<n_layers>.*
// incl. nextn.enorm/hnorm/eh_proj/shared_head_norm - glm_model.cu L264-274):
//   1. in     = eh_proj @ concat[ rms_norm(emb(next_tok), enorm), rms_norm(h_final, hnorm) ]
//               (2*n_embd -> n_embd; emb is the trunk's token embedding row, h_final the trunk's last
//                hidden state at the draft's base position)
//   2. block  = one decoder layer over `in` as a SINGLE stream (NO mHC): the same KDA-or-DSA mixer and
//               dense/MoE FFN the trunk layer il=n_layers would run, with the block's own weights and
//               its own recurrent / latent caches (glm_model.cu sizes mtp_il_ caches at L3396-3403)
//   3. head   = argmax over the draft vocabulary of output.weight @ rms_norm(block_out, shared_head_norm)
//               (the draft vocabulary is the model's head restricted to byte-level BPE tokens;
//                glm_mtp.cu L98-166, mtp_vocab_scan)
//
// A ROUND (glm_mtp.cu L4-15): draft up to N tokens by chaining the NextN block (STRATA_GLM_MTP_DRAFT,
// 3), run a VERIFY WINDOW of [y, d1..dN] through the trunk in one batched pass, accept each d_{t+1}
// while it equals the pick at row t, take the first mismatch as the new token, advance the recurrent
// states by the kept rows, and refresh the NextN cache with the kept rows' trunk hidden states. The
// emitted tokens are exactly what the token-at-a-time loop would produce for the same samples.
#pragma once

#include <cstdint>

namespace u::glm {

/// Assemble the NextN block's eh_proj input row: out[0..n_embd) = rms_norm(emb, enorm),
/// out[n_embd..2*n_embd) = rms_norm(h_final, hnorm), elementwise (one thread per element). The
/// result feeds a (2*n_embd -> n_embd) GEMV against nextn.eh_proj (the shared quantized matmul).
/// Ported structure from the reference's NextN input stage (glm_model.cu L3628); the two rms norms
/// use the model's rms epsilon, matching every other norm in the graph.
void glm_nextn_eh_input(const float* emb, const float* h_final, const float* enorm_w, const float* hnorm_w,
                        int n_embd, float eps, float* out, void* stream);

// TODO(gpu-box): the verify-window acceptance reducer and the DFlash2 candidate-lattice walk are not
// ported here (deep, and only exercisable on the box). Port from:
//   - glm_fast_path.cu::decode_spec (the pipelined accept/commit loop)
//   - glm_window.cu (the batched verify window: each row's pick = the token-at-a-time decode's pick)
//   - glm_dflash.cu (GlmDflash: the dense block-diffusion drafter GGUF, its K/V ring built from the
//     trunk's hidden states, the grouped dynamic convolution, and the top-k candidate selector).
// The unified-engine graph (family/glm_graph.cu) calls the NextN block through the trunk kernels and
// this eh_input helper; wiring the full round is the box's Plan-1 drafting milestone (plan T1.3/T1.6).

}  // namespace u::glm
