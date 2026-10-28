// strata-unified-engine: i-quant (IQ2_XXS / IQ2_S / IQ3_XXS) decode MMVQ for GLM-5.3 MoE experts.
// Ported from engines/maya-ref/src/kernels/cuda/iq_kernels.cu (path A) + ggml-common.h codebook grids.
// common.cu's mmvq dispatch routes these three GGML type ids here (they are not in the k-quant template).
#pragma once

#include <cstddef>

namespace u::k {

// True for the three i-quant GGML type ids this engine decodes (16 IQ2_XXS, 18 IQ3_XXS, 22 IQ2_S).
[[nodiscard]] bool iq_supported(int type);

// Bytes of one weight row (n_in elements) in the given i-quant type (256-element super-blocks:
// IQ2_XXS 66 B, IQ2_S 82 B, IQ3_XXS 98 B per block). 0 for an unsupported type.
[[nodiscard]] std::size_t iq_row_bytes(int type, int n_in);

// One single-column decode matvec y[n_out] = W(type) . x, where x_q8_1 is the activation column already
// quantized to Q8_1 (n_in/32 blocks, the layout u::k::quantize_q8_1 produces). One warp per output row.
void iq_mmvq_col(int type, const void* w, const void* x_q8_1, float* y, int n_in, int n_out, void* stream);
// The same over `ncols` (1..8) activation columns, column c at x_q8_1 + c*x_stride_blocks Q8_1 blocks and
// y + c*n_out: one launch, each column BITWISE an iq_mmvq_col call (the speculative verify window).
void iq_mmvq_multi(int type, const void* w, const void* x_q8_1, long long x_stride_blocks, float* y, int n_in,
                   int n_out, int ncols, void* stream);

// Where routed expert j of one MoE layer is read, per expert_route_plan (eff[j], on the device):
//   >= 0            its VRAM cache slot
//   kEffZeroCopy    the pinned host arena, zero-copy over PCIe (staging off)
//   <= kEffStage0   VRAM staging slot (kEffStage0 - eff[j]), filled by expert_route_plan
//   kEffSkip        a tail expert that is not computed: its output rows are zero
//   kEffCpu         a miss the CPU expert lane computes (expert_lane_plan): the grouped kernels write zero
//                   rows and read nothing; lane_merge copies the CPU's rows in
constexpr int kEffZeroCopy = -1;
constexpr int kEffStage0 = -2;
constexpr int kEffSkip = -1000000;
constexpr int kEffCpu = -2000000;

// Plan one layer's k routed experts ON THE DEVICE and stage the misses (upstream verify.cpp's PCIe share,
// pcie_mode 2: "stage it with a copy kernel, then point at staging"). For j < k, e = ids[j]:
// res[e] >= 0 => eff[j] = res[e]; else, when 0 < *tail_rank <= j (upstream STRATA_ROUTE_TAIL_SKIP: a missed
// expert routed at rank >= *tail_rank; ids are in descending weight order; the rank is read on the device,
// so one captured graph serves prefill (off) and decode (on)) => kEffSkip; else, with
// `stage` set, expert e's gate|up|down are copied from the mapped host arena (src_gate/up/down + e*stride,
// device pointers to mapped pinned memory) into staging slot j (stage + j*stage_stride, the matrices at
// 0, gate_bytes, gate_bytes+up_bytes) by many blocks of coalesced 16-byte zero-copy loads, and eff[j] =
// kEffStage0 - j; with `stage` null => kEffZeroCopy (the grouped kernel then reads the arena itself).
// k may cover several tokens' entries (the verify window: k = T * k_per_tok, token-major); an entry's
// tail-skip rank is its index within its token (j % k_per_tok; k_per_tok <= 0 means k).
void expert_route_plan(const int* ids, const int* res, int k, const int* tail_rank, const void* src_gate,
                       long long gate_stride, long long gate_bytes, const void* src_up, long long up_stride,
                       long long up_bytes, const void* src_down, long long down_stride, long long down_bytes,
                       void* stage, long long stage_stride, int* eff, void* stream, int k_per_tok = 0);

// ---- the CPU expert lane's device half (upstream verify.cpp's per-layer doorbell; u_cpu_lane is the host half)
//
// expert_lane_plan: one layer's n <= kLaneMaxEntries entries (token-major, k_per_tok a token) planned ON THE
// DEVICE by upstream's rule (expert_source.cpp, expert_pool_dispatch_multi): the distinct experts in routing
// order; a resident one => its slot; a missed one that every token routes at rank >= *tail_rank (> 0) =>
// kEffSkip (STRATA_ROUTE_TAIL_SKIP); of the other misses, the LAST (n_miss * lane[1]) >> 8 go to the GPU
// (kEffZeroCopy: lane[1] is upstream's pcie_num = pcie_frac * 256) and the rest to the CPU (kEffCpu). An
// expert's entries share its code. lane[0] == 0 (the lane off for this launch) => every miss kEffZeroCopy.
// *ncpu = the entries coded kEffCpu.
constexpr int kLaneMaxEntries = 128;
void expert_lane_plan(const int* ids, const int* res, int n, int k_per_tok, const int* tail_rank, const int* lane,
                      int* eff, int* ncpu, void* stream);
// lane_doorbell (upstream doorbell_publish_kernel): with the lane on, the layer's ids and eff into the mapped
// m_ids / m_eff, its MoE input x [nx floats] into the mapped m_x when *ncpu > 0, then -- after a system fence --
// the mapped *m_seq = want (the host's serve loop spins on it). m_* are device pointers to mapped pinned memory.
void lane_doorbell(const float* x, int nx, const int* ids, const int* eff, int n, const int* lane, const int* ncpu,
                   float* m_x, int* m_ids, int* m_eff, unsigned* m_seq, unsigned want, void* stream);
// lane_merge (upstream copy_rows_from_mapped_after): with the lane on and *ncpu > 0, each entry j coded kEffCpu
// waits for the mapped *m_flag >= want (the host served the layer) and copies its row m_ymiss[j] -> parts[j]
// (width floats, a multiple of 4; 16-byte aligned rows).
void lane_merge(const int* eff, int n, const int* lane, const int* ncpu, const unsigned* m_flag, unsigned want,
                const float* m_ymiss, float* parts, int width, void* stream);

// Grouped decode matvec over the k routed experts of one MoE layer, one launch: y_j[n_out] = W_{e_j} . x_j
// for j < k. Expert e_j = ids[j] (device). Its matrix base is resolved ON THE DEVICE from eff[j] (see
// expert_route_plan), so the launch needs no host-side routing: a VRAM cache slot (vram_base +
// eff*slot_bytes + mat_off), a staging slot (stage_base + (kEffStage0 - eff)*stage_stride + mat_off), the
// pinned host arena read zero-copy over PCIe (host_base + e*host_stride; a device pointer to mapped pinned
// memory), or kEffSkip / kEffCpu (y_j = 0). x_j = x + (j / x_group)*x_stride_blocks Q8_1 blocks (x_group entries share
// an activation column: a token's k gate/up entries; 1 = a column each); y_j = y + j*y_stride floats. One
// warp per output row, blockIdx.y = j.
void iq_mmvq_grouped(int type, const int* ids, const int* eff, const void* vram_base, long long slot_bytes,
                     long long mat_off, const void* stage_base, long long stage_stride, const void* host_base,
                     long long host_stride, const void* x_q8_1, long long x_stride_blocks, float* y,
                     long long y_stride, int n_in, int n_out, int k, void* stream, int x_group = 1);

// Dequantize an IQ4_XS buffer of `n` elements (n % 256 == 0) to f32 -- the qwen4exp token_embd gather.
void dequant_iq4xs(const void* w, int n, float* out, void* stream);
// The same for row *d_row of a table of `row_bytes` rows, the row index read on the device (the token id,
// so a decode step's embed gather has fixed launch arguments: CUDA-graph capturable).
void dequant_iq4xs_row(const void* table, std::size_t row_bytes, const int* d_row, int n, float* out, void* stream);

}  // namespace u::k
