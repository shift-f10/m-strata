// strata-unified-engine: Gemma-4-26B-A4B ZERO-HOST-SYNC on-device MoE expert dispatch (arch
// "gemma4"). This is the faithful port of the reference engine's device-table MoE
// (engines/gemma4/src/g4_kernels.cu + g4_engine.cu, the read-only 26/26-verified parity oracle)
// onto the unified tree. It REPLACES, for gemma4 only, the host-driven u::ExpertTier::run_moe path
// (which copies router picks to the host every layer to index the stacked GGUF tensors and folds
// ffn_down_exps.scale on the host — ~30-60 host syncs/token). The other families keep
// u::ExpertTier::{register_layer,run_moe,run_window} untouched.
//
// HOW IT KILLS THE SYNCS (exactly the reference's design):
//   * Each layer's 128 experts live in a cudaHostAllocMapped arena (device-accessible over PCIe),
//     repacked AT LOAD into the reference's blob layout (Q8_0 split into an int8 plane + an fp16
//     scale plane; BF16 raw). The hottest S experts are also mirrored in VRAM slots.
//   * Per layer a DEVICE `ExpertPtr table[n_expert]` holds each expert's gate|up and down weight+
//     scale pointers — a resident expert points at its VRAM slot, the rest at the mapped-host blob.
//   * `k_matvec_experts<KIND>` is launched with grid.y = n_pick; block (row, k) reads
//     table[picks[k]] with picks a DEVICE array straight from u::k::router_topk. The device picks
//     index the device table, so NO host round-trip is needed to find an expert's weights.
//   * `k_moe_combine` applies the per-expert ffn_down_exps.scale on the DEVICE via the device picks.
//   * The live rebalance is gated by the caller to run at most ~every 8 tokens; only then is there a
//     single D2H copy (the routing counts) to decide the swaps. Between those, zero host syncs.
//
// PARITY: the Q8_0 and BF16 dots, the repack, the GeGLU and the combine are byte-for-byte the
// reference's (same fma order, same gelu_tanh constants, same Q8_0 dequant-against-f32 dot — NOT the
// unified MMVQ Q8_1 path). The box parity-checks this against engines/gemma4.
//
// Host-NON-includable (uses __half / CUDA): included only by gemma4_graph.cu and gemma4_moe.cu, both
// compiled behind STRATA_U_CUDA on the GPU box. Never reached on a host/CI build.
#pragma once

#include <cuda_fp16.h>

#include <cstdint>
#include <string>
#include <vector>

