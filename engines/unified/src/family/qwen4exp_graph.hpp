// strata-unified-engine: the Qwen3.8-Flash-Next (arch "qwen4exp") per-family model graph.
//
// qwen4exp is ported from the MAIN upstream Strata engine (upstream/src/program/generate.cpp, the
// "strata generate" driver) and its components under upstream/src/core/* and upstream/src/kernels/*.
// Unlike qwen35moe (which upstream serves from a self-contained src/qwen35/q36_engine.cu on a plain
// GGUF own-path), qwen4exp is served by the main engine over the IQ_PACK expert path: a prebuilt
// experts pack (experts.bin + index.txt + native_experts.txt, built by upstream/tools/iq_pack.py),
// a disk-backed PLE n-gram table (--ple-gguf), a committed expert-hit profile (--expert-profile), and
// an external MTP draft layer (--mtp runtime dir). This is the MAIN NEW SEAM for this family and the
// reason it does not share qwen35moe's loader (contract: §"IQ_PACK / ExpertTier seam" below).
//
// THE REAL GRAPH (NOT the one-line "full attention + MoE" summary in the design-doc §0 table). The
// published Flash-Next geometry (upstream/include/strata/core/geometry.hpp kCompiledGeometry) is a
// hybrid, and this port is faithful to it:
//   * 48 trunk layers; hidden 2560; vocab ~248320; rms_eps 1e-6.
//   * ATTENTION is a GDN / QSA hybrid, NOT uniform full attention:
//       - 36 layers are GDN  (Gated-DeltaNet linear attention, no KV cache) — ssm_state 128,
//         group_count 16, time_step_rank 48, conv_kernel 4, conv_channels 10240, value_dim 6144.
//       - 12 layers are QSA  (Qwen Sparse Attention: a sparse-indexer + selected-cell flash attn) —
//         every `full_attention_interval`(=4)th layer, i.e. layers 3,7,...,47. heads 24, kv_heads 2,
//         head_dim 256, n_rot 64 (NEOX RoPE on the first 64 of head_dim), indexer heads 4 /
//         key_dim 128 / block 4 / top_k 2048.
//   * PER-LAYER EMBEDDING (PLE): every layer adds an n-gram-indexed per-layer embedding row, read
//     from the 26.8 GB (IQ4_NL) / 51.2 GB (FP8) SSD table (never held in RAM), through the host-side
//     PleReader (upstream/src/ngram/ple_reader.cpp) + a small postprojection kernel.
//   * HYPER-CONNECTIONS: every layer is wrapped in a gated-residual hyper-connection (count 4,
//     low_rank 320) instead of a plain pre-norm residual.
//   * MoE on every layer: softmax top-10 router over 512 experts (pruned Coder variants ship fewer) +
//     1 shared expert, n_ff 640 — the routed experts come from the iq_pack arena via u::ExpertTier.
//   * MTP: a separate draft layer (upstream/src/core/mtp.cpp, tools/mtp_rt.py) proposes tokens the
//     trunk verifies (speculative decode). Loaded from --mtp; off by default (spec_hits/steps = 0).
//
// LEAN CUT (design goal i — we port only what we serve). Dropped from the upstream engine: vision /
// mmproj, multi-GPU layer-split, the CPU expert pool / PCIe-overlap / NUMA machinery, batch groups,
// EXL3 / NVFP4 / Hadamard-fold quant paths, conversation-cache/session-save, and the many A/B diag
// flags. Text serve path, single GPU, one sequence.
//
// HOST/BOX SPLIT (identical to the raon1/glm lanes). This header is host-includable (the dispatch in
// u_family.hpp calls make_qwen4exp_graph with only host types; device state is held as raw pointers).
// The method bodies and all CUDA live in qwen4exp_graph.cu + src/kernels/qwen4exp_*.cu, built ONLY
// behind STRATA_U_CUDA on the owner's GPU box (design §1.3). On a host/CI build this factory is not
// linked and u::make_family_graph returns the host stub.
#pragma once

