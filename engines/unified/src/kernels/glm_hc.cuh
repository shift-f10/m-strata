// engines/unified/src/kernels/glm_hc.cuh - the glm5-next mHC (manifold-constrained hyper-connections),
// the GLM-specific residual-stream mixer for the unified engine.
//
// PORTED FROM (reviewed, verbatim math, renamed into u::glm):
//   engines/maya-ref/include/strata/kernels/glm_hc.hpp   (the math + GlmHcShapes/Workspace)
//   engines/maya-ref/src/kernels/cuda/glm_hc.cu          (inv_rms, pre, post)
//   engines/maya-ref/src/core/glm_model.cu               (mean_streams_kernel L417; hc_init = repeat)
// Reference spec: engines/maya-ref/ref/glm.py::hc_pre / hc_sinkhorn / hc_post, transcribed from
//   engines/maya-ref/ref/upstream-llama.cpp/glm5-next.cpp L371-548.
//
// glm5-next keeps a residual STACK of `hc` (= 4) streams, R = (n_embd, hc). Each sub-layer (attn and
// ffn) reads the stack through a projection + three affine gates + a per-token Sinkhorn mix, and
// writes its output back into a fresh stack. For one token, R the (hc, n_embd) stack:
//
//     flat      = R flattened, stream-major (element (e, s) at s*n_embd + e)
//     inv       = rsqrt(mean(flat^2) + norm_eps)       ONE rms over the WHOLE stack (streams jointly)
//     mixes[m]  = sum_k w_fn[m*hc_dim + k] * flat[k] * inv     (hc_mix_dim = (2+hc)*hc values)
//     pre[s]    = sigmoid(mixes[s]*scale[0] + base[s]) + hc_eps
//     post[s]   = 2 * sigmoid(mixes[hc+s]*scale[1] + base[hc+s])
//     comb_raw[d, s] = mixes[2hc + d + hc*s]*scale[2] + base[2hc + d + hc*s]     SRC-MAJOR rows
//     comb      = sinkhorn(comb_raw)
//     mixed[e]  = sum_s pre[s] * R[s*n_embd + e]
//   write-back (hc_post): out[d*n_embd + e] = block_out[e]*post[d] + sum_s comb[d,s]*residual[s*n_embd+e]
//
// PRECISION CONTRACT: F32 weights and F32 activations throughout, matching ref/glm.py.
#pragma once

#include <cstddef>
#include <cstdint>

namespace u::glm {

struct GlmHcShapes {
    int64_t n_embd = 0;
    int64_t hc = 0;            // the architecture fixes 4; the kernels stay generic in it
    int64_t sinkhorn_iters = 0;

    int64_t hc_dim() const { return hc * n_embd; }
    int64_t hc_mix_dim() const { return (2 + hc) * hc; }
};

/// One float: the joint-stack inverse rms, computed by its own kernel because the mixes GEMV
/// consumes it. Carved by the caller once (glm_hc_workspace_bytes).
struct GlmHcWorkspace {
    float* inv_rms = nullptr;
    size_t bytes = 0;
};

size_t glm_hc_workspace_bytes(const GlmHcShapes& s);
void glm_hc_workspace_init(const GlmHcShapes& s, void* base, GlmHcWorkspace& ws);

/// The read half. R is (hc*n_embd) f32, stream-major; w_fn is (hc_mix_dim * hc_dim) with
/// w_fn[m*hc_dim + k] contiguous in k; w_scale is (3,), w_base is (hc_mix_dim,). Outputs: mixed
/// (n_embd), pre (hc), post (hc), comb (hc*hc) with comb[d*hc + s]. norm_eps is the model's rms
/// epsilon; hc_eps is the hyper-connection epsilon (the gate floor and every Sinkhorn divisor's guard).
void glm_hc_pre(const float* R, const float* w_fn, const float* w_scale, const float* w_base,
                float norm_eps, float hc_eps, const GlmHcShapes& s, GlmHcWorkspace& ws,
                float* mixed, float* pre, float* post, float* comb, void* stream);

/// The write half. block_out (n_embd), residual (hc*n_embd), post (hc), comb (hc*hc, d*hc + s).
/// out must not alias residual (the graph writes into a fresh stack).
void glm_hc_post(const float* block_out, const float* residual, const float* post, const float* comb,
                 const GlmHcShapes& s, float* out, void* stream);

/// hc_init: R[s*n_embd + e] = emb[e] for every stream s (ref/glm.py: R = repeat(x, hc)).
void glm_hc_init(const float* emb, float* R, int n_embd, int hc, void* stream);

/// build_hc_mean: y[e] = mean_s R[s*n_embd + e] - the head's plain mean over the hc streams, before
/// the final output_norm + head GEMV. Ported from glm_model.cu mean_streams_kernel (T=1 serve path).
void glm_hc_mean(const float* R, float* y, int n_embd, int hc, int T, void* stream);

}  // namespace u::glm