namespace u::gemma4 {

// Which dot a matrix uses. Mirrors g4k::WKind; gemma4 experts are Q8_0 or BF16 only (never F32 —
// the reference's matvec_experts has no F32 path; an F32 expert tensor is a documented box
// extension, rejected at load here).
enum class WKind : int { q8 = 0, bf16 = 1 };

// One expert's two matrices as the GPU reads them (g4_kernels.cuh:30 g4k::ExpertPtr). For a Q8_0
// matrix `*_w` is the int8 plane [rows*cols] and `*_s` the fp16 scale plane [rows*cols/32]; for a
// BF16 matrix `*_w` is the raw uint16 plane [rows*cols] and `*_s` is null. The pointers are device
// memory (a VRAM slot) or mapped pinned host memory (the GPU reads it over PCIe).
struct ExpertPtr {
  const void* gu_w;    // fused gate|up weights  ([2*n_ff_exp, n_embd])
  const __half* gu_s;  // fused gate|up scales   (Q8_0 only, else null)
  const void* dn_w;    // down weights           ([n_embd, n_ff_exp])
  const __half* dn_s;  // down scales            (Q8_0 only, else null)
};

// ---- the ported reference kernels (g4_kernels.cu), exposed so the box can parity-check them ----

// Grouped-by-expert matvec. For k < n_pick: y[k*rows + row] = W_{picks[k]} . x[k*x_stride ..], where
// W is table[picks[k]].gate_up (which == 0) or .down (which == 1). `picks` is a DEVICE array. Mirrors
// g4k::matvec_experts (g4_kernels.cu:683) / k_matvec_experts (137). x_stride == 0 shares one x row.
void matvec_experts(WKind kind, int which, int rows, int cols, const ExpertPtr* table, const int* picks,
                    int n_pick, const float* x, int x_stride, float* y, void* stream);

// m[r*n + i] = gelu_tanh(g[r*g_stride + i]) * u[r*u_stride + i]. The fused gate|up output of
// matvec_experts is read with g = gu, u = gu + n_ff_exp, both strides 2*n_ff_exp. Mirrors g4k::geglu
// (g4_kernels.cu:745) / k_geglu (470).
void geglu_experts(const float* g, int g_stride, const float* u, int u_stride, float* m, int rows, int n,
                   void* stream);

// moe[i] = sum_{k<n_pick} y[k*n + i] * weights[k] * down_scale[picks[k]]. `picks` and `down_scale`
// are DEVICE arrays (the per-expert ffn_down_exps.scale is applied here, on the device). Mirrors
// g4k::moe_combine (g4_kernels.cu:749) / k_moe_combine (477).
void moe_combine(const float* y, const float* weights, const int* picks, const float* down_scale, int n_pick,
                 int n, float* moe, void* stream);

// counts[picks[k]] += 1 for k < n_pick (DEVICE picks + DEVICE counts), the per-token routing tally the
// gated rebalance reads. Mirrors the atomicAdd g4k::k_router_topk folds into its counts output
// (g4_kernels.cu:462); kept a separate kernel so u::k::router_topk stays untouched. `n_pick` may be the
// whole chunk's picks (T*n_exp_used) on the BATCHED PREFILL path, so the launcher grids over it.
void count_picks(const int* picks, int n_pick, int* counts, void* stream);

// ---- BATCHED PREFILL helpers (the forward_batch fast-path; decode never calls these) ----
//
// These three are the faithful port of g4_kernels.cu's gather_rows / dequant_expert / scatter_combine
// (g4k::gather_rows 796, dequant_expert 785, scatter_combine 801), the pieces the reference's host-driven
// moe_batch (g4_engine.cu:591) needs. The device kernels and the Q8_0/BF16 dequant are byte-for-byte the
// oracle's, so a bucketed per-expert cublasSgemm over the gathered rows reproduces the decode MoE's math.

// Gather the token rows the expert buckets index: out[r*n + i] = x[(order[r]/n_used)*n + i], r < count.
// `order[r]` is a pick-slot index a in [0, count); its token is a/n_used. Mirrors g4k::gather_rows.
void gather_rows(const float* x, const int* order, int count, int n, int n_used, float* out, void* stream);

// Dequantise one expert matrix of table[e] to dense f32 [rows*cols] (which 0 = fused gate|up, 1 = down),
// for the per-expert prefill cublasSgemm. Reads the device ExpertPtr's repacked planes (Q8_0 int8 + fp16
// scale, or BF16) exactly as the oracle's g4k::dequant_expert. `out` is DEVICE [rows*cols].
void dequant_expert(WKind kind, int which, int rows, int cols, const ExpertPtr* table, int e, float* out,
                    void* stream);

// Scatter the per-expert down outputs back to token order and combine with the router weights and the
// per-expert ffn_down_exps.scale: moe[t*n + i] = sum_{k<n_used} y[inv[t*n_used+k]*n + i] *
// weights[t*n_used+k] * down_scale[picks[t*n_used+k]]. DEVICE arrays throughout. Mirrors
// g4k::scatter_combine (the fma order and the device down-scale fold match k_moe_combine).
void scatter_combine(const float* y, const int* inv, const float* weights, const int* picks,
                     const float* down_scale, int T, int n, int n_used, float* moe, void* stream);

// ================================ the device-table MoE facility ================================
//
// Owns the mapped expert arena, the VRAM resident slots, the per-layer device ExpertPtr tables, the
// device ffn_down_exps.scale, the decode scratch and the routing counts. The gemma4 graph owns one
// instance (in its PIMPL). Lifetime: open() -> load_layer() per layer -> finalize() -> run_moe()
// per token (+ gated rebalance()).
class Gemma4Experts {
 public:
  Gemma4Experts() = default;
  ~Gemma4Experts();
  Gemma4Experts(const Gemma4Experts&) = delete;
  Gemma4Experts& operator=(const Gemma4Experts&) = delete;

