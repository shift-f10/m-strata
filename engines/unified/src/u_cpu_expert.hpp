// engines/unified/src/u_cpu_expert.hpp - the CPU expert-compute lane (calibration Stage C3).
//
// Computes a non-resident MoE expert's FFN on the HOST CPU, straight from the memory-mapped quant
// weights, so the pcie-frac split can offload some streamed experts off the PCIe copy path onto spare
// CPU cores. Host-only (no CUDA): the ExpertTier (u_experts.cu) owns the thread pool + the CPU/PCIe
// split + the merge back to the device; this module is the per-expert CPU arithmetic. The i-quant
// dequant mirrors the GPU path (engines/unified/src/kernels/u_iq.cu) using byte-identical codebook
// grids (u_cpu_expert_grids.inc), so the result is numerically valid (not bit-identical to the GPU's
// fused Q8_1 int path -- the CPU lane therefore makes the MoE output split-dependent, which is why it
// is off by default and opt-in via calibration).
#pragma once

#include <cstdint>

namespace u {

// True iff an expert whose gate/up/down are these GGML quant type ids can be computed on the CPU lane
// (a host dequant exists). Today: IQ2_XXS (16), IQ3_XXS (18), IQ2_S (22) -- the GLM-5.3 expert set --
// plus IQ4_NL (20), IQ3_S (21), IQ4_XS (23) and the Strata-custom Q2_0 (42) -- the qwen4exp set.
[[nodiscard]] bool cpu_expert_supported(int gate_type, int up_type, int down_type);

// Compute one expert's FFN on the CPU (single-threaded; the caller parallelizes across experts):
//   g = W_gate . x ; u = W_up . x ; h = act(g, u, limit) ; out = W_down . h
// gate/up are row-major [n_ff, n_embd]; down is [n_embd, n_ff]. x is [n_embd], out is [n_embd].
// act: 0 = geglu, 1 = swiglu, 2 = swiglu_clamped (matches u::k::Act). `scratch` must hold >= 2*n_ff
// floats. n_embd and n_ff must be multiples of 256.
void cpu_expert_ffn(int gate_type, const void* gate, int up_type, const void* up, int down_type,
                    const void* down, int n_embd, int n_ff, int act, float limit, const float* x,
                    float* out, float* scratch);

}  // namespace u
