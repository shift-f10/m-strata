// engines/unified/src/kernels/glm_ffn.cuh - the glm5-next FFN activation: the GLM-family clamped SwiGLU.
//
// PORTED FROM engines/maya-ref/include/strata/kernels/glm_ffn.hpp + src/kernels/cuda/glm_ffn.cu
// (verbatim), renamed into u::glm. Reference spec: engines/maya-ref/ref/glm.py::swiglu_clamp,
// transcribed from ggml_swiglu_clamp (ggml cpu-ops.cpp L3449).
//
// glm5-next's FFNs (the 3 dense lead layers, the routed experts, and the shared expert) all use the
// GLM-family clamped SwiGLU, whose asymmetry is easy to read past:
//
//     gate = min(gate, limit)            clamped ABOVE ONLY - no lower clamp on the gate
//     up   = clamp(up, -limit, +limit)   clamped on BOTH sides
//     out  = silu(gate) * up
//
// The limit is the config's swiglu_limit (10.0 on the released model); the DENSE and SHARED paths use
// swiglu_clamp_shexp, the routed experts use swiglu_clamp_exp (usually equal). This activation is NOT
// one of the shared generic kernels (u::k exposes plain silu/gelu); it is a GLM-lineage kernel the
// graph applies to the dense + shared FFN, and the shared u::ExpertTier applies the same clamp
// internally to the routed experts (it is constructed with the family's activation + limit; see
// family/glm_graph.cu). The clamp also appears in qwen4exp/gemma's lineage with different limits -
// if the substrate later promotes a generic clamped-SwiGLU, the graph can switch to it.
#pragma once

#include <cstdint>

namespace u::glm {

/// out[i] = silu(min(gate[i], limit)) * clamp(up[i], -limit, +limit), elementwise over n. Uses
/// precise expf (not __expf): the gates reach |g| ~ 30 where the fast intrinsic's ulp error exceeds
/// the parity tolerance.
void glm_swiglu_clamp(const float* gate, const float* up, float limit, int64_t n, float* out, void* stream);

}  // namespace u::glm