  // Dimensions (the pinned 26B-A4B shape: n_expert 128, n_embd 2816, n_ff_exp 704, n_exp_used 8).
  // Allocates the decode scratch and the routing-count buffers. False with `err` on a bad geometry
  // or a failed allocation.
  [[nodiscard]] bool open(int n_layers, int n_expert, int n_embd, int n_ff_exp, int n_exp_used,
                          std::string& err);

  // Repack layer `layer`'s experts from the mmap into this layer's cudaHostAllocMapped arena.
  // `gu_src` / `dn_src` are `mapped + tensor_offset` for ffn_gate_up_exps / ffn_down_exps (the fused
  // gate|up tensor and the down tensor); `gu_type` / `dn_type` are GGML type ids (8 = Q8_0,
  // 30 = BF16). `down_scale` is the host [n_expert] ffn_down_exps.scale (uploaded to the device in
  // finalize). Call once per layer, 0..n_layers-1, before finalize.
  [[nodiscard]] bool load_layer(int layer, const std::uint8_t* gu_src, int gu_type, const std::uint8_t* dn_src,
                                int dn_type, const float* down_scale, std::string& err);

  // After every layer is loaded: allocate S VRAM resident slots per layer within `vram_budget_bytes`
  // (cold start — experts 0..S-1 resident; the live rebalance converges the hot set over the first
  // tokens), build the per-layer device ExpertPtr tables, upload the down scales and zero the counts.
  // Best-effort for the slots: if the VRAM allocation falls short the tier runs fully mapped-host
  // (S = 0), which is correct, just slower. False with `err` only on a fatal (table/scale) failure.
  [[nodiscard]] bool finalize(std::int64_t vram_budget_bytes, std::string& err);

  // One token's MoE over layer `layer`:
  //   moe[i] = sum_{k<n_exp_used} geglu-FFN_{picks[k]}(e_in)[i] * wts[k] * down_scale[picks[k]].
  // `picks_dev` / `wts_dev` are the DEVICE outputs of u::k::router_topk; `e_in` and `moe` are DEVICE
  // [n_embd]. Enqueues entirely on `stream`; NO host sync. Also tallies the routing counts on device.
  [[nodiscard]] bool run_moe(int layer, const int* picks_dev, const float* wts_dev, const float* e_in,
                             float* moe, void* stream, std::string& err);

  // Live expert-hit rebalance. The caller gates this to run at most ~every 8 tokens. One D2H copy of
  // the routing counts (the only sync), then per layer decay the scores and swap the hottest
  // non-resident expert into the coldest resident slot while the gain clears a fixed margin, patching
  // the two device table entries. Returns the number of swaps, or -1 with `err`. A no-slot tier (S=0)
  // returns 0.
  // `count_scale` weights this call's routing counts into the decayed score (default 1.0 = a decode
  // gated step). A batched-prefill rebalance folds a whole chunk's picks, so it passes ~8/T to keep
  // the per-step magnitude comparable and avoid spiking the score (which would bias decode's hot set).
  [[nodiscard]] std::int64_t rebalance(int max_swaps, void* stream, std::string& err, double count_scale = 1.0);

  void close();
  [[nodiscard]] int slots_per_layer() const { return n_slots_; }

