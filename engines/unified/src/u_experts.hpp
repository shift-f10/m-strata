// strata-unified-engine: u::ExpertTier -- the ONE shared expert-placement facility
// for every family (design 2026-10-23 s3.5). It replaces each engine's ad-hoc MoE
// placement with a single hybrid VRAM/RAM tier plus an expert-hit profile:
//
//   * pin all experts in host RAM (an arena the family fills from the GGUF), and
//     keep S VRAM slots that hold the hottest experts;
//   * WARM-START: at load, fill each layer's slots from a committed-or-saved
//     profile, hottest first, so decode AND prefill are fast from the first token
//     (not after a live rebalance converges -- gemma4's weakness, s3.5);
//   * learn one: --expert-profile-save dumps the decayed routing scores as a
//     per-model profile; a later start reads it (`read_expert_profile`);
//   * LIVE REBALANCE: between rounds, swap the most-routed non-resident experts
//     into the coldest resident slots, as the correction on top of the warm start;
//   * compute(layer, ids, weights, x) -> out: run the routed experts' FFN, the
//     resident ones from VRAM and the rest streamed from the host arena, SwiGLU
//     them and combine by the router weights (+ an optional shared expert).
//
// Ported from Maya's expert_cache.{hpp,cpp} (the slot storage, residency table,
// per-layer admission, profile format STRP, warm-start fill) and
// expert_source.hpp (pin-all-in-RAM arena, the resident/non-resident split by
// router index). Trimmed to the lean single facility; the DMA-overlap/PCIe-share
// perf machinery and the prefill multi-token dispatch are left for the GPU box.
//
// This header is pure C++ (device memory is held as void*/uint8_t*), so it is only
// compiled into the CUDA build (u_experts.cu, behind STRATA_U_CUDA); family graphs
// include it to place and run their experts.
#pragma once