#include "u_family.hpp"
#include "u_geometry.hpp"
#include "u_experts.hpp"   // u::ExpertTier + the shared u::WSlot
#include "u_ple.hpp"       // u::ple::Spec — the shared host PLE addressing (host-safe)

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace u {

// ================================================================================================
// Qwen4ExpGeometry — the qwen4exp dims the graph's kernels need, beyond the common u::Geometry.
//
// The common fields (block_count/embedding_length/head_count/key_length/expert_* /context/vocab) come
// from the guarded u::Geometry (u_geometry.cpp; qwen4exp uses the GENERIC ranges there, not exact()
// pins — a Flash-Next GGUF that omits a key keeps the published default, exactly as the upstream
// geometry_from_gguf does: upstream/include/strata/core/family_layout.hpp). The GDN/QSA/HC/PLE
// internals below are the kCompiledGeometry defaults (upstream/include/strata/core/geometry.hpp
// L21-72), read from the `qwen4exp.*` metadata where a file carries them.
// ================================================================================================
struct Qwen4ExpGeometry final {
  // --- common (mirrored from the guarded u::Geometry) ---
  int n_embd = 2560;      // embedding_length
  int n_layers = 48;      // block_count MINUS n_mtp_layers (the trunk; the in-file MTP block, if any)
  int n_vocab = 248320;   // tokenizer.ggml.tokens length (the guard reads vocab_size)
  int n_expert = 512;     // expert_count (Coder prune ships fewer)
  int n_exp_used = 10;    // expert_used_count — softmax top-10 router
  int n_ff_exp = 640;     // expert_feed_forward_length (MoE + shared-expert FFN width)
  int n_shared = 1;       // one always-on shared expert (expert_shared_feed_forward_length == n_ff_exp)
  float rms_eps = 1e-6f;  // attention.layer_norm_rms_epsilon

  // --- the GDN / QSA attention schedule ---
  int qsa_interval = 4;   // full_attention_interval: layer % qsa_interval == qsa_interval-1 is QSA
  // is_qsa(il) mirrors upstream is_qsa_layer (geometry.hpp L80): every 4th layer is full (QSA).
  [[nodiscard]] bool is_qsa(int il) const { return il % qsa_interval == qsa_interval - 1; }
  [[nodiscard]] int n_qsa() const { return n_layers / qsa_interval; }
  [[nodiscard]] int n_gdn() const { return n_layers - n_qsa(); }

  // --- GDN (Gated-DeltaNet linear attention) — ssm.* keys ---
  int ssm_state = 128;        // ssm.state_size: key head width AND value head width (S)
  int ssm_k_heads = 16;       // ssm.group_count (q/k head count)
  int ssm_v_heads = 48;       // ssm.time_step_rank (v head count)
  int ssm_d_conv = 4;         // ssm.conv_kernel (causal short-conv tap count)
  int ssm_conv_channels = 10240;  // 2*state*k_heads + state*v_heads = 2*128*16 + 128*48 (derived)
  int ssm_value_dim = 6144;   // ssm.inner_size = state*v_heads = 128*48

  // --- QSA (Qwen Sparse Attention) ---
  int n_head = 24;        // attention.head_count
  int n_head_kv = 2;      // attention.head_count_kv (GQA)
  int head_dim = 256;     // attention.key_length (== value_length; the guard reads key_length)
  int n_rot = 64;         // rope.dimension_count: the rotated slice of head_dim and of idx_key_dim
  int idx_q_heads = 4;    // attention.indexer.head_count
  int idx_key_dim = 128;  // attention.indexer.key_length
  int idx_block = 4;      // compress_ratios nonzero entry: one pooled key per 4 cells
  int idx_top_k = 2048;   // attention.indexer.top_k: selection budget (cells)
  double rope_freq_base = 0.0;  // rope.freq_base (from the guarded Geometry; qwen4exp is a RoPE family)

  // --- hyper-connections (gated residual, every layer) ---
  int hc = 4;        // hyper_connection.count
  int hc_lr = 320;   // hyper_connection.low_rank

  // --- MTP draft (external, --mtp) ---
  int n_mtp_layers = 0;  // in-file MTP blocks after the trunk (qwen4exp ships its MTP as an external
                         // pack, so this is 0; qwen35moe counts blk.40 here — kept for symmetry)

  // Reads the qwen4exp.* metadata (beyond the common u::Geometry) into `out`, applying the published
  // kCompiledGeometry defaults where a key is absent (upstream geometry_from_gguf behaviour). Fails
  // closed with `err` on a key present with an impossible value. Defined in qwen4exp_graph.cu.
  static bool from_gguf(const GgufFile& file, const Geometry& common, Qwen4ExpGeometry& out,
                        std::string& err);
};

// ================================================================================================
// The iq_pack descriptors — the structural model of the pack the upstream FileExpertSource loads.
//
// A qwen4exp model is NOT one GGUF: it is a PACK DIRECTORY plus side GGUFs (the frontend composes the
// args; upstream/setup.py ~L8810-8824):
//     --pack <dir>              index.txt + native_experts.txt + experts.bin + dense.bin + tokenizer/
//     --native <dense.gguf>     quantized dense tensors + token_embd + output, served from the GGUF
//     --ple-gguf <ple.gguf>     the per-layer-embedding n-gram table (per_layer_token_embd.weight)
//     --embd-gguf <embd.gguf>   optional BF16 token embedding
//     --mtp <rt dir>            the MTP draft layer runtime
//     --expert-profile <bin>    the committed STRP expert-hit profile for ExpertTier warm start
//
// These structs capture the layout the box's shared loader must parse; the parsers are sketched in
// qwen4exp_graph.cu with precise upstream TODOs (iq_pack.py writes these files; expert_source.cpp
// reads them).
// ================================================================================================

// One layer's routed-expert blob layout inside experts.bin, from native_experts.txt
// ("layer gu_type d_type offset blob_bytes gate_off up_off down_off [shard]" — iq_pack.py
// expert_layout(), upstream/tools/iq_pack.py L430-488; parsed by FileExpertSource,
// upstream/src/core/expert_source.cpp L1370-1421).
struct Qwen4ExpPackLayer final {
  std::int64_t offset = 0;      // absolute byte offset of this layer's 512 blobs inside experts.bin
  std::int64_t blob_bytes = 0;  // bytes of one expert blob (gate | up | down), per-layer (mixed quants)
  std::int32_t gu_type = 0;     // ggml type id of gate+up (IQ1_M/IQ2_XS/IQ3_XXS/IQ3_S/...)
  std::int32_t d_type = 0;      // ggml type id of down (Q2_0/IQ4_NL/...)
  std::int64_t gate_off = 0;    // byte offset of gate inside a blob (0)
  std::int64_t up_off = 0;      // byte offset of up inside a blob
  std::int64_t down_off = 0;    // byte offset of down inside a blob
};

// The whole experts pack: per-layer layout + the mapped arena base (experts.bin, or the --native GGUF
// shards read in place when experts.bin is absent — upstream "CS-T", expert_source.cpp L1422-1433).
struct Qwen4ExpPack final {
  std::string pack_dir;                      // --pack
  std::string dense_gguf;                    // --native
  std::string ple_gguf;                      // --ple-gguf
  std::string embd_gguf;                     // --embd-gguf (optional)
  std::string mtp_dir;                       // --mtp (optional)
  std::string expert_profile;                // --expert-profile (optional)
  int n_expert_in_pack = 512;                // router rows in the pack (N_EXPERT; Coder prune < 512)
  std::vector<Qwen4ExpPackLayer> layers;     // per trunk layer
  // the mapped RAM arena (experts.bin mmap/read, pinned), filled by the box's shared loader:
  const std::uint8_t* arena_base = nullptr;  // blob (layer,e) at arena_base + layers[layer].offset + e*blob_bytes
  std::int64_t arena_bytes = 0;
};

// The PLE n-gram table descriptor (PleTable::open: validated GGUF parse giving the byte offset and row
// count — upstream/include/strata/ngram/ple_reader.hpp; the rows are read on demand, never in RAM).
struct Qwen4ExpPleTable final {
  std::string path;              // the --ple-gguf file
  std::int64_t table_offset = 0; // byte offset of row 0
  std::int64_t n_rows = 0;       // row count (e.g. 320,001,536)
  std::uint32_t row_bytes = 90;  // 90 (IQ4_NL) or 160 (FP8 E4M3)
};

// ================================================================================================
// Qwen4ExpGraph — the Qwen3.8-Flash-Next graph on the shared substrate.
// ================================================================================================
class Qwen4ExpGraph final : public FamilyGraph {
 public:
  Qwen4ExpGraph(const Geometry& common, std::uint32_t context);
  ~Qwen4ExpGraph() override;
  Qwen4ExpGraph(const Qwen4ExpGraph&) = delete;
  Qwen4ExpGraph& operator=(const Qwen4ExpGraph&) = delete;

  [[nodiscard]] FamilyId family() const override { return FamilyId::qwen4exp; }
  [[nodiscard]] std::uint32_t context() const override { return context_; }
  [[nodiscard]] bool available() const override { return loaded_; }
  [[nodiscard]] std::string describe() const override;
  [[nodiscard]] bool generate(const Gen& request, const LineSink& sink, std::string& error) override;

  // Uploads the non-expert qwen4exp tensors to the device weight arena, reads the exact geometry from
  // the GGUF, sizes the state + scratch arenas for `max_ctx`, and hands the routed-expert tensors to the
  // tier. The substrate's dispatch (u_main.cpp) calls this with the merged (multi-shard) GGUF and its
  // mapped data base after make_qwen4exp_graph constructs the graph. Mirrors GlmGraph::load.
  [[nodiscard]] bool load(const GgufFile& gguf, const std::uint8_t* mapped, std::string& error) override;

 private:
  // One token through all layers at cache position `pos_` (decode path, T=1). The HC-wrapped trunk:
  // embed -> hc-init -> 48 layers (hc-read + norm + GDN|QSA mixer + hc-write, then hc-read + norm + MoE
  // + hc-write) -> hc-collapse + output-norm + head. PLE is deferred (external table; 1 layer).
  [[nodiscard]] bool step(std::int32_t token, std::string& error);
  [[nodiscard]] bool sample_next(const Gen& request, std::int32_t& out_id, std::string& error);
  void reset();  // zero the residual streams + every GDN recurrence/conv + the QSA caches; pos_ = 0

  Qwen4ExpGeometry g_{};
  Geometry geom_{};            // the guarded common geometry (for the from_gguf refine in load)
  std::uint32_t context_ = 0;
  bool loaded_ = false;
  std::int64_t pos_ = 0;       // the next cache position
  std::int64_t max_ctx_ = 0;

  std::unique_ptr<ExpertTier> experts_;   // the shared MoE facility (softmax top-10, 512/10/1)

  // ---- the device weight arena (non-expert tensors) + the name -> slot map.
  float* w_arena_ = nullptr;
  std::uint64_t w_arena_bytes_ = 0;
  std::map<std::string, WSlot> ws_;
  [[nodiscard]] const WSlot& W(const std::string& name) const { return ws_.at(name); }
  [[nodiscard]] const WSlot& W(int il, const char* suffix) const;
  [[nodiscard]] bool has(int il, const char* suffix) const;

  // ---- the state arena (persists across tokens): the hc residual streams live at its head (ping-pong),
  // then per GDN layer the recurrence state S + the conv history, per QSA layer the K/V caches + the
  // indexer pooled-key cache. Offsets are floats into state_; -1 = the layer has no such state.
  float* state_ = nullptr;
  std::uint64_t state_bytes_ = 0;
  std::vector<std::int64_t> gdn_S_, gdn_conv_, qsa_k_, qsa_v_, qsa_pool_;

  // ---- the scratch arena (one token's activations; reused each token). Offsets are floats into sc_.
  float* sc_ = nullptr;
  std::uint64_t sc_bytes_ = 0;
  int* d_pos_ = nullptr;   // the step's position, pos_rows_ copies (one per rope row: k::rope_neox_base
                           // reads pos[row]); QSA append / attention read entry 0
  int pos_rows_ = 1;       // max(n_head, n_head_kv)
  // The graph's compute stream (cudaStream_t). Every launch in step() goes on it: a non-null stream keeps
  // u::k's sync_if_needed() from draining the device after each kernel (it syncs on the null stream).
  void* cs_ = nullptr;
  // Device-decided routed MoE: the router writes each layer's ids to d_ids_all_[layer*k]; one async D2H per
  // token copies them to h_ids_all_ (pinned) for the routing heat / hit counters (flush_routing, after a
  // cs_ sync). moe_dev_[layer] != 0 => that layer runs ExpertTier::reg_run_token_device.
  int* d_ids_all_ = nullptr;
  std::int32_t* h_ids_all_ = nullptr;
  std::vector<char> moe_dev_;
  bool moe_dev_any_ = false;
  bool ids_pending_ = false;
  void flush_routing();
  // The decode step as a CUDA graph: step() writes the per-token inputs to pinned host buffers (token,
  // position, PLE rows), then replays graph_exec_ (cudaGraphExec_t), captured once from device_step_
  // after an eager warm-up step. STRATA_QWEN_GRAPH=0 (or a debug tap / profiler env) keeps it eager.
  [[nodiscard]] bool device_step_(std::int32_t token, std::string& error);
  void ple_prepare(std::int32_t token);
  void* graph_exec_ = nullptr;
  bool graph_enabled_ = false;
  bool graph_warm_ = false;
  int tail_rank_ = 0;            // decode's STRATA_ROUTE_TAIL_SKIP rank (0 = off); prefill always runs 0
  bool adapt_async_ = false;     // reg_rebalance_async (copies beside the decode) vs the blocking reg_rebalance
  int adapt_every_ = 8;          // decode tokens between adaptive-tier rounds
  int adapt_swaps_ = 0;          // swap cap a round (<= 0: none)
  bool cpu_lane_on_ = false;     // the CPU expert lane is enabled (STRATA_QWEN_CPU_LANE=1, reg_lane_*)
  bool lane_decode_ = false;     // the lane takes misses now (decode); prefill runs every miss on the GPU
  int* d_tok_in_ = nullptr;      // device: this step's token id (the embed gather's row)
  int* h_tok_in_ = nullptr;      // pinned host: written by step()
  int* h_pos_ = nullptr;         // pinned host: this step's position (-> d_pos_)
  float* h_ple_emb_ = nullptr;   // pinned host: this step's PLE rows [n_embd]
  int* d_tok_ = nullptr;   // the sampler's one-int output

  // ---- matmul-dispatch scratch for gemv_() (Q8_1 activation for the quantized mmvq path, a BF16
  // activation buffer for the bf16 path). Sized for the widest reduction dim in load().
  void* q8_scratch_ = nullptr;
  std::uint16_t* bf16_x_ = nullptr;
  std::int64_t max_n_in_ = 0;
  void gemv_(const WSlot& w, const float* x, float* out, std::int64_t n_out, std::int64_t n_in, void* stream);
  void rmsnorm_to(float* out, const float* in, const float* w, std::int64_t n, float eps, void* stream);

  // Scratch slices (floats into sc_), carved in load(). embed + HC, then GDN, QSA, MoE, head.
  std::int64_t sc_emb = 0, sc_xn = 0, sc_lo = 0, sc_hg = 0, sc_inj = 0, sc_bin = 0;  // embed + HC
  std::int64_t sc_qkv = 0, sc_cv = 0, sc_q = 0, sc_k = 0, sc_v = 0, sc_al = 0, sc_be = 0;  // GDN
  std::int64_t sc_gt = 0, sc_z = 0, sc_o = 0, sc_y = 0;                                    // GDN
  std::int64_t sc_qg = 0, sc_qq = 0, sc_qg2 = 0, sc_qk = 0, sc_qv = 0, sc_at = 0;          // QSA
  std::int64_t sc_mixer = 0, sc_rlog = 0, sc_ids = 0, sc_rw = 0, sc_moe = 0;               // MoE
  std::int64_t sc_sg = 0, sc_su = 0, sc_sh = 0, sc_shd = 0, sc_ffn = 0;                    // shared expert
  std::int64_t sc_head = 0, sc_logits = 0;
  // PLE (per-layer embedding, applied at ple_layer_ only): emb (n_embd), key (hc_dim), value (n_embd),
  // key_norm/query_norm/gated/normed-gated/conv (hc_dim), gate (hc).
  std::int64_t sc_ple_emb = 0, sc_ple_key = 0, sc_ple_val = 0, sc_ple_kn = 0, sc_ple_qn = 0;
  std::int64_t sc_ple_gate = 0, sc_ple_gated = 0, sc_ple_ng = 0, sc_ple_conv = 0;

  // ---- PLE n-gram embedding (qwen4exp.ple.*). ple_layer_ < 0 disables it (table/metadata absent).
  int ple_layer_ = -1;                  // the one layer that applies PLE (qwen4exp.ple.layers[0])
  int ple_ngram_ = 0, ple_per_gram_ = 0, ple_n_heads_ = 0, ple_head_dim_ = 0, ple_kern_ = 0;
  std::int64_t ple_eos_ = 0;
  std::vector<std::uint64_t> ple_mult_;     // layer_multipliers[ngram]
  std::vector<std::uint32_t> ple_off_, ple_vocab_;  // head_offsets / head_vocab_sizes [n_heads]
  u::ple::Spec ple_spec_;                   // the same params as the shared host addressing (u_ple.hpp)
  const std::uint8_t* ple_table_ = nullptr;  // host-mapped per_layer_token_embd row 0 (IQ4_NL, 90 B/row)
  std::int64_t ple_n_rows_ = 0;
  std::uint32_t ple_row_bytes_ = 90;
  std::int64_t ple_hist_ = -1;              // state offset: (kern-1)*ngram vectors of hc_dim, oldest-first
  std::vector<std::int32_t> ple_tokens_;    // the token ids seen so far (for the n-gram window; EOS resets)
  std::vector<float> ple_emb_host_;         // host scratch: the 16 gathered+dequantized rows -> n_embd

  // ---- speculative decode: the T-token VERIFY WINDOW (docs/superpowers/specs/2026-10-24-qwen4exp-
  // speculative-decode-design.md, S2). T tokens at pos_.. go through every layer at once; row t's argmax is
  // bitwise what single-token greedy decode would produce after token t (multi-column GEMVs whose columns
  // are bitwise single-column calls, every other kernel per token). The window reads the GDN conv/recurrent
  // states and the PLE history without writing them; commit(n_keep) replays the kept tokens' stored inputs
  // in place. K/V cells past the kept prefix are overwritten when those positions are processed again.
  static constexpr int kWinMax = 8;
  int spec_t_ = 0;                  // STRATA_QWEN_SPEC_T: the window size (0 / 1 = plain decode)
  float* wa_ = nullptr;             // window scratch: buffer x at wa_ + kWinMax*sc_x, token t at + t*len(x)
  float* rw_ = nullptr;             // [kWinMax][hc_dim] the window tokens' residual streams
  float* rp_ = nullptr;             // per GDN layer, the replay inputs of the window's tokens
  std::vector<std::int64_t> rp_qkv_, rp_q_, rp_k_, rp_v_, rp_gt_, rp_be_;  // floats into rp_ (-1: not GDN)
  float* ple_ng_win_ = nullptr;     // [kWinMax][hc_dim] the PLE conv inputs (replayed at commit)
  float* ple_hist_win_ = nullptr;   // the window's working copy of the PLE conv history
  void* q8_win_ = nullptr;          // Q8_1 activation scratch for kWinMax columns
  int* d_tok_win_ = nullptr;        // device [kWinMax] token ids, from the pinned h_tok_win_
  int* h_tok_win_ = nullptr;
  int* d_pos_win_ = nullptr;        // device [kWinMax] positions, from the pinned h_pos_win_
  int* h_pos_win_ = nullptr;
  int* d_pos_rows_win_ = nullptr;   // device [kWinMax][pos_rows_] the rope rows' positions
  int* h_pos_rows_win_ = nullptr;
  float* h_ple_win_ = nullptr;      // pinned [kWinMax][n_embd] the window tokens' PLE rows
  int* d_ids_win_ = nullptr;        // device [n_layers][kWinMax][k] routed ids
  std::int32_t* h_ids_win_ = nullptr;
  int* d_out_win_ = nullptr;        // device [kWinMax] the rows' greedy picks
  int* h_out_win_ = nullptr;
  std::vector<void*> win_exec_, commit_exec_;  // cudaGraphExec_t, index = T / n_keep
  std::vector<char> win_seen_, commit_seen_;   // ran eagerly once (lazy allocations), so capturable next time
  std::vector<std::int32_t> spec_drafts_;      // STRATA_QWEN_DRAFT_FILE: a reference stream to draft from (S2)
  int spec_corrupt_ = 0;                       // STRATA_QWEN_DRAFT_CORRUPT=K: every K-th window's last draft wrong
  [[nodiscard]] bool window_forward_(int T, std::string& error);
  [[nodiscard]] bool window_commit_dev_(int n_keep, std::string& error);
  [[nodiscard]] bool run_window_(int T, const std::int32_t* tokens, std::string& error);
  [[nodiscard]] bool commit_window_(int n_keep, const std::int32_t* tokens, std::string& error);
  [[nodiscard]] bool launch_captured_(std::vector<void*>& exec, std::vector<char>& seen, int n, bool window,
                                      std::string& error);
  [[nodiscard]] bool decode_spec_(const Gen& request, const LineSink& sink, std::uint32_t& produced,
                                  std::string& error);
  void ple_prepare_into(std::int32_t token, std::int64_t p, float* dst);

  // ---- the MTP draft layer (S3; family/qwen4exp_mtp.cu), from upstream's mtp/rt files (STRATA_QWEN_MTP=<dir>).
  // Cell i pairs the main model's final hc residual at position i with the token at i+1 (rope position i);
  // its output predicts the token at i+2 and its own residual feeds the next draft step. Projections Q8_0,
  // HC/router BF16, norms F32 (+1 applied), the 512 routed experts Q2_0 (all in VRAM), the head the main
  // output.weight's rows of the draft vocabulary. Drafts only: the verify window decides every token.
  bool mtp_on_ = false;
  float spec_min_p_ = 0.7f;         // a draft chain continues while the draft's probability clears this
  std::map<std::string, WSlot> mtpw_;
  void* mtp_dense_ = nullptr;       // dense.bin on the device
  std::uint8_t* mtp_exp_ = nullptr; // [512] experts, each gate|up|down in GGUF Q2_0 blocks
  std::int64_t mtp_blob_ = 0, mtp_ff_ = 0, mtp_nexp_ = 0;
  int* mtp_res_ = nullptr;          // identity residency [512] (every expert resident)
  int* mtp_tail0_ = nullptr;        // a 0 tail-skip rank
  void* mtp_head_ = nullptr;        // the draft head: output.weight rows of the draft vocabulary
  WSlot mtp_head_slot_{};
  int* mtp_dvocab_ = nullptr;       // device [n_dv] draft row -> token id
  std::int64_t mtp_ndv_ = 0;
  float* mtp_kc_ = nullptr;         // the layer's own K/V [head_dim][n_head_kv][max_ctx]
  float* mtp_vc_ = nullptr;
  float* mtp_sc_ = nullptr;         // scratch (offsets below), sized for kWinMax K/V rows
  std::int64_t mo_emb_ = 0, mo_en_ = 0, mo_e2_ = 0, mo_hn_ = 0, mo_h2_ = 0, mo_r_ = 0, mo_xn_ = 0, mo_lo_ = 0,
               mo_hg_ = 0, mo_mixed_ = 0, mo_inj_ = 0, mo_k_ = 0, mo_v_ = 0, mo_qf_ = 0, mo_q_ = 0, mo_qg_ = 0,
               mo_at_ = 0, mo_at2_ = 0, mo_bo_ = 0, mo_inj2_ = 0, mo_rlog_ = 0, mo_rw_ = 0, mo_parts_ = 0, mo_g_ = 0,
               mo_u_ = 0, mo_h_ = 0, mo_sg_ = 0, mo_su_ = 0, mo_sh_ = 0, mo_shd_ = 0, mo_sgl_ = 0, mo_moe_ = 0,
               mo_ffn_ = 0, mo_sample_ = 0, mo_hlog_ = 0;
  int* mtp_ids_ = nullptr;          // [k] routed ids, [k] eff
  int* mtp_eff_ = nullptr;
  void* mtp_xq8_ = nullptr;         // Q8_1 of one n_embd column / of k n_ff columns
  void* mtp_hq8_ = nullptr;
  int* d_mtok_ = nullptr;           // device [kWinMax] the rows' tokens; [kWinMax] cells; [kWinMax][pos_rows_] rope
  int* d_mcell_ = nullptr;
  int* d_mrows_ = nullptr;
  int* h_mio_ = nullptr;            // pinned: tokens | cells | rope rows (one region per call kind)
  int* d_mpick_ = nullptr;          // the head's argmax row, its token id, its probability
  int* d_mtokout_ = nullptr;
  float* d_mprob_ = nullptr;
  int* h_mout_ = nullptr;           // pinned {token id, probability bits}
  std::int64_t mtp_draft_cell_ = -1;  // the cell the last draft row ran at (its residual is mo_r_ row 0)
  std::int64_t mtp_rounds_ = 0, mtp_steps_ = 0;
  [[nodiscard]] bool mtp_load_(const std::string& dir, std::string& error);
  [[nodiscard]] bool mtp_forward_(int n, bool full, const float* rin, std::string& error);
  void mtp_read_(const std::string& prefix, float* R, int n, float* mixed, float* inj);
  // K/V only for cells cell0..cell0+n-1 (n <= kWinMax): rin = their main-model residuals, toks = the next tokens
  [[nodiscard]] bool mtp_kv_rows_(const float* rin, int n, const std::int32_t* toks, std::int64_t cell0,
                                  std::string& error);
  // the full layer on one cell: rin = the main residual (a round's first draft) or nullptr (the MTP's own
  // residual of the previous draft step); the draft token and its probability back on the host
  [[nodiscard]] bool mtp_draft_row_(const float* rin, std::int32_t tok, std::int64_t cell, std::int32_t& out,
                                    float& prob, std::string& error);
  // a chain of up to max_drafts drafts after the main residual `rin` at `cell` and the token `tok` there+1
  [[nodiscard]] int mtp_chain_(const float* rin, std::int32_t tok, std::int64_t cell, int max_drafts,
                               std::int32_t* drafts, std::string& error);
  void gemv_multi_(const WSlot& w, const float* x, float* out, std::int64_t n_out, std::int64_t n_in, int ncols,
                   void* stream);

  friend std::unique_ptr<FamilyGraph> make_qwen4exp_graph(const Geometry&, std::uint32_t, std::string&);
};

// The qwen4exp factory (design contract). Builds and loads the graph for a guarded geometry; returns
// nullptr with `error` set on a load failure. The dispatch wires this into the CUDA branch of
// u::make_family_graph for FamilyId::qwen4exp (a shared-file change this lane does not own).
[[nodiscard]] std::unique_ptr<FamilyGraph> make_qwen4exp_graph(const Geometry& geom,
                                                               std::uint32_t context, std::string& error);

}  // namespace u