  // ---- instrumentation (NO behaviour change): routing hits/misses + slot fills, tallied host-side in
  // rebalance from the counts it already copies + the current residency, so there is no kernel change
  // and no extra sync. reset_stats() lets the caller isolate a phase (e.g. decode-only).
  void reset_stats() { stat_hits_ = stat_misses_ = stat_fills_ = 0; }
  [[nodiscard]] std::uint64_t stat_hits() const { return stat_hits_; }
  [[nodiscard]] std::uint64_t stat_misses() const { return stat_misses_; }
  [[nodiscard]] std::uint64_t stat_fills() const { return stat_fills_; }
  [[nodiscard]] std::int64_t resident_bytes() const;  // sum over layers of n_slots * blob_bytes

  // ---- read-only accessors for the BATCHED PREFILL moe_batch (gemma4_graph.cu) ----
  // They expose this layer's device ExpertPtr table, its expert quant kinds, its device
  // ffn_down_exps.scale slice and its device routing-count slice, plus the fused gate|up row count, so
  // the graph's moe_batch can bucket the chunk's picks on the host and run a per-expert cublasSgemm. They
  // only READ the state finalize() built; the decode run_moe()/rebalance() path is untouched. All return
  // null / a safe default when `layer` is out of range or finalize() has not run.
  [[nodiscard]] const ExpertPtr* layer_table(int layer) const;
  [[nodiscard]] WKind layer_gu_kind(int layer) const;
  [[nodiscard]] WKind layer_dn_kind(int layer) const;
  [[nodiscard]] const float* layer_down_scale_dev(int layer) const;
  [[nodiscard]] int* layer_counts_dev(int layer) const;
  [[nodiscard]] int gu_rows() const { return gu_rows_; }

 private:
  struct Layer {
    WKind gu_kind{WKind::q8};
    WKind dn_kind{WKind::q8};
    std::size_t gu_w{0}, gu_s{0}, dn_w{0}, dn_s{0}, blob_bytes{0};
    std::uint8_t* host{nullptr};   // pinned, mapped: n_expert blobs
    std::uint8_t* alias{nullptr};  // the device's address for `host`
    std::uint8_t* slots{nullptr};  // device: n_slots blobs (null when S == 0)
    std::vector<int> slot_of;      // expert -> slot, or -1 when it lives in the mapped host arena
    std::vector<int> expert_in;    // slot -> expert
    std::vector<double> score;     // decayed routing counts (rebalance)
    std::vector<float> down_scale; // host [n_expert], uploaded to the device at finalize
    std::vector<ExpertPtr> table_host;
    ExpertPtr* table{nullptr};     // device [n_expert]
  };

  [[nodiscard]] ExpertPtr expert_ptr(const Layer& L, const std::uint8_t* blob) const;

  int n_layers_{0}, n_expert_{0}, n_embd_{0}, n_ff_exp_{0}, n_exp_used_{0}, gu_rows_{0};
  int n_slots_{0};
  std::uint64_t stat_hits_{0}, stat_misses_{0}, stat_fills_{0};  // instrumentation (reset_stats to zero)
  bool opened_{false};
  std::vector<Layer> layers_;

  int* counts_dev_{nullptr};        // device [n_layers * n_expert]: routing counts since the last rebalance
  float* down_scale_dev_{nullptr};  // device [n_layers * n_expert]: every layer's ffn_down_exps.scale
  std::vector<int> counts_host_;    // staging for the one rebalance D2H copy

  // decode scratch (device), sized in open() for one token's n_exp_used routed experts
  float* gu_{nullptr};  // [n_exp_used * 2*n_ff_exp] fused gate|up
  float* mx_{nullptr};  // [n_exp_used * n_ff_exp]   geglu(gate,up)
  float* yg_{nullptr};  // [n_exp_used * n_embd]     down projection
};

}  // namespace u::gemma4