#include "kernels/common.cuh"
#include "u_cpu_lane.hpp"  // the CPU expert lane beside the device path (reg_lane_*)

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace u {

inline constexpr std::int32_t kNotResident = -1;

// One served weight tensor: F32 (dequantized at load) or a ggml-quantized row
// tensor read raw and dotted by the device mmvq. `type` is the GGML type id
// (0 = F32, 30 = BF16, else a k::Quant id); `q` is the base pointer (host or
// device per the family), `f32` the same when F32. For a stacked per-expert tensor
// (gate_exps/up_exps/down_exps), expert e's matrix begins at `q + e*stride`.
// Shared here (was glm_graph.hpp) so ExpertTier::register_layer can name it.
struct WSlot final {
  const float* f32 = nullptr;
  int type = 0;
  const void* q = nullptr;
  std::int64_t n_in = 0;
  std::int64_t n_out = 0;
};

// ---- the expert-hit profile (format byte-for-byte Maya's tools/make_profile.py:
// "STRP", then uint32 {version, n_layers, n_expert, slots, n_ranked}, then n_ranked
// (uint16 layer, uint16 expert) pairs ranked by descending routing frequency, then
// an n_layers*n_expert int32 table of each pair's rank (-1 = not ranked)). ----

// Reads `path` for `n_layers`x`n_expert`; fills `ranked` (hottest first) and the
// profile's own `slots`. False with `err` on any mismatch (fail closed).
[[nodiscard]] bool read_expert_profile(const std::string& path, std::int64_t n_layers, std::int64_t n_expert,
                                       std::vector<std::pair<std::int32_t, std::int32_t>>& ranked,
                                       std::int64_t& slots, std::string& err);

// Writes `ranked` in that format, atomically (a .tmp renamed over `path`).
[[nodiscard]] bool write_expert_profile(const std::string& path, std::int64_t n_layers, std::int64_t n_expert,
                                        const std::vector<std::pair<std::int32_t, std::int32_t>>& ranked,
                                        std::string& err);

// Ranks EVERY (layer, expert): the experts resident now first (where the swaps
// left the tier), then the rest; within each, by `heat` (decayed routing counts,
// descending), then by `prior` (the profile this run started from), then by index.
// `resident` and `heat` are n_layers*n_expert (`heat` may be empty: the prior
// decides). Maya expert_cache.cpp rank_learned_profile.
[[nodiscard]] std::vector<std::pair<std::int32_t, std::int32_t>> rank_learned_profile(
    std::int64_t n_layers, std::int64_t n_expert, const std::vector<std::uint8_t>& resident,
    const std::vector<double>& heat, const std::vector<std::pair<std::int32_t, std::int32_t>>& prior);

class ExpertTier {
 public:
  // The gated FFN activation the family uses between up/gate and the down
  // projection. A shared facility must offer all three: GLM/raon1 clamp, qwen is
  // plain SwiGLU, gemma4 is GeGLU.
  enum class Act { swiglu, swiglu_clamped, geglu };

  // The MoE geometry and the expert blob layout. Every expert's bytes are one
  // contiguous blob of `blob_bytes`, holding gate[n_ff,n_embd], up[n_ff,n_embd]
  // and down[n_embd,n_ff] at the given offsets, each in its quant format. (A
  // shared expert, when present, is blob index `n_expert` of its layer.)
  struct Config {
    std::int64_t n_layers = 0;
    std::int64_t n_expert = 0;   // routed experts per layer
    std::int64_t n_embd = 0;     // hidden width
    std::int64_t n_ff = 0;       // expert (MoE) intermediate width
    k::Quant gate = k::Quant::q4_k;
    k::Quant up = k::Quant::q4_k;
    k::Quant down = k::Quant::q6_k;
    Act act = Act::swiglu;       // gated activation (GLM/raon1 => swiglu_clamped)
    float act_limit = 10.0f;     // the swiglu_clamped limit (config's swiglu_limit)
    std::int64_t gate_off = 0;   // byte offset of each matrix inside a blob
    std::int64_t up_off = 0;
    std::int64_t down_off = 0;
    std::int64_t blob_bytes = 0; // bytes of one expert blob (gate+up+down, padded)
    std::int64_t vram_slots = 0; // S total VRAM slots (0 = all experts stream from RAM)
    bool per_layer_admission = true;
    bool has_shared_expert = false;
  };

  ExpertTier() = default;
  ~ExpertTier();
  ExpertTier(const ExpertTier&) = delete;
  ExpertTier& operator=(const ExpertTier&) = delete;

  // Allocates the VRAM slot arena, the residency table and the decode scratch,
  // and checks the allocation against the card. False with `err` on a bad config
  // or an allocation short of what was asked for.
  [[nodiscard]] bool open(const Config& cfg, std::string& err);
  void close();
  [[nodiscard]] bool valid() const { return opened_; }

  // The host arena: `base` holds every expert blob, pinned (the family mmaps/reads
  // the GGUF into it; ExpertTier best-effort cudaHostRegisters it for DMA). Blob
  // (layer, expert) is at `base + blob_index(layer, expert) * blob_bytes`, with a
  // shared expert at expert index n_expert. `bytes` is the arena size for the
  // registration and bounds check.
  [[nodiscard]] bool set_host_arena(const std::uint8_t* base, std::int64_t bytes, std::string& err);

  // WARM START: fill the VRAM slots from `ranked` (hottest first), per-layer. Runs
  // blocking host->device copies and verifies each slot. Call once after
  // set_host_arena. An empty `ranked` leaves the tier cold (streams everything).
  [[nodiscard]] bool warm_start(const std::vector<std::pair<std::int32_t, std::int32_t>>& ranked,
                                std::string& err);

  // Residency query: (layer, expert) -> slot or kNotResident. Bounds-checked.
  [[nodiscard]] std::int32_t slot_of(std::int64_t layer, std::int64_t expert) const;
  [[nodiscard]] std::int64_t resident_count() const { return admitted_; }
  [[nodiscard]] std::int64_t slots() const { return vram_slots_; }

  // LIVE REBALANCE. `note_routing` decays the per-(layer,expert) usage and bumps
  // the layer's routed ids (call each decode step). `rebalance` then, per layer,
  // swaps a non-resident expert whose usage exceeds the coldest resident's (by the
  // margin) into that slot, re-filling it from the host arena on `stream`. Returns
  // the number of swaps, or -1 with `err`.
  void note_routing(std::int64_t layer, const std::int32_t* ids, std::int64_t k);
  [[nodiscard]] std::int64_t rebalance(float margin, void* stream, std::string& err);

  // COMPUTE one token's MoE FFN:
  //   out[n_embd] = sum_{i<k} weights[i] * down(silu(gate(x)) * up(x))[expert ids[i]]
  //                 (+ the shared expert when configured).
  // `ids` and `weights` are HOST arrays of length k (k <= 64); `x` and `out` are
  // DEVICE [n_embd]. Resident experts are read from their VRAM slot, non-resident
  // ones streamed from the host arena into a reused staging slot (ordered on
  // `stream`). All work enqueues on `stream`. False with `err` on a bad call.
  [[nodiscard]] bool compute(std::int64_t layer, const std::int32_t* ids, const float* weights,
                             const float* x, float* out, std::int64_t k, void* stream, std::string& err);

  // Save what the tier learned (warm-start prior + live routing) as a profile.
  [[nodiscard]] bool save_profile(const std::string& path, std::string& err) const;

  // ================================ registered per-layer path ================================
  //
  // GLM-5.3-Flash, DeepSeek-4 and raon1 do NOT pack their experts into one
  // interleaved blob (the arena/compute path above); their GGUF carries three
  // STACKED per-layer tensors -- gate_exps/up_exps/down_exps -- and the graph
  // produces routed ids/weights on the DEVICE (from the sigmoid router). This path
  // records those stacked tensors (host pointers into the mapped GGUF) and, per
  // routed expert, streams its slice to a reused device staging buffer and runs the
  // FFN (clamped SwiGLU -- all three families clamp). It owns its own scratch, so a
  // family uses EITHER open()/set_host_arena()/compute() OR this path, never both.

  // Record layer `layer`'s three stacked expert tensors. `gate`/`up` are
  // [n_expert][n_ff_exp, n_embd], `down` is [n_expert][n_embd, n_ff_exp], each in
  // its WSlot quant (type == GGML id; 0 = F32, 30 = BF16). n_embd is taken from
  // gate.n_in. Allocates/grows the streaming scratch. False with `err`.
  // `act` is the gated activation (gemma4 => geglu; GLM/ds4/raon1 => swiglu_clamped, the default).
  // The *_stride args are the per-expert advance in bytes: pass 0 (the default) when each matrix is
  // its own stacked tensor (GLM: advance == the matrix size), or an explicit stride when gate and up
  // share one fused tensor (gemma4's ffn_gate_up_exps: gate at offset 0, up at +matrix_bytes, both
  // advancing by the fused per-expert size). The bytes STREAMED per expert are always the matrix's
  // own size (derived from the WSlot), never the stride.
  [[nodiscard]] bool register_layer(int layer, const WSlot& gate, const WSlot& up, const WSlot& down,
                                    int n_expert, int n_ff_exp, float swiglu_limit, std::string& err,
                                    Act act = Act::swiglu_clamped, std::int64_t gate_stride = 0,
                                    std::int64_t up_stride = 0, std::int64_t down_stride = 0);

  // One token's routed MoE over the registered `layer`:
  //   out[n_embd] = sum_{i<k} weights[i] * expert(ids[i])(x).
  // `ids`/`weights`, `x` and `out` are DEVICE; `xq` is an optional precomputed Q8_1
  // of x (null => the facility quantizes x). NO shared expert (the graphs add theirs
  // inline). ids are copied to the host (one sync) to index the stacked tensors.
  [[nodiscard]] bool run_moe(int layer, const int* ids, const float* weights, int k, const float* x,
                             const void* xq, float* out, void* stream, std::string& err);

  // A window of T tokens through the registered `layer`: row t uses ids/weights at
  // [t*k] and x/out at [t*n_embd]. `n_embd`/`n_ff`/`swiglu_limit` are the caller's
  // geometry (n_embd/n_ff checked against the registration; limit governs the join).
  [[nodiscard]] bool run_window(int layer, const float* x, const int* ids, const float* weights, int T,
                                int k, int n_embd, int n_ff, float swiglu_limit, float* out, void* stream,
                                std::string& err);

  // REGISTERED-PATH VRAM RESIDENCY. Call once after every register_layer: allocate VRAM slots within
  // `vram_budget_bytes` (each slot holds one expert's gate|up|down), per-layer admission, and warm-
  // start them with the hottest experts from `ranked` (empty => cold: experts 0..quota-1 per layer).
  // run_moe then reads a resident expert straight from its slot and streams only the non-resident
  // rest from the mmap. A budget of 0 leaves the tier fully streaming (the prior behaviour).
  [[nodiscard]] bool reg_finalize(std::int64_t vram_budget_bytes,
                                  const std::vector<std::pair<std::int32_t, std::int32_t>>& ranked,
                                  double warm_seed, int stream_prefetch, std::string& err);
  // Decay the per-(layer,expert) usage and bump this token's routed ids (call each decode step).
  void reg_note_routing(int layer, const std::int32_t* ids_host, int k);
  // Between rounds, per layer, swap the hottest non-resident expert into the coldest resident slot
  // while the usage gain clears `margin` (re-filling it from the mmap). Returns swaps, or -1 w/ err.
  [[nodiscard]] std::int64_t reg_rebalance(float margin, void* stream, std::string& err);
  // The same policy for the device-decided path, without blocking the decode: the evictions are published
  // to the device residency table on `stream` (the decode's stream) at once, and the swapped-in experts are
  // copied into their slots on the copy stream after that upload (so no step still reading an evicted slot
  // races its refill); they turn resident on a later call, once their copies have landed. One batch is in
  // flight at a time (a call while it copies plans nothing); at most `max_swaps` swaps a batch (<= 0: no
  // cap). Returns the swaps planned, or -1 with err.
  [[nodiscard]] std::int64_t reg_rebalance_async(float margin, int max_swaps, void* stream, std::string& err);
  // Between rounds (call every step; a cheap event query): when the batch in flight has landed, its experts
  // turn resident and the table is republished on `stream`. False with err on a failed copy.
  [[nodiscard]] bool reg_land_async(void* stream, std::string& err);
  // Learned ranking of the registered experts (resident first, then by heat), for --expert-profile.
  [[nodiscard]] std::vector<std::pair<std::int32_t, std::int32_t>> reg_ranked() const;

  // For the INFO line / startup report.
  [[nodiscard]] std::int64_t fills() const { return fills_; }
  [[nodiscard]] std::int64_t stream_misses() const { return stream_misses_; }
  // Registered-path residency report (after reg_finalize): total VRAM slots, resident-hit and
  // stream-miss expert reads since load (reset_reg_stats zeroes the two counters, e.g. before decode).
  [[nodiscard]] std::int64_t reg_slots() const { return reg_vram_slots_; }
  [[nodiscard]] std::int64_t reg_resident_hits() const { return reg_resident_hits_; }
  [[nodiscard]] std::int64_t reg_stream_misses() const { return stream_misses_; }
  void reset_reg_stats() { reg_resident_hits_ = stream_misses_ = reg_tail_skipped_ = 0; }

 private:
  std::int64_t blob_index(std::int64_t layer, std::int64_t expert) const;  // into the host arena
  void layer_slot_range(std::int64_t layer, std::int64_t& lo, std::int64_t& hi) const;
  std::int32_t admit(std::int64_t layer, std::int64_t expert);  // next free slot or kNotResident
  bool fill_slot_blocking(std::int32_t slot, std::int64_t layer, std::int64_t expert, std::string& err);

  Config cfg_{};
  bool opened_ = false;
  bool host_registered_ = false;

  const std::uint8_t* host_base_ = nullptr;  // the pinned RAM arena (family-owned)
  std::int64_t host_bytes_ = 0;

  std::uint8_t* vram_base_ = nullptr;        // S slots of blob_bytes, device
  std::int64_t vram_slots_ = 0;
  std::vector<std::int32_t> residency_;      // [n_layers*n_expert] -> slot or -1
  std::vector<std::int64_t> layer_next_;     // per-layer next free slot (per-layer admission)
  std::int64_t next_free_ = 0;               // global next free (when not per-layer)
  std::int64_t admitted_ = 0;
  std::int64_t fills_ = 0;
  std::int64_t stream_misses_ = 0;

  std::vector<double> usage_;                // [n_layers*n_expert] decayed routing counts
  std::vector<std::pair<std::int32_t, std::int32_t>> prior_;  // the profile this run started from

  // decode scratch (device), sized in open()
  std::uint8_t* d_stage_ = nullptr;          // one blob, reused for non-resident experts
  void* d_xq8_embd_ = nullptr;               // Q8_1 of x (n_embd)
  void* d_hq8_ = nullptr;                     // Q8_1 of h (n_ff)
  float* d_g_ = nullptr;                      // gate output [n_ff]
  float* d_u_ = nullptr;                      // up output   [n_ff]
  float* d_h_ = nullptr;                      // silu(g)*u   [n_ff]
  float* d_parts_ = nullptr;                  // [(k_max) * n_embd]
  float* d_shared_ = nullptr;                 // shared expert output [n_embd]
  float* d_weights_ = nullptr;                // [k_max]
  std::int64_t k_max_ = 64;

  // ---- registered per-layer path (register_layer / run_moe / run_window) ----
  struct RegLayer {
    WSlot gate, up, down;             // host pointers into the mapped GGUF (stacked over experts)
    std::int64_t n_expert = 0, n_embd = 0, n_ff = 0;
    float limit = 0.0f;              // clamped-SwiGLU limit (unused for geglu)
    Act act = Act::swiglu_clamped;   // gated activation (gemma4 => geglu)
    std::int64_t gate_stride = 0, up_stride = 0, down_stride = 0;  // per-expert ADVANCE in bytes
    std::int64_t gate_bytes = 0, up_bytes = 0, down_bytes = 0;     // one matrix's bytes (stream size)
    bool valid = false;
  };
  // Grow (or allocate) the registered-path scratch to cover these dims + staging.
  [[nodiscard]] bool reg_ensure(std::int64_t n_embd, std::int64_t n_ff, std::int64_t stage_bytes,
                                std::string& err);
  // One token's routed FFN over `rl` (layer `layer`; ids on the HOST), combined by `weights_dev`.
  // Resident experts are read from their VRAM slot; the rest stream from the mmap.
  [[nodiscard]] bool reg_run_token(const RegLayer& rl, int layer, const std::int32_t* ids_host,
                                   const float* weights_dev, int k, const float* x, const void* xq, float* out,
                                   float limit, void* stream, std::string& err);

  std::vector<RegLayer> reg_;                 // indexed by layer id
  bool reg_scratch_ = false;
  std::int64_t reg_n_embd_ = 0, reg_n_ff_ = 0;
  std::uint8_t* rg_stage_ = nullptr;          // one expert matrix (reused for gate/up/down), device
  void* rg_xq8_ = nullptr;                    // Q8_1 of x   [n_embd]
  void* rg_hq8_ = nullptr;                    // Q8_1 of h   [n_ff]
  float* rg_g_ = nullptr;                     // gate output [n_ff]
  float* rg_u_ = nullptr;                     // up output   [n_ff]
  float* rg_h_ = nullptr;                     // join(g,u)   [n_ff]
  float* rg_parts_ = nullptr;                 // [k_max * n_embd]
  std::uint16_t* rg_bf16x_ = nullptr;         // bf16 activation for bf16 experts [max(n_embd,n_ff)]
  std::int64_t rg_stage_cap_ = 0, rg_xq8_cap_ = 0, rg_hq8_cap_ = 0;
  std::int64_t rg_g_cap_ = 0, rg_u_cap_ = 0, rg_h_cap_ = 0, rg_parts_cap_ = 0, rg_bf16x_cap_ = 0;
  std::vector<std::int32_t> rg_ids_host_;     // [k_max] staging for the device->host id copy

  // ---- C2 (spec 2026-10-24): async PCIe-overlap prefetch ring for STREAMED (non-resident) experts.
  // A dedicated copy stream prefetches a streamed expert's gate|up|down (resident-slot layout) into a
  // ring buffer while the compute stream runs earlier experts. rg_ready_ev_[r] signals that ring
  // buffer r's copy is done; rg_free_ev_[r] signals the compute stream is done reading it (so the copy
  // stream may refill it). Depth 1 => serial (no overlap); depth N => up to N copies in flight. Streams
  // and events are void* here to keep this header CUDA-free (cast in u_experts.cu).
  void* rg_copy_ = nullptr;                   // cudaStream_t (the prefetch copies)
  // The expert sub-block runs on its OWN compute stream, not the caller's stream: the caller is the
  // legacy default stream (nullptr), which implicitly serializes with every other stream, so the copy
  // stream could not overlap it. run_moe syncs caller->compute at entry (x ready) and compute->caller
  // at exit (out ready); within, copy stream || compute stream gives the real overlap.
  void* rg_compute_ = nullptr;                // cudaStream_t
  void* rg_in_ev_ = nullptr;                  // cudaEvent_t: x ready on the caller's stream
  void* rg_out_ev_ = nullptr;                 // cudaEvent_t: out ready on the compute stream
  int rg_ring_depth_ = 1;                     // = CalibrationProfile::stream_prefetch
  std::int64_t rg_ring_cap_ = 0;              // bytes of one ring buffer (= max reg_slot_bytes_)
  std::vector<std::uint8_t*> rg_ring_;        // rg_ring_depth_ device buffers, each rg_ring_cap_
  std::vector<void*> rg_ready_ev_, rg_free_ev_;  // cudaEvent_t per ring buffer

  // ---- C3: CPU expert-compute lane. A fraction of a token's STREAMED experts are computed on the host
  // CPU (u::cpu_expert_ffn, reading the mmap directly) instead of over PCIe, in parallel with the GPU.
  // Off (frac 0) by default; CPU results are not bit-exact with the GPU, so the MoE output is then
  // split-dependent. Host buffers: x copied D2H once per token, per-expert f32 parts (H2D'd into
  // rg_parts_), and per-worker dequant scratch.
  float rg_cpu_frac_ = 0.0f;                  // = CalibrationProfile::cpu_expert_frac
  int rg_cpu_workers_ = 0;                    // = CalibrationProfile::cpu_workers (0 = one thread/expert)
  float* rg_xhost_ = nullptr;                 // pinned [reg_n_embd_]: x for the CPU lane
  float* rg_cpu_parts_ = nullptr;             // pinned [k_max_ * reg_n_embd_]: CPU expert outputs
  float* rg_cpu_scratch_ = nullptr;           // [k_max_ * 2 * reg_n_ff_]: per-expert dequant scratch
 public:
  // Enable/configure the CPU lane (call after reg_finalize, before generate). frac in [0,1].
  void reg_set_cpu_lane(float frac, int workers) { rg_cpu_frac_ = frac; rg_cpu_workers_ = workers; }
  // Copy each registered layer's stacked gate/up/down expert tensors from the (pageable) mmap into
  // PINNED host memory and repoint the layer at the copies, so a streamed expert's cudaMemcpyAsync is a
  // true async DMA instead of a host-side staging copy on the calling thread (nsys: ~94 us of host time
  // per pageable copy). One cudaHostAlloc per tensor (pinned in slices); stops, keeping the mmap for the
  // rest, at `budget_bytes` or the first failed allocation. Fused/strided layouts keep the mmap. Call
  // after register_layer and before reg_finalize. Returns the bytes pinned.
  std::int64_t reg_pin_host_arena(std::int64_t budget_bytes);

  // ---- device-decided routed MoE (opt-in; qwen4exp). The router's ids stay on the device and the k
  // experts run as grouped launches (u::k::iq_mmvq_grouped) whose weight bases are chosen ON THE DEVICE:
  // the resident VRAM slot, else the expert's matrices in the pinned host arena read zero-copy over PCIe.
  // No per-layer id copy / stream sync / host planning, ~7 launches per layer instead of ~50, and the
  // launches have fixed arguments (CUDA-graph capturable). Needs reg_pin_host_arena + i-quant experts.
  // Upload the residency table to the device (call after reg_finalize; reg_rebalance re-uploads it).
  [[nodiscard]] bool reg_publish_device_residency(std::string& err);
  [[nodiscard]] bool reg_device_ready(int layer) const;
  // n_tok > 1 (the verify window): n_tok tokens' routings at once -- ids/weights [n_tok][k], x/out
  // [n_tok][n_embd] -- each (token, expert) computed exactly as the single-token call computes it.
  [[nodiscard]] bool reg_run_token_device(int layer, const int* ids_dev, const float* weights_dev, int k,
                                          const float* x, float* out, void* stream, std::string& err,
                                          int n_tok = 1);
  // Host bookkeeping for tokens run on the device path, from a host copy of their routed ids
  // (ids_all[layer*k ...]): routing heat for reg_rebalance and the resident/stream counters. Only the
  // layers with mask[layer] != 0 (the others noted themselves inside run_moe).
  void reg_note_device_token(const std::int32_t* ids_all, int k, const std::vector<char>& mask);
  // The device path's misses (call before the first reg_run_token_device): `stage` copies a layer's
  // non-resident experts into VRAM staging slots with one wide copy kernel before the grouped launches
  // (upstream's PCIe share, pcie_mode 2), else they are read zero-copy inside them.
  void reg_set_device_miss_path(bool stage) { gr_stage_on_ = stage; }
  // Skip a missed expert routed at rank >= `rank` on the device path (upstream STRATA_ROUTE_TAIL_SKIP; 0 =
  // off, exact). Read by the kernels from device memory, so it may change between captured-graph replays
  // (prefill off, decode on). Blocking; call between steps, after reg_publish_device_residency.
  [[nodiscard]] bool reg_set_tail_skip(int rank, std::string& err);
  [[nodiscard]] std::int64_t reg_tail_skipped() const { return reg_tail_skipped_; }
  // Fraction of the registered (layer, expert) pairs that have a VRAM slot.
  [[nodiscard]] double reg_cached_fraction() const;

  // ---- the CPU expert lane beside the device path (upstream's ExpertPool + verify.cpp's per-layer doorbell).
  // A layer's missed experts are planned on the device (u::k::expert_lane_plan): of the misses that are not
  // tail-skipped, the last pcie_num/256 in routing order are read by the GPU zero-copy and the rest are computed
  // by a host thread pool (u::cpu::LanePool) from the pinned arena, while the GPU computes the resident ones.
  // The GPU rings a mapped doorbell per layer (ids, eff, x, then seq = layer + 1); the host loop that
  // reg_lane_serve runs after each launch spins on it, serves the layer's CPU entries into mapped rows, and
  // raises flag = layer + 1; the GPU's merge waits for the flag only on a layer that has CPU entries.
  // The arena must be cached for the CPU's reads: reg_set_host_cached(true) before reg_pin_host_arena (the
  // default is write-combined, which the GPU's zero-copy reads prefer and the CPU cannot read at speed).
  void reg_set_host_cached(bool cached) { rg_host_cached_ = cached; }
  // After reg_publish_device_residency: t_max tokens a launch (the verify window), k experts a token,
  // `workers` pool threads (<= 0: one per physical core but the caller's), pcie_num in [0, 256]. Layers whose
  // quants have no CPU dot keep the plain device path.
  [[nodiscard]] bool reg_lane_enable(int t_max, int k, int workers, int pcie_num, std::string& err);
  [[nodiscard]] bool reg_lane_ready() const { return lane_pool_ != nullptr; }
  // Before a launch on `stream`: on = the launch's layers ring and reg_lane_serve serves them (only for a
  // replayed graph or a launch with no blocking call inside it); off = every miss is the GPU's, as without the
  // lane. The caller has synced the previous launch (the doorbell words are reset here).
  [[nodiscard]] bool reg_lane_arm(bool on, void* stream, std::string& err);
  // After that launch: serve every ringing layer in order (n_tok tokens). Times out (20 s) or fails if the
  // stream ends without ringing; the GPU's waits are released either way.
  [[nodiscard]] bool reg_lane_serve(int n_tok, void* stream, std::string& err);
  [[nodiscard]] bool reg_lane_set_pcie(int pcie_num, std::string& err);
  void reg_lane_report() const;  // one stderr line: experts served, their phases, the host's wait
  void reg_lane_reset_stats();
 private:
  std::vector<void*> rg_pinned_;              // the pinned copies made by reg_pin_host_arena
  struct RegDev {                             // device pointers of a layer's pinned stacked expert tensors
    const void* gate = nullptr;
    const void* up = nullptr;
    const void* down = nullptr;
  };
  std::vector<RegDev> reg_dev_;
  int* d_res_ = nullptr;                      // device copy of reg_residency_ (slot, or -1)
  // reg_rebalance_async: the batch of swaps whose copies are in flight, its completion event, the pinned
  // mirror the table uploads read, and the event of the last upload (the mirror is rewritten after it).
  struct PendingFill {
    int layer = 0;
    std::int32_t e = 0, slot = 0;
  };
  std::vector<PendingFill> rg_pending_;
  std::int64_t rg_round_ = 0;                 // rounds planned (the first layer of a capped round rotates)
  void* rg_fill_ev_ = nullptr;                // cudaEvent_t
  void* rg_res_ev_ = nullptr;                 // cudaEvent_t
  int* h_res_ = nullptr;                      // pinned [reg_residency_.size()]
  [[nodiscard]] bool reg_upload_residency_async(void* stream, std::string& err);
  [[nodiscard]] int reg_land_pending(std::string& err);  // 1 landed, 0 none / still copying, -1 failed
  float* gr_g_ = nullptr;                     // grouped scratch: gate/up/join [k*n_ff], parts [k*n_embd]
  float* gr_u_ = nullptr;
  float* gr_h_ = nullptr;
  float* gr_parts_ = nullptr;
  void* gr_xq8_ = nullptr;                    // Q8_1 of x [n_embd/32 blocks]
  void* gr_hq8_ = nullptr;                    // Q8_1 of the k joins [k * n_ff/32 blocks]
  int* gr_eff_ = nullptr;                     // [k] where each routed expert is read (u::k::expert_route_plan)
  std::uint8_t* gr_stage_ = nullptr;          // [k * gr_stage_stride_] VRAM staging for the layer's misses
  std::int64_t gr_stage_stride_ = 0;
  bool gr_stage_on_ = true;
  int gr_tail_rank_ = 0;                      // host mirror of *gr_tail_dev_ (for the skip counter)
  int* gr_tail_dev_ = nullptr;                // [1] the tail-skip rank the kernels read
  std::int64_t reg_tail_skipped_ = 0;
  std::int64_t gr_k_ = 0, gr_ntok_ = 0, gr_n_embd_ = 0, gr_n_ff_ = 0;
  int gr_stage_k_ = 0;                        // staging slots (one token's k)

  // the CPU expert lane (reg_lane_*)
  bool rg_host_cached_ = false;               // reg_pin_host_arena: cached (the lane reads it) or write-combined
  std::unique_ptr<cpu::LanePool> lane_pool_;
  std::vector<cpu::LaneLayer> lane_L_;        // per layer: its experts in the pinned arena
  std::vector<char> lane_ok_;                 // per layer: every quant has a CPU dot
  std::vector<int> lane_layers_;              // the layers that ring a launch, ascending (seen by reg_run_token_device)
  int* lane_dev_ = nullptr;                   // device [2]: on, pcie_num
  int* lane_ncpu_dev_ = nullptr;              // device [1]: the current layer's CPU entries
  void* lane_host_ = nullptr;                 // one mapped allocation holding the words and rows below
  volatile unsigned* h_lane_seq_ = nullptr;   // the GPU's doorbell: the last layer + 1 published
  volatile unsigned* h_lane_flag_ = nullptr;  // the host's answer: the last layer + 1 served
  float* h_lane_x_ = nullptr;                 // [t_max * n_embd] the ringing layer's MoE input
  int* h_lane_ids_ = nullptr;                 // [n_layers * lane_slot_] each layer's ids (its own slot)
  int* h_lane_eff_ = nullptr;                 // [n_layers * lane_slot_] each layer's eff codes
  float* h_lane_ymiss_ = nullptr;             // [t_max * k * n_embd] the CPU's rows
  void* d_lane_host_ = nullptr;               // the device alias of lane_host_
  std::int64_t lane_off_x_ = 0, lane_off_ids_ = 0, lane_off_eff_ = 0, lane_off_ymiss_ = 0;
  int lane_tmax_ = 0, lane_k_ = 0, lane_slot_ = 0, lane_pcie_num_ = 0;
  bool lane_on_ = false, lane_dev_on_ = false;
  std::int64_t lane_launches_ = 0, lane_cpu_layers_ = 0, lane_cpu_entries_ = 0;
  double lane_ms_wait_ = 0.0, lane_ms_serve_ = 0.0;
  [[nodiscard]] bool gr_ensure(int k, std::int64_t n_embd, std::int64_t n_ff, std::string& err, int n_tok = 1);

  // ---- registered-path VRAM residency (reg_finalize / reg_note_routing / reg_rebalance) ----
  // Copy one expert's gate|up|down (layer `layer`, expert `e`) from the mmap into VRAM `slot`,
  // blocking. The slot holds the three matrices at offsets 0, gate_bytes, gate_bytes+up_bytes.
  [[nodiscard]] bool reg_fill_slot(std::int32_t slot, int layer, std::int32_t e, std::string& err);
  [[nodiscard]] std::int32_t reg_slot_of(int layer, std::int32_t e) const;  // slot or kNotResident
  std::int64_t reg_n_layers_ = 0, reg_n_expert_ = 0;
  std::uint8_t* reg_vram_ = nullptr;          // the slot pool (per-layer regions), device
  std::int64_t reg_vram_slots_ = 0, reg_slot_bytes_ = 0;  // total slots; the LARGEST layer's slot bytes
  // Per-layer slot geometry: layer l's slots hold experts of its own quants (gate|up|down bytes of l),
  // global slots [reg_lfirst_[l], reg_lfirst_[l] + reg_lcount_[l]) at byte offset reg_lbase_[l].
  std::vector<std::int64_t> reg_lslot_bytes_, reg_lfirst_, reg_lcount_, reg_lbase_;
  [[nodiscard]] std::uint8_t* reg_slot_ptr(int layer, std::int32_t slot) const;
  void reg_local_residency(std::vector<int>& out) const;  // residency as LAYER-LOCAL slot (or -1)
  std::vector<std::int32_t> reg_residency_;   // [n_layers*n_expert] -> slot or kNotResident
  std::vector<std::int64_t> reg_layer_next_;  // per-layer next free slot
  std::vector<double> reg_usage_;             // [n_layers*n_expert] decayed routing counts
  std::int64_t reg_fills_ = 0, reg_resident_hits_ = 0;
};

}  // namespace u
