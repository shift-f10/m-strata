// src/program/generate.cpp - P2.S6: `strata generate`.
//
// THE DRIVER, and the first program in this project that answers a question.  Everything below it is a
// component; this is the thing that composes them into a token:
//
//     embed_row(token)  ->  48 captured layer graphs (the CPU expert pool behind the doorbell)  ->
//     lm_head(R)        ->  sample        ->  embed_row(next)  ->  ...
//
// WHAT IT IS NOT.  There is no tokenizer here.  `pack/full/tokenizer/` and `tools/strata_tokenizer.py` exist,
// and a C++ BPE is Phase 1's deliverable rather than this program's, so the prompt arrives as IDS via
// `--tokens`.  That is not a placeholder: it is exactly what Gate C1 needs, because C1 compares logits against
// llama.cpp on the SAME ids, and a tokenizer on only one side of that comparison is a second variable.
//
// AND IT IS PHASE 2, so hit rate is `h = 0` and the number it prints is slow on purpose
// (`phase-2-correct-engine.md:5-9`).  What it is FOR is the honest tok/s figure and the logit dump.

#include "strata/core/expert_cache.hpp"
#include "strata/core/expert_source.hpp"
#include "strata/core/remote_experts.hpp"
#include "strata/kernels/cpu/expert.hpp"
#include "strata/kernels/cpu/pool.hpp"
#include "strata/kernels/cpu/expert_layout.hpp"
#include "strata/kernels/s2_expert_grouped.hpp"
#include "strata/kernels/sampler.hpp"
#include "strata/kernels/native_rope.hpp"
#include "strata/kernels/mrope.hpp"
#include "strata/kernels/kv_q4.hpp"
#include "strata/kernels/qsa.hpp"
#include "strata/core/glm_model.hpp"
#include "strata/core/mtp_rows.hpp"
#include "strata/core/prefill_gaps.hpp"
#include "strata/core/progress.hpp"
#ifndef NOMINMAX
#define NOMINMAX   // gguf_reader.hpp includes windows.h
#endif
#include "strata/artifact/gguf_reader.hpp"
#include "strata/core/prefix_cut.hpp"
#include "strata/core/split_groups.hpp"
#include "strata/core/prefill_pieces.hpp"
#include "strata/kernels/cvec.hpp"
#include "strata/program/logits_selection.hpp"
#include "strata/platform/env_alias.hpp"
#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#include <io.h>
#else
#include <unistd.h>
#include <cerrno>
#endif

#include <cuda_runtime.h>

#include <array>
#include <chrono>
#include <algorithm>
#include <iostream>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <charconv>
#include <climits>
#include <cmath>
#include <limits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <set>
#include <vector>

namespace {
// perf-review D-4: the lent slots are refilled with queued copies and one wait; STRATA_REFILL_BLOCKING=1 waits on each
bool refill_blocking() {
    static const bool v = std::getenv("STRATA_REFILL_BLOCKING") != nullptr;
    return v;
}

using Clock = std::chrono::steady_clock;

struct Options {
    std::string pack = "pack/full";
    std::string glm_pack;             // --glm-pack: a GLM-5.3-Flash pack dir (the M3.1 path; see glm_pack_generate)
    std::vector<int64_t> tokens;      // the prompt, PRE-TOKENIZED
    int64_t max_new = 16;
    int64_t max_context = 4096;
    bool greedy = true;
    uint64_t seed = 0;
    int top_k = 20;
    float top_p = 0.95f;
    float temperature = 1.0f;
    std::string dump_logits;          // one line of logits per generated position
    int64_t logits_stride = 1;        // storage selection; all prompt tokens remain conditioned
    /// **THE RESIDUAL, SO THE HEAD CAN BE CHECKED WITHOUT THE LAYERS.**
    ///
    /// C1 fails (LEDGER L116) and the pipeline is `embed -> 48 layers -> head`.  Dumping `R` splits it in half:
    /// the head is one norm, two bf16 projections and one 794 MB GEMV, all of which can be recomputed in Python
    /// from the manifest.  If Python agrees with the engine on the same `R`, the head is right and the layers
    /// are wrong; if it disagrees, the head is wrong.  Nothing else in the engine can be split that cheaply.
    std::string ple_gguf;              // the ORIGINAL second GGUF shard: the PLE table is not in the pack
    bool no_ple = false;              // explicit diagnostic ablation; never a normal inference default
    bool stream_token = false;        // R2.6 experiment: ordered work on the session stream
    bool check_logits = false;        // optional full-vocabulary finite scan
    bool gr_fp32_activations = false;  // pinned CUDA single-token BF16 activation contract
    bool gr_native_mmvf = false;       // pinned projection reduction tree as well as FP32 inputs
    bool native_bf16 = false;          // SSM gates, router and indexer projections only
    bool native_bf16_extra = false;    // PLE value and shared expert scalar gate
    bool native_ple_key = false;       // unchanged Q2_0 key and CUDA Q8_1 activations
    bool native_moe_combine = false;   // pinned fused CUDA weighted reduction
    bool native_gdn = false;          // pinned CUDA recurrence and preprocessing
    bool native_flash_attn_short = false; // diagnostic pinned attention, context <=256
    bool native_qsa_indexer = false;  // pinned F16 key cache and F32 pooling
    bool native_qsa = false;          // pinned F32 QSA norms and gate
    bool native_rope = false;         // pinned text-only CUDA rotary arithmetic
    bool native_ple_postops = false;  // pinned PLE postprojection arithmetic
    bool native_router = false;       // pinned fused 512-expert top-10 router
    bool cpu_oracle_q8_0 = false;      // pinned x86 activation scales/codes at both expert stages
    std::string native_head_gguf;      // native output.weight experiment; same model shard as the pack
    std::vector<std::string> native_dense_gguf; // repeat for native GDN/QSA projection shards
    /// Plan v0.3 P1: the whole native arithmetic set as ONE switch (model shard 1). It enables exactly the
    /// combination recorded in bench/results/2026-09-23-attention-ple plus the native indexer, and never the
    /// <=256-token attention adapter. It becomes the default once P0 shows it is not slower.
    std::string native_preset;
    /// Plan v0.3 P2: how the n-gram table is read. Direct (default) = unbuffered SSD reads, table never in RAM.
    std::string ple_io = "direct";
    int64_t ple_row_cache = 1 << 20;   ///< bounded row cache (rows of 90 B); 0 disables
    int ple_inflight = 64;
    double ple_delay_us = 0;           ///< fault injection: every row read completes no earlier than this
    bool ple_sync_submit = false;      ///< A/B arm: submit reads on the token thread, no I/O worker
    std::string kv = "fp16";           ///< plan v0.3 P7: KV storage, fp16 (default) or int8 (half the VRAM)
    int64_t kv_resident = 0;           ///< KV streaming: resident cells per QSA layer (0: all in VRAM)
    std::string kv_streaming;          ///< --kv-streaming on|off: streaming with kv_resident's window (32768 if none)
    std::string dump_residual;
    /// The head input, `bb.mixed`.  It exists so the head can be SPLIT: steps 1-4 (the per-stream norm, the two
    /// bf16 projections and the stream mean) recompute cheaply in Python, and only the 794 MB GEMV does not.
    std::string dump_mixed;
    /// One residual snapshot per layer per position: `n_layers * hc * n_embd` floats per position, appended in
    /// position order.  This is the C1 BISECTION LADDER - it is what `llama-debug --tensor-filter l_last` prints
    /// for the reference, so the first layer whose `sum` diverges is the layer that holds the bug.  It needs the
    /// captured path (the expert pool only exists there), so it is refused with `--no-capture`.
    std::string dump_layers;
    /// `2 * n_embd + 2 * hc` floats per layer per position: the attention half's block output, the MoE half's,
    /// and the two injection vectors.  It separates `linear_attn_out-<l>` from `ffn_out-<l>`, which the residual
    /// ladder cannot.  **CAPTURED INTO THE LAYER GRAPHS**, so it must be armed before `session_capture`.
    std::string dump_halves;
    /// P0.S8's routing trace, and a prerequisite the Phase 3 plan names explicitly.  One record per layer per
    /// position: `int32 layer, int32 k, k int32 ids, k float weights`.  It is what a hit-rate curve for a
    /// candidate VRAM expert cache is computed from, and it needs no new kernels - the doorbell already
    /// publishes exactly this much to pinned memory.
    std::string dump_routing;
    bool no_capture = false;          // run the layers directly instead of replaying graphs
    bool no_pool = false;             // skip the CPU expert pool: the GPU-only floor
    bool sync_every_layer = false;
    /// Per-stage CUDA-event timings inside the layer halves.  `--no-capture` only: an event recorded inside a
    /// stream capture is silently dropped, so the captured path cannot carry this.
    bool stage_timing = false;
    /// Launch the 48 captured `pre` graphs back to back with no host work between them and report the pure GPU
    /// time per token.  This is the only measurement that separates host-bound from GPU-bound, because the
    /// stage events include every gap where the GPU waited for the host.
    bool graph_only = false;
    bool gpu_only_full = false;   ///< R0.3: pre + post + head, the true per-token GPU floor
    int pool_workers = 0;         ///< R2.2: 0 = "all physical cores minus the host's"; >0 overrides
    /// R2.2's first half, as an A/B arm.  **ON by default**, because the measurement that justifies it is the
    /// pool's own drain: 33.7 GB/s against 5/6 x 44.14 = 36.8 for five workers, on a machine whose sixth core
    /// is reserved for a host thread that has nothing to do while the drain runs.
    bool no_host_worker = false;
    bool mmap_experts = false;    ///< R2.1: opt OUT of the resident arena, back to MapViewOfFile
    bool resident_cpu_experts = false; ///< mmap-backed static-cache misses copied into ordinary RAM
    /// R4: slots of VRAM-resident experts.  **0 = off, and off is the default.**
    /// **THE COMMENT THAT USED TO BE HERE WAS FALSE AND ROUND 328 MEASURED IT.**  It said "the cache has no
    /// consumer yet - `moe_hit_grouped_s2` does not exist - so switching it on costs the fill traffic and
    /// saves nothing".  The kernel exists (`src/kernels/cuda/s2_expert_grouped.cu`), it is wired at line ~660
    /// via `expert_hit_run`, and switching the cache on **does** move work off the CPU pool: the drain fell
    /// **19.076 -> 10.312 ms/token** at 4096 per-layer slots, for **-2.7 ms/token** end to end.  What was
    /// true is that the ADMISSION POLICY gave every slot to the first position, which is why the earlier
    /// measurement found nothing - see `expert_cache_per_layer`.
    int expert_cache = 0;
    std::array<int, 3> expert_cache_remote{}; ///< CUDA1..3 slots; CUDA0 keeps dense/state/MTP
    std::string expert_cache_remote_placement = "stripe"; ///< stripe experts or assign complete layers to CUDA1..3
    bool expert_cache_cpu_order = false;
    /// **R4.2g.  ROUND 328 MEASURED THAT THE GLOBAL ADMISSION POLICY CANNOT WORK, AND THIS IS THE FIX.**
    /// The default policy hands out slots in arrival order from one counter shared by all 48 layers, so the
    /// first `n_slots` distinct pairs - about 26 LAYERS OF POSITION 0 - take every slot and hits are confined
    /// to them.  Measured at 256 slots: **1781 of 60000 = 2.97%**, against **21.4%** for 8 slots per layer and
    /// **70.4%** for 64, from `Memory/cache_allocation.py` on the same run's routing.  Off by default.
    bool expert_cache_per_layer = false;
    /// The PLE gather's prefetch, as an A/B arm.  The gather measured 2.10-2.61 ms/token because its sixteen
    /// row reads are sixteen SEPARATE page faults into a 26.8 GB mapping; see `ple_prefetch_enable`.
    bool no_ple_prefetch = false;
    /// R4.2e: a `profile.bin` from `tools/make_profile.py`.  **When given, it decides residency instead of the
    /// compulsory-miss policy**, which is the whole point: a profile ranked by routing frequency over a whole
    /// trace is what the plan's `h = 0.6447` refers to, and compulsory-miss measured 0.4864 because it fills
    /// with whatever the prompt touched FIRST.  Empty means no profile.  A GLM pack (--glm-pack) reads it too: its
    /// ranking decides each layer's VRAM slots and the warm-up's order (data/expert-profile-glm.bin).
    std::string expert_profile;
    /// #477 (--serve, opt-in): where to save what the expert tiers learned, as a profile `--expert-profile` reads
    /// (the resident experts first, then the routing counted); on QUIT and every `expert_profile_save_min` minutes
    /// between requests.  Empty (the default): nothing is written.  GLM packs (--glm-pack) only.
    std::string expert_profile_save;
    double expert_profile_save_min = 10.0;
    /// R4.2d: **ON by default**, because the measurement is unambiguous and the alternative is known-broken.
    /// Without it, 17 of 10,562 layers had the hit work done when the pool returned; with it, 9,190.  The
    /// A/B arm is `--no-hit-poke`.
    bool no_hit_poke = false;
    /// R0.9: capture each layer as THREE graphs and time them from outside the capture, which is the only
    /// valid way to get a per-stage table on the real graph.  Prints and exits; it is a measurement, not a run.
    bool gpu_stages = false;
    bool stats = false;
    bool shared_late = false;          ///< plan v0.3 P3 A/B: shared expert inside post[l] (old order)
    bool keep_canonical = false;       ///< plan v0.3 P1 A/B: load canonical copies of natively served tensors
    bool no_token_graph = false;       ///< plan v0.3 P3 A/B: two graphs per layer instead of one per token
    bool no_fused_gr = false;          ///< plan v0.3 P3 A/B: the six-kernel native gr_read + separate gr_write
    bool no_fast_attn = false;         ///< plan v0.3 P3 A/B: gather + one-block-per-head QSA attention
    bool no_publish_kernel = false;    ///< plan v0.3 P3 A/B: memcpy nodes for the doorbell and QSA step
    bool no_fused_gdn = false;         ///< plan v0.3 P3 A/B: llama.cpp-layout GDN step + separate out norm
    bool no_fast_select = false;       ///< plan v0.3 P7 A/B: FP64 row scores + bit-serial cell top-k
    /// Plan v0.3 P4: `--expert-cache auto` sizes the VRAM tier from what is free after the weights, the session
    /// and the KV state, minus this reserve for the graphs, the hit scratch and the head.
    int vram_reserve_mib = 700;
    bool vram_reserve_given = false;   ///< --vram-reserve-mib on the command line (--glm-pack: a small card's default gives way)
    /// `--vram-reserve-later-mib N` (--glm-pack): the reserve on a layer split's later GPUs (-1: --vram-reserve-mib's)
    int vram_reserve_later_mib = -1;
    /// Plan v0.3 P5: batched prompt processing in chunks of this many tokens (0 = the token path).
    int64_t prefill_chunk = 0;
    /// `--prefill auto`: the largest chunk (up to 8192) whose buffers the expert cache can lend.  Every expert a chunk
    /// routes to is streamed once per chunk, so a bigger chunk streams fewer bytes per token (the "ubatch" effect).
    bool prefill_auto = false;
    std::string prefill_arg;           ///< `--prefill` as given (the GLM engine reads it as STRATA_GLM_PREFILL)
    std::string dflash;                ///< `--dflash`: a DFlash2 draft GGUF for the GLM engine (STRATA_GLM_DFLASH)
    bool no_split_rows = false;        ///< plan v0.3 P4 A/B: one whole expert per pool thread
    /// Plan v0.3 P5: the prompt path borrows the top expert-cache slots for its buffers and refills them after
    /// the prompt (default); `--no-prefill-borrow` reserves the buffers' VRAM for the whole session instead.
    bool no_prefill_borrow = false;
    /// Plan v0.3 P5 validation: batch only positions [0, P) and run the rest of the prompt through the token path
    /// (teacher-forced), so the logits of positions >= P - which depend on the batched state - can be scored
    /// against the oracle at many positions.  0 = the whole prompt but the last position.
    int64_t prefill_until = 0;
    /// Plan v0.3 P6: after every processed position, append the residual after the last layer (hc x n_embd
    /// floats, the MTP draft head's input) to this file.  Token path only.
    std::string dump_final_r;
    /// Plan v0.3 P6: speculative decoding with a verify window of this many tokens (the last accepted token and
    /// spec-1 drafts); 0 = plain decode.  `spec_oracle` drafts from a token file (the expected continuation, for
    /// the exactness test); `spec_corrupt` N > 0 replaces every Nth draft with a wrong token.
    int spec = 0;
    std::string spec_oracle;
    std::string spec_follow, spec_follow_save;   // --glm-pack: STRATA_GLM_SPEC_FOLLOW / _SAVE (glm_pack_generate)
    int spec_corrupt = 0;
    /// Plan v0.3 P6: the MTP draft layer's runtime directory (tools/mtp_rt.py); drafts come from it.
    std::string mtp;
    int64_t mtp_window = 32768;   ///< the draft layer attends to the last N cells (0 = every cell)
    /// Plan v0.3 P6: the share (0..1) of each layer's distinct missed experts the GPU reads over PCIe from the
    /// pinned arena while the CPU computes the rest (verify windows).
    double pcie_frac = -1.0;   ///< < 0: the model's default (0.2 direct for the Q2_0 pack, 0.55 DMA for native packs)
    std::string pcie_mode = "auto";   ///< auto | dma | kernel | direct
    /// Plan v0.3 P6: every `adapt_every` rounds, swap up to `adapt_swaps` of the most-routed missing experts into
    /// the VRAM tier in place of the least-routed resident ones (decayed counts).  0 = static residency.
    int adapt_every = 4;
    /// Plan v0.3 P6: a draft enters the verify window only while every draft before it (and itself) has at least
    /// this probability under the draft layer; 0 = always --spec-1 drafts.
    double spec_min_p = 0.0;
    bool spec_min_p_set = false;   ///< --spec-min-p given (the DFlash decode's default cut is 0.9 otherwise)
    /// Stop when the model emits an end-of-turn token (<|endoftext|> 248044, <|im_end|> 248046, or --eos-ids).
    bool stop_eos = false;
    bool eos_given = false;    ///< --stop-eos or --eos-ids appeared: the glm path stops only when asked (defaults are qwen's)
    std::vector<int64_t> eos_ids = {248044, 248046};
    bool spec_split = false;   ///< opt-in split verify window (the overlap study: exact, ~7% slower)
    /// --serve, multi-GPU layer split: "K" or "K1,K2,.." (the first layer of each later stage) or "auto" (placed
    /// from each GPU's free VRAM); empty = one GPU
    std::string layer_split;
    /// the later stages' devices "D1,D2,.." (default: the next visible GPUs; "0" with one K: both stages on this
    /// GPU, sharing everything - the bit-exact A/B of the hand-off)
    std::string split_device;
    /// Plan v0.3 P8: stay resident and take requests on stdin (see the --serve block in main).
    bool serve = false;
    /// The vision path: keep a per-cell (t, h, w) rotary position table so --serve can take GENI requests.
    bool vision = false;
    int adapt_swaps = 96;
    /// --serve: how many conversation checkpoints to keep between requests (0 = every request reads its whole
    /// prompt again, the v0.1.2 behaviour).  One is the GDN recurrence of the 36 layers, the QSA indexer tails and
    /// the PLE history (~118 MB of host RAM); the KV cache itself is positional and stays where it is.
    int prompt_cache = 6;
    /// --serve: also keep a checkpoint every N freshly read prompt tokens (0 = only at the last turn boundary)
    int64_t prompt_cache_every = 16384;
    /// --serve: a prompt read from token 0 is also checkpointed at its first turn boundary - the end of the system
    /// prompt, which every chat of the same client shares - when that is at least N tokens (0 = never)
    int64_t prompt_cache_root = 2048;
    /// --serve: the token that opens a chat turn (<|im_start|>).  The last one in a prompt is where the chat's
    /// history ends and the new assistant turn begins, which is the checkpoint the next request can reuse.
    int64_t turn_token = 248045;
    /// --serve: a text part of the prompt of at most N tokens (a chat message, the assistant header, a short tool
    /// result) goes through the verify windows, S tokens at a time, instead of the batched prompt path (0 = always
    /// the batched path)
    int64_t short_read = 64;
    /// The suffix drafter (prompt lookup): when the text being written repeats an earlier stretch of the context (code
    /// edits, quoted input, tool-call JSON) by at least this many tokens, the window may be filled with what followed
    /// it there instead of the MTP's drafts, where the MTP's own first guess agrees and the draft policy expects it to
    /// pay (strata/spec/draft_policy.hpp).  On by default; 0 = MTP only.
    int suffix_draft = 3;
    /// The MTP's own window cap (0 = --spec): with --spec 6 --mtp-max-t 4 the long windows come from suffix matches.
    int mtp_max_t = 0;
    /// A control vector on the residual stream (strata/kernels/cvec.hpp), with llama.cpp's flags: the
    /// `experimental-speed-projection` profile passes `--control-vector-scaled FILE:1.0 --control-vector-layer-range
    /// 4 44 --cvec-mode project --cvec-dir per-layer`.  None by default; --serve switches a loaded one per request.
    std::vector<std::pair<std::string, float>> cvec_files;
    int cvec_first = -1, cvec_last = -1;   ///< llama.cpp's defaults: 1 .. the last layer
    int cvec_mode = 1;                     ///< 0 = project, 1 = add (llama.cpp's default)
    int cvec_single = -1;                  ///< --cvec-dir single:L (project mode): layer L's direction everywhere
};

void usage() {
    std::fprintf(stderr,
                 "maya generate --pack DIR --tokens \"1,2,3\" [options]\n"
                 "\n"
                 "  --pack DIR           the pack directory (default pack/full)\n"
                 "  --glm-pack DIR       a GLM-5.3-Flash pack (M3.1): the verified Glm5Model runner behind the\n"
                 "                       same wire contract (T <id> / DONE; --serve reads GEN lines)\n"
                 "  --dflash GGUF        (--glm-pack) draft with a DFlash2 block drafter made for the model: each\n"
                 "                       round it drafts up to --spec T - 1 tokens (default its block size - 1), one\n"
                 "                       verify window checks them; --spec-min-p P stops a draft at a selector\n"
                 "                       confidence below P (default 0.5 on 3+ GPUs, 0.95 on one; two GPUs: at\n"
                 "                       least one draft, up to 4 while the confidences' product stays at 0.9 -\n"
                 "                       MAYA_GLM_DFLASH_MIN / _CUM).  The model's\n"
                 "                       MTP block is not loaded with it.  Env: MAYA_GLM_DFLASH\n"
                 "  --spec-follow PATH   (--glm-pack) speed A/B: every request decodes this continuation (token ids,\n"
                 "                       as --tokens-file takes them) instead of its own picks and ends with it; the\n"
                 "                       drafting decodes keep exactly the drafts that match it, so two builds or\n"
                 "                       settings process the same text.  Env: MAYA_GLM_SPEC_FOLLOW\n"
                 "  --spec-follow-save PATH  (--glm-pack) write each request's decoded token ids there (for the above).\n"
                 "                       Env: MAYA_GLM_SPEC_FOLLOW_SAVE\n"
                 "  --expert-cache auto|N  (--glm-pack) the expert pool in VRAM: auto (the default) takes the free\n"
                 "                       VRAM less the reserve and checks it again once written; N caps the first\n"
                 "                       GPU's at N of its largest experts.  Env: MAYA_GLM_EXPERT_CACHE\n"
                 "  --vram-reserve-mib N (--glm-pack) VRAM kept free beside the expert pool (default 700; a small\n"
                 "                       card's gives way to the pool's floor).  Env: MAYA_GLM_RESERVE_MB\n"
                 "  --vram-reserve-later-mib N  (--glm-pack) the same on a split's later GPUs (default: the above).\n"
                 "                       Env: MAYA_GLM_RESERVE_LATER_MB\n"
                 "  --glm-ram-resident   the pinned RAM tier holds every expert VRAM does not, and nothing is evicted\n"
                 "                       to disk. The start fails if the tier cannot hold them (raise MAYA_GLM_RAM_GB\n"
                 "                       or lower --max-context). Env: MAYA_GLM_RAM_RESIDENT=1\n"
                 "  --glm-ram-slack N    extra RAM-tier slots per MoE layer (MAYA_GLM_RAM_SLACK; default 16 when\n"
                 "                       resident, 0 otherwise)\n"
                 "  --parallel N         --serve: up to N requests decode at once (1-8, default 1). One GPU decodes\n"
                 "                       them together (each weight read once for all); a split pipelines them\n"
                 "                       across its cards. Each extra conversation keeps its own context state\n"
                 "  --tokens LIST        the prompt as comma-separated token IDS (required)\n"
                 "  --tokens-file PATH   pretokenized prompt, commas or whitespace (alternative to --tokens)\n"
                 "  --ple-gguf PATH      required PLE table (original second GGUF shard)\n"
                 "  --no-ple             explicit diagnostic ablation of the PLE layer\n"
                 "  --ple-io direct|mmap  n-gram table reads (plan v0.3 P2). direct (default): unbuffered SSD\n"
                 "                       reads, the table never enters RAM or the file cache; mmap: A/B arm\n"
                 "  --ple-row-cache N    bounded cache of fetched rows, 90 B each (default 1048576; 0 = off)\n"
                 "  --ple-inflight N     outstanding SSD reads (default 64)\n"
                 "  --ple-delay-us U     fault injection: each row read completes no earlier than U us\n"
                 "  --ple-sync-submit    A/B arm: submit table reads on the token thread (default: an I/O thread)\n"
                 "  --kv fp16|int8       KV storage (plan v0.3 P7): int8 codes + fp16 scale per 64 values, half the\n"
                 "                       VRAM; default fp16 until gate G-C accepts int8\n"
                 "  --kv q4_0            4-bit K/V after a Hadamard rotation (PR #21): half of int8's memory,\n"
                 "                       slightly lower precision\n"
                 "  --kv k8v4            hybrid: INT8 K (exact attention scores) + rotated Q4_0 V, 816 B/cell\n"
                 "                       (vs int8's 1,056); not with --kv-resident\n"
                 "  --kv-resident N      KV streaming: keep N cells of each QSA layer in VRAM (min 20480) and the\n"
                 "                       whole K/V in pinned RAM; the freed VRAM goes to expert slots. 0 (default):\n"
                 "                       all of it in VRAM. A context of N cells or fewer is not streamed. With\n"
                 "                       --glm-pack: N positions of each DSA layer's latent cache\n"
                 "                       (MAYA_GLM_KV_RESIDENT)\n"
                 "  --kv-streaming on|off  KV streaming as a switch (the setup writes `on` into the config): on keeps\n"
                 "                       --kv-resident's N cells in VRAM, 32768 when it gives none; off keeps all\n"
                 "                       of the KV in VRAM whatever --kv-resident says\n"
                 "  --stream-token       enqueue token work on the session stream (experimental)\n"
                 "  --check-logits       copy and check all logits in the stream-token path\n"
                 "  --gr-fp32-activations  experimental CUDA-oracle GR activation precision\n"
                 "  --gr-native-mmvf      experimental pinned GR norm/projections; implies FP32 activations\n"
                 "  --native-bf16         experimental CUDA-oracle SSM/router/indexer BF16 projections\n"
                 "  --native-bf16-extra   experimental CUDA-oracle PLE/shared gate BF16 projections\n"
                 "  --native-ple-key      experimental native PLE key; requires --native-dense-gguf\n"
                 "  --native-moe-combine  experimental pinned CUDA routed/shared combination\n"
                 "  --native-gdn          experimental pinned CUDA GDN norms/gates/recurrence\n"
                 "  --native-flash-attn-short  diagnostic pinned vector attention; --max-context <=256\n"
                 "  --native-qsa-indexer  experimental pinned indexer key cache and pooling\n"
                 "  --native-qsa          experimental pinned QSA normalization and output gate\n"
                 "  --native-rope         experimental pinned text-only CUDA rotary arithmetic\n"
                 "  --native-ple-postops  experimental pinned PLE postprojection arithmetic\n"
                 "  --native-router       experimental pinned CUDA 512-expert top-10 routing\n"
                 "  --cpu-oracle-q8-0     experimental pinned CPU expert quantization and dot reduction\n"
                 "  --native SHARD1      every full-context native path at once (plan v0.3 P1): stream-token,\n"
                 "                       GR MMVF, BF16, head, dense + PLE key, MoE combine, GDN, router, QSA,\n"
                 "                       indexer, RoPE, PLE postops, and the CPU q8_0 contract unless the\n"
                 "                       expert cache is on. Individual --native-* flags stay for A/B.\n"
                 "  --native-head-gguf PATH  native Q5_K head from model shard 1; requires --stream-token\n"
                 "  --native-dense-gguf PATH native GDN/QSA/shared projections; repeat for each source model shard\n"
                 "  --expert-cache-cpu-order  experimental GPU expert reduction matching CPU order\n"
                 "  --max-new N          tokens to generate (default 16)\n"
                 "  --max-context N      KV/state capacity (default 4096)\n"
                 "  --greedy             argmax (the default)\n"
                 "  --seed S             enable sampling with this Philox seed\n"
                 "  --top-k N --top-p F --temperature F\n"
                 "  --dump-logits PATH   write one line of raw logits per position\n"
                 "  --logits-stride N    store every Nth row plus final input (default 1); N>1 requires --max-new 1\n"
                 "  --dump-residual PATH write the final R (hc x n_embd, f32) for head bisection\n"
                 "  --dump-layers PATH   write R after EVERY layer, per position: the C1 bisection ladder\n"
                 "  --dump-halves PATH   write both halves' block_out and inject per layer: the half bisection\n"
                 "  --dump-routing PATH  write the routed expert ids and weights per layer per position (P0.S8)\n"
                 "  --no-capture         run the layers directly instead of replaying graphs\n"
                 "  --shared-late        A/B: shared expert after the CPU pool (default: overlapped with it)\n"
                 "  --keep-canonical     A/B: also load canonical copies of natively served tensors (more VRAM)\n"
                 "  --vision             --serve takes images too (GENI requests; embeddings from maya-vision)\n"
                 "  --prompt-cache N     --serve: keep N conversation checkpoints between requests (default 6, ~118 MB\n"
                 "                       of RAM each; 0 = read every prompt from the start)\n"
                 "  --prompt-cache-every N  --serve: also checkpoint every N fresh prompt tokens (default 16384, 0 = off)\n"
                 "  --turn-token ID      --serve: the token that opens a chat turn (default 248045, <|im_start|>)\n"
                 "  --short-read N       --serve: read at most N fresh text tokens through the decode windows instead\n"
                 "                       of the batched prompt path (default 64, 0 = off)\n"
                 "  --suffix-draft N     prompt lookup: draft from an earlier repeat of the last N+ tokens of context\n"
                 "                       when it pays (default 3; 0 = MTP only)\n"
                 "  --mtp-max-t M        cap the MTP's windows at M tokens (0 = --spec; longer ones come from suffixes)\n"
                 "  --control-vector-scaled FILE:SCALE[,...]  a control vector GGUF on the residual stream (llama.cpp's\n"
                 "                       format; --control-vector FILE = scale 1).  --serve: requests switch it (cvec=0|1)\n"
                 "  --control-vector-layer-range A B  the layers it follows (inclusive; default 1 .. the last)\n"
                 "  --cvec-mode add|project  h += s v (default) or h -= s (h.v) v with v unit\n"
                 "  --cvec-dir per-layer|single:L  each layer's own direction (default) or layer L's everywhere (project)\n"
                 "  --no-token-graph     A/B: two graphs per layer (the host launches each) instead of one per token\n"
                 "  --no-fused-gr        A/B: the six-kernel hyper-connection read and a separate write (native)\n"
                 "  --prefill CHUNK      batched prompt processing in chunks of CHUNK tokens (needs --native); auto =\n"
                 "                       the largest chunk up to 8192 whose buffers the expert cache can lend\n"
                 "  --no-pool            skip the CPU expert pool (the GPU-only floor)\n"
                 "  --sync-every-layer   debug: synchronise after every layer\n"
                 "  --ple-gguf PATH      the n-gram/PLE shard.  WITHOUT IT LAYER 1's PLE IS SILENTLY SKIPPED,\n"
                 "                       which changes every number downstream - pass it for any real run\n"
                 "  --dump-mixed PATH    write the post-attention residual (n_embd, f32)\n"
                 "  --stage-timing       per-stage KERNEL-COUNT shares.  NOT a time profile: an uncaptured\n"
                 "                       event interval includes host gaps, so run with --gpu-only-full first\n"
                 "  --graph-only         MEASURE: replay the 48 `pre` graphs only.  OMITS the 48 `post` graphs\n"
                 "                       and the LM head, so it is NOT the GPU floor (R0.3, Memory/ERRORS.md A4)\n"
                 "  --gpu-only-full      MEASURE: replay pre+post for all 48 layers plus the LM head, no pool.\n"
                 "                       THE TRUE PER-TOKEN GPU FLOOR.  Quote this one, not --graph-only.\n"
                 "  --stats              print the per-stage breakdown\n"
                 "  --gpu-stages         R0.9: capture the layer as three graphs (mixer / ffn+router / post)\n"
                 "                       and time them from OUTSIDE the capture.  The per-stage table on the\n"
                 "                       real graph that --stage-timing cannot give.  Prints and exits.\n"
                 "  --expert-profile P   R4.2e: pre-load the VRAM tier from a `profile.bin` (see\n"
                 "                       tools/make_profile.py) instead of admitting on first use.\n"
                 "  --expert-profile-save P  --serve, #477 (--glm-pack): save what the expert tiers learned (the\n"
                 "                       experts in VRAM, then the routing counted) as a profile at P, on QUIT\n"
                 "                       and every --expert-profile-save-every MIN minutes (default 10; 0 = on\n"
                 "                       QUIT only) between requests; start from it with --expert-profile P\n"
                 "  --no-hit-poke        R4.2d's A/B arm.  The hit path pokes the driver once right after its\n"
                 "                       launch so the GPU starts while the CPU pool runs; without it the work\n"
                 "                       waits for the next driver entry and does not overlap at all.\n"
                 "  --no-ple-prefetch     A/B arm: read the PLE table's sixteen rows one at a time, instead of\n"
                 "                       issuing them in one PrefetchVirtualMemory call.\n"
                 "  --expert-cache N     R4: keep N expert blobs resident in VRAM and compute their rows on the\n"
                 "                       GPU via `moe_hit_grouped_s2`.  DEFAULT 0.  Measured at 4096 slots\n"
                 "                       with --expert-cache-per-layer: 54.4%% hits, CPU pool drain 19.1 -> 10.3\n"
                 "                       ms/token, -2.7 ms/token end to end.\n"
                 "  --expert-cache-device1 N  pre-fill N experts on CUDA1 (experimental)\n"
                 "  --expert-cache-device2 N  pre-fill N more experts on CUDA2\n"
                 "  --expert-cache-device3 N  pre-fill N more experts on CUDA3\n"
                 "  --expert-cache-remote-placement stripe|layer  distribute expert ranks or whole\n"
                 "                       layers across CUDA1..3 (default: stripe)\n"
                 "  --expert-cache-per-layer  R4.2g: give each layer its OWN slots instead of letting the first\n"
                 "                       position take all of them.  The default policy fills in arrival order\n"
                 "                       from one shared counter, so 256 slots went to ~26 layers of position 0\n"
                 "                       and measured **2.97%%**.  Per-layer, the same routing gives 21.4%% at 8\n"
                 "                       slots/layer and 70.4%% at 64.\n"
                 "  --no-host-worker     R2.2: the A/B arm.  By default the HOST THREAD joins the drain, so the\n"
                 "                       pool is six threads on six cores instead of five plus an idle core;\n"
                 "                       this flag restores the five-worker form for comparison on `pool phases`.\n"
                 "  --pool-workers N     R2.2: CPU expert pool worker count.  Default 0 = every physical core\n"
                 "                       except the one the host loop spins on.  A sweep is how the pool's\n"
                 "                       deviation from `cpu_s2` is attributed.\n"
                 "  --mmap-experts       R2.1: opt OUT of the resident expert arena, back to MapViewOfFile.\n"
                 "                       The A/B arm: the mmap's rate depends on the OS page cache holding\n"
                 "                       34 GB, and measured 71.97 vs 34.78 ms/token cold vs warm.\n"
                 "  --resident-cpu-experts  with mmap and a static profile, keep CPU misses resident in ordinary RAM.\n"
                 "                       Borrowed GPU-cache entries may read from mmap during prompt prefill.\n");
}

/// All shards of a split GGUF, from shard 1's path ("...-00001-of-00002.gguf"); just the path when it is not split.
std::vector<std::string> model_shards(const std::string& first) {
    const std::string tag = "-00001-of-";
    const size_t at = first.rfind(tag);
    if (at == std::string::npos || first.size() < at + tag.size() + 10) return {first};
    const int total = std::atoi(first.substr(at + tag.size(), 5).c_str());
    std::vector<std::string> out;
    for (int i = 1; i <= total && i <= 99; ++i) {
        char num[8];
        std::snprintf(num, sizeof num, "%05d", i);
        std::string p = first;
        p.replace(at + 1, 5, num);
        if (std::ifstream(p, std::ios::binary)) out.push_back(p);
    }
    return out.empty() ? std::vector<std::string>{first} : out;
}

bool parse_i64_list(const char* s, std::vector<int64_t>& out, std::string& err) {
    out.clear();
    std::string text(s);
    for (char& c : text) if (c == ',') c = ' ';
    std::istringstream input(text);
    std::string token;
    while (input >> token) {
        int32_t id = 0;
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), id);
        if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() || id < 0) {
            err = "invalid token id: expected an integer in [0, 2147483647]";
            out.clear();
            return false;
        }
        out.push_back(id);
    }
    if (out.empty()) { err = "token list was empty"; return false; }
    return true;
}

/// The pool's adapter plus the wall-clock it spent, so the report can say how much of the token was the CPU.
struct Drive {
    strata::core::ExpertDispatch d;
    double cpu_ms = 0;
    int64_t calls = 0;
    /// THE ROUTING TRACE, which is P0.S8 and a stated prerequisite of Phase 3.  `drive_pool` is called once
    /// per layer from the main loop - the workers live inside `expert_pool_dispatch` - so a single FILE* here
    /// needs no locking.  `d.layers` is the CURRENT layer on entry (the adapter increments it as it walks the
    /// blob), which is why the layer index comes from there rather than from a counter of our own.
    std::FILE* routing = nullptr;
};



/// Layer split: every verify stage shares one Drive (its counters, usage and failure flags); the GPU plan, the expert
/// cache and the PCIe share the pool uses for a layer are those of the stage that runs it.
struct SplitDrive {
    static constexpr int kMax = 8;
    Drive* base = nullptr;
    int n = 0;                                    ///< stages
    int64_t end[kMax] = {};                       ///< stage i runs the layers from end[i - 1] (0) below end[i]
    strata::core::GpuPlanSink* plan[kMax] = {};
    const uint8_t* cache_base[kMax] = {};
    const uint64_t* cache_slot_off[kMax] = {};
    int pcie_num[kMax] = {};
};


// ---- issue #31: what the watchdog prints before it stops a stalled engine
struct MemSample {
    unsigned long long faults = 0, rss_mib = 0, avail_mib = 0, commit_mib = 0;   // faults: hard (Linux) / all (Windows)
};
MemSample mem_sample() {
    MemSample m;
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc{};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) {
        m.faults = pmc.PageFaultCount;
        m.rss_mib = pmc.WorkingSetSize >> 20;
        m.commit_mib = pmc.PagefileUsage >> 20;
    }
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms)) m.avail_mib = ms.ullAvailPhys >> 20;
#else
    if (std::FILE* f = std::fopen("/proc/self/stat", "r")) {
        char buf[4096];
        const size_t n = std::fread(buf, 1, sizeof buf - 1, f);
        buf[n] = 0;
        std::fclose(f);
        const char* s = std::strrchr(buf, ')');   // fields after the command name: 3 state ... 12 majflt
        for (int field = 2; s && field < 12; ++field) s = std::strchr(s + 1, ' ');
        if (s) m.faults = std::strtoull(s + 1, nullptr, 10);
    }
    auto kb = [](const char* path, const char* key) -> unsigned long long {
        unsigned long long v = 0;
        if (std::FILE* f = std::fopen(path, "r")) {
            char line[256];
            const size_t kl = std::strlen(key);
            while (std::fgets(line, sizeof line, f))
                if (std::strncmp(line, key, kl) == 0) { v = std::strtoull(line + kl, nullptr, 10); break; }
            std::fclose(f);
        }
        return v;
    };
    m.rss_mib = kb("/proc/self/status", "VmRSS:") >> 10;
    m.commit_mib = kb("/proc/self/status", "VmSwap:") >> 10;
    m.avail_mib = kb("/proc/meminfo", "MemAvailable:") >> 10;
#endif
    return m;
}

void stall_report(std::FILE* f, uint64_t layers_during) {
    strata::core::Progress& p = strata::core::progress();
    std::fprintf(f, "maya serve: stall report (engine %s): stage \"%s %lld\" for %lld s; %llu layers served since the "
                    "last finished step (0 = stopped, more = slow)\n", STRATA_VERSION, p.where.load(),
                 (long long) p.detail.load(), (long long) ((strata::core::progress_now_ms() - p.since_ms.load()) / 1000),
                 (unsigned long long) layers_during);
    for (int pass = 0; pass < 2; ++pass) {
        if (pass == 1) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            std::fprintf(f, "  2 s later:\n");
        }
        if (auto fn = strata::core::diag_pool_fn().load()) fn(f);
        if (auto fn = strata::core::diag_verify_fn().load()) fn(f);
        const MemSample m = mem_sample();
        std::fprintf(f, "  memory: %llu MiB resident, %llu MiB %s, %llu MiB RAM available; %llu %s\n", m.rss_mib,
                     m.commit_mib,
#if defined(_WIN32)
                     "committed", m.avail_mib, m.faults, "page faults so far"
#else
                     "in swap", m.avail_mib, m.faults, "major page faults so far"
#endif
        );
        std::fflush(f);
    }
#if defined(_WIN32)
    // every thread's stack, to read against this build's PDB: a few MB beside the engine's working directory
    if (HMODULE dbg = LoadLibraryA("dbghelp.dll")) {
        using Fn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);
        if (auto write = (Fn) GetProcAddress(dbg, "MiniDumpWriteDump")) {
            char path[64];
            std::snprintf(path, sizeof path, "maya-stall-%lu.dmp", (unsigned long) GetCurrentProcessId());
            HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                const int kThreadInfo = 0x1000;   // MiniDumpWithThreadInfo; MiniDumpNormal = 0
                const BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), h, kThreadInfo, nullptr, nullptr, nullptr);
                CloseHandle(h);
                char full[MAX_PATH];
                if (!GetFullPathNameA(path, MAX_PATH, full, nullptr)) std::snprintf(full, sizeof full, "%s", path);
                std::fprintf(f, "  %s the thread stacks to %s (attach it to the issue)\n", ok ? "wrote" : "could not write",
                             full);
            }
        }
    }
#endif
}

// issue #29: a request whose heartbeat (tokens, prompt chunks or layers, verify windows) stops for STRATA_WATCHDOG_S
// seconds (default_s when unset; 0 = off) is stuck on a flag nobody will raise - end the engine with where it was, so
// the server starts it again instead of the GPU spinning forever.  Issue #31: before it does, it reports what every
// part was doing (stall_report), so one occurrence says where the wait is.  Issue #40: the GLM serve has it too.
void start_serve_watchdog(int default_s) {
    const char* ws = std::getenv("STRATA_WATCHDOG_S");
    const int limit = ws ? std::atoi(ws) : default_s;
    if (limit <= 0) return;
    std::thread([limit] {
        strata::core::Progress& p = strata::core::progress();
        uint64_t last = p.beats.load(), ticks_at = p.ticks.load();
        auto since = std::chrono::steady_clock::now();
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const auto now = std::chrono::steady_clock::now();
            const uint64_t b = p.beats.load();
            if (!p.busy.load() || b != last) { last = b; ticks_at = p.ticks.load(); since = now; continue; }
            if (now - since < std::chrono::seconds(limit)) continue;
            std::fprintf(stderr, "maya serve: no progress for %d s during a request (%s %lld) - stopping "
                                 "the engine so the server starts it again (issue #29)\n",
                         limit, p.where.load(), (long long) p.detail.load());
            stall_report(stderr, p.ticks.load() - ticks_at);
            std::fflush(stderr);
            // not abort(): Windows Error Reporting can hold the process open, and a core dump of tens of GB of
            // pinned expert tiers takes minutes - the stall report above already says where it stopped
            std::_Exit(3);
        }
    }).detach();
}

/// STRATA_TRACE=1: the VRAM left at a step of the startup (finds what fills the card after the cache is sized)
void mem_mark(const char* where) {
    static const bool on = std::getenv("STRATA_TRACE") != nullptr;
    if (!on) return;
    size_t free_b = 0, total_b = 0;
    cudaMemGetInfo(&free_b, &total_b);
    std::fprintf(stderr, "maya trace: %lld MiB free after %s\n", (long long) (free_b >> 20), where);
}

int argmax(const std::vector<float>& v) {
    int best = 0;
    for (size_t i = 1; i < v.size(); ++i)
        if (v[i] > v[best]) best = (int) i;
    return best;
}

// ---- --serve's conversation cache.  A chat or an agent sends the whole conversation again with every request, and
// reading it again is what made a long session wait minutes for every turn.  What a sequence leaves behind splits in
// two, and only one half needs copying:
//   * POSITIONAL state - the KV cache of the 12 QSA layers and their pooled indexer keys, the draft layer's KV.  A
//     cell is written once for its position and read only by later positions (the block scores take `dead` for the
//     block being filled, never its pooled row), so rewinding to a position just means writing from there again.
//   * RUNNING state - the 36 GDN recurrences and conv histories, each QSA layer's indexer tail (the unfinished
//     block's raw keys) and the PLE's normalized history.  Each describes "everything so far" and cannot be
//     rewound, so a checkpoint is a copy of exactly these: ~118 MB, the same set the verifier snapshots to roll
//     back rejected drafts.
// A checkpoint is only valid while the positional cells below it still hold ITS tokens, so the serve loop keeps
// just the checkpoints that are prefixes of the tokens the session holds now.
struct ImgKey {
    int64_t start = 0;      ///< the image's first <|image_pad|> position
    uint64_t hash = 0;      ///< its embeddings and grid: the pad tokens alone are the same for every picture
    bool operator==(const ImgKey& o) const { return start == o.start && hash == o.hash; }
};

struct ConvCheckpoint {
    std::vector<int32_t> ids;     ///< the tokens this state has consumed
    std::vector<ImgKey> imgs;     ///< the images among them
    std::vector<uint8_t> gdn, ple, tails;
    uint64_t used = 0;            ///< last-use stamp for the retention policy (conv_cache.hpp)
    std::vector<ConvCheckpoint> stage_parts;   ///< a layer split's later stages: their sessions' running state
};

uint64_t fnv1a(const void* data, size_t n, uint64_t h = 1469598103934665603ull) {
    const uint8_t* p = (const uint8_t*) data;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

struct ConvStateSizes {
    size_t gdn = 0, ple = 0, tail = 0;
};





// The effective host->device bandwidth of the PCIe link: copies from pinned host memory, as the expert arena's
// reads are.  The native default share (0.55) was measured on x16 links (~26-28 GB/s); a x8 card in a x8 slot
// carries about half of that.  Returns < 0 when the probe cannot run (then the caller keeps the default).
double probe_pcie_h2d_gbps() {
    constexpr size_t kBytes = 256ull << 20;
    constexpr int kIters = 4;
    void* h = nullptr;
    void* d = nullptr;
    cudaEvent_t ev0, ev1;
    if (cudaMallocHost(&h, kBytes) != cudaSuccess) return -1.0;
    if (cudaMalloc(&d, kBytes) != cudaSuccess || cudaEventCreate(&ev0) != cudaSuccess ||
        cudaEventCreate(&ev1) != cudaSuccess) {
        if (d != nullptr) cudaFree(d);
        cudaFreeHost(h);
        return -1.0;
    }
    std::memset(h, 0, kBytes);   // fault the pages in before timing
    cudaMemcpyAsync(d, h, kBytes, cudaMemcpyHostToDevice);   // warmup: context up, copy engine primed
    cudaEventRecord(ev0);
    for (int i = 0; i < kIters; ++i) cudaMemcpyAsync(d, h, kBytes, cudaMemcpyHostToDevice);
    cudaEventRecord(ev1);
    const bool ok = cudaEventSynchronize(ev1) == cudaSuccess;
    float ms = 0.f;
    const bool timed = ok && cudaEventElapsedTime(&ms, ev0, ev1) == cudaSuccess && ms > 0.01f;
    const double bw = timed ? ((double) kIters * (double) kBytes / (ms * 1e-3)) / 1e9 : -1.0;
    cudaEventDestroy(ev0);
    cudaEventDestroy(ev1);
    cudaFree(d);
    cudaFreeHost(h);
    return bw;
}

// ================================ the glm5-next pack path (M3.1) ================================
//
// A GLM-5.3-Flash pack (tools/iq_pack.py output) runs the VERIFIED runner - Glm5Model, the same
// path glm_pack_test drives (docs/GLM5-FLASH.md §9) - behind this driver's wire contract:
// `T <id>` per generated token on stdout, `ERR <message>` when a request cannot run, the DONE
// summary at the end, and --serve reading `GEN <max_new> <id,id,...>` lines (QUIT ends).
//
// NOT wired here yet, and refused rather than approximated: prompt-prefix reuse and per-request
// state continuity across GEN lines (each GEN starts a fresh sequence), drafts,
// the batched prompt path (the prompt runs one token at a time), images, control vectors and
// layer splits.  Sampling is the engine's own sampler (strata::kernels::sample_tokens, greedy by
// default), so the top-k/top-p/temperature chain behaves exactly as in the qwen4exp path - the
// penalties too (#273): the request's history (its prompt, then what it wrote) rides in
// SamplerParams::host_history, and every pick counts its last penalty_last_n tokens
// (Glm5Model::pick_tokens; a verify window's row t: those, then the drafts before it).
// The stop set is GLM's own, from the GGUF metadata (the ids llama.cpp marks EOG): 154820
// <|endoftext|> (eos), 154827 <|user|> (eot, the assistant yields the floor) and 154829 <|observation|> (a tool
// call waits for its result); --eos-ids
// overrides it.  Serve stops on them by default; non-serve stops only when asked (--stop-eos or
// --eos-ids), like the qwen path.
// a variable of this process's environment (the GLM model reads its switches from there)
static void set_env(const char* k, const char* v) {
#if defined(_WIN32)
    _putenv_s(k, v);
#else
    setenv(k, v, 1);
#endif
}

static int glm_pack_generate(const Options& o) {
#if !defined(_WIN32)
    // serving: the server ends the engine with QUIT (or by closing its input), and the engine then sets the conversation
    // aside (STRATA_GLM_SLOT_KEEP).  A SIGTERM to the whole process group (systemd stopping a service, a supervisor
    // killing the server's group) must not end it mid-write; a server that is already gone must not end it with
    // SIGPIPE when it writes its last lines.
    if (o.serve) {
        std::signal(SIGTERM, SIG_IGN);
        std::signal(SIGINT, SIG_IGN);
        std::signal(SIGPIPE, SIG_IGN);
    }
#endif
    strata::core::Glm5Model model;
    std::string err;
    // the prompt chunk, set in the config's args like Strata's: --prefill auto | N (a chunk of N tokens) | 0
    // (the engine reads it as STRATA_GLM_PREFILL; without either, auto)
    if (!o.prefill_arg.empty()) set_env("STRATA_GLM_PREFILL", o.prefill_arg.c_str());
    // the DFlash2 drafter (--dflash, else STRATA_GLM_DFLASH): loaded with the model, on the device of its head
    if (!o.dflash.empty()) set_env("STRATA_GLM_DFLASH", o.dflash.c_str());
    // --spec-follow / --spec-follow-save (else STRATA_GLM_SPEC_FOLLOW / _SAVE): read below, once the vocabulary is known
    if (!o.spec_follow.empty()) set_env("STRATA_GLM_SPEC_FOLLOW", o.spec_follow.c_str());
    if (!o.spec_follow_save.empty()) set_env("STRATA_GLM_SPEC_FOLLOW_SAVE", o.spec_follow_save.c_str());
    // the expert profile (Strata's profile.bin: setup passes data/expert-profile-glm.bin) and the routing trace
    // (--dump-routing: tools/make_profile.py's input), before the load reads the one and the first prompt the other
    if (!o.expert_profile.empty()) model.set_expert_profile(o.expert_profile);
    if (!o.dump_routing.empty() && !strata::core::Glm5Model::set_routing_dump(o.dump_routing, err)) {
        std::fprintf(stderr, "maya generate: --dump-routing: %s\n", err.c_str());
        return 1;
    }
    // the layer split across the visible GPUs: STRATA_GLM_SPLIT, else --layer-split (auto | K1,K2,..)
    if (!model.load_pack_env(o.glm_pack, o.max_context, err, o.layer_split)) {
        std::fprintf(stderr, "maya generate: %s\n", err.c_str());
        return 1;
    }
    const int64_t n_vocab = model.geometry().n_vocab;
    // the driver samples on the device (sample_token), so the fast path never downloads the logits
    model.set_host_logits(false);
    if (model.fast()) {
        // INFO facts for the server's Monitor tab (strata app): the expert tiers this engine runs with
        const auto st = model.fast_stats();
        // (+ the CPU lane setup's calibration reads: its threads and its PCIe share, as the engine started)
        std::printf("INFO context=%lld kv=%s kv_resident=%lld expert_slots=%lld expert_cache_mib=%lld engine_kind=glm-fast "
                    "experts=%d vram_slots=%lld vram_gb=%.1f ram_slots=%lld ram_gb=%.1f mtp=%d dflash=%d vision_lend=%zu "
                    "cpu_threads=%d pcie_share=%.2f logit_bias=1\n",
                    (long long) o.max_context, model.kv_name(), (long long) model.kv_resident(),
                    (long long) st.pool_slots,
                    (long long) (st.pool_gb * 1024.0), model.geometry().n_expert * (model.geometry().n_layers - model.geometry().dense_lead),
                    (long long) st.pool_slots, st.pool_gb, (long long) st.ram_slots, st.ram_gb, model.has_mtp() ? 1 : 0,
                    model.dflash_ready() ? 1 : 0, model.vision_lend_bytes(), model.cpu_lane_threads(), model.pcie_share());
        // the Monitor's map of each GPU: what each part holds on its card, in bytes (Glm5Model::VramReport) - the
        // server finds the card NVML numbers by its PCI address and adds what NVML sees besides
        for (const auto& r : model.vram_report())
            std::printf("VRAM dev=%d pci=%s total=%llu free=%llu weights=%llu kv=%llu kv_ram=%llu act=%llu drafter=%llu "
                        "window=%llu pool=%llu tail=%llu borrow=%llu vision=%llu slots=%lld ctx=%lld layers=%d-%d\n",
                        r.dev, r.pci.empty() ? "-" : r.pci.c_str(), (unsigned long long) r.total,
                        (unsigned long long) r.free, (unsigned long long) r.weights, (unsigned long long) r.kv,
                        (unsigned long long) r.kv_ram, (unsigned long long) r.act, (unsigned long long) r.drafter,
                        (unsigned long long) r.window, (unsigned long long) r.pool, (unsigned long long) r.tail,
                        (unsigned long long) r.borrow, (unsigned long long) r.vision, (long long) r.slots,
                        (long long) r.ctx, r.l0, r.l1);
        // (Windows on an APU: the commit - the page file - kept experts off the GPU; the server's log, Monitor and
        // /v1/status say so, as for DeepSeek's engine)
        if (const auto cc = model.commit_capped(); cc.first > 0) {
            std::printf("NOTE experts: %.0f GB stay off the GPU - Windows' page file is too small to hold them there; "
                        "a page file of %zu GB keeps them on the GPU (System > About > Advanced system settings > "
                        "Performance > Advanced > Virtual memory), then restart Maya\n", (double) cc.first / 1e9, cc.second);
            std::printf("INFO gpu_commit_capped_gb=%.0f pagefile_hint_gb=%zu\n", std::ceil((double) cc.first / 1e9),
                        cc.second);
        }
    }

    strata::kernels::SamplerParams sp;
    sp.greedy = o.greedy;
    sp.top_k = o.top_k;
    sp.top_p = o.top_p;
    sp.temperature = o.temperature;
    sp.seed = o.seed;

    // The end-of-turn ids are the loaded model's, not one family's constant: glm5-next's <|endoftext|>/<|user|>/
    // <|observation|>, or raon1's <|im_end|>(259202) + <|endoftext|>(259200) -- its generation_config.json declares
    // eos_token_id [259202, 259200], and <|im_end|> is the ChatML turn terminator (the pack's tokenizer metadata
    // carries only 259202, so the second is added here to match the model's declared stop set).  Without this the
    // engine checked glm5's ids against raon1 output and never saw an eos, so every raon1 reply ran to max_new.
    const std::vector<int64_t> glm_eos = model.geometry().is_raon1
                                             ? std::vector<int64_t>{259202, 259200}
                                             : std::vector<int64_t>{154820, 154827, 154829};
    const std::vector<int64_t>& eos = o.eos_given ? o.eos_ids : glm_eos;
    // STRATA_GLM_KL_DUMP=<file>,<n> (a test of the K/V cache formats): --tokens-file's tokens teacher-forced - all but
    // the last n through the prompt path, those n one at a time through the decode - and the logits after each of the
    // n into file (n x n_vocab F32: row i predicts token size - n + i + 1)
    if (const char* kd = std::getenv("STRATA_GLM_KL_DUMP")) {
        const std::string spec(kd);
        const size_t c = spec.rfind(',');
        const std::string path = spec.substr(0, c);
        const int64_t n = c == std::string::npos ? 64 : std::atoll(spec.c_str() + c + 1);
        const auto& t = o.tokens;
        if (n <= 0 || (int64_t) t.size() <= n) {
            std::fprintf(stderr, "maya generate: STRATA_GLM_KL_DUMP needs more than %lld tokens\n", (long long) n);
            return 2;
        }
        model.set_host_logits(true);
        std::string err;
        std::vector<float> lg;
        const std::vector<int32_t> head(t.begin(), t.end() - n);
        const bool pok = model.fast() && model.prefill(head, err, (int32_t) t[t.size() - (size_t) n]);
        if (!pok && (!err.empty() || !model.forward(head, lg, err))) {
            std::fprintf(stderr, "maya generate: %s\n", err.c_str());
            return 1;
        }
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (f == nullptr) {
            std::fprintf(stderr, "maya generate: cannot write %s\n", path.c_str());
            return 1;
        }
        for (size_t i = t.size() - (size_t) n; i < t.size(); ++i) {
            if (!model.forward({(int32_t) t[i]}, lg, err) || (int64_t) lg.size() < n_vocab) {
                std::fprintf(stderr, "maya generate: %s\n", err.empty() ? "no logits" : err.c_str());
                std::fclose(f);
                return 1;
            }
            std::fwrite(lg.data(), sizeof(float), (size_t) n_vocab, f);
        }
        std::fclose(f);
        std::fprintf(stderr, "maya generate: %lld positions' logits in %s\n", (long long) n, path.c_str());
        return 0;
    }

    const bool stop_on_eos = o.eos_given || o.stop_eos || o.serve;

    // ---- stdin on its own thread (the qwen serve's pattern): a STOP line must reach a request that is
    // still running.  read(2), not std::cin - glibc's exit() flushes stdio and waits on stdin's lock,
    // which getline holds while it waits for input.
    std::atomic<bool> stop_req{false}, quit_req{false};
    // WRAP (the server is stopping): a request still thinking closes its reasoning at the next token and answers
    std::atomic<bool> wrap_req{false};
    // PAUSE (the server's fair slices): STOP, and the conversation's state is kept with what the request wrote (the
    // snapshot moves to the last token fed), so the request that continues it reads only its last token
    std::atomic<bool> pause_req{false}, pause_keep{false};
    // several conversations at once (STRATA_GLM_SEQS): STOP <rid> / PAUSE <rid> [keep] for one request; the request
    // whose prompt is being read stops at the next chunk
    std::mutex ctl_mu;
    std::map<int, int> ctl;   // rid -> 1 STOP, 2 PAUSE, 3 PAUSE keep
    std::atomic<int> admitting_rid{-1};
    // STRATA_GLM_PREFILL_INTERLEAVE (strata/core/prefill_pieces.hpp): the other requests decode between the pieces of
    // a prompt being read, so a STOP / PAUSE for the request being admitted stops its read alone (admit_stop) - not
    // stop_req, which every decoding request reads as its own.  Off: stop_req as before (nothing decodes meanwhile)
    static const strata::core::PrefillInterleave interleave =
        strata::core::prefill_interleave_parse(std::getenv("STRATA_GLM_PREFILL_INTERLEAVE"));
    // STRATA_GLM_PREFILL_GAPS (strata/core/prefill_gaps.hpp): the other requests decode at the chunk boundaries of a
    // prompt being read (one call, its chunks pipelined as without) - like interleave, a STOP for the request being
    // read stops its read alone.  Over interleave when both are set.
    static const strata::core::PrefillGaps read_gaps =
        strata::core::prefill_gaps_parse(std::getenv("STRATA_GLM_PREFILL_GAPS"));
    std::atomic<bool> admit_stop{false};
    const auto read_stopped = [&] { return stop_req.load() || admit_stop.load(); };
    std::mutex in_mu;
    std::condition_variable in_cv;
    std::deque<std::string> in_lines;
    bool in_eof = false;
    if (o.serve) {
        std::thread([&] {
            std::string l, buf;
            char chunk[4096];
            auto getline_fd = [&](std::string& out) -> bool {
                for (;;) {
                    const size_t nlpos = buf.find('\n');
                    if (nlpos != std::string::npos) {
                        out.assign(buf, 0, nlpos);
                        buf.erase(0, nlpos + 1);
                        return true;
                    }
#if defined(_WIN32)
                    const int n = _read(0, chunk, (unsigned) sizeof chunk);
#else
                    const ssize_t n = ::read(0, chunk, sizeof chunk);
                    if (n < 0 && errno == EINTR) continue;
#endif
                    if (n <= 0) {
                        if (buf.empty()) return false;
                        out.swap(buf);
                        buf.clear();
                        return true;
                    }
                    buf.append(chunk, (size_t) n);
                }
            };
            while (getline_fd(l)) {
                if (!l.empty() && l.back() == '\r') l.pop_back();
                if (l == "STOP") { stop_req.store(true); continue; }
                if (l.rfind("STOP ", 0) == 0 || (l.rfind("PAUSE ", 0) == 0 && l != "PAUSE keep")) {
                    std::istringstream ls(l);
                    std::string c0, kw;
                    int rid = -1;
                    ls >> c0 >> rid;
                    // (no request id there: nothing to stop - a negative one would match admitting_rid's idle -1 and
                    // set the global stop, which every running request reads as its own)
                    if (ls.fail() || rid < 0) continue;
                    ls >> kw;
                    {
                        std::lock_guard<std::mutex> lk(ctl_mu);
                        ctl[rid] = c0 == "STOP" ? 1 : kw == "keep" ? 3 : 2;
                    }
                    if (rid == admitting_rid.load()) {
                        admit_stop.store(true);
                        if (interleave.tokens == 0 && read_gaps.percent == 0) stop_req.store(true);
                    }
                    continue;
                }
                if (l == "PAUSE" || l == "PAUSE keep") {   // keep: the prompt's state is set aside first
                    pause_keep.store(l == "PAUSE keep");
                    pause_req.store(true);
                    stop_req.store(true);
                    continue;
                }
                if (l == "WRAP") { wrap_req.store(true); continue; }
                if (l == "QUIT") {                       // cancel an in-flight request AND queue the line:
                    quit_req.store(true);
                    stop_req.store(true);                // (the decode checks stop_req; reset by the next GEN)
                }
                std::lock_guard<std::mutex> lk(in_mu);   // a GEN written before QUIT must still run (pipes)
                in_lines.push_back(l);
                in_cv.notify_one();
            }
            std::lock_guard<std::mutex> lk(in_mu);
            in_eof = true;
            in_cv.notify_one();
        }).detach();
    }
    const auto next_line = [&](std::string& out) -> bool {
        model.lane_drift_check();
        model.lane_retime();
        std::unique_lock<std::mutex> lk(in_mu);
        in_cv.wait(lk, [&] { return !in_lines.empty() || in_eof || quit_req.load(); });
        if (in_lines.empty()) return false;   // EOF or QUIT with nothing queued behind it
        out = std::move(in_lines.front());
        in_lines.pop_front();
        return true;
    };

    std::vector<int32_t> snap_tokens;   // the prompt prefix the model's saved state belongs to (conversation reuse)
    // images (GENI): this request's <|image|> positions and rows, and a hash of the rows the saved state read - the
    // same tokens with another picture must not reuse it
    std::vector<int64_t> cur_img_pos;
    std::vector<float> cur_img_rows;
    // the thinking budget (GEN keys think_budget=<n> think_end=<id>, the server's run config): a reasoning block still
    // open after n tokens is closed - the next token is </think> instead of the sampled one - and the answer follows
    // in the same decode
    int64_t req_think_budget = 0;
    int32_t req_think_end = -1;
    // the request's logit_bias (GEN key logit_bias=<id>:<bias>,...): added to its logits before every sample
    std::vector<std::pair<int32_t, float>> req_bias;
    uint64_t snap_img_hash = 1469598103934665603ull;
    const auto img_hash = [&](size_t len) {
        uint64_t h = 1469598103934665603ull;   // FNV-1a over the positions and rows inside [0, len)
        const size_t E = (size_t) model.geometry().n_embd;
        for (size_t k = 0; k < cur_img_pos.size() && (size_t) cur_img_pos[k] < len; ++k) {
            const unsigned char* b = (const unsigned char*) (cur_img_rows.data() + k * E);
            for (size_t j = 0; j < E * sizeof(float); ++j) h = (h ^ b[j]) * 1099511628211ull;
            h = (h ^ (uint64_t) cur_img_pos[k]) * 1099511628211ull;
        }
        return h;
    };
    // CONVERSATION SLOTS (the fast path): a request that does not continue the conversation in the model first sets
    // that conversation aside in a file (Glm5Model::slot_save: its snapshot and DSA caches), then takes back the slot
    // it does continue (slot_load) - chats, and an agent's side requests, that interleave no longer re-read each
    // other's prompts.  STRATA_GLM_SLOTS=<n> files (default 4, 0 off), for conversations of STRATA_GLM_SLOT_MIN tokens
    // or more (1024), in STRATA_GLM_SLOT_DIR (default <pack>/slots, emptied at start), at most STRATA_GLM_SLOT_GB on
    // disk (16) and never below 8 GB free.  Text only: a conversation with pictures is not set aside.  Or the slots
    // live in RAM and nothing reaches the disk: STRATA_GLM_SLOT_RAM_GB (0, off) caps their bytes there - the states
    // are host buffers that die with the run, so SLOT_DIR, SLOT_GB (1) and the disk-space guard do not apply.
    struct Slot {
        std::vector<int32_t> tokens;
        std::string path;                       // empty when the slot is a RAM buffer (slot_ram)
        uint64_t bytes = 0, used = 0;
        std::shared_ptr<std::vector<uint8_t>> data;   // the RAM slot's bytes (null: a file slot)
        // a paused request's prompt (PAUSE keep): the next turn of that chat continues it, not the written answer
        // (its history drops the reasoning), so its own continuations do not supersede it until it is taken back
        bool anchor = false;
        // the end of a system prompt (slot_prefix): other chats start with it, so its conversation's later states
        // never supersede it - it goes only as the least recently used (#179)
        bool prefix = false;
    };
    // (static: the multi path's Ctx, a local struct, uses it as a default member value - MSVC accepts that of a static
    // only, and a local constant left Ctx without a default constructor: "std::construct_at: no matching function")
    static constexpr uint64_t kNoImages = 1469598103934665603ull;   // img_hash() of a prefix without pictures
    std::vector<Slot> slots;
    uint64_t slot_clock = 0, slot_seq = 0;
    bool snap_in_slot = false;   // the model's snapshot is one of the slots, unchanged since
    const auto env_num = [](const char* k, double d) { const char* v = getenv(k); return v ? std::atof(v) : d; };
    int slot_max = model.fast() && getenv("STRATA_GLM_NO_REUSE") == nullptr ? (int) env_num("STRATA_GLM_SLOTS", 4) : 0;
    const size_t slot_min = (size_t) std::max(1.0, env_num("STRATA_GLM_SLOT_MIN", 1024));
    const uint64_t slot_budget = (uint64_t) (env_num("STRATA_GLM_SLOT_GB", 16) * 1073741824.0);
    // slots in RAM: the conversation's state is parked in a host buffer instead of a file.  The cap moves to RAM too
    // (slot_cap below); the states die with the run, so SLOT_KEEP, SLOT_DAYS, SLOT_DIR and the 8 GB-free disk guard
    // are theirs no longer.  Nothing on the disk is touched: in RAM mode the slots folder is neither used nor wiped.
    const bool slot_ram = env_num("STRATA_GLM_SLOT_RAM_GB", 0) > 0;
    const uint64_t slot_cap =
        slot_ram ? (uint64_t) (env_num("STRATA_GLM_SLOT_RAM_GB", 16) * 1073741824.0) : slot_budget;
    // STRATA_GLM_SLOT_KEEP=1: the slots outlive the engine - the folder is not emptied at start (its files are indexed
    // and kept when they belong to this model), the conversation in the model is set aside when the engine ends
    // (QUIT or end of input), and a file not used for STRATA_GLM_SLOT_DAYS days (7) is removed.  Every slot then has a
    // .meta beside it: this model's identity, the conversation's tokens and the slot's size; a slot whose .meta is
    // missing, foreign or does not match its file is deleted.  Both files are written under a temporary name first.
    const bool slot_keep = getenv("STRATA_GLM_SLOT_KEEP") != nullptr && std::atoi(getenv("STRATA_GLM_SLOT_KEEP")) != 0;
    const double slot_days = env_num("STRATA_GLM_SLOT_DAYS", 7);
    namespace fs = std::filesystem;
    std::filesystem::path slot_dir;
    // what a kept slot must match: the session format, the engine version, the pack and the model's shards (names and
    // sizes) and whether the draft block is loaded.  The split itself is checked by slot_load (each part's layers).
    const std::string slot_identity = [&] {
        std::ostringstream id;
        id << "maya-session-1 engine=" << STRATA_VERSION << " mtp=" << (model.has_mtp() ? 1 : 0);
        std::error_code ec;
        for (const char* f : {"index.txt", "dense.bin", "native_experts.txt"})
            id << " " << f << ":" << (unsigned long long) fs::file_size(fs::path(o.glm_pack) / f, ec);
        std::vector<std::string> shards;
        for (const auto& de : fs::directory_iterator(fs::path(o.glm_pack).parent_path(), ec))
            if (de.path().extension() == ".gguf")
                shards.push_back(de.path().filename().string() + ":" + std::to_string(de.file_size(ec)));
        std::sort(shards.begin(), shards.end());
        for (const auto& s : shards) id << " " << s;
        return id.str();
    }();
    constexpr uint64_t kMetaMagic = 0x3153534559414d47ull;   // "GMAYESS1"
    const auto meta_of = [](const std::string& bin) { return bin.substr(0, bin.size() - 4) + ".meta"; };
    const auto meta_write = [&](const std::string& path, const std::vector<int32_t>& toks, uint64_t bytes) -> bool {
        const std::string tmp = path + ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            const uint64_t il = slot_identity.size(), nt = toks.size();
            f.write((const char*) &kMetaMagic, 8);
            f.write((const char*) &il, 8);
            f.write(slot_identity.data(), (std::streamsize) il);
            f.write((const char*) &bytes, 8);
            f.write((const char*) &nt, 8);
            f.write((const char*) toks.data(), (std::streamsize) (nt * sizeof(int32_t)));
            f.write((const char*) &kMetaMagic, 8);
            f.flush();
            if (!f) return false;
        }
        std::error_code ec;
        fs::rename(tmp, path, ec);
        return !ec;
    };
    // a .meta of this model -> its tokens and the slot's size (false: foreign, damaged or not this model's)
    const auto meta_read = [&](const std::string& path, std::vector<int32_t>& toks, uint64_t& bytes) -> bool {
        std::ifstream f(path, std::ios::binary);
        uint64_t m0 = 0, il = 0, nt = 0, m1 = 0;
        if (!f.read((char*) &m0, 8) || m0 != kMetaMagic || !f.read((char*) &il, 8) || il != slot_identity.size())
            return false;
        std::string id(il, '\0');
        if (!f.read(id.data(), (std::streamsize) il) || id != slot_identity) return false;
        if (!f.read((char*) &bytes, 8) || !f.read((char*) &nt, 8) || nt == 0 || nt > (uint64_t) o.max_context) return false;
        toks.resize(nt);
        return (bool) f.read((char*) toks.data(), (std::streamsize) (nt * sizeof(int32_t))) && f.read((char*) &m1, 8) &&
               m1 == kMetaMagic;
    };
    const auto slot_age_days = [](const std::string& p) {
        std::error_code ec;
        const auto t = fs::last_write_time(p, ec);
        if (ec) return 1e9;
        return std::chrono::duration<double>(fs::file_time_type::clock::now() - t).count() / 86400.0;
    };
    // NOTE lines on stdout: the server shows them in its own log (what happens to the conversations)
    const auto note = [&](const std::string& m) {
        std::fprintf(stderr, "glm slots: %s\n", m.c_str());
        if (o.serve) std::printf("NOTE sessions: %s\n", m.c_str());
    };
    const auto gb = [](uint64_t b) {
        char s[32];
        std::snprintf(s, sizeof s, "%.2f GB", (double) b / 1e9);
        return std::string(s);
    };
    if (slot_max > 0 && slot_ram)
        note("slots parked in RAM (" + gb(slot_cap) + " max) - nothing is written to disk, the states die with the run");
    if (slot_max > 0 && !slot_ram) {
        slot_dir = getenv("STRATA_GLM_SLOT_DIR") ? std::filesystem::path(getenv("STRATA_GLM_SLOT_DIR"))
                                                 : std::filesystem::path(o.glm_pack) / "slots";
        std::error_code ec;
        std::filesystem::create_directories(slot_dir, ec);
        struct Found { std::string bin; std::vector<int32_t> toks; uint64_t bytes; fs::file_time_type t; };
        std::vector<Found> found;
        size_t refused = 0;
        for (const auto& de : std::filesystem::directory_iterator(slot_dir, ec)) {   // a previous run's slots
            const std::string fn = de.path().filename().string(), p = de.path().string();
            if (fn.rfind("slot-", 0) != 0) continue;
            const bool is_bin = fn.size() > 9 && fn.compare(fn.size() - 4, 4, ".bin") == 0;
            const bool is_meta = fn.size() > 10 && fn.compare(fn.size() - 5, 5, ".meta") == 0;
            if (!slot_keep || (!is_bin && !is_meta)) {   // not kept, or a .tmp an interrupted write left
                std::filesystem::remove(de.path(), ec);
                continue;
            }
            if (!is_meta) continue;                      // a .bin is judged by its .meta
            const std::string bin = p.substr(0, p.size() - 5) + ".bin";
            Found fd{bin, {}, 0, fs::last_write_time(de.path(), ec)};
            const uint64_t have = fs::exists(bin, ec) ? (uint64_t) fs::file_size(bin, ec) : 0;
            if (meta_read(p, fd.toks, fd.bytes) && have == fd.bytes && fd.bytes > 0 && slot_age_days(p) <= slot_days) {
                found.push_back(std::move(fd));
            } else {
                ++refused;
                std::filesystem::remove(p, ec);
                std::filesystem::remove(bin, ec);
            }
        }
        if (slot_keep)                                   // a .bin without its .meta was never completed
            for (const auto& de : std::filesystem::directory_iterator(slot_dir, ec)) {
                const std::string p = de.path().string(), fn = de.path().filename().string();
                if (fn.rfind("slot-", 0) == 0 && fn.size() > 9 && fn.compare(fn.size() - 4, 4, ".bin") == 0 &&
                    !fs::exists(meta_of(p), ec))
                    std::filesystem::remove(de.path(), ec);
            }
        std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.t < b.t; });
        uint64_t total = 0;
        for (auto& fd : found) {
            total += fd.bytes;
            slots.push_back({std::move(fd.toks), fd.bin, fd.bytes, ++slot_clock});
            // a system prompt's state (slot_prefix) goes only as the least recently used, after a restart too: the
            // .meta has no flag for it, its tokens tell (no user turn)
            slots.back().prefix = strata::core::is_system_prefix(slots.back().tokens, 154827);
        }
        if (slot_keep) {
            std::ostringstream m;
            m << "folder " << slot_dir.string() << ": " << slots.size() << " conversation(s) kept from earlier runs ("
              << gb(total) << ")";
            if (refused) m << ", " << refused << " not this model's or incomplete - removed";
            note(m.str());
        }
        if (!std::filesystem::is_directory(slot_dir, ec)) {
            std::fprintf(stderr, "glm slots: %s is not writable - conversations are not set aside\n",
                         slot_dir.string().c_str());
            slot_max = 0;
        }
    }
    const auto slot_drop = [&](size_t i) {
        if (slots[i].data)                       // a RAM slot: its buffer simply dies
            slots[i].data.reset();
        else {
            std::error_code ec;
            std::filesystem::remove(slots[i].path, ec);
            std::filesystem::remove(meta_of(slots[i].path), ec);
        }
        slots.erase(slots.begin() + (long) i);
    };
    const auto slot_lru = [&](size_t keep) {   // the least recently used slot other than `keep`
        size_t lru = SIZE_MAX;
        for (size_t i = 0; i < slots.size(); ++i)
            if (i != keep && (lru == SIZE_MAX || slots[i].used < slots[lru].used)) lru = i;
        return lru;
    };
    // the conversation in the model (its snapshot, snap_tokens) into a slot
    const auto slot_put = [&](const char* why) {
        if (slot_max <= 0 || snap_in_slot || snap_tokens.size() < slot_min || snap_img_hash != kNoImages) return;
        for (size_t i = slots.size(); i-- > 0;)   // an earlier state of this conversation is superseded
            if (!slots[i].anchor && !slots[i].prefix && slots[i].tokens.size() <= snap_tokens.size() &&
                std::equal(slots[i].tokens.begin(), slots[i].tokens.end(), snap_tokens.begin()))
                slot_drop(i);
        while (!slots.empty() && slots.size() >= (size_t) slot_max) slot_drop(slot_lru(SIZE_MAX));
        if (slot_ram) {   // the conversation is parked in a host buffer; nothing touches the disk
            const auto t0 = std::chrono::steady_clock::now();
            std::string e;
            auto buf = std::make_shared<std::vector<uint8_t>>();
            buf->reserve(1 << 20);
            const uint64_t bytes = model.slot_save(*buf, e);
            if (bytes == 0) {
                note("setting the conversation aside in RAM failed (" + (e.empty() ? std::string("it did not copy") : e) +
                     ") - it is not parked and will be read from token 0 when it comes back");
                return;
            }
            slots.push_back({snap_tokens, "", bytes, ++slot_clock});
            slots.back().data = buf;
            snap_in_slot = true;
            uint64_t total = 0;
            for (const auto& s : slots) total += s.bytes;
            size_t evicted = 0;
            while (slots.size() > 1 && total > slot_cap) {
                const size_t i = slot_lru(slots.size() - 1);
                total -= slots[i].bytes;
                slot_drop(i);
                ++evicted;
            }
            char m[288];
            std::snprintf(m, sizeof m, "saved a %zu-token conversation%s (%s in RAM in %.1f s); %zu kept, %s%s",
                          snap_tokens.size(), why, gb(bytes).c_str(),
                          std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), slots.size(),
                          gb(total).c_str(), evicted ? ", least recently used dropped to stay under the RAM cap" : "");
            note(m);
            return;
        }
        std::error_code ec;
        const auto space = std::filesystem::space(slot_dir, ec);
        if (!ec && space.available < (uint64_t) 8 << 30) {
            std::fprintf(stderr, "glm slots: under 8 GB free in %s - the conversation is not set aside\n",
                         slot_dir.string().c_str());
            return;
        }
        const auto t0 = std::chrono::steady_clock::now();
        const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch()).count();
        const std::string path = (slot_dir / ("slot-" + std::to_string(stamp) + "-" + std::to_string(++slot_seq) +
                                              ".bin")).string();
        std::string e;
        const uint64_t bytes = model.slot_save(path + ".tmp", e);
        if (bytes == 0 || (std::filesystem::rename(path + ".tmp", path, ec), ec) ||
            !meta_write(meta_of(path), snap_tokens, bytes)) {
            std::filesystem::remove(path + ".tmp", ec);
            std::filesystem::remove(path, ec);
            std::fprintf(stderr, "glm slots: setting the conversation aside failed (%s)\n",
                         e.empty() ? "writing its files" : e.c_str());
            return;
        }
        slots.push_back({snap_tokens, path, bytes, ++slot_clock});
        snap_in_slot = true;
        uint64_t total = 0;
        for (const auto& s : slots) total += s.bytes;
        size_t evicted = 0;
        while (slots.size() > 1 && total > slot_cap) {
            const size_t i = slot_lru(slots.size() - 1);
            total -= slots[i].bytes;
            slot_drop(i);
            ++evicted;
        }
        char m[256];
        std::snprintf(m, sizeof m, "saved a %zu-token conversation%s (%s in %.1f s); %zu kept, %s%s",
                      snap_tokens.size(), why, gb(bytes).c_str(),
                      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), slots.size(),
                      gb(total).c_str(), evicted ? ", least recently used removed to stay under the size cap" : "");
        note(m);
    };
    // the longest slot this prompt continues, back in the model: the tokens it covers (0: none)
    const auto slot_take = [&](const std::vector<int32_t>& prompt) -> size_t {
        if (slot_max <= 0 || !cur_img_pos.empty()) return 0;
        size_t best = SIZE_MAX, len = 0;
        for (size_t i = 0; i < slots.size(); ++i) {
            const auto& t = slots[i].tokens;
            if (t.size() > len && t.size() < prompt.size() && std::equal(t.begin(), t.end(), prompt.begin())) {
                best = i;
                len = t.size();
            }
        }
        if (best == SIZE_MAX) return 0;
        const auto t0 = std::chrono::steady_clock::now();
        std::string e;
        const bool back = slots[best].data
                              ? model.slot_load(*slots[best].data, (int64_t) len, e)
                              : model.slot_load(slots[best].path, (int64_t) len, e);
        if (!back) {
            note("taking a conversation back failed (" + e + ") - reading its prompt again");
            slot_drop(best);
            snap_tokens.clear();
            return 0;
        }
        snap_tokens = slots[best].tokens;
        snap_img_hash = kNoImages;
        snap_in_slot = true;
        slots[best].used = ++slot_clock;
        slots[best].anchor = false;
        if (slot_keep && !slots[best].data) {   // its age counts from its last use (a file slot's only)
            std::error_code ec;
            fs::last_write_time(meta_of(slots[best].path), fs::file_time_type::clock::now(), ec);
        }
        char m[160];
        std::snprintf(m, sizeof m, "took back a %zu-token conversation from %s (%.1f s); %zu new tokens to read", len,
                      slots[best].data ? "RAM" : "disk",
                      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(),
                      prompt.size() - len);
        note(m);
        return len;
    };
    // a request's DFlash window and draft cut (its GEN line's spec= / spec_min_p=; 0 / -1: as the engine started)
    int req_spec = 0;
    double req_spec_min_p = -1.0;
    int req_spec_n_min = -1;       // ... its drafts kept whatever their confidence (spec_n_min=; -1: the engine's)
    double req_spec_cum = -1.0;    // ... and the cut on the product of the confidences (spec_cum=; -1: the engine's)
    // THE PROMPT of a request (run_request, and the several-conversation scheduler's admission): the conversation
    // reuse (the snapshot, else a slot), the prompt path for all but the last token, the token path for the rest - the
    // state saved before the last token and the first sample's logits on the tail.  0, or 1 after an ERR line.
    // `tag` prefixes the PP / ERR lines ("<rid> " with several conversations at once).
    std::string tag;
    // CONVERSATION CHECKPOINTS (Glm5Model::ckpt_save): besides the snapshot (the last prompt but its last token), up to
    // STRATA_GLM_CKPTS (4; 0 = none) states of the conversation in the model at earlier positions, in host RAM (~150 MB
    // each with GLM-5.3-Flash): at the end of the system prompt (the first user turn's marker), at the start of the
    // prompt's last turn (its last user or tool-result marker) and every STRATA_GLM_CKPT_EVERY (16384) fresh prompt
    // tokens - only where at least 512 fresh tokens lie before.  A prompt that leaves the conversation before the
    // snapshot's position - an edited or replaced last message, another chat with the same system prompt and tools, a
    // client that rewrote an older turn - restores the longest checkpoint it starts with and reads only the rest,
    // where it read everything from token 0.  One conversation at a time (not the several-conversation scheduler) and
    // text prompts only; STRATA_GLM_NO_REUSE turns it off with the rest of the reuse.
    struct Ckpt {
        std::vector<int32_t> tokens;   // the conversation's first tokens the checkpoint holds the state after (empty: free)
        uint64_t used = 0;
    };
    std::vector<Ckpt> ckpts;
    uint64_t ckpt_clock = 0;
    const int ckpt_max = model.fast() && model.seq_count() <= 1 && getenv("STRATA_GLM_NO_REUSE") == nullptr
                             ? std::max(0, std::min(16, (int) env_num("STRATA_GLM_CKPTS", 4)))
                             : 0;
    const int64_t ckpt_every = std::max<int64_t>(0, (int64_t) env_num("STRATA_GLM_CKPT_EVERY", 16384));
    // a prompt writes the state from position `from` on: the checkpoints past it no longer hold this conversation
    const auto ckpt_drop_past = [&](size_t from) {
        for (auto& c : ckpts)
            if (c.tokens.size() > from) c.tokens.clear();
    };
    // the positions in [from + 512, end) a prompt read from `from` keeps checkpoints at (see above), ascending
    const auto ckpt_marks = [&](const std::vector<int32_t>& prompt, size_t from, size_t end) {
        constexpr int32_t kUserTok = 154827, kObsTok = 154829;   // <|user|>, <|observation|>
        std::vector<size_t> at;
        if (ckpt_max <= 0 || !cur_img_pos.empty() || end <= from + 512) return at;
        size_t first = 0, last = 0;
        for (size_t i = 1; i < end; ++i)
            if (prompt[i] == kUserTok || prompt[i] == kObsTok) {
                if (first == 0 && prompt[i] == kUserTok) first = i;
                last = i;
            }
        std::vector<size_t> turns;
        for (const size_t p : {first, last})
            if (p >= from + 512 && p < end) turns.push_back(p);
        at = turns;
        // the periodic ones, but none within 512 tokens of a turn boundary (that one holds the same state, more useful)
        if (ckpt_every > 0)
            for (size_t p = from + (size_t) ckpt_every; p < end; p += (size_t) ckpt_every) {
                bool by_turn = false;   // (not `near`: Windows' headers define it away)
                for (const size_t t : turns) by_turn = by_turn || (p + 512 > t && t + 512 > p);
                if (!by_turn) at.push_back(p);
            }
        std::sort(at.begin(), at.end());
        at.erase(std::unique(at.begin(), at.end()), at.end());
        // at most ckpt_max a prompt, the latest kept (the turn's start, then the ones before it)
        if ((int) at.size() > ckpt_max) at.erase(at.begin(), at.end() - ckpt_max);
        // every prompt-path call between them must reach its minimum (STRATA_GLM_PREFILL_MIN, 32: fewer go through the
        // token path, one token at a time): only the piece after the last one may be shorter
        std::vector<size_t> kept;
        for (const size_t p : at)
            if (p >= (kept.empty() ? from : kept.back()) + 64) kept.push_back(p);
        return kept;
    };
    // ... and with the disk slots on, the state at the end of the system prompt (p) also into a slot of its own: the
    // checkpoints live with the model's state, so an unrelated conversation in between drops them, and another chat
    // with this system prompt read it all again (#179); the slot outlives that.  Once per system prompt.
    const auto slot_prefix = [&](const std::vector<int32_t>& prompt, size_t p) {
        if (slot_max <= 0 || p < slot_min || !cur_img_pos.empty()) return;
        for (auto& sl : slots)
            if (sl.prefix && sl.tokens.size() == p && std::equal(sl.tokens.begin(), sl.tokens.end(), prompt.begin())) {
                sl.used = ++slot_clock;
                return;
            }
        if (!model.snapshot_save()) return;
        snap_tokens.assign(prompt.begin(), prompt.begin() + (long) p);
        snap_img_hash = img_hash(p);
        snap_in_slot = false;
        slot_put(" (its system prompt, for other chats that start with it)");
        if (snap_in_slot && !slots.empty() && slots.back().tokens.size() == p) slots.back().prefix = true;
    };
    // the state now (after prompt[0, p)) into a free or the least recently used checkpoint
    const auto ckpt_keep = [&](const std::vector<int32_t>& prompt, size_t p, bool from_snapshot = false) {
        if ((int) ckpts.size() < ckpt_max) ckpts.emplace_back();
        size_t c = 0;
        for (size_t i = 0; i < ckpts.size(); ++i) {
            if (ckpts[i].tokens.empty()) { c = i; break; }
            if (ckpts[i].used < ckpts[c].used) c = i;
        }
        std::string e;
        ckpts[c].tokens.clear();
        if (!(from_snapshot ? model.ckpt_save_snapshot((int) c, e) : model.ckpt_save((int) c, e))) {
            std::fprintf(stderr, "glm checkpoints: not kept at %zu (%s)\n", p, e.c_str());
            return;
        }
        ckpts[c].tokens.assign(prompt.begin(), prompt.begin() + (long) p);
        ckpts[c].used = ++ckpt_clock;
    };
    // A route that found a non-finite score (or an expert id out of range) means a NaN or an infinity in the state the
    // request ran on: its conversation must not be served again.  The kept snapshot and every slot this prompt
    // extends (one may have been the poisoned state's source) are dropped and the model is reset, so the server's
    // retry reads the prompt from token 0.  The ERR line names it "non-finite" (Glm5Model::route_ok's words).
    const auto drop_if_poisoned = [&](const std::vector<int32_t>& prompt, const std::string& why) {
        if (why.find("non-finite") == std::string::npos) return;
        snap_tokens.clear();
        snap_in_slot = false;
        ckpt_drop_past(0);
        for (size_t i = slots.size(); i-- > 0;) {
            const auto& t = slots[i].tokens;
            if (t.size() <= prompt.size() && std::equal(t.begin(), t.end(), prompt.begin())) slot_drop(i);
        }
        model.reset();
        std::fprintf(stderr, "glm nan guard: %s - this conversation's state is dropped; it is read again from token 0\n",
                     why.c_str());
    };
    // set by the several-conversation scheduler while it admits a request (STRATA_GLM_PREFILL_INTERLEAVE): the piece the
    // prompt path reads next (0: the rest of the segment, as before) and what runs between two pieces - 0 go on,
    // 1 stopped (the request's STOP), 2 failed (prompt_between_err says why; every request ended, the state reset)
    std::function<size_t()> prompt_piece;
    std::function<int()> prompt_between;
    std::string prompt_between_err;
    // STRATA_GLM_PREFILL_GAPS: true inside a gap (the decode steps at a chunk boundary of a prompt's read, or between
    // two of its calls) - no tier boundary runs there, nothing binds another context (a PAUSE's snapshot waits for the
    // read's end: deferred_pauses), and an engine error leaves the contexts' reset to gap_reset, after the read has
    // ended (the pool's tail is lent in it); gap_failed: a gap inside a prompt-path call failed (prompt_between_err
    // says why); gaps_over: a gap of this read overran <max gap ms>, so the rest of the read has none
    bool in_read_gap = false, gap_failed = false, read_gap_first = false, gaps_over = false;
    // a request PAUSEd inside a gap: its context stays busy (nothing else binds or reads into it) until its snapshot is
    // taken as it would have been at once - after the read's call, once its first token is taken (flush_pauses)
    struct DeferredPause {
        int ctx = 0;
        std::vector<int32_t> prompt, emitted;
        bool keep = false;
    };
    std::vector<DeferredPause> deferred_pauses;
    std::function<void()> gap_reset;
    std::chrono::steady_clock::time_point gap_mark;   // the read's start (after any restore), then each gap's end
    const auto prep_prompt = [&](const std::vector<int32_t>& prompt, size_t& reuse, const char*& finish,
                                 std::chrono::steady_clock::time_point& t0) -> int {
        // CONVERSATION REUSE (the fast path): the previous prompt's state, saved just before its last token, is
        // restored when this prompt extends it - a chat's next turn reads only what is new
        strata::core::progress_at("the request: restoring a conversation");
        const bool snap_ok = model.fast() && !snap_tokens.empty() && snap_tokens.size() < prompt.size() &&
                             std::equal(snap_tokens.begin(), snap_tokens.end(), prompt.begin()) &&
                             img_hash(snap_tokens.size()) == snap_img_hash && getenv("STRATA_GLM_NO_REUSE") == nullptr;
        // the longest checkpoint this prompt starts with (it holds less than the snapshot when both match)
        int ck = -1;
        if (ckpt_max > 0 && cur_img_pos.empty())
            for (int c = 0; c < (int) ckpts.size(); ++c) {
                const auto& t = ckpts[(size_t) c].tokens;
                if (!t.empty() && t.size() < prompt.size() && (ck < 0 || t.size() > ckpts[(size_t) ck].tokens.size()) &&
                    std::equal(t.begin(), t.end(), prompt.begin()))
                    ck = c;
            }
        const size_t ck_len = ck >= 0 ? ckpts[(size_t) ck].tokens.size() : 0;
        bool from_ckpt = false;
        if (snap_ok && snap_tokens.size() >= ck_len && model.snapshot_restore()) reuse = snap_tokens.size();
        if (reuse == 0 && ck >= 0) {
            slot_put("");              // the conversation in the model goes on differently past the checkpoint: kept
            size_t slot_len = 0;       // a slot that holds more of this prompt is taken instead (below)
            if (slot_max > 0)
                for (const auto& sl : slots)
                    if (sl.tokens.size() > slot_len && sl.tokens.size() < prompt.size() &&
                        std::equal(sl.tokens.begin(), sl.tokens.end(), prompt.begin()))
                        slot_len = sl.tokens.size();
            std::string ce;
            if (slot_len > ck_len) {
            } else if (model.ckpt_restore(ck, ce)) {
                reuse = ck_len;
                from_ckpt = true;
                ckpts[(size_t) ck].used = ++ckpt_clock;
                snap_tokens.clear();   // the snapshot's position lies past the checkpoint: overwritten from here on
                std::fprintf(stderr, "glm checkpoints: %zu of the prompt's %zu tokens from a checkpoint\n", reuse,
                             prompt.size());
            } else {
                std::fprintf(stderr, "glm checkpoints: %s\n", ce.c_str());
                ckpts[(size_t) ck].tokens.clear();
            }
        }
        // THE TURN'S START: a prompt that goes on from the snapshot and opens its last turn (<|user|> / <|observation|>)
        // within 512 tokens of it gets no checkpoint there from ckpt_marks - so an edit of that turn's message read the
        // whole conversation again (13.5K tokens: 20.8 s).  The snapshot itself (the previous prompt but its last
        // token, a few tokens before the turn) is kept as a checkpoint instead: copied to host RAM on a side thread
        // while this prompt runs, so it costs the prompt nothing.  STRATA_GLM_CKPT_TURN=0: off.
        if (reuse > 0 && !from_ckpt && reuse == snap_tokens.size() && reuse >= 512 && ckpt_max > 0 &&
            cur_img_pos.empty()) {
            static const bool turn_on = [] { const char* v = getenv("STRATA_GLM_CKPT_TURN"); return !v || atoi(v) != 0; }();
            constexpr int32_t kUserTok = 154827, kObsTok = 154829;   // <|user|>, <|observation|>
            size_t last = 0;
            for (size_t i = prompt.size() - 1; i-- > reuse;)
                if (prompt[i] == kUserTok || prompt[i] == kObsTok) { last = i; break; }
            bool have = false;
            for (const auto& c : ckpts)
                have = have || (c.tokens.size() == reuse && std::equal(c.tokens.begin(), c.tokens.end(), prompt.begin()));
            if (turn_on && last >= reuse && last < reuse + 512 && !have) ckpt_keep(prompt, reuse, true);
        }
        if (reuse == 0 && slot_max > 0) {
            slot_put("");              // the conversation in the model, before this prompt overwrites it
            reuse = slot_take(prompt);
            if (reuse > 0) ckpt_drop_past(0);   // another conversation's state now
        } else if (reuse > 0 && !from_ckpt && slot_max > 0 && getenv("STRATA_GLM_SLOT_ROUNDTRIP") != nullptr) {
            // a test: the restored state goes through a slot file and back - the same tokens must follow
            snap_in_slot = false;
            slot_put("");
            const size_t back = slot_take(prompt);
            if (back != reuse) {
                std::fprintf(stderr, "glm slots: ROUNDTRIP took back %zu of %zu tokens\n", back, reuse);
                reuse = back;
            }
        }
        if (reuse == 0) model.reset();
        ckpt_drop_past(reuse);   // this prompt writes the state from `reuse` on (all of it after a reset)
        std::vector<float> lg;
        t0 = std::chrono::steady_clock::now();
        gap_mark = t0;   // (STRATA_GLM_PREFILL_GAPS: the first gap's stage starts with the read, not the restore)
        // the prompt one CHUNK at a time (PP reports per chunk: the server shows it and counts it as a heartbeat);
        // the state is saved before the LAST prompt token, so an identical or extending prompt can reuse it
        bool snapped = false;
        // the batched prompt path (glm_prefill.cu) reads all but the last prompt token in chunks, layer by layer;
        // the last one goes through the token path, which leaves the logits the first sample reads
        const int pchunk = model.fast() ? model.prefill_chunk() : 0;
        size_t i_first = reuse;
        if (pchunk > 0 && prompt.size() - reuse > 1) {
            // one call for everything but the last token: the two halves of a split pipeline the chunks; PP lines
            // (the server's progress and heartbeat) come from the progress callback, possibly on the tail's thread.
            // A checkpoint position (ckpt_marks) ends a call: the state there is kept, and the next call goes on.
            std::mutex pp_mu;
            const auto pp_line = [&](size_t done) {
                if (!o.serve) return;
                std::lock_guard<std::mutex> lk(pp_mu);
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                std::printf("PP %s%zu %zu %.1f %.1f %zu\n", tag.c_str(), reuse + done, prompt.size(), ms,
                            (double) done / (ms / 1000.0 + 1e-9), reuse);
                std::fflush(stdout);
            };
            std::vector<size_t> marks = ckpt_marks(prompt, reuse, prompt.size() - 1);
            // the first user turn's marker: the end of the system prompt (ckpt_marks' `first`)
            const size_t sys_end = strata::core::system_prompt_end(prompt.data(), prompt.size(), 154827);   // <|user|>
            // without checkpoints (STRATA_GLM_CKPTS=0, the several-conversation scheduler) the read still ends a call
            // there with the slots on, for slot_prefix: another conversation with this system prompt (a new conversation
            // of the same client, another request of the same kind) restores it instead of reading it again.
            // STRATA_GLM_PREFIX_CUT=0: off
            static const bool prefix_cut_on = env_num("STRATA_GLM_PREFIX_CUT", 1) != 0;
            if (ckpt_max <= 0 && slot_max > 0 && prefix_cut_on && cur_img_pos.empty())
                strata::core::add_mark(marks, strata::core::prefix_cut(prompt.data(), prompt.size(), reuse, slot_min,
                                                                        154827), reuse);
            size_t at = reuse;   // the next token the prompt path reads
            bool pok = true, between_failed = false;
            // one call to the next checkpoint mark (or the prompt's last token) - with STRATA_GLM_PREFILL_INTERLEAVE and
            // requests decoding, a piece of it, and their decode steps between two pieces (prompt_between)
            size_t mi = 0, seg_start = at;
            // STRATA_GLM_PREFILL_GAPS: a gap before the read too (a round for the running requests) - back-to-back reads
            // would otherwise stall them for the last stage of one and the first of the next
            if (read_gap_first && prompt_between && at < prompt.size() - 1) {
                const int r = prompt_between();
                if (r == 1) {
                    pok = false;
                    err = "cancelled";
                } else if (r == 2) {
                    pok = false;
                    between_failed = true;
                }
            }
            int seg_chunk = 0;   // (exact pieces: the chunk the segment's uncut read takes, from its first piece on)
            while (pok && at < prompt.size() - 1) {
                const size_t seg_end = mi < marks.size() ? marks[mi] : prompt.size() - 1;
                size_t piece = prompt_piece ? prompt_piece() : 0;
                if (piece > 0 && interleave.exact) {
                    // whole chunks of the uncut read, each call cut in them: the same arithmetic, the same answer
                    if (at == seg_start || seg_chunk == 0) seg_chunk = model.prefill_chunk_for((int64_t) (seg_end - seg_start));
                    piece = strata::core::prefill_aligned_piece(piece, (size_t) seg_chunk);
                }
                const size_t end = strata::core::prefill_piece_end(at, seg_end, piece);
                // (an interleaved piece cut freely (,cut): both GPUs of a split at work - two chunks, or ~4096-token ones
                // for a larger piece: a call fills and drains the split's pipeline, a GPU idle for a chunk each way)
                model.prefill_min_chunks = piece > 0 && !interleave.exact ? std::max<int>(2, (int) (piece / 4096)) : 1;
                model.prefill_force_chunk = seg_chunk > 0 && interleave.exact ? seg_chunk : 0;
                const std::vector<int32_t> part(prompt.begin() + (long) at, prompt.begin() + (long) end);
                const size_t base = at - reuse;
                model.prefill_progress = [&](int64_t done, int64_t total) {
                    (void) total;
                    pp_line(base + (size_t) done);
                    return !read_stopped();
                };
                pok = model.prefill(part, err, prompt[end]);   // (the NextN block's caches read the next token)
                model.prefill_progress = nullptr;
                model.prefill_min_chunks = 1;
                model.prefill_force_chunk = 0;
                if (!pok && gap_failed) between_failed = true;   // (a decode gap inside the call failed)
                if (pok) {
                    at = end;
                    if (end == seg_end && mi < marks.size()) {
                        if (ckpt_max > 0) ckpt_keep(prompt, end);   // (no checkpoints: the mark is a prefix cut's only)
                        if (end == sys_end) slot_prefix(prompt, end);
                        ++mi;
                        seg_start = end;
                        seg_chunk = 0;
                    }
                    // more to read: the decoding requests' turn (none after the last piece - its last token's logits
                    // must still be on the tail when the first sample reads them)
                    if (at < prompt.size() - 1 && prompt_between) {
                        pp_line(at - reuse);
                        const int r = prompt_between();
                        if (r == 1) {
                            pok = false;
                            err = "cancelled";
                        } else if (r == 2) {
                            pok = false;
                            between_failed = true;
                        }
                    }
                }
            }
            if (between_failed) {   // every context was reset (multi_fail): nothing of this prompt is left to keep
                drop_if_poisoned(prompt, prompt_between_err);   // (a non-finite error: its slots too, as below)
                std::printf("ERR %s%s\n", tag.c_str(), prompt_between_err.c_str());
                if (gap_reset) gap_reset();   // (a gap's failure: the reset multi_fail left for after the read)
                return 1;
            }
            // a call the prompt path did not take (fewer tokens than STRATA_GLM_PREFILL_MIN: false, no error) leaves the
            // rest to the token path below - from where the prompt path stopped, not from `reuse`
            i_first = at;
            if (pok) {
                i_first = prompt.size() - 1;
                pp_line(prompt.size() - 1 - reuse);
            } else if (err == "cancelled") {
                snap_tokens.clear();
                model.reset();
                ckpt_drop_past(0);
                finish = "cancel";
                i_first = prompt.size();
            } else if (!err.empty()) {
                snap_tokens.clear();
                drop_if_poisoned(prompt, err);
                std::printf("ERR %s%s\n", tag.c_str(), err.c_str());
                return 1;
            }
        }
        for (size_t i = i_first; i < prompt.size();) {
            strata::core::progress_at("the prompt: the token path, at token", (int64_t) i);
            strata::core::progress_beat();
            if (model.fast() && !snapped && i == prompt.size() - 1) {
                if (model.snapshot_save()) {
                    snap_tokens.assign(prompt.begin(), prompt.end() - 1);
                    snap_img_hash = img_hash(snap_tokens.size());
                    snap_in_slot = false;
                } else {
                    snap_tokens.clear();
                }
                snapped = true;
            }
            size_t chunk = std::min<size_t>(8, prompt.size() - i);
            if (model.fast() && !snapped && i + chunk > prompt.size() - 1) chunk = prompt.size() - 1 - i;
            std::vector<int32_t> part(prompt.begin() + (long) i, prompt.begin() + (long) (i + chunk));
            const bool ok = model.forward(part, lg, err);
            if (!ok) {
                snap_tokens.clear();
                drop_if_poisoned(prompt, err);
                std::printf("ERR %s%s\n", tag.c_str(), err.c_str());
                return 1;
            }
            i += chunk;
            if (o.serve) {
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                std::printf("PP %s%zu %zu %.1f %.1f %zu\n", tag.c_str(), i, prompt.size(), ms,
                            (double) (i - reuse) / (ms / 1000.0 + 1e-9), reuse);
            }
            if (read_stopped()) { finish = "cancel"; break; }
        }
        return 0;
    };
    // PAUSE: the snapshot moves from the prompt to the last token fed (the decode feeds every written token but
    // the last), so prompt + what was written continues it with one token to read instead of all it wrote.
    // Text only, and only when the state stands exactly there (the decode paths leave pos = fed tokens).
    const auto pause_snapshot = [&](const std::vector<int32_t>& prompt, const std::vector<int32_t>& emitted,
                                    bool keep) {
        if (model.fast() && cur_img_pos.empty() && !emitted.empty() && getenv("STRATA_GLM_NO_REUSE") == nullptr) {
            const int64_t p = model.position();
            const int64_t full = (int64_t) (prompt.size() + emitted.size());
            if (keep && slot_max > 0 && p > (int64_t) prompt.size() && p < full) {
                // the request's first slice: its prompt's state goes to a slot (unless one holds it already) and is
                // anchored there, for this chat's next turn
                bool held = false;
                for (auto& sl : slots)
                    if (sl.tokens == snap_tokens) sl.anchor = held = true;
                if (!held && !snap_tokens.empty()) {
                    slot_put(" (a paused request's prompt, kept for its next turn)");
                    for (auto& sl : slots)
                        if (sl.tokens == snap_tokens) sl.anchor = true;
                }
            }
            if (p > (int64_t) prompt.size() && p < full && model.snapshot_save()) {
                snap_tokens.assign(prompt.begin(), prompt.end());
                snap_tokens.insert(snap_tokens.end(), emitted.begin(), emitted.begin() + (p - (int64_t) prompt.size()));
                snap_img_hash = img_hash(snap_tokens.size());
                snap_in_slot = false;
                std::fprintf(stderr, "glm slots: paused at %lld tokens (%lld written): the state is kept there\n",
                             (long long) p, (long long) emitted.size());
            } else if (p > (int64_t) prompt.size()) {
                std::fprintf(stderr, "glm slots: paused, but the state stands at %lld of %lld tokens: not kept\n",
                             (long long) p, (long long) full);
            }
        }
    };
    // STRATA_GLM_SPEC_FOLLOW=<file> (--spec-follow): every request decodes this continuation instead of its own picks
    // and ends with it (Glm5Model::set_follow) - a speed A/B on one text.  STRATA_GLM_SPEC_FOLLOW_SAVE=<file>
    // (--spec-follow-save): each request's decoded ids are written there, the next run's continuation.  A text made by
    // another model, or by the same model with other settings, favours the drafts that made it: compare a change on
    // the same model and its own text.
    std::vector<int32_t> follow_ids;
    const char* follow_save = getenv("STRATA_GLM_SPEC_FOLLOW_SAVE");
    if (follow_save != nullptr && follow_save[0] == '\0') follow_save = nullptr;
    if (const char* fp = getenv("STRATA_GLM_SPEC_FOLLOW"); fp != nullptr && fp[0] != '\0') {
        std::ifstream input(fp);
        std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        std::vector<int64_t> v;
        std::string e;
        if (!input && !input.eof()) e = "cannot read it";
        else if (!parse_i64_list(text.c_str(), v, e)) e = "not a list of token ids: " + e;
        for (const int64_t t : v)
            if (e.empty() && t >= n_vocab) e = "token " + std::to_string(t) + " is past the vocabulary";
        if (!e.empty()) {
            std::fprintf(stderr, "maya generate: MAYA_GLM_SPEC_FOLLOW %s: %s\n", fp, e.c_str());
            return 2;
        }
        follow_ids.assign(v.begin(), v.end());
        std::fprintf(stderr, "glm follow: every request decodes the %zu tokens of %s (MAYA_GLM_SPEC_FOLLOW)\n",
                     follow_ids.size(), fp);
    }
    const auto run_request = [&](const std::vector<int64_t>& ids, int64_t max_new,
                                 const strata::kernels::SamplerParams& rq_in) -> int {
        strata::kernels::SamplerParams sp = rq_in;   // per request (the server sends sampling keys per GEN)
        stop_req.store(false);
        pause_req.store(false);
        pause_keep.store(false);
        model.force_next(-1);
        model.set_follow(follow_ids);   // STRATA_GLM_SPEC_FOLLOW: this request's tokens (empty: its own picks)
        if (!follow_ids.empty()) max_new = std::min<int64_t>(max_new, (int64_t) follow_ids.size());
        bool in_think = req_think_budget > 0 && req_think_end >= 0;
        std::vector<int32_t> prompt;
        prompt.reserve(ids.size());
        for (const int64_t t : ids) {
            if (t < 0 || t >= n_vocab) {
                std::printf("ERR token %lld out of range [0, %lld)\n", (long long) t, (long long) n_vocab);
                return 1;
            }
            prompt.push_back((int32_t) t);
        }
        if (prompt.empty() || max_new <= 0) {
            std::printf("ERR empty prompt or max_new\n");
            return 1;
        }
        // the penalties' history (#273): what the state is fed - the prompt, then every token written (on_token)
        std::vector<int32_t> pen_hist;
        if (sp.penalty_last_n > 0) {
            pen_hist = prompt;
            sp.host_history = &pen_hist;
        }
        size_t reuse = 0;
        const char* finish = "length";
        auto t0 = std::chrono::steady_clock::now();
        if (prep_prompt(prompt, reuse, finish, t0) != 0) return 1;
        std::vector<float> lg;
        if (stop_req.load()) finish = "cancel";
        const auto t1 = std::chrono::steady_clock::now();
        const auto st1 = model.fast_stats();
        int64_t produced = 0;
        // one emitted token: the T line, the live tier counters every 8 tokens, the stop conditions (false: stop)
        // LOOP GUARD: a heavily quantized model can fall into repeating a short pattern ("0 0 0 ...") until the
        // thinking budget or max_tokens ends it - minutes of nothing.  When the last W tokens are one pattern of at most
        // 32 tokens repeated exactly, thinking is closed at once (as the budget does); in the answer, what the model is in the
        // middle of is closed (a tool call's argument, else the turn), and a second loop ends it.
        // W: STRATA_GLM_LOOP_THINK (256) while thinking, STRATA_GLM_LOOP_ANSWER (1024) in the answer - long enough
        // for legitimately repetitive output (a zero-filled array); 0 turns that check off.
        static const int loop_w_think = (int) env_num("STRATA_GLM_LOOP_THINK", 256);
        static const int loop_w_answer = (int) env_num("STRATA_GLM_LOOP_ANSWER", 1024);
        std::vector<int> gen_hist;
        std::vector<int32_t> emitted;   // every token this request wrote (a PAUSE keeps the state they lead to)
        bool in_arg = false;     // inside a tool call's <arg_value> (the answer's position, for the loop guard)
        int guard_fired = 0;     // soft landings in this answer: a second loop ends it
        const auto looping = [&](int w) -> int {   // the pattern's period, 0 when the tail is not one
            const int n = (int) gen_hist.size();
            if (w <= 0 || n < w) return 0;
            for (int per = 1; per <= 32 && per < w / 4; ++per) {
                bool rep = true;
                for (int i = n - w + per; i < n && rep; ++i) rep = gen_hist[(size_t) i] == gen_hist[(size_t) (i - per)];
                if (rep) return per;
            }
            return 0;
        };
        const auto on_token = [&](int tok) -> bool {
            std::printf("T %d\n", tok);
            ++produced;
            emitted.push_back((int32_t) tok);
            if (sp.host_history != nullptr) pen_hist.push_back((int32_t) tok);   // (fed next: the picks after count it)
            strata::core::progress_at("decode: after token", produced);   // the watchdog's heartbeat (issue #40)
            strata::core::progress_beat();
            gen_hist.push_back(tok);
            if (in_think && tok != req_think_end) {
                if (const int per = looping(loop_w_think)) {
                    model.force_next(req_think_end);   // as the budget does: the next token closes the reasoning
                    in_think = false;
                    char m[200];
                    std::snprintf(m, sizeof m, "thinking was repeating a %d-token pattern (%d tokens in): closed, "
                                  "answering", per, (int) produced);
                    std::fprintf(stderr, "glm loop guard: %s\n", m);
                    if (o.serve) std::printf("NOTE %s\n", m);
                    gen_hist.clear();
                }
            } else if (!in_think) {
                // where the answer stands: inside a tool call's argument value, the place a loop must be closed for
                // an agent to go on (GLM's single-token tags)
                constexpr int kArgValue = 154849, kArgValueEnd = 154850, kUser = 154827;
                if (tok == kArgValue) in_arg = true;
                if (tok == kArgValueEnd) in_arg = false;
                if (const int per = looping(loop_w_answer)) {
                    char m[240];
                    if (guard_fired == 0) {
                        // a soft landing: close what the model is in the middle of instead of cutting the response -
                        // a tool call's argument ends (the call completes with what was written, the agent runs it
                        // and sees the result), plain text ends the turn (finish "stop")
                        model.force_next(in_arg ? kArgValueEnd : kUser);
                        std::snprintf(m, sizeof m, "the answer was repeating a %d-token pattern (%d tokens in): %s",
                                      per, (int) produced, in_arg ? "the tool call's argument was closed there"
                                                                  : "the turn was ended there");
                    } else {
                        std::snprintf(m, sizeof m, "the answer kept repeating a %d-token pattern (%d tokens in): "
                                      "ended", per, (int) produced);
                    }
                    std::fprintf(stderr, "glm loop guard: %s\n", m);
                    if (o.serve) std::printf("NOTE %s\n", m);
                    gen_hist.clear();
                    if (guard_fired++ > 0) {   // it did not stop: the hard way
                        finish = "length";
                        return false;
                    }
                }
            }
            if (in_think && (tok == req_think_end || produced >= req_think_budget || wrap_req.load())) {
                if (tok != req_think_end) model.force_next(req_think_end);   // the budget: the next token closes it
                in_think = false;
                gen_hist.clear();
            }
            // live expert-tier counters for the server's Monitor (serve/server.py parses STAT lines), every 8 tokens
            if (o.serve && model.fast() && produced % 8 == 0) {
                const auto sn = model.fast_stats();
                const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
                const double n = (double) produced;
                const uint64_t h = sn.hits - st1.hits, m = sn.misses - st1.misses;
                std::printf("STAT tok_s=%.2f ms_tok=%.2f vram_hit=%.4f ram_fetch=%.3f disk=%.3f promo=%.3f "
                            "vram_used=%lld vram_slots=%lld ram_used=%lld ram_slots=%lld ram_gb=%.1f vram_gb=%.1f\n",
                            n / std::max(1e-9, el), 1000.0 * el / n, (double) h / (double) std::max<uint64_t>(1, h + m),
                            (double) (sn.ram_hits - st1.ram_hits) / n, (double) (sn.disk_reads - st1.disk_reads) / n,
                            (double) (sn.promotions - st1.promotions) / n, (long long) sn.pool_used,
                            (long long) sn.pool_slots, (long long) sn.ram_used, (long long) sn.ram_slots, sn.ram_gb,
                            sn.pool_gb);
            }
            bool eos_hit = false;
            for (const int64_t e : eos)
                if (e == tok) eos_hit = true;
            if (eos_hit && stop_on_eos) {
                finish = "stop";
                return false;
            }
            if (stop_req.load()) {
                finish = "cancel";
                return false;
            }
            return produced < max_new;
        };
        const uint64_t spec0 = model.spec_steps_, hits0 = model.spec_hits_;
        if (std::strcmp(finish, "cancel") != 0 && model.fast() && model.dflash_ready() && req_spec != 1) {
            // the DFlash decode: a block of drafts a round, one verify window checks them (same tokens as the loop
            // below for the same samples); --spec T caps a round's window at T tokens
            int64_t n_spec = 0;
            const int spec = req_spec >= 2 ? req_spec : o.spec;   // this request's window, else --spec
            const int n_max = spec >= 2 ? spec - 1 : -1;            // -1: the engine's (decode_dflash)
            // the drafts stop at a selector confidence below the cut: the request's, else --spec-min-p, else the
            // engine's (-1: decode_dflash picks it for the window it verifies with)
            const float p_min = req_spec_min_p >= 0.0 && req_spec_min_p <= 1.0 ? (float) req_spec_min_p
                                : o.spec_min_p_set                         ? (float) o.spec_min_p
                                                                           : -1.0f;
            if (!model.decode_dflash(sp, max_new, n_max, p_min, on_token, n_spec, err, req_spec_n_min,
                                     req_spec_cum >= 0.0 && req_spec_cum <= 1.0 ? (float) req_spec_cum : -1.0f)) {
                drop_if_poisoned(prompt, err);
                std::printf("ERR %s\n", err.c_str());
                return 1;
            }
        } else if (std::strcmp(finish, "cancel") != 0 && model.fast() && model.mtp_ready() && req_spec != 1) {
            // the one-GPU MTP decode (glm_mtp.cu; by default on the Strix / Gorgon Halo APUs): the NextN block drafts,
            // one verify window checks the drafts (same tokens as the loop below for the same samples)
            int64_t n_spec = 0;
            const uint64_t rounds0 = model.mtp_rounds_;
            uint64_t len0[9];
            std::copy(model.mtp_round_n_, model.mtp_round_n_ + 9, len0);
            if (!model.decode_mtp(sp, max_new, on_token, n_spec, err)) {
                drop_if_poisoned(prompt, err);
                std::printf("ERR %s\n", err.c_str());
                return 1;
            }
            if (model.mtp_rounds_ > rounds0) {
                std::string lens;
                for (int i = 0; i < 9; ++i)
                    if (model.mtp_round_n_[i] > len0[i])
                        lens += " " + std::to_string(i) + ":" + std::to_string(model.mtp_round_n_[i] - len0[i]);
                std::fprintf(stderr, "glm mtp: %llu rounds, %.2f tokens a round | rounds by draft length%s\n",
                             (unsigned long long) (model.mtp_rounds_ - rounds0),
                             (double) n_spec / (double) (model.mtp_rounds_ - rounds0), lens.c_str());
            }
        } else if (std::strcmp(finish, "cancel") != 0 && model.fast() && model.spec_ready()) {
            // the pipelined speculative decode: the two halves of the split work on consecutive tokens, the head
            // running the NextN block's draft (same tokens as the loop below)
            int64_t n_spec = 0;
            if (!model.decode_spec(sp, max_new, on_token, n_spec, err)) {
                drop_if_poisoned(prompt, err);
                std::printf("ERR %s\n", err.c_str());
                return 1;
            }
        } else {
            for (;;) {
                if (stop_req.load()) {
                    finish = "cancel";
                    break;
                }
                int tok = model.sample_token(sp, err);
                if (tok < 0) {
                    drop_if_poisoned(prompt, err);
                    std::printf("ERR %s\n", err.c_str());
                    return 1;
                }
                tok = model.forced(tok);
                sp.counter += 1;
                if (!on_token(tok)) break;
                if (!model.forward({(int32_t) tok}, lg, err)) {
                    drop_if_poisoned(prompt, err);
                    std::printf("ERR %s\n", err.c_str());
                    return 1;
                }
            }
        }
        const auto t2 = std::chrono::steady_clock::now();
        const double prompt_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        const double decode_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
        // the decode phase's expert-tier counters (the fast path; zeros on the reference path)
        const auto st2 = model.fast_stats();
        const uint64_t d_hits = st2.hits - st1.hits, d_miss = st2.misses - st1.misses;
        std::printf("DONE %lld %lld %.1f %.1f %s %llu %llu %zu %llu %llu\n", (long long) produced,
                    (long long) prompt.size(), prompt_ms, decode_ms, finish,
                    (unsigned long long) (model.spec_hits_ - hits0), (unsigned long long) (model.spec_steps_ - spec0),
                    reuse, (unsigned long long) d_hits, (unsigned long long) (d_hits + d_miss));
        std::fflush(stdout);
        if (pause_req.load() && std::strcmp(finish, "cancel") == 0) pause_snapshot(prompt, emitted, pause_keep.load());
        // this machine's expert usage, for the next start's warm-up (after DONE: the client is not kept waiting)
        if (model.fast()) model.save_usage();
        if (model.fast() && produced > 0) {
            const double n = (double) produced;
            std::fprintf(stderr,
                         "glm stat: decode %.2f ms/tok (%.2f tok/s) | vram hit %.2f%% | ram fetches %.2f/tok | "
                         "disk %.2f reads/tok in %.2f waits/tok, %.2f ms/tok | promotions %.2f/tok | cpu lane %.2f/tok "
                         "in %.2f ms/tok | prompt %.1f ms/tok | vram %lld/%lld ram %lld/%lld\n",
                         decode_ms / n, 1000.0 * n / decode_ms,
                         100.0 * (double) d_hits / (double) std::max<uint64_t>(1, d_hits + d_miss),
                         (double) (st2.ram_hits - st1.ram_hits) / n, (double) (st2.disk_reads - st1.disk_reads) / n,
                         (double) (st2.miss_layers - st1.miss_layers) / n, (st2.disk_ms - st1.disk_ms) / n,
                         (double) (st2.promotions - st1.promotions) / n,
                         (double) (st2.cpu_experts - st1.cpu_experts) / n, (st2.cpu_ms - st1.cpu_ms) / n,
                         prompt_ms / (double) std::max<size_t>(1, prompt.size()), (long long) st2.pool_used,
                         (long long) st2.pool_slots, (long long) st2.ram_used, (long long) st2.ram_slots);
            if (st2.disk_skips > st1.disk_skips)
                std::fprintf(stderr, "glm skip: %.2f disk-only experts/tok left out (MAYA_GLM_DISK_SKIP_W / _FROM)\n",
                             (double) (st2.disk_skips - st1.disk_skips) / n);
            if (model.spec_steps_ > spec0)
                std::fprintf(stderr, "glm spec: %llu of %llu drafts accepted (%.1f%%)\n",
                             (unsigned long long) (model.spec_hits_ - hits0),
                             (unsigned long long) (model.spec_steps_ - spec0),
                             100.0 * (double) (model.spec_hits_ - hits0) / (double) (model.spec_steps_ - spec0));
            if (!follow_ids.empty())
                std::fprintf(stderr, "glm follow: %zu tokens of the continuation decoded, %zu of them differ from the "
                                     "model's own pick\n", model.follow_used(), model.follow_differ());
            if (model.kv_resident() > 0) {
                // KV streaming, cumulative over the process: the blocks the decode's selections named, and those
                // read from RAM (every part summed)
                const auto kv = model.kv_stream_stats();
                std::fprintf(stderr, "glm kv: KV streaming: %.2f%% of %llu block reads hit VRAM, %.1f MiB read from RAM%s\n",
                             kv.lookups ? 100.0 * (double) (kv.lookups - kv.misses) / (double) kv.lookups : 100.0,
                             (unsigned long long) kv.lookups,
                             (double) kv.misses * kv.block_bytes / 1048576.0,
                             kv.overflow ? " - OVERFLOW (too few resident positions)" : "");
            }
        }
        model.set_follow({});
        // STRATA_GLM_SPEC_FOLLOW_SAVE: this request's decoded ids, the continuation another run can follow
        if (follow_save != nullptr && !emitted.empty()) {
            const std::string tmp = std::string(follow_save) + ".tmp";
            {
                std::ofstream f(tmp, std::ios::trunc);
                for (size_t i = 0; i < emitted.size(); ++i) f << (i ? "," : "") << emitted[i];
                f << "\n";
            }
            std::error_code ec;
            std::filesystem::rename(tmp, follow_save, ec);
            if (ec)
                std::fprintf(stderr, "glm follow: could not write %s: %s\n", follow_save, ec.message().c_str());
            else
                std::fprintf(stderr, "glm follow: %zu decoded ids written to %s (MAYA_GLM_SPEC_FOLLOW_SAVE)\n",
                             emitted.size(), follow_save);
        }
        return 0;
    };

    // ---- SEVERAL CONVERSATIONS AT ONCE (serve, STRATA_GLM_SEQS=<n> > 1).  GEN lines carry rid=<id>, and every line
    // a request causes carries it after its keyword (T <rid> <id>, PP <rid> ..., DONE <rid> ..., ERR <rid> ...);
    // STOP <rid> and PAUSE <rid> [keep] end one request, STOP alone all of them.  Each request runs in one of the n
    // sequence states, a CONTEXT: an idle context whose snapshot the prompt extends is taken as it stands (nothing
    // read, nothing loaded), else the least recently used idle one, whose conversation goes to a slot first.  A
    // request alone decodes through the speculative path (the fastest for one); as soon as a second one comes, both go
    // through the pipelined decode (Glm5Model::multi_issue), each card of the split on a different conversation.
    const bool multi = o.serve && model.fast() && model.seq_count() > 1;
    if (multi && !follow_ids.empty()) {
        std::fprintf(stderr, "glm follow: not used with MAYA_GLM_SEQS=%d - one conversation at a time only\n",
                     model.seq_count());
        follow_ids.clear();
    }
    // STRATA_GLM_MULTI_MTP=<drafts>: the running requests' batched steps carry verify windows - each one's last token
    // and the NextN block's drafts after it (Glm5Model::mtpw_*, strata/core/mtp_rows.hpp)
    const bool mtp_rows = multi && model.mtpw_ok();
    if (multi && strata::core::Glm5Model::multi_mtp_setting() > 0 && !mtp_rows)
        std::fprintf(stderr, "glm multi mtp: MAYA_GLM_MULTI_MTP is set but not used here - it needs MAYA_GLM_SPLIT_ROWS "
                             "on a split (or one GPU), the NextN block loaded (no MAYA_GLM_NO_MTP) and no --dflash\n");
    if (mtp_rows)
        std::fprintf(stderr, "glm multi mtp: the running requests verify up to %d drafts each in their batched steps "
                             "(%d rows a group)\n", strata::core::Glm5Model::multi_mtp_setting(),
                     strata::core::Glm5Model::rows_max());
    if (multi && model.dflash_ready())
        std::fprintf(stderr, "glm dflash: not used with MAYA_GLM_SEQS=%d - the several-conversation scheduler decodes "
                             "without it (and without the MTP block, which --dflash leaves unloaded)\n",
                     model.seq_count());
    struct Ctx {   // a context's driver-side state while another one is bound (the bound one's is in the globals)
        std::vector<int32_t> snap_tokens;
        uint64_t snap_img_hash = kNoImages;
        bool snap_in_slot = false;
        uint64_t used = 0;
        bool busy = false;
    };
    std::vector<Ctx> ctxs((size_t) model.seq_count());
    int cur_ctx = 0;
    uint64_t ctx_clock = 0;
    const auto bind_ctx = [&](int c) {
        if (c != cur_ctx) {
            Ctx& was = ctxs[(size_t) cur_ctx];
            was.snap_tokens = std::move(snap_tokens);
            was.snap_img_hash = snap_img_hash;
            was.snap_in_slot = snap_in_slot;
            Ctx& now = ctxs[(size_t) c];
            snap_tokens = std::move(now.snap_tokens);
            now.snap_tokens.clear();
            snap_img_hash = now.snap_img_hash;
            snap_in_slot = now.snap_in_slot;
            cur_ctx = c;
        }
        model.seq_bind_all(c);   // (the pipelined decode binds the model's sequences itself)
    };
    struct Gen {
        int rid = -1, ctx = 0;
        strata::kernels::SamplerParams sp;
        int64_t max_new = 0, produced = 0;
        std::vector<int32_t> prompt, emitted;
        std::vector<int32_t> pen;   // the penalties' history (sp.host_history): the prompt, then every token written
        bool in_think = false;
        int64_t think_budget = 0;
        int32_t think_end = -1;
        std::vector<int> hist;   // the loop guard's window
        bool in_arg = false;
        int guard = 0;
        int32_t force = -1;      // the next token instead of the sampled one (the thinking budget, the loop guard)
        int32_t pending = -1;    // written, not fed yet
        bool ready = false;      // just admitted: the first sample's logits are on the tail
        bool inflight = false, done = false, pause = false, keep = false;
        const char* finish = "length";
        size_t reuse = 0;
        std::chrono::steady_clock::time_point t0, t1;
        uint64_t spec_steps0 = 0, spec_hits0 = 0;
        strata::core::Glm5Model::FastStats st1;
        // STRATA_GLM_MULTI_MTP: the NextN block's drafts for the positions after `pending` (made at position
        // draft_at after draft_tok: stale once either moved), and this request's verify statistics
        std::vector<int32_t> drafts;
        int64_t draft_at = -1;
        int32_t draft_tok = -1;
        uint64_t mtp_steps = 0, mtp_drafts = 0, mtp_acc = 0;
    };
    std::vector<std::unique_ptr<Gen>> gens;
    std::deque<Gen*> flight;     // issued, oldest first
    std::deque<int> flight_groups;   // split rows: the sizes of the issued groups, oldest first
    // a request no idle context can hold yet (its prompt needs the large first context, which is busy): it waits
    // here with what its GEN line said, and is admitted when one frees
    struct Pending {
        int rid;
        std::vector<int64_t> ids;
        int64_t max_new;
        strata::kernels::SamplerParams rq;
        int64_t think_budget;
        int32_t think_end;
        std::vector<int64_t> img_pos;
        std::vector<float> img_rows;
        std::vector<std::pair<int32_t, float>> bias;
    };
    std::deque<Pending> pending;
    // the room a context must leave for the answer (at most what the request asked for): a long prompt goes to a
    // context with this much left after it, and its max tokens are cut to that context
    static const int64_t multi_room = (int64_t) env_num("STRATA_GLM_MULTI_ROOM", 16384);
    static const int multi_boundary_every = (int) env_num("STRATA_GLM_MULTI_BOUNDARY", 32);
    static const bool multi_spec = env_num("STRATA_GLM_MULTI_SPEC", 1) != 0;
    static const int split_rows = strata::core::Glm5Model::split_rows_setting();
    // STRATA_GLM_EVEN_GROUPS=1: the split's groups cut evenly from all the running requests, two at least, instead of
    // k at a time (strata/core/split_groups.hpp): k 4 no longer makes one group - and idle GPUs - at 4 requests or fewer
    static const bool even_groups = env_num("STRATA_GLM_EVEN_GROUPS", 0) != 0;
    // Reuse split-row scheduler buffers; allocating vectors for each decode group defeats part of the batching gain.
    std::vector<Gen*> split_group;
    std::vector<int> split_seqs;
    std::vector<int32_t> split_toks;
    std::vector<strata::kernels::SamplerParams> split_sps;
    if (split_rows > 0) {
        const size_t cap = (size_t) split_rows;
        split_group.reserve(cap);
        split_seqs.reserve(cap);
        split_toks.reserve(cap);
        split_sps.reserve(cap);
    }
    int since_boundary = 0;
    uint64_t multi_n = 0;        // tokens since the engine last had nothing to do (the STAT lines' rate)
    std::chrono::steady_clock::time_point multi_t1;
    strata::core::Glm5Model::FastStats multi_st1;
    const auto input_pending = [&]() -> bool {
        std::lock_guard<std::mutex> lk(in_mu);
        return !in_lines.empty() || quit_req.load();
    };
    const auto try_line = [&](std::string& out) -> bool {
        std::lock_guard<std::mutex> lk(in_mu);
        if (in_lines.empty()) return false;
        out = std::move(in_lines.front());
        in_lines.pop_front();
        return true;
    };
    // one written token of a request: its T line, the loop guard and the thinking budget (as run_request's
    // on_token), the stop conditions.  false: the request ends here (g.finish says why)
    const auto gen_token = [&](Gen& g, int tok) -> bool {
        std::printf("T %d %d\n", g.rid, tok);
        ++g.produced;
        ++multi_n;
        g.emitted.push_back((int32_t) tok);
        if (g.sp.host_history != nullptr) g.pen.push_back((int32_t) tok);
        strata::core::progress_at("decode: after token", g.produced);
        strata::core::progress_beat();
        g.hist.push_back(tok);
        const auto looping = [&](int w) -> int {
            const int n = (int) g.hist.size();
            if (w <= 0 || n < w) return 0;
            for (int per = 1; per <= 32 && per < w / 4; ++per) {
                bool rep = true;
                for (int i = n - w + per; i < n && rep; ++i) rep = g.hist[(size_t) i] == g.hist[(size_t) (i - per)];
                if (rep) return per;
            }
            return 0;
        };
        static const int loop_w_think = (int) env_num("STRATA_GLM_LOOP_THINK", 256);
        static const int loop_w_answer = (int) env_num("STRATA_GLM_LOOP_ANSWER", 1024);
        if (g.in_think && tok != g.think_end) {
            if (const int per = looping(loop_w_think)) {
                g.force = g.think_end;
                g.in_think = false;
                char m[200];
                std::snprintf(m, sizeof m, "thinking was repeating a %d-token pattern (%d tokens in): closed, "
                              "answering", per, (int) g.produced);
                std::fprintf(stderr, "glm loop guard: %s\n", m);
                std::printf("NOTE %s\n", m);
                g.hist.clear();
            }
        } else if (!g.in_think) {
            constexpr int kArgValue = 154849, kArgValueEnd = 154850, kUser = 154827;
            if (tok == kArgValue) g.in_arg = true;
            if (tok == kArgValueEnd) g.in_arg = false;
            if (const int per = looping(loop_w_answer)) {
                char m[240];
                if (g.guard == 0) {
                    g.force = g.in_arg ? kArgValueEnd : kUser;
                    std::snprintf(m, sizeof m, "the answer was repeating a %d-token pattern (%d tokens in): %s", per,
                                  (int) g.produced, g.in_arg ? "the tool call's argument was closed there"
                                                             : "the turn was ended there");
                } else {
                    std::snprintf(m, sizeof m, "the answer kept repeating a %d-token pattern (%d tokens in): ended",
                                  per, (int) g.produced);
                }
                std::fprintf(stderr, "glm loop guard: %s\n", m);
                std::printf("NOTE %s\n", m);
                g.hist.clear();
                if (g.guard++ > 0) {
                    g.finish = "length";
                    return false;
                }
            }
        }
        if (g.in_think && (tok == g.think_end || g.produced >= g.think_budget || wrap_req.load())) {
            if (tok != g.think_end) g.force = g.think_end;
            g.in_think = false;
            g.hist.clear();
        }
        if (multi_n % 8 == 0) {   // the Monitor's live counters, over everything decoded since the engine was idle
            const auto sn = model.fast_stats();
            const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - multi_t1).count();
            const double n = (double) std::max<uint64_t>(1, multi_n);
            const uint64_t h = sn.hits - multi_st1.hits, m = sn.misses - multi_st1.misses;
            std::printf("STAT tok_s=%.2f ms_tok=%.2f vram_hit=%.4f ram_fetch=%.3f disk=%.3f promo=%.3f "
                        "vram_used=%lld vram_slots=%lld ram_used=%lld ram_slots=%lld ram_gb=%.1f vram_gb=%.1f "
                        "active=%zu\n",
                        n / std::max(1e-9, el), 1000.0 * el / n, (double) h / (double) std::max<uint64_t>(1, h + m),
                        (double) (sn.ram_hits - multi_st1.ram_hits) / n,
                        (double) (sn.disk_reads - multi_st1.disk_reads) / n,
                        (double) (sn.promotions - multi_st1.promotions) / n, (long long) sn.pool_used,
                        (long long) sn.pool_slots, (long long) sn.ram_used, (long long) sn.ram_slots, sn.ram_gb,
                        sn.pool_gb, gens.size());
        }
        for (const int64_t e : eos)
            if (e == tok && stop_on_eos) {
                g.finish = "stop";
                return false;
            }
        {
            std::lock_guard<std::mutex> lk(ctl_mu);
            const auto it = ctl.find(g.rid);
            if (it != ctl.end()) {
                g.pause = it->second >= 2;
                g.keep = it->second == 3;
                ctl.erase(it);
                g.finish = "cancel";
                return false;
            }
        }
        if (stop_req.load()) {
            g.finish = "cancel";
            return false;
        }
        return g.produced < g.max_new;
    };
    // the PAUSE snapshots a read's gaps kept back (STRATA_GLM_PREFILL_GAPS): taken once no prompt is being read and no
    // request waits for its first token (the tail's logits are its own until then), their contexts idle again
    const auto flush_pauses = [&] {
        for (const auto& d : deferred_pauses) {
            bind_ctx(d.ctx);
            pause_snapshot(d.prompt, d.emitted, d.keep);
            ctxs[(size_t) d.ctx].busy = false;
        }
        deferred_pauses.clear();
    };
    // a request's end: its DONE line (the engine's counters over its lifetime are shared with the others running
    // meanwhile), the paused state kept, its context idle again
    const auto gen_finish = [&](Gen& g) {
        const auto t2 = std::chrono::steady_clock::now();
        const double prompt_ms = std::chrono::duration<double, std::milli>(g.t1 - g.t0).count();
        const double decode_ms = std::chrono::duration<double, std::milli>(t2 - g.t1).count();
        const auto st2 = model.fast_stats();
        const uint64_t d_hits = st2.hits - g.st1.hits, d_miss = st2.misses - g.st1.misses;
        std::printf("DONE %d %lld %lld %.1f %.1f %s %llu %llu %zu %llu %llu\n", g.rid, (long long) g.produced,
                    (long long) g.prompt.size(), prompt_ms, decode_ms, g.finish,
                    (unsigned long long) (model.spec_hits_ - g.spec_hits0 + g.mtp_acc),
                    (unsigned long long) (model.spec_steps_ - g.spec_steps0 + g.mtp_drafts), g.reuse,
                    (unsigned long long) d_hits, (unsigned long long) (d_hits + d_miss));
        if (mtp_rows && g.mtp_steps > 0)
            std::fprintf(stderr, "glm multi mtp: request %d: %llu of %llu drafts accepted (%.1f%%) in %llu verify steps, "
                                 "%.2f tokens a step\n", g.rid, (unsigned long long) g.mtp_acc,
                         (unsigned long long) g.mtp_drafts,
                         100.0 * (double) g.mtp_acc / (double) std::max<uint64_t>(1, g.mtp_drafts),
                         (unsigned long long) g.mtp_steps,
                         (double) (g.mtp_steps + g.mtp_acc) / (double) g.mtp_steps);
        bool deferred = false;
        if (g.pause && std::strcmp(g.finish, "cancel") == 0) {
            if (in_read_gap) {   // (another context's prompt is being read and stays bound: the snapshot after it)
                deferred_pauses.push_back({g.ctx, g.prompt, g.emitted, g.keep});
                deferred = true;
            } else {
                bind_ctx(g.ctx);
                pause_snapshot(g.prompt, g.emitted, g.keep);
            }
        }
        if (g.produced > 0)
            std::fprintf(stderr, "glm multi: request %d done (%s): %lld tokens in %.1f s (%.1f tok/s), %zu running\n",
                         g.rid, g.finish, (long long) g.produced, decode_ms / 1000.0,
                         1000.0 * (double) g.produced / std::max(1.0, decode_ms), gens.size() - 1);
        {
            std::lock_guard<std::mutex> lk(ctl_mu);
            ctl.erase(g.rid);
        }
        ctxs[(size_t) g.ctx].busy = deferred;   // (a deferred snapshot: busy until flush_pauses)
        ctxs[(size_t) g.ctx].used = ++ctx_clock;
        g.done = true;
    };
    const auto gen_sweep = [&] {
        gens.erase(std::remove_if(gens.begin(), gens.end(), [](const std::unique_ptr<Gen>& p) { return p->done; }),
                   gens.end());
        if (gens.empty()) {
            // (a prompt being read between decode steps keeps a STOP for all: its read ends too)
            if (admitting_rid.load() >= 0 && stop_req.load()) admit_stop.store(true);
            stop_req.store(false);
            // this machine's expert usage, for the next start's warm-up (not inside a prompt's read)
            if (model.fast() && !in_read_gap) model.save_usage();
            if (!deferred_pauses.empty() && !in_read_gap && admitting_rid.load() < 0) flush_pauses();
        }
    };
    // the token a request takes after one came back: forced, else as sampled
    const auto gen_took = [&](Gen& g, int tok) {
        if (g.force >= 0) {
            tok = g.force;
            g.force = -1;
        }
        if (!gen_token(g, tok)) gen_finish(g);
        else g.pending = tok;
    };
    // STRATA_GLM_MULTI_MTP: the groups of verify windows in flight, oldest first - each past its front (every part but
    // the last), `back` once its last part and head are queued too
    struct WinGroup {
        std::vector<Gen*> g;
        std::vector<int> seqs, counts;
        std::vector<int32_t> toks;
        std::vector<strata::kernels::SamplerParams> sps;
        bool back = false;
    };
    std::deque<WinGroup> wflight;
    const auto win_back = [&](WinGroup& w) -> bool {
        if (w.back) return true;
        w.back = true;
        return model.mtpw_back(w.seqs.data(), w.counts.data(), (int) w.seqs.size(), w.toks.data(), w.sps.data(), err);
    };
    // the oldest group back: its picks; each window's rows written up to the first draft that differs from what was
    // written before it (T lines, the stop conditions, a forced token - gen_took's rules); the commit; then the next
    // windows' drafts - the next group's last part is queued between the drafts and their wait, so the last GPU goes
    // on with it; the requests that ended, ended
    const auto win_take = [&]() -> bool {
        WinGroup w = std::move(wflight.front());
        wflight.pop_front();
        const int n = (int) w.seqs.size();
        int rows = 0;
        for (const int c : w.counts) rows += c;
        std::vector<int32_t> picks((size_t) rows);
        if (!win_back(w) || !model.mtpw_picks(w.seqs.data(), w.counts.data(), n, picks.data(), err)) return false;
        std::vector<int> keep((size_t) n, 1), nd((size_t) n, 0);
        std::vector<int32_t> written, last((size_t) n, -1);
        std::vector<char> ended((size_t) n, 0);
        const int D = strata::core::Glm5Model::multi_mtp_setting();
        for (int i = 0, row = 0; i < n; row += w.counts[(size_t) i], ++i) {
            Gen& g = *w.g[(size_t) i];
            g.inflight = false;
            std::vector<int32_t> wr;
            keep[(size_t) i] = strata::core::mtp_accept(
                w.toks.data() + row, picks.data() + row, w.counts[(size_t) i],
                [&](int, int32_t pick, int32_t& out) {
                    int32_t tok = pick;
                    if (g.force >= 0) {   // (gen_took's: the forced token instead of the pick)
                        tok = g.force;
                        g.force = -1;
                    }
                    out = tok;
                    wr.push_back(tok);
                    g.sp.counter += 1;
                    if (!gen_token(g, tok)) {
                        ended[(size_t) i] = 1;
                        return strata::core::MtpTake::stop;
                    }
                    return g.force >= 0 ? strata::core::MtpTake::forced : strata::core::MtpTake::more;
                });
            const int k = keep[(size_t) i];
            g.mtp_steps += 1;
            g.mtp_drafts += (uint64_t) (w.counts[(size_t) i] - 1);
            g.mtp_acc += (uint64_t) (k - 1);
            for (int t = 0; t + 1 < k; ++t) written.push_back(wr[(size_t) t]);   // (the tokens at p0 + 1 .. p0 + k - 1)
            last[(size_t) i] = wr[(size_t) k - 1];
            g.drafts.clear();
            g.draft_at = -1;
            if (!ended[(size_t) i]) {
                g.pending = last[(size_t) i];
                // the next window's drafts: no more than its tokens left after the pending one and its context's
                // room (draft j's NextN cache entry is at the window's start + j - 1; the position is still this
                // window's start + its rows until the commit below)
                nd[(size_t) i] = (int) std::max<int64_t>(
                    0, std::min<int64_t>({(int64_t) D, g.max_new - g.produced - 1,
                                          model.seq_max_ctx(g.ctx) - 2 -
                                              (model.seq_pos(g.ctx) - w.counts[(size_t) i] + k)}));
            }
        }
        if (!model.mtpw_commit(w.seqs.data(), w.counts.data(), keep.data(), n, err)) return false;
        if (!model.mtpw_draft_queue(w.seqs.data(), keep.data(), n, written.data(), last.data(), nd.data(), err))
            return false;
        if (!wflight.empty() && !win_back(wflight.front())) return false;
        {
            std::vector<int32_t> out((size_t) n * (size_t) std::max(1, D), -1);
            if (!model.mtpw_draft_take(out.data(), err)) return false;
            for (int i = 0; i < n; ++i) {
                Gen& g = *w.g[(size_t) i];
                if (ended[(size_t) i]) continue;
                for (int j = 0; j < D && out[(size_t) i * D + (size_t) j] >= 0; ++j)
                    g.drafts.push_back(out[(size_t) i * D + (size_t) j]);
                g.draft_at = model.seq_pos(g.ctx);
                g.draft_tok = g.pending;
            }
        }
        for (int i = 0; i < n; ++i)
            if (ended[(size_t) i]) gen_finish(*w.g[(size_t) i]);
        since_boundary += n;
        return true;
    };
    // every issued token back (before a prompt, a boundary or the speculative path)
    const auto multi_drain = [&]() -> bool {
        while (!wflight.empty())
            if (!win_take()) return false;
        while (!flight.empty()) {
            Gen& g = *flight.front();
            flight.pop_front();
            const int tok = model.multi_take(g.ctx, err);
            g.inflight = false;
            if (tok < 0) return false;
            gen_took(g, tok);
        }
        flight_groups.clear();
        return true;
    };
    // a just-admitted request's first token, while the tail's logits are still its own
    const auto resolve_ready = [&]() -> bool {
        for (auto& gp : gens) {
            Gen& g = *gp;
            if (!g.ready || g.done) continue;
            bind_ctx(g.ctx);
            const int tok = model.sample_token(g.sp, err);
            g.ready = false;
            if (tok < 0) return false;
            g.sp.counter += 1;
            gen_took(g, model.forced(tok));
        }
        return true;
    };
    // an engine error in the middle: every running request gets it, the sequences are reset
    uint64_t multi_fails = 0;      // (a prompt read between decode steps sees one happen: STRATA_GLM_PREFILL_INTERLEAVE)
    std::string multi_fail_why;
    const auto multi_fail = [&](const std::string& why) {
        ++multi_fails;
        multi_fail_why = why;
        for (auto& gp : gens) {
            if (gp->done) continue;
            if (why.find("non-finite") != std::string::npos)   // the slots it may have come from (drop_if_poisoned)
                for (size_t i = slots.size(); i-- > 0;) {
                    const auto& t = slots[i].tokens;
                    if (t.size() <= gp->prompt.size() && std::equal(t.begin(), t.end(), gp->prompt.begin()))
                        slot_drop(i);
                }
            std::printf("ERR %d %s\n", gp->rid, why.c_str());
            ctxs[(size_t) gp->ctx].busy = false;
            gp->done = true;
        }
        flight.clear();
        flight_groups.clear();
        wflight.clear();
        if (in_read_gap) {   // (a prompt is being read: its call ends first, then gap_reset resets them all)
            gap_failed = true;
            prompt_between_err = "the engine failed while other requests decoded during this prompt's read: " + why;
            gen_sweep();
            return;
        }
        for (const auto& d : deferred_pauses) ctxs[(size_t) d.ctx].busy = false;   // (their states go with the rest)
        deferred_pauses.clear();
        for (int c = 0; c < (int) ctxs.size(); ++c) {
            bind_ctx(c);
            snap_tokens.clear();
            model.reset();
        }
        gen_sweep();
    };
    // the contexts' reset a failed gap left (STRATA_GLM_PREFILL_GAPS; multi_fail), once the read has ended
    gap_reset = [&] {
        if (!gap_failed) return;
        gap_failed = false;
        for (const auto& d : deferred_pauses) ctxs[(size_t) d.ctx].busy = false;   // (their states go with the rest)
        deferred_pauses.clear();
        for (int c = 0; c < (int) ctxs.size(); ++c) {
            bind_ctx(c);
            snap_tokens.clear();
            model.reset();
        }
    };
    // the decode steps between two pieces of a prompt being read in context read_ctx (STRATA_GLM_PREFILL_INTERLEAVE;
    // set below, after multi_step): prompt_between's 0 / 1 / 2
    std::function<int(int)> interleave_round;
    // one decode gap of a prompt being read (STRATA_GLM_PREFILL_GAPS; set below, after multi_step): 0 / 1 / 2 as
    // prompt_between's; gap_mark: the read's start, then each gap's end
    std::function<int()> read_gap;
    uint64_t gap_count = 0, gap_tok = 0;
    double gap_ms = 0.0;
    // the last gap's decode round (ms; 0: none yet) - a boundary's gap is due once its budget is half of one
    // (prefill_gap_due; STRATA_GLM_PREFILL_GAP_CREDIT=0: every boundary, as before)
    double gap_round_ms = 0.0;
    static const bool gap_credit = env_num("STRATA_GLM_PREFILL_GAP_CREDIT", 1) != 0;
    const auto gen_admit = [&](int rid, const std::vector<int64_t>& ids, int64_t max_new,
                               const strata::kernels::SamplerParams& rq) {
        if (!multi_drain() || !resolve_ready()) {
            const std::string why = err;
            multi_fail(why);
            std::printf("ERR %d %s\n", rid, why.c_str());   // (this request too: its client would wait forever)
            return;
        }
        if (!deferred_pauses.empty()) flush_pauses();   // (a PAUSE in the last read's gaps: its state, kept first)
        auto g = std::make_unique<Gen>();
        g->rid = rid;
        g->sp = rq;
        g->max_new = max_new;
        g->in_think = req_think_budget > 0 && req_think_end >= 0;
        g->think_budget = req_think_budget;
        g->think_end = req_think_end;
        for (const int64_t t : ids) {
            if (t < 0 || t >= n_vocab) {
                std::printf("ERR %d token %lld out of range [0, %lld)\n", rid, (long long) t, (long long) n_vocab);
                return;
            }
            g->prompt.push_back((int32_t) t);
        }
        if (g->prompt.empty() || max_new <= 0) {
            std::printf("ERR %d empty prompt or max_new\n", rid);
            return;
        }
        if (g->sp.penalty_last_n > 0) {   // its penalties' history (#273; the Gen lives behind a unique_ptr: it stays put)
            g->pen = g->prompt;
            g->sp.host_history = &g->pen;
        }
        // the context: an idle one that holds the prompt and the room after it, and whose snapshot this prompt
        // extends (the longest), else the least recently used of those
        const int64_t want = (int64_t) g->prompt.size() + std::min<int64_t>(max_new, multi_room) + 8;
        const auto fits = [&](int c) { return model.seq_max_ctx(c) >= want; };
        int best = -1;
        size_t blen = 0;
        for (int c = 0; c < (int) ctxs.size(); ++c) {
            if (ctxs[(size_t) c].busy || !fits(c)) continue;
            const auto& st = c == cur_ctx ? snap_tokens : ctxs[(size_t) c].snap_tokens;
            const uint64_t hh = c == cur_ctx ? snap_img_hash : ctxs[(size_t) c].snap_img_hash;
            if (!st.empty() && st.size() < g->prompt.size() && st.size() > blen &&
                std::equal(st.begin(), st.end(), g->prompt.begin()) && img_hash(st.size()) == hh) {
                best = c;
                blen = st.size();
            }
        }
        if (best < 0)
            for (int c = 0; c < (int) ctxs.size(); ++c)
                if (!ctxs[(size_t) c].busy && fits(c) &&
                    (best < 0 || ctxs[(size_t) c].used < ctxs[(size_t) best].used))
                    best = c;
        if (best < 0) {
            bool ever = false;
            for (int c = 0; c < (int) ctxs.size(); ++c) ever = ever || fits(c);
            if (!ever) {
                std::printf("ERR %d prompt (%zu tokens) + room for the answer exceeds every context\n", rid,
                            g->prompt.size());
                return;
            }
            pending.push_back({rid, ids, max_new, rq, req_think_budget, req_think_end, cur_img_pos, cur_img_rows,
                               req_bias});
            std::fprintf(stderr, "glm multi: request %d waits for a context that holds %lld tokens\n", rid,
                         (long long) want);
            return;
        }
        g->max_new = std::min<int64_t>(max_new, model.seq_max_ctx(best) - (int64_t) g->prompt.size() - 8);
        {   // its logit_bias: the table of the context it runs in (no earlier request's sample reads it any more)
            std::string be;
            g->sp.logit_bias = model.logit_bias_table(best, req_bias, be);
            if (!be.empty()) {
                std::printf("ERR %d %s\n", rid, be.c_str());
                return;
            }
        }
        if (gens.empty()) stop_req.store(false);
        gap_failed = false;
        bind_ctx(best);
        model.set_image_rows(cur_img_pos, cur_img_rows);
        model.force_next(-1);
        ctxs[(size_t) best].busy = true;
        g->ctx = best;
        tag = std::to_string(rid) + " ";
        // STRATA_GLM_PREFILL_GAPS: the running requests decode at the read's chunk boundaries and between its calls
        // (the split's batched rows: they never bind a context; text prompts only, as interleave)
        const bool use_gaps = read_gaps.percent > 0 && cur_img_pos.empty() && split_rows > 0 && model.split_rows_ok();
        if (read_gaps.percent > 0 && !use_gaps && !gens.empty()) {
            static bool told = false;
            if (!cur_img_pos.empty()) {
                std::fprintf(stderr, "glm multi: request %d's prompt has images: read whole, no decode gaps "
                                     "(MAYA_GLM_PREFILL_GAPS is for text prompts)\n", rid);
            } else if (!told) {
                told = true;
                std::fprintf(stderr, "glm multi: MAYA_GLM_PREFILL_GAPS needs a split with MAYA_GLM_SPLIT_ROWS: off "
                                     "here%s\n", interleave.tokens > 0 ? " (MAYA_GLM_PREFILL_INTERLEAVE instead)" : "");
            }
        }
        gaps_over = false;
        if (use_gaps) {
            model.prefill_gap_wanted = [&] {
                if (gaps_over ||
                    !std::any_of(gens.begin(), gens.end(), [](const std::unique_ptr<Gen>& p) { return !p->done; }))
                    return false;
                if (!gap_credit) return true;
                const double stage_ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - gap_mark).count();
                return strata::core::prefill_gap_due(read_gaps, stage_ms, gap_round_ms);
            };
            model.prefill_gap = [&] { return read_gap(); };
            prompt_between = [&] { return read_gap(); };
            read_gap_first = true;
            gap_count = gap_tok = 0;
            gap_ms = 0.0;
        }
        // STRATA_GLM_PREFILL_INTERLEAVE: while other requests decode, the prompt in pieces with their steps between
        // (text prompts only: a prompt's image rows are the driver's globals while it is read)
        if (interleave.tokens > 0 && read_gaps.percent == 0 && !cur_img_pos.empty() && !gens.empty())
            std::fprintf(stderr, "glm multi: request %d's prompt has images: read whole, not in pieces "
                                 "(MAYA_GLM_PREFILL_INTERLEAVE is for text prompts)\n", rid);
        if (!use_gaps && interleave.tokens > 0 && cur_img_pos.empty()) {
            // (STRATA_GLM_PREFILL_INTERLEAVE_ALWAYS=1, a test: the pieces with nothing decoding too - to compare with
            // the prompt read whole, the decode alone)
            static const bool always = env_num("STRATA_GLM_PREFILL_INTERLEAVE_ALWAYS", 0) != 0;
            prompt_piece = [&]() -> size_t {
                for (const auto& gp : gens)
                    if (!gp->done) return interleave.tokens;
                return always ? interleave.tokens : 0;   // nothing decodes (any more): the rest in one call, as before
            };
            prompt_between = [&, best]() { return interleave_round(best); };
        }
        admit_stop.store(false);
        admitting_rid.store(rid);
        const int rc = prep_prompt(g->prompt, g->reuse, g->finish, g->t0);
        admitting_rid.store(-1);
        prompt_piece = nullptr;
        prompt_between = nullptr;
        model.prefill_gap = nullptr;
        model.prefill_gap_wanted = nullptr;
        read_gap_first = false;
        if (use_gaps && gap_count > 0)
            std::fprintf(stderr, "glm multi: request %d's prompt (%zu tokens) read with %llu decode gaps: %.1f s of "
                                 "decode, %llu tokens for the others (%.1f tok/s there)\n",
                         rid, g->prompt.size(), (unsigned long long) gap_count, gap_ms / 1000.0,
                         (unsigned long long) gap_tok, 1000.0 * (double) gap_tok / std::max(1.0, gap_ms));
        tag.clear();
        if (rc != 0) {
            ctxs[(size_t) best].busy = false;
            admit_stop.store(false);
            std::lock_guard<std::mutex> lk(ctl_mu);   // (a STOP for it that came during its prompt: nothing to stop now)
            ctl.erase(rid);
            return;
        }
        // a STOP <rid> during its prompt (with interleave: admit_stop alone)
        const bool stopped_all = stop_req.exchange(false), stopped_this = admit_stop.exchange(false);
        if (stopped_all || stopped_this) g->finish = "cancel";
        g->t1 = std::chrono::steady_clock::now();
        g->st1 = model.fast_stats();
        g->spec_steps0 = model.spec_steps_;
        g->spec_hits0 = model.spec_hits_;
        if (gens.empty()) {
            multi_n = 0;
            multi_t1 = g->t1;
            multi_st1 = g->st1;
        }
        if (std::strcmp(g->finish, "cancel") == 0) {
            gens.push_back(std::move(g));
            gen_finish(*gens.back());
            gen_sweep();
            return;
        }
        g->ready = true;
        gens.push_back(std::move(g));
        if (gens.size() > 1 && !resolve_ready()) multi_fail(err);   // the others decode: its first token now
        if (!deferred_pauses.empty() && gens.size() > 1) flush_pauses();   // (its first token taken: the tail is free)
        gen_sweep();
    };
    // the waiting requests, in order, that an idle context can now hold (a STOP for one ends it unread)
    const auto admit_pending = [&] {
        for (size_t i = 0; i < pending.size();) {
            const Pending& w = pending[i];
            bool stopped = false;
            {
                std::lock_guard<std::mutex> lk(ctl_mu);
                const auto it = ctl.find(w.rid);
                if (it != ctl.end()) {
                    stopped = true;
                    ctl.erase(it);
                }
            }
            if (stopped) {
                std::printf("DONE %d 0 %zu 0.0 0.0 cancel 0 0 0 0 0\n", w.rid, w.ids.size());
                pending.erase(pending.begin() + (long) i);
                continue;
            }
            const int64_t want = (int64_t) w.ids.size() + std::min<int64_t>(w.max_new, multi_room) + 8;
            bool free_fit = false;
            for (int c = 0; c < (int) ctxs.size(); ++c)
                free_fit = free_fit || (!ctxs[(size_t) c].busy && model.seq_max_ctx(c) >= want);
            if (!free_fit) {
                ++i;
                continue;
            }
            Pending w2 = std::move(pending[i]);
            pending.erase(pending.begin() + (long) i);
            req_think_budget = w2.think_budget;
            req_think_end = w2.think_end;
            req_bias = std::move(w2.bias);
            cur_img_pos = std::move(w2.img_pos);
            cur_img_rows = std::move(w2.img_rows);
            gen_admit(w2.rid, w2.ids, w2.max_new, w2.rq);
        }
    };
    // one step of the scheduler: a stint of the speculative path for a request alone, else one token back from the
    // pipelined decode (and every request with a token to feed issued first).  Errors end the running requests.
    bool step_no_spec = false;   // (between a prompt's pieces: the speculative stint runs until input comes - not there)
    const auto multi_step = [&] {
        std::vector<float> lg;
        // (a PAUSE in the last read's gaps: its snapshot, once no request waits for its first token)
        if (!deferred_pauses.empty() && !in_read_gap && admitting_rid.load() < 0 &&
            std::none_of(gens.begin(), gens.end(), [](const std::unique_ptr<Gen>& p) { return p->ready && !p->done; }))
            flush_pauses();
        Gen* only = gens.size() == 1 ? gens[0].get() : nullptr;
        if (only != nullptr && !only->inflight && multi_spec && !step_no_spec && model.spec_ready()) {
            Gen& g = *only;
            bind_ctx(g.ctx);
            if (g.pending >= 0) {   // its last token through the token path: the next one's logits on the tail
                if (!model.forward({g.pending}, lg, err)) {
                    multi_fail(err);
                    return;
                }
                g.pending = -1;
            }
            g.ready = false;
            model.force_next(g.force);
            g.force = -1;
            int32_t last = -1, forced_unused = -1;
            bool interrupted = false;
            int64_t n_spec = 0;
            const bool ok = model.decode_spec(g.sp, g.max_new - g.produced, [&](int tok) {
                forced_unused = -1;   // a force handed over is consumed by the token that comes next
                last = tok;
                if (!gen_token(g, tok)) return false;
                if (g.force >= 0) {
                    model.force_next(g.force);
                    forced_unused = g.force;
                    g.force = -1;
                }
                if (input_pending()) {
                    interrupted = true;
                    return false;
                }
                return true;
            }, n_spec, err);
            if (forced_unused >= 0) {   // handed over, never taken: it stays the request's
                g.force = forced_unused;
                model.force_next(-1);
            }
            if (!ok) {
                multi_fail(err);
                return;
            }
            // not interrupted by input: decode_spec ended the request (a stop, its budget, the context)
            if (interrupted) g.pending = last;
            else gen_finish(g);
            gen_sweep();
            return;
        }
        // a request admitted alone that the speculative path does not take: its first token, from its prompt's logits
        if (!resolve_ready()) {
            multi_fail(err);
            return;
        }
        // STRATA_GLM_MULTI_MTP: the ready requests' windows (their last token and the drafts made for the positions
        // after it) in groups as the split's batched rows take them, each group through every part but the last;
        // then the oldest group back (win_take) - its last part queued now if it was not
        // (STRATA_GLM_MULTI_MTP_MAX, 4: the windows only while that many requests or fewer run - with more, every
        // GPU is already busy and a window's rows cost about what the tokens they save cost; 0: always. Switching
        // drains what the other way has in flight)
        static const int mtp_max = (int) env_num("STRATA_GLM_MULTI_MTP_MAX", 4);
        int n_run = 0;
        for (auto& gp : gens) n_run += !gp->done;
        const bool use_win = mtp_rows && (mtp_max <= 0 || n_run <= mtp_max);
        if ((use_win && !flight.empty()) || (!use_win && !wflight.empty())) {
            if (!multi_drain()) {
                multi_fail(err);
                return;
            }
            gen_sweep();
            return;
        }
        if (use_win) {
            int ready = 0, active = 0;
            for (auto& gp : gens) {
                if (gp->done) continue;
                ready += !gp->inflight && gp->pending >= 0;
                active += gp->inflight || gp->pending >= 0;
            }
            const int k = std::max(1, split_rows > 0 ? split_rows : model.seq_count());
            const std::vector<int> sizes = strata::core::split_groups(ready, active, k, even_groups);
            std::vector<Gen*> rdy;
            for (auto& gp : gens)
                if (!gp->done && !gp->inflight && gp->pending >= 0) rdy.push_back(gp.get());
            size_t at = 0;
            for (const int gs : sizes) {
                WinGroup w;
                for (int j = 0; j < gs && at < rdy.size(); ++j) w.g.push_back(rdy[at++]);
                for (Gen* gp : w.g) {
                    const int64_t pos = model.seq_pos(gp->ctx);
                    const int d = gp->draft_at == pos && gp->draft_tok == gp->pending ? (int) gp->drafts.size() : 0;
                    w.seqs.push_back(gp->ctx);
                    w.counts.push_back(strata::core::mtp_rows_count(d, gp->max_new - gp->produced,
                                                                    model.seq_max_ctx(gp->ctx) - 1 - pos));
                    w.sps.push_back(gp->sp);
                }
                strata::core::mtp_rows_fit(w.counts, strata::core::Glm5Model::rows_max());
                for (size_t j = 0; j < w.g.size(); ++j) {
                    w.toks.push_back(w.g[j]->pending);
                    for (int t = 1; t < w.counts[j]; ++t) w.toks.push_back(w.g[j]->drafts[(size_t) t - 1]);
                }
                if (w.g.empty()) continue;
                if (!model.mtpw_front(w.seqs.data(), w.counts.data(), (int) w.seqs.size(), w.toks.data(), err)) {
                    multi_fail(err);
                    return;
                }
                for (Gen* gp : w.g) gp->inflight = true;
                wflight.push_back(std::move(w));
            }
            if (wflight.empty()) {
                gen_sweep();
                return;
            }
            if (!win_take()) {
                multi_fail(err);
                return;
            }
            if (multi_boundary_every > 0 &&
                since_boundary >= multi_boundary_every * (int) std::max<size_t>(1, gens.size())) {
                since_boundary = 0;
                if (!multi_drain() || !model.multi_boundary(err)) {
                    multi_fail(err);
                    return;
                }
            }
            gen_sweep();
            return;
        }
        // ONE GPU: every request with a token to feed goes through one step together, each weight read once for all
        if (model.rows_ok()) {
            std::vector<Gen*> rg;
            std::vector<int> seqs, out;
            std::vector<int32_t> toks;
            std::vector<strata::kernels::SamplerParams> sps;
            for (auto& gp : gens)
                if (!gp->done && gp->pending >= 0 && (int) rg.size() < strata::core::Glm5Model::rows_max()) {
                    rg.push_back(gp.get());
                    seqs.push_back(gp->ctx);
                    toks.push_back(gp->pending);
                    sps.push_back(gp->sp);
                }
            if (!rg.empty()) {
                out.resize(rg.size());
                if (!model.multi_rows(seqs.data(), toks.data(), sps.data(), (int) rg.size(), out.data(), err)) {
                    multi_fail(err);
                    return;
                }
                for (size_t i = 0; i < rg.size(); ++i) {   // (the step keeps the tiers' boundary itself)
                    Gen& g = *rg[i];
                    g.sp.counter += 1;
                    g.pending = -1;
                    gen_took(g, out[i]);
                }
            }
            gen_sweep();
            return;
        }
        if (split_rows > 0 && model.split_rows_ok()) {
            split_group.clear();
            const auto issue = [&]() -> bool {          // one batched step of the gathered group
                if (split_group.empty()) return true;
                split_seqs.clear();
                split_toks.clear();
                split_sps.clear();
                for (Gen* gp : split_group) {
                    split_seqs.push_back(gp->ctx);
                    split_toks.push_back(gp->pending);
                    split_sps.push_back(gp->sp);
                }
                if (!model.multi_issue_rows(split_seqs.data(), split_toks.data(), split_sps.data(),
                                           (int) split_group.size(), err))
                    return false;
                for (Gen* gp : split_group) {
                    gp->sp.counter += 1;
                    gp->pending = -1;
                    gp->inflight = true;
                    flight.push_back(gp);
                }
                flight_groups.push_back((int) split_group.size());
                split_group.clear();
                return true;
            };
            int ready = 0, active = 0;
            for (auto& gp : gens) {
                if (gp->done) continue;
                ready += !gp->inflight && gp->pending >= 0;
                active += gp->inflight || gp->pending >= 0;
            }
            const std::vector<int> sizes = strata::core::split_groups(ready, active, split_rows, even_groups);
            size_t gi = 0;
            for (auto& gp : gens) {
                if (gp->done || gp->inflight || gp->pending < 0) continue;
                split_group.push_back(gp.get());
                if (gi < sizes.size() && (int) split_group.size() == sizes[gi]) {
                    ++gi;
                    if (!issue()) { multi_fail(err); return; }
                }
            }
            if (!issue()) { multi_fail(err); return; }
            if (flight.empty()) { gen_sweep(); return; }
            const int k = flight_groups.front();   // the oldest group, all of it
            flight_groups.pop_front();
            for (int j = 0; j < k; ++j) {
                Gen& g = *flight.front();
                flight.pop_front();
                const int tok = model.multi_take(g.ctx, err);
                g.inflight = false;
                if (tok < 0) { multi_fail(err); return; }
                gen_took(g, tok);
            }
            since_boundary += k;
            if (multi_boundary_every > 0 && !in_read_gap &&
                since_boundary >= multi_boundary_every * (int) std::max<size_t>(1, gens.size())) {
                since_boundary = 0;
                if (!multi_drain() || !model.multi_boundary(err)) { multi_fail(err); return; }
            }
            gen_sweep();
            return;
        }
        for (auto& gp : gens) {
            Gen& g = *gp;
            if (g.done || g.inflight || g.pending < 0) continue;
            if (!model.multi_issue(g.ctx, g.pending, g.sp, err)) {
                multi_fail(err);
                return;
            }
            g.sp.counter += 1;
            g.pending = -1;
            g.inflight = true;
            flight.push_back(&g);
        }
        if (flight.empty()) {
            gen_sweep();
            return;
        }
        Gen& g = *flight.front();
        flight.pop_front();
        const int tok = model.multi_take(g.ctx, err);
        g.inflight = false;
        if (tok < 0) {
            multi_fail(err);
            return;
        }
        gen_took(g, tok);
        if (multi_boundary_every > 0 && ++since_boundary >= multi_boundary_every * (int) std::max<size_t>(1, gens.size())) {
            since_boundary = 0;
            if (!multi_drain() || !model.multi_boundary(err)) {
                multi_fail(err);
                return;
            }
        }
        gen_sweep();
    };

    // Between two pieces of a prompt being read (STRATA_GLM_PREFILL_INTERLEAVE): `rounds` rounds of the scheduler's own
    // steps - each until every request running at its start has a token more (or ended) - then every issued token back
    // (the prompt path needs the GPUs to itself) and the reading context bound again.  The steps are multi_step's, so
    // a request that ends meanwhile ends as it would (DONE, its pause snapshot), and a STOP for one ends it; an engine
    // error fails them all (multi_fail resets every context, the one being read too).  The new prompt's own STOP is
    // admit_stop.  0: go on, 1: the read was stopped, 2: failed (prompt_between_err).
    interleave_round = [&](int read_ctx) -> int {
        if (read_stopped()) return 1;
        if (gens.empty()) return 0;   // (nothing decodes: no sweep either - an empty one clears stop_req)
        const uint64_t fails0 = multi_fails;
        const auto failed = [&] {
            prompt_between_err = "the engine failed while other requests decoded between this prompt's pieces: " +
                                 multi_fail_why;
            return 2;
        };
        step_no_spec = true;
        for (int r = 0; r < interleave.rounds && !gens.empty(); ++r) {
            std::map<int, int64_t> want;   // rid -> the produced count that ends this round for it
            for (const auto& gp : gens)
                if (!gp->done) want[gp->rid] = gp->produced + 1;
            // (bounded: a step takes one token back at least while any request runs)
            for (int it = 0; it < 4 * (int) want.size() + 8 && !want.empty(); ++it) {
                multi_step();
                if (multi_fails != fails0) {
                    step_no_spec = false;
                    return failed();
                }
                for (auto w = want.begin(); w != want.end();) {
                    const Gen* g = nullptr;
                    for (const auto& gp : gens)
                        if (gp->rid == w->first) g = gp.get();
                    w = g == nullptr || g->done || g->produced >= w->second ? want.erase(w) : std::next(w);
                }
            }
        }
        step_no_spec = false;
        if (!multi_drain()) {
            multi_fail(err);
            return failed();
        }
        if (std::any_of(gens.begin(), gens.end(), [](const std::unique_ptr<Gen>& p) { return p->done; })) gen_sweep();
        bind_ctx(read_ctx);
        return read_stopped() ? 1 : 0;
    };

    // One decode gap of a prompt's read (STRATA_GLM_PREFILL_GAPS): at a chunk boundary inside a prompt-path call (every
    // part idle, the pool's tail lent; Glm5Model::prefill_gap) or between two of its calls.  The running requests' own
    // steps (multi_step's: the split's batched rows, which never bind a context) in rounds - each until every request
    // running at its start has a token more or ended - for percent / (100 - percent) of the time the read ran since
    // the last gap (one round at least), then every issued token back: the read goes on with the GPUs to itself.  No
    // tier boundary runs in it (in_read_gap), a request that ends meanwhile ends as it would (a PAUSE's snapshot after
    // the read: deferred_pauses), a STOP for one ends it; an engine error fails them all, the contexts' reset waits
    // for the read's end (gap_reset).  A gap that overruns <max gap ms> (its one round can, where experts stream) ends
    // the gaps of this read (gaps_over).  0: go on, 1: the read was stopped, 2: failed (prompt_between_err).
    read_gap = [&]() -> int {
        if (read_stopped()) return 1;
        const auto t_in = std::chrono::steady_clock::now();
        const double stage_ms = std::chrono::duration<double, std::milli>(t_in - gap_mark).count();
        if (gens.empty() || gaps_over) {
            gap_mark = t_in;
            return 0;
        }
        const double budget = strata::core::prefill_gap_budget_ms(read_gaps, stage_ms);
        const uint64_t fails0 = multi_fails, n0 = multi_n;
        in_read_gap = true;
        step_no_spec = true;
        int rounds = 0;
        for (bool first = true; !gens.empty() && multi_fails == fails0; first = false) {
            if (!first && std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_in).count() >=
                              budget)
                break;
            ++rounds;
            std::map<int, int64_t> want;   // rid -> the produced count that ends this round for it
            for (const auto& gp : gens)
                if (!gp->done) want[gp->rid] = gp->produced + 1;
            if (want.empty()) break;   // (every request ended: nothing to decode)
            for (int it = 0; it < 4 * (int) want.size() + 8 && !want.empty() && multi_fails == fails0; ++it) {
                multi_step();
                for (auto w = want.begin(); w != want.end();) {
                    const Gen* g = nullptr;
                    for (const auto& gp : gens)
                        if (gp->rid == w->first) g = gp.get();
                    w = g == nullptr || g->done || g->produced >= w->second ? want.erase(w) : std::next(w);
                }
            }
        }
        if (multi_fails == fails0 && !multi_drain()) multi_fail(err);
        step_no_spec = false;
        in_read_gap = false;
        gap_mark = std::chrono::steady_clock::now();
        ++gap_count;
        gap_tok += multi_n - n0;
        const double this_ms = std::chrono::duration<double, std::milli>(gap_mark - t_in).count();
        gap_ms += this_ms;
        if (rounds > 0) gap_round_ms = this_ms / rounds;   // (the drain's share too: a gap costs it once)
        if (this_ms > read_gaps.max_ms && !gaps_over) {
            // (a round is one token for every running request, whatever the budget: where their experts stream from
            // RAM or disk inside the loan it can take far longer than the stage - the rest of this read has no gaps)
            gaps_over = true;
            std::fprintf(stderr, "glm multi: a decode gap took %.0f ms (more than %.0f): no more gaps in this read\n",
                         this_ms, read_gaps.max_ms);
        }
        if (multi_fails != fails0) return 2;
        if (std::any_of(gens.begin(), gens.end(), [](const std::unique_ptr<Gen>& p) { return p->done; })) gen_sweep();
        return read_stopped() ? 1 : 0;
    };

    // STRATA_GLM_MULTI_BENCH=<n>,<prompt>,<gen>[,<boundary rounds>] (needs STRATA_GLM_SEQS >= n): n synthetic
    // prompts of <prompt> tokens, then <gen> greedy tokens each - sequence 0 alone through the token path first (the
    // reference), then all n through the selected together, split-rows or pipelined path; prints both rates and whether
    // same tokens both ways.
    if (const char* mb = getenv("STRATA_GLM_MULTI_BENCH")) {
        int n = 2, L = 512, G = 256, K = 32, same_prompt = 0;
        std::sscanf(mb, "%d,%d,%d,%d,%d", &n, &L, &G, &K, &same_prompt);
        if (n > model.seq_count() || n < 1 || L < 2 || G < 2) {
            std::fprintf(stderr, "multi bench: %d sequences asked, %d states (MAYA_GLM_SEQS)\n", n, model.seq_count());
            return 2;
        }
        model.set_host_logits(false);
        strata::kernels::SamplerParams g0 = sp;
        g0.greedy = true;
        std::vector<std::vector<int32_t>> prompts((size_t) n);
        uint64_t rs = 0x9e3779b97f4a7c15ull;
        for (int s = 0; s < n; ++s) {
            if (same_prompt) rs = 0x9e3779b97f4a7c15ull;
            for (int i = 0; i < L; ++i) {
                if (!o.tokens.empty()) {   // --tokens: a real prompt (repeated / cut to L), the same for every sequence
                    prompts[(size_t) s].push_back((int32_t) o.tokens[(size_t) i % o.tokens.size()]);
                    continue;
                }
                rs = rs * 6364136223846793005ull + 1442695040888963407ull;
                prompts[(size_t) s].push_back((int32_t) (1000 + (rs >> 33) % 120000));
            }
        }
        std::vector<float> lg;
        const auto now = [] { return std::chrono::steady_clock::now(); };
        const auto secs = [](auto a, auto b) { return std::chrono::duration<double>(b - a).count(); };
        for (int s = 0; s < n; ++s) {   // every prompt read once, its state saved before its last token
            model.seq_bind_all(s);
            model.reset();
            const std::vector<int32_t> head(prompts[(size_t) s].begin(), prompts[(size_t) s].end() - 1);
            if (!model.prefill(head, err, prompts[(size_t) s].back()) && !err.empty()) {
                std::fprintf(stderr, "multi bench: prefill: %s\n", err.c_str());
                return 1;
            }
            if (!model.snapshot_save()) {
                std::fprintf(stderr, "multi bench: snapshot\n");
                return 1;
            }
        }
        // the reference: sequence 0 alone, the classic token path - once untimed first (the tiers warm up on it, as
        // they would before any timed run; and is the token path itself repeatable here?), then timed
        std::vector<int> solo, solo2;
        model.seq_bind_all(0);
        double solo_s = 0;
        strata::core::Glm5Model::FastStats st_a, st_b;   // the expert tiers over each timed phase
        const auto tiers = [](const char* what, const strata::core::Glm5Model::FastStats& a,
                              const strata::core::Glm5Model::FastStats& b, int tokens) {
            const double h = (double) (b.hits - a.hits), m = (double) (b.misses - a.misses), t = std::max(1, tokens);
            std::printf("  %s: VRAM hit %.1f%%, a token: %.1f RAM fetches, %.2f disk reads, %.1f promotions, %.1f CPU "
                        "experts\n", what, 100.0 * h / std::max(1.0, h + m), (double) (b.ram_hits - a.ram_hits) / t,
                        (double) (b.disk_reads - a.disk_reads) / t, (double) (b.promotions - a.promotions) / t,
                        (double) (b.cpu_experts - a.cpu_experts) / t);
        };
        for (std::vector<int>* run : {&solo2, &solo}) {
            model.snapshot_restore();
            st_a = model.fast_stats();
            if (!model.forward({prompts[0].back()}, lg, err)) { std::fprintf(stderr, "multi bench: %s\n", err.c_str()); return 1; }
            const auto t0 = now();
            run->push_back(model.last_tok_);
            while ((int) run->size() < G) {
                if (!model.forward({(int32_t) run->back()}, lg, err)) { std::fprintf(stderr, "multi bench: %s\n", err.c_str()); return 1; }
                run->push_back(model.last_tok_);
            }
            solo_s = secs(t0, now());
            st_b = model.fast_stats();
        }
        const strata::core::Glm5Model::FastStats solo_a = st_a, solo_b = st_b;
        // all n through the pipelined decode
        std::vector<std::vector<int>> out((size_t) n);
        for (int s = 0; s < n; ++s) {
            model.seq_bind_all(s);
            model.snapshot_restore();
            if (!model.forward({prompts[(size_t) s].back()}, lg, err)) { std::fprintf(stderr, "multi bench: %s\n", err.c_str()); return 1; }
            out[(size_t) s].push_back(model.last_tok_);
        }
        std::deque<int> q;
        st_a = model.fast_stats();
        const auto t0 = now();
        int since = 0, boundaries = 0;
        // STRATA_GLM_MULTI_MTP: every sequence's verify windows through the batched rows (the server's order of calls)
        const bool wrows = model.mtpw_ok() && (model.rows_ok() || (split_rows > 0 && model.split_rows_ok()));
        const bool rows = model.rows_ok() && !wrows;   // one GPU: all n together, one step per token of each
        const bool srows = !rows && !wrows && split_rows > 0 && model.split_rows_ok();
        const char* path = wrows ? "mtp-windows" : rows ? "together" : srows ? "split-rows" : "pipelined";
        uint64_t w_steps = 0, w_drafts = 0, w_acc = 0;
        if (wrows) {
            const int D = strata::core::Glm5Model::multi_mtp_setting();
            std::vector<std::vector<int32_t>> drafts((size_t) n);
            struct BG {
                std::vector<int> seqs, counts;
                std::vector<int32_t> toks;
                bool back = false;
            };
            const auto back = [&](BG& b) -> bool {
                if (b.back) return true;
                b.back = true;
                const std::vector<strata::kernels::SamplerParams> sps(b.seqs.size(), g0);
                return model.mtpw_back(b.seqs.data(), b.counts.data(), (int) b.seqs.size(), b.toks.data(), sps.data(),
                                       err);
            };
            for (;;) {
                std::vector<int> act;
                for (int s = 0; s < n; ++s)
                    if ((int) out[(size_t) s].size() < G) act.push_back(s);
                if (act.empty()) break;
                std::vector<BG> gs;
                size_t first = 0;
                for (const int k : strata::core::split_groups((int) act.size(), (int) act.size(),
                                                              std::max(1, split_rows > 0 ? split_rows : n), even_groups)) {
                    BG b;
                    for (int j = 0; j < k; ++j) {
                        const int s = act[first + (size_t) j];
                        b.seqs.push_back(s);
                        b.counts.push_back(strata::core::mtp_rows_count(
                            (int) drafts[(size_t) s].size(), G - (int64_t) out[(size_t) s].size(),
                            model.seq_max_ctx(s) - 1 - model.seq_pos(s)));
                    }
                    first += (size_t) k;
                    strata::core::mtp_rows_fit(b.counts, strata::core::Glm5Model::rows_max());
                    for (size_t j = 0; j < b.seqs.size(); ++j) {
                        const int s = b.seqs[j];
                        b.toks.push_back((int32_t) out[(size_t) s].back());
                        for (int t = 1; t < b.counts[j]; ++t) b.toks.push_back(drafts[(size_t) s][(size_t) t - 1]);
                    }
                    if (!model.mtpw_front(b.seqs.data(), b.counts.data(), (int) b.seqs.size(), b.toks.data(), err)) {
                        std::fprintf(stderr, "multi bench: %s\n", err.c_str());
                        return 1;
                    }
                    gs.push_back(std::move(b));
                }
                for (size_t gi = 0; gi < gs.size(); ++gi) {
                    BG& b = gs[gi];
                    const int m = (int) b.seqs.size();
                    int rows_n = 0;
                    for (const int c : b.counts) rows_n += c;
                    std::vector<int32_t> picks((size_t) rows_n), written, last((size_t) m);
                    std::vector<int> keep((size_t) m), nd((size_t) m, D);
                    if (!back(b) || !model.mtpw_picks(b.seqs.data(), b.counts.data(), m, picks.data(), err)) {
                        std::fprintf(stderr, "multi bench: %s\n", err.c_str());
                        return 1;
                    }
                    for (int i = 0, row = 0; i < m; row += b.counts[(size_t) i], ++i) {
                        const int s = b.seqs[(size_t) i];
                        std::vector<int32_t> wr;
                        keep[(size_t) i] = strata::core::mtp_accept(
                            b.toks.data() + row, picks.data() + row, b.counts[(size_t) i],
                            [&](int, int32_t pick, int32_t&) {
                                wr.push_back(pick);
                                out[(size_t) s].push_back(pick);
                                return (int) out[(size_t) s].size() < G ? strata::core::MtpTake::more
                                                                        : strata::core::MtpTake::stop;
                            });
                        const int k = keep[(size_t) i];
                        ++w_steps;
                        w_drafts += (uint64_t) (b.counts[(size_t) i] - 1);
                        w_acc += (uint64_t) (k - 1);
                        for (int t = 0; t + 1 < k; ++t) written.push_back(wr[(size_t) t]);
                        last[(size_t) i] = wr[(size_t) k - 1];
                        // (no more drafts than the tokens left after the pending one, nor past the context)
                        nd[(size_t) i] = (int) std::max<int64_t>(
                            0, std::min<int64_t>({(int64_t) D, (int64_t) G - (int64_t) out[(size_t) s].size() - 1,
                                                  model.seq_max_ctx(s) - 2 -
                                                      (model.seq_pos(s) - b.counts[(size_t) i] + k)}));
                    }
                    std::vector<int32_t> dr((size_t) m * (size_t) std::max(1, D), -1);
                    if (!model.mtpw_commit(b.seqs.data(), b.counts.data(), keep.data(), m, err) ||
                        !model.mtpw_draft_queue(b.seqs.data(), keep.data(), m, written.data(), last.data(), nd.data(),
                                                err) ||
                        (gi + 1 < gs.size() && !back(gs[gi + 1])) || !model.mtpw_draft_take(dr.data(), err)) {
                        std::fprintf(stderr, "multi bench: %s\n", err.c_str());
                        return 1;
                    }
                    for (int i = 0; i < m; ++i) {
                        auto& d = drafts[(size_t) b.seqs[(size_t) i]];
                        d.clear();
                        for (int j = 0; j < D && dr[(size_t) i * D + (size_t) j] >= 0; ++j)
                            d.push_back(dr[(size_t) i * D + (size_t) j]);
                    }
                    since += m;
                }
                if (K > 0 && since >= K * n) {
                    if (!model.multi_boundary(err)) {
                        std::fprintf(stderr, "multi bench: %s\n", err.c_str());
                        return 1;
                    }
                    ++boundaries;
                    since = 0;
                }
            }
            std::printf("  mtp windows: %llu of %llu drafts accepted (%.1f%%), %.2f tokens a window\n",
                        (unsigned long long) w_acc, (unsigned long long) w_drafts,
                        100.0 * (double) w_acc / (double) std::max<uint64_t>(1, w_drafts),
                        (double) (w_steps + w_acc) / (double) std::max<uint64_t>(1, w_steps));
        }
        if (rows) {
            std::vector<int> seqs((size_t) n), next((size_t) n);
            std::vector<int32_t> toks((size_t) n);
            const std::vector<strata::kernels::SamplerParams> sps((size_t) n, g0);
            for (int s = 0; s < n; ++s) seqs[(size_t) s] = s;
            while ((int) out[0].size() < G) {
                for (int s = 0; s < n; ++s) toks[(size_t) s] = out[(size_t) s].back();
                if (!model.multi_rows(seqs.data(), toks.data(), sps.data(), n, next.data(), err)) { std::fprintf(stderr, "multi bench: %s\n", err.c_str()); return 1; }
                for (int s = 0; s < n; ++s) out[(size_t) s].push_back(next[(size_t) s]);
                ++boundaries;   // (one before every step, in the step itself)
            }
        }
        for (int s = 0; s < n && !rows && !srows && !wrows; ++s) {
            if (!model.multi_issue(s, out[(size_t) s].back(), g0, err)) { std::fprintf(stderr, "multi bench: %s\n", err.c_str()); return 1; }
            q.push_back(s);
        }
        if (srows) {
            std::deque<int> qg;
            const auto issue_rows = [&](const std::vector<int>& seqs) -> bool {
                // (STRATA_GLM_EVEN_GROUPS: the groups the server makes, from the sequences still decoding)
                int active = 0;
                for (int s = 0; s < n; ++s) active += (int) out[(size_t) s].size() < G;
                size_t first = 0;
                for (const int k : strata::core::split_groups((int) seqs.size(), active, split_rows, even_groups)) {
                    std::vector<int> group_seqs;
                    std::vector<int32_t> toks;
                    std::vector<strata::kernels::SamplerParams> sps((size_t) k, g0);
                    for (int j = 0; j < k; ++j) {
                        const int s = seqs[first + (size_t) j];
                        group_seqs.push_back(s);
                        toks.push_back((int32_t) out[(size_t) s].back());
                    }
                    if (!model.multi_issue_rows(group_seqs.data(), toks.data(), sps.data(), k, err)) {
                        std::fprintf(stderr, "multi bench: %s\n", err.c_str());
                        return false;
                    }
                    for (const int s : group_seqs) q.push_back(s);
                    qg.push_back(k);
                    first += (size_t) k;
                }
                return true;
            };
            std::vector<int> initial((size_t) n);
            for (int s = 0; s < n; ++s) initial[(size_t) s] = s;
            if (!issue_rows(initial)) return 1;
            while (!qg.empty()) {
                std::vector<int> again;
                const auto take_group = [&](int k) -> bool {
                    for (int j = 0; j < k; ++j) {
                        const int s = q.front();
                        q.pop_front();
                        const int tok = model.multi_take(s, err);
                        if (tok < 0) {
                            std::fprintf(stderr, "multi bench: %s\n", err.c_str());
                            return false;
                        }
                        out[(size_t) s].push_back(tok);
                        if ((int) out[(size_t) s].size() < G) again.push_back(s);
                    }
                    return true;
                };
                const int k = qg.front();
                qg.pop_front();
                if (!take_group(k)) return 1;
                since += k;
                if (K > 0 && since >= K * n) {
                    while (!qg.empty()) {
                        const int k2 = qg.front();
                        qg.pop_front();
                        if (!take_group(k2)) return 1;
                    }
                    if (!model.multi_boundary(err)) {
                        std::fprintf(stderr, "multi bench: %s\n", err.c_str());
                        return 1;
                    }
                    ++boundaries;
                    since = 0;
                }
                if (!issue_rows(again)) return 1;
            }
        }
        while (!q.empty()) {
            const int s = q.front();
            q.pop_front();
            const int tok = model.multi_take(s, err);
            if (tok < 0) { std::fprintf(stderr, "multi bench: %s\n", err.c_str()); return 1; }
            out[(size_t) s].push_back(tok);
            std::vector<int> again;
            if ((int) out[(size_t) s].size() < G) again.push_back(s);
            if (K > 0 && ++since >= K * n) {   // drain: every token in flight taken, the tiers' boundary, then on
                since = 0;
                ++boundaries;
                while (!q.empty()) {
                    const int s2 = q.front();
                    q.pop_front();
                    const int t2 = model.multi_take(s2, err);
                    if (t2 < 0) { std::fprintf(stderr, "multi bench: %s\n", err.c_str()); return 1; }
                    out[(size_t) s2].push_back(t2);
                    if ((int) out[(size_t) s2].size() < G) again.push_back(s2);
                }
                if (!model.multi_boundary(err)) { std::fprintf(stderr, "multi bench: %s\n", err.c_str()); return 1; }
            }
            for (const int s2 : again) {
                if (!model.multi_issue(s2, out[(size_t) s2].back(), g0, err)) { std::fprintf(stderr, "multi bench: %s\n", err.c_str()); return 1; }
                q.push_back(s2);
            }
        }
        const double multi_s = secs(t0, now());
        st_b = model.fast_stats();
        tiers("solo", solo_a, solo_b, G);
        tiers(path, st_a, st_b, n * G);
        const auto agree = [](const std::vector<int>& a, const std::vector<int>& b) {
            size_t k = 0;
            while (k < a.size() && k < b.size() && a[k] == b[k]) ++k;
            return k;
        };
        const size_t same = agree(solo, out[0]);
        std::printf("  solo vs its untimed warm-up run: %zu of %zu agree\n", agree(solo, solo2), solo.size());
        for (int s = 1; s < n; ++s)
            std::printf("  seq %d vs seq 0: %zu agree, vs solo: %zu\n", s, agree(out[(size_t) s], out[0]),
                        agree(out[(size_t) s], solo));
        std::printf("MULTI BENCH %d seqs, prompt %d, %d tokens each: solo %.1f tok/s | %s %.1f tok/s total, "
                    "%.1f per sequence (%d boundaries) | sequence 0 same tokens: %zu of %zu\n", n, L, G,
                    (G - 1) / solo_s, path, (double) n * (G - 1) / multi_s,
                    (G - 1) / multi_s, boundaries, same, solo.size());
        for (int s = 0; s < n; ++s) {
            std::printf("  seq %d:", s);
            for (int i = 0; i < 12 && i < (int) out[(size_t) s].size(); ++i) std::printf(" %d", out[(size_t) s][(size_t) i]);
            std::printf("\n");
        }
        std::printf("  solo :");
        for (int i = 0; i < 12 && i < (int) solo.size(); ++i) std::printf(" %d", solo[(size_t) i]);
        std::printf("\n");
        // STRATA_GLM_MULTI_DUMP=<file>: every sequence's tokens, a line each (then the solo run's) - two builds' or
        // settings' runs compared whole, not by their first 12
        if (const char* dump = getenv("STRATA_GLM_MULTI_DUMP")) {
            if (std::FILE* f = std::fopen(dump, "w")) {
                for (int s = 0; s <= n; ++s) {
                    const std::vector<int>& v = s < n ? out[(size_t) s] : solo;
                    if (s < n) std::fprintf(f, "seq %d:", s);
                    else std::fputs("solo:", f);
                    for (const int t : v) std::fprintf(f, " %d", t);
                    std::fputc('\n', f);
                }
                std::fclose(f);
            }
        }
        std::fflush(stdout);
        return 0;
    }

    if (!o.serve) {
        if (o.tokens.empty()) {
            std::fprintf(stderr, "maya generate: --tokens is required (this build has no tokenizer; see the "
                                 "header of src/program/generate.cpp)\n");
            return 2;
        }
        // STRATA_GLM_SCORE=<c>: score --tokens instead of generating.  Every token goes through the one-token decode
        // path (the tiers and the CPU lane as in a decode); the tokens after the first c are scored given the ones
        // before: mean NLL, perplexity, top-1 hits.  STRATA_GLM_SCORE_DUMP=<file> writes each scored position's
        // log-softmax (n_vocab floats); STRATA_GLM_SCORE_REF=<file> reads such a dump and reports KL(ref || this) and
        // how often both pick the same top token - the quality yardstick for engine changes and for quants.
        // STRATA_GLM_SCORE_TOPK=<file> is the compact reference (the FP8 model's, tools/maya_quant): an int32 k, then
        // per scored position k int32 token ids (best first) and their k float log-probs.  KL over those k tokens plus
        // one bucket for the rest of the vocabulary.
        if (const char* sc = getenv("STRATA_GLM_SCORE")) {
            const size_t c = (size_t) std::max(0L, std::atol(sc));
            const char* dump = getenv("STRATA_GLM_SCORE_DUMP");
            const char* ref = getenv("STRATA_GLM_SCORE_REF");
            const char* tkf = getenv("STRATA_GLM_SCORE_TOPK");
            std::FILE* df = dump && dump[0] ? std::fopen(dump, "wb") : nullptr;
            std::FILE* rf = ref && ref[0] ? std::fopen(ref, "rb") : nullptr;
            std::FILE* tf = tkf && tkf[0] ? std::fopen(tkf, "rb") : nullptr;
            int32_t tk = 0;
            if ((dump && dump[0] && !df) || (ref && ref[0] && !rf) || (tkf && tkf[0] && !tf) ||
                (tf && (std::fread(&tk, sizeof tk, 1, tf) != 1 || tk <= 0 || tk > n_vocab))) {
                std::fprintf(stderr, "maya generate: cannot open the score dump/reference\n");
                return 1;
            }
            std::vector<int32_t> tid((size_t) std::max(tk, 1));
            std::vector<float> tlp((size_t) std::max(tk, 1));
            model.set_host_logits(true);
            model.reset();
            std::vector<float> lg, ls((size_t) n_vocab), rl((size_t) n_vocab);
            double nll = 0.0, kl = 0.0, kl_max = 0.0;
            size_t n = 0, top1 = 0, same = 0, nref = 0;
            const auto ts = std::chrono::steady_clock::now();
            // STRATA_GLM_SCORE_PREFILL=1: the unscored context (all but its last token) through the prompt path, so a
            // long context is scored in minutes (the KV-cache formats at 128K); the scored tokens stay token by token
            size_t i0 = 0;
            if (getenv("STRATA_GLM_SCORE_PREFILL") != nullptr && model.fast() && model.prefill_chunk() > 0 && c > 2 &&
                c < o.tokens.size()) {
                const std::vector<int32_t> part(o.tokens.begin(), o.tokens.begin() + (long) (c - 1));
                if (!model.prefill(part, err, (int32_t) o.tokens[c - 1])) {
                    std::printf("ERR %s\n", err.empty() ? "prefill" : err.c_str());
                    return 1;
                }
                i0 = c - 1;
            }
            for (size_t i = i0; i + 1 < o.tokens.size(); ++i) {
                if (!model.forward({(int32_t) o.tokens[i]}, lg, err)) {
                    std::printf("ERR %s\n", err.c_str());
                    return 1;
                }
                if (i + 1 <= c) continue;
                float mx = lg[0];
                size_t am = 0;
                for (size_t v = 1; v < (size_t) n_vocab; ++v)
                    if (lg[v] > mx) {
                        mx = lg[v];
                        am = v;
                    }
                double se = 0.0;
                for (size_t v = 0; v < (size_t) n_vocab; ++v) se += std::exp((double) lg[v] - mx);
                const double lse = mx + std::log(se);
                for (size_t v = 0; v < (size_t) n_vocab; ++v) ls[v] = (float) (lg[v] - lse);
                const size_t nx = (size_t) o.tokens[i + 1];
                nll -= ls[nx];
                top1 += am == nx;
                ++n;
                if (df) std::fwrite(ls.data(), sizeof(float), ls.size(), df);
                if (rf && std::fread(rl.data(), sizeof(float), rl.size(), rf) == rl.size()) {
                    double k = 0.0;
                    size_t ra = 0;
                    for (size_t v = 0; v < (size_t) n_vocab; ++v) {
                        const double p = std::exp((double) rl[v]);
                        if (p > 0.0) k += p * ((double) rl[v] - (double) ls[v]);
                        if (rl[v] > rl[ra]) ra = v;
                    }
                    kl += k;
                    kl_max = std::max(kl_max, k);
                    same += ra == am;
                    ++nref;
                }
                if (tf && std::fread(tid.data(), sizeof(int32_t), (size_t) tk, tf) == (size_t) tk &&
                    std::fread(tlp.data(), sizeof(float), (size_t) tk, tf) == (size_t) tk) {
                    double k = 0.0, pr = 0.0, pt = 0.0;
                    for (int32_t j = 0; j < tk; ++j) {
                        const double p = std::exp((double) tlp[j]);
                        k += p * ((double) tlp[j] - (double) ls[(size_t) tid[j]]);
                        pr += p;
                        pt += std::exp((double) ls[(size_t) tid[j]]);
                    }
                    const double rr = std::max(1.0 - pr, 1e-12), rt = std::max(1.0 - pt, 1e-12);
                    if (pr < 1.0) k += rr * (std::log(rr) - std::log(rt));
                    kl += k;
                    kl_max = std::max(kl_max, k);
                    same += (size_t) tid[0] == am;
                    ++nref;
                }
            }
            if (df) std::fclose(df);
            if (rf) std::fclose(rf);
            if (tf) std::fclose(tf);
            const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - ts).count();
            std::printf("SCORE %zu tokens: nll %.5f ppl %.4f top1 %.2f%%", n, nll / (double) std::max<size_t>(1, n),
                        std::exp(nll / (double) std::max<size_t>(1, n)), 100.0 * (double) top1 / (double) std::max<size_t>(1, n));
            if (nref > 0)
                std::printf(" | vs ref: KL %.6f (max %.4f), same top token %.2f%%", kl / (double) nref, kl_max,
                            100.0 * (double) same / (double) nref);
            const auto stc = model.fast_stats();
            std::printf(" | %.1f ms/token | cpu lane %llu experts, vram hit %.2f%%\n",
                        1000.0 * sec / (double) std::max<size_t>(1, o.tokens.size() - 1),
                        (unsigned long long) stc.cpu_experts,
                        100.0 * (double) stc.hits / (double) std::max<uint64_t>(1, stc.hits + stc.misses));
            return 0;
        }
        const int rc = run_request(o.tokens, o.max_new, sp);
        return rc;
    }

    // STRATA_GLM_WARM_PROMPT=<n> (default 0 = off; 32 or more): one synthetic prompt through the whole part chain
    // before the server is told READY, so the session's first request does not pay what only a session's first
    // prompt pays (issue #8: it read 95-133 ms/token against 23-44 warm - the first chunk carries the one-time work:
    // every kernel's first launch, the prompt path's own allocations, the tiers' first reads).  It costs about one
    // chunk of the smallest part's size (~15 s on a four-way split, beside the tiers' own 90-151 s warm-up), its ids
    // are spread over the vocabulary so the routes are not one expert's, and the state is zeroed right after it: no
    // conversation can see it (a request with no slot in use resets anyway, before its own prompt).
    if (const char* wv = getenv("STRATA_GLM_WARM_PROMPT")) {
        const int wn = std::atoi(wv);
        if (wn > 0 && model.prefill_chunk() > 0) {
            const int wtn = std::max(32, wn);
            std::vector<int32_t> wids((size_t) wtn);
            for (int i = 0; i < wtn; ++i) wids[(size_t) i] = (int32_t) (100 + (i * 131) % 20000);
            std::fprintf(stderr, "glm fast: warming the prompt path with a %d-token prompt before READY "
                                 "(STRATA_GLM_WARM_PROMPT)\n", wtn);
            const auto wt0 = std::chrono::steady_clock::now();
            std::string werr;
            const bool wok = model.prefill(wids, werr);
            const double wms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wt0).count();
            model.reset();
            if (wok) {
                std::fprintf(stderr, "glm fast: the prompt path is warm (%d tokens in %.0f ms, %.1f ms/token)\n", wtn,
                             wms, wms / (double) wtn);
            } else {
                std::fprintf(stderr, "glm fast: the prompt warm-up did not run%s%s - the first prompt pays it\n",
                             werr.empty() ? "" : ": ", werr.c_str());
            }
        }
    }
    // issue #40: a prompt that stopped moving (the GPU idle, a host thread spinning on a flag) waited forever - the
    // server's own stall check counts a spinning thread as work.  A prompt layer or a token is the heartbeat; a big
    // chunk's layer on a slow PC takes seconds, so three minutes of none is a hang.
    start_serve_watchdog(180);
    // the VRAM left on every GPU once everything is loaded.  Below ~256 MiB (a split's later GPUs: 512 - their verify
    // windows' graphs are captured on first use) a later allocation fails, and under Windows' WDDM the driver pages GPU
    // memory to system RAM instead: decode falls to a few tok/s and a request can stall for good.  Said, never changed:
    // the line names the setting that leaves room (--vram-reserve-mib is deducted before the expert pool is sized).
    const auto pools = model.fast() ? model.pool_vram() : std::vector<strata::core::Glm5Model::PoolVram>();
    const auto free_mib_on = [](int dev) -> int64_t {
        int cur = 0;
        cudaGetDevice(&cur);
        cudaSetDevice(dev);
        size_t fb = 0, tb = 0;
        const bool ok = cudaMemGetInfo(&fb, &tb) == cudaSuccess;
        if (!ok) cudaGetLastError();
        cudaSetDevice(cur);
        return ok ? (int64_t) (fb >> 20) : -1;
    };
    for (size_t i = 0; i < pools.size(); ++i) {
        const auto& pv = pools[i];
        const int64_t fm = free_mib_on(pv.dev);
        if (fm < 0) continue;
        const int64_t ok_mib = i == 0 ? 256 : 512;
        if (fm >= ok_mib) {
            std::fprintf(stderr, "maya serve: CUDA%d: %lld MiB of VRAM free with everything loaded\n", pv.dev, (long long) fm);
        } else if (pv.adapted) {
            std::fprintf(stderr, "maya serve: WARNING: CUDA%d: %lld MiB of VRAM free with everything loaded - this card "
                                 "only just fits the model (the VRAM reserve was lowered to %d MiB so the expert pool "
                                 "fits): requests may stall. Close other programs that use the GPU, or lower "
                                 "--max-context\n", pv.dev, (long long) fm, pv.reserve_mib);
        } else {
            std::fprintf(stderr, "maya serve: CUDA%d: %lld MiB of VRAM free with everything loaded - LOW: requests may "
                                 "stall; add %s %lld to the config's args (or lower --max-context)\n", pv.dev,
                         (long long) fm, i == 0 ? "--vram-reserve-mib" : "--vram-reserve-later-mib",
                         (long long) (pv.reserve_mib + (i == 0 ? 512 : 768) - fm));
            // a number of the user's own (--expert-cache N) is not checked against the VRAM once written (auto is)
            if (pv.fixed > 0)
                std::fprintf(stderr, "maya serve: the expert pool was set with --expert-cache %lld: a smaller one - or "
                                     "--expert-cache auto, which checks the free VRAM once the pool is written - leaves "
                                     "room\n", (long long) pv.fixed);
        }
    }
    // the serve wire contract (serve/server.py): READY <max_context> [stop] once, then GEN <max_new>
    // [<key>=<value> ...] <id,id,...> lines - the sampling keys are per request and the ids are the LAST
    // token.  "stop" is NOT advertised: a request runs to its end (no mid-request cancel in M3.1).
    // "pause": PAUSE keeps the written state; "multi=<n>": n conversations at once (GEN rid=<id>, see gen_admit)
    if (multi) std::printf("READY %lld stop pause multi=%d\n", (long long) o.max_context, model.seq_count());
    else std::printf("READY %lld stop pause\n", (long long) o.max_context);
    std::fprintf(stderr, "maya generate: glm5-next serve (M3.1): GEN <max_new> [keys] <ids>, GENI <max_new> [keys] "
                         "<embeddings> <ids>; QUIT to end\n");
    // GENI: the embeddings file is one or more maya-vision records (int32 'SVE1', n, nx, ny, n_embd, then n x n_embd
    // floats) in prompt order; their rows stand in for the prompt's <|image|> tokens, in order (GLM-5.3's image
    // tokens take no positions of their own: the rows only replace the embeddings)
    constexpr int64_t kGlmImageToken = 154854;   // <|image|>
    std::vector<int32_t> lp_ctx;   // LOGP: the context the saved state belongs to (the state before its last token)
    // #477 --expert-profile-save: what the expert tiers learned, every --expert-profile-save-every minutes before the
    // next request (a prompt's lent slots are back by then) and after the last one
    Clock::time_point profile_saved_at = Clock::now();
    const auto save_profile = [&](const char* why) {
        if (o.expert_profile_save.empty() || !model.fast()) return;
        std::string e;
        if (model.save_expert_profile(o.expert_profile_save, e))
            std::fprintf(stderr, "maya serve: expert profile saved to %s (%s)\n", o.expert_profile_save.c_str(), why);
        else
            std::fprintf(stderr, "maya serve: the expert profile was not saved: %s\n", e.c_str());
        profile_saved_at = Clock::now();
    };
    std::string line;
    for (;;) {
        if (multi && !pending.empty()) admit_pending();
        if (multi && !gens.empty()) {   // requests running: a line when one is there, else one scheduler step
            if (!try_line(line)) {
                struct StepScope {
                    StepScope() { strata::core::progress().busy.store(true); }
                    ~StepScope() { strata::core::progress().busy.store(false); }
                } step_scope;
                multi_step();
                continue;
            }
        } else if (!next_line(line)) {
            break;
        }
        std::istringstream ss(line);
        std::string cmd;
        ss >> cmd;
        if (cmd == "QUIT") break;
        if (multi && !gens.empty() && (cmd == "VLEND" || cmd == "VRECLAIM" || cmd == "LOGP")) {
            std::printf("ERR %s needs the engine idle (%zu requests running)\n", cmd.c_str(), gens.size());
            continue;
        }
        if (cmd == "STOP") continue;   // a stray one between requests: ignore
        if (o.expert_profile_save_min > 0 &&
            Clock::now() - profile_saved_at >= std::chrono::duration<double>(o.expert_profile_save_min * 60.0))
            save_profile("periodic");
        // the watchdog watches a command from here until this iteration ends, whichever way it ends
        struct BusyScope {
            BusyScope() { strata::core::progress().busy.store(true); strata::core::progress_at("the request"); }
            ~BusyScope() { strata::core::progress().busy.store(false); strata::core::progress_at("idle"); }
        } busy_scope;
        // LOGP <n_ctx> <id,id,...>: the summed log-probability of the tokens from n_ctx on given those before
        // ("LP <sum> <n> <1 if every one was the most likely>") - the multiple-choice scoring of the zero-shot suite
        // (tools/maya_quant/zs_tasks.py).  A context identical to the previous request's is restored, not read again
        // (the choices of one question share it).
        // VLEND / VRECLAIM (between requests): the on-demand vision encoder is about to start on the first GPU - the
        // expert pool's tail is emptied and freed for it ("VLENT <bytes>") - and has ended ("VRECLAIMED"; an ERR while
        // its memory is not free yet: try again)
        if (cmd == "VLEND") {
            size_t lent = 0;
            if (model.vision_lend(lent, err)) std::printf("VLENT %zu\n", lent);
            else std::printf("ERR %s\n", err.c_str());
            std::fflush(stdout);
            continue;
        }
        if (cmd == "VRECLAIM") {
            if (model.vision_reclaim(err)) std::printf("VRECLAIMED\n");
            else std::printf("ERR %s\n", err.c_str());
            std::fflush(stdout);
            continue;
        }
        if (cmd == "LOGP") {
            int64_t n_ctx = 0;
            std::string ids_s;
            ss >> n_ctx >> ids_s;
            std::vector<int64_t> t64;
            std::string e;
            if (n_ctx < 2 || ids_s.empty() || !parse_i64_list(ids_s.c_str(), t64, e) || (int64_t) t64.size() <= n_ctx) {
                std::printf("ERR malformed LOGP line\n");
                std::fflush(stdout);
                continue;
            }
            std::vector<int32_t> t(t64.begin(), t64.end());
            snap_tokens.clear();                  // the saved state is LOGP's now, not a conversation's
            ckpt_drop_past(0);                    // ... and so are the caches the checkpoints stood on
            model.set_host_logits(true);
            const std::vector<int32_t> ctx(t.begin(), t.begin() + n_ctx);
            bool ok = true;
            if (!(model.fast() && ctx == lp_ctx && model.snapshot_restore())) {
                model.reset();
                std::vector<int32_t> head(t.begin(), t.begin() + n_ctx - 1);
                ok = model.prefill(head, err, t[(size_t) n_ctx - 1]);
                if (!ok && err.empty()) {   // no prompt path: the token path, 8 at a time
                    ok = true;
                    std::vector<float> lg0;
                    for (size_t i = 0; ok && i < head.size(); i += 8) {
                        const std::vector<int32_t> part(head.begin() + (long) i,
                                                        head.begin() + (long) std::min(head.size(), i + 8));
                        ok = model.forward(part, lg0, err);
                    }
                }
                lp_ctx.clear();
                if (ok && model.fast() && model.snapshot_save()) lp_ctx = ctx;
            }
            std::vector<float> lgv;
            double lp = 0.0;
            bool greedy = true;
            for (int64_t j = n_ctx; ok && j < (int64_t) t.size(); ++j) {
                ok = model.forward({t[(size_t) j - 1]}, lgv, err);
                if (!ok) break;
                float mx = lgv[0];
                size_t am = 0;
                for (size_t v = 1; v < lgv.size(); ++v)
                    if (lgv[v] > mx) { mx = lgv[v]; am = v; }
                double se = 0.0;
                for (const float x : lgv) se += std::exp((double) x - mx);
                lp += (double) lgv[(size_t) t[(size_t) j]] - mx - std::log(se);
                greedy = greedy && am == (size_t) t[(size_t) j];
            }
            model.set_host_logits(false);
            if (!ok) {
                lp_ctx.clear();
                std::printf("ERR %s\n", err.c_str());
            } else {
                std::printf("LP %.6f %lld %d\n", lp, (long long) (t.size() - (size_t) n_ctx), greedy ? 1 : 0);
            }
            std::fflush(stdout);
            continue;
        }
        if (cmd == "GEN" || cmd == "GENI") {
            int64_t max_new = 0;
            ss >> max_new;
            strata::kernels::SamplerParams rq = sp;
            bool any_key = false, greedy_req = false;
            req_think_budget = 0;
            req_think_end = -1;
            double req_pcie = -1.0;   // setup's calibration: this request's CPU lane (as the engine started: -1 / 0)
            int req_threads = 0;
            int req_chunks = 0;   // ... and its disk reads' pieces (0: as the engine started)
            double req_msw = -1.0;   // ... and its light-miss bar / first layer (STRATA_GLM_MISS_SKIP_W / _FROM; < 0: the engine's)
            int req_msf = -1;
            int req_rid = -1;
            req_spec = 0;
            req_spec_min_p = -1.0;
            req_spec_n_min = -1;
            req_spec_cum = -1.0;
            req_bias.clear();
            std::string bias_err;
            int pen_n = 0;
            float pen_repeat = 1.0f, pen_freq = 0.0f, pen_present = 0.0f;
            std::string tok2, ids, emb_path;
            while (ss >> tok2) {
                const size_t eq = tok2.find('=');
                if (eq == std::string::npos) {
                    if (cmd == "GENI" && emb_path.empty()) emb_path = tok2;   // GENI: the file, then the ids
                    else ids = tok2;   // the ids list is the last token on the line
                    continue;
                }
                const std::string k = tok2.substr(0, eq), v = tok2.substr(eq + 1);
                if (k == "temperature") {   // temperature=0 (or below): greedy, whatever else the line says
                    rq.temperature = (float) std::atof(v.c_str());
                    if (rq.temperature <= 0.0f) greedy_req = true;
                    else any_key = true;
                }
                else if (k == "top_p") { rq.top_p = (float) std::atof(v.c_str()); any_key = true; }
                else if (k == "top_k") { rq.top_k = std::atoi(v.c_str()); any_key = true; }
                else if (k == "min_p") { rq.min_p = (float) std::atof(v.c_str()); any_key = true; }
                else if (k == "seed") { rq.seed = (uint64_t) std::atoll(v.c_str()); }
                else if (k == "think_budget") { req_think_budget = std::atoll(v.c_str()); }
                else if (k == "think_end") { req_think_end = std::atoi(v.c_str()); }
                else if (k == "pcie_frac") { req_pcie = std::atof(v.c_str()); }
                else if (k == "cpu_threads") { req_threads = std::atoi(v.c_str()); }
                else if (k == "read_chunks") { req_chunks = std::atoi(v.c_str()); }
                else if (k == "miss_skip_w") { req_msw = std::atof(v.c_str()); }
                else if (k == "miss_skip_from") { req_msf = std::atoi(v.c_str()); }
                else if (k == "rid") { req_rid = std::atoi(v.c_str()); }
                else if (k == "spec") { req_spec = std::atoi(v.c_str()); }
                else if (k == "spec_min_p") { req_spec_min_p = std::atof(v.c_str()); }
                else if (k == "spec_n_min") { req_spec_n_min = std::atoi(v.c_str()); }
                else if (k == "spec_cum") { req_spec_cum = std::atof(v.c_str()); }
                else if (k == "logit_bias") {   // <id>:<bias>,... - -100 or below bans the token
                    for (size_t at = 0; at < v.size() && bias_err.empty();) {
                        size_t end = v.find(',', at);
                        if (end == std::string::npos) end = v.size();
                        const std::string e1 = v.substr(at, end - at);
                        const size_t colon = e1.find(':');
                        char* p1 = nullptr;
                        char* p2 = nullptr;
                        const long id = colon == std::string::npos ? -1 : std::strtol(e1.c_str(), &p1, 10);
                        const float b = colon == std::string::npos ? 0.0f : std::strtof(e1.c_str() + colon + 1, &p2);
                        if (id < 0 || id >= n_vocab || p1 != e1.c_str() + colon || p2 == nullptr || *p2 != '\0' ||
                            !std::isfinite(b))
                            bias_err = "malformed logit_bias entry '" + e1 + "'";
                        else
                            req_bias.emplace_back((int32_t) id, b <= -100.0f ? -INFINITY : b);
                        at = end + 1;
                    }
                }
                // the penalties (#273), llama.cpp's: over the last penalty_last_n tokens the request's state was fed
                // (its prompt, then what it wrote).  They do not choose between greedy and sampled
                else if (k == "penalty_last_n") { pen_n = std::atoi(v.c_str()); }
                else if (k == "penalty_repeat") { pen_repeat = std::strtof(v.c_str(), nullptr); }
                else if (k == "penalty_freq") { pen_freq = std::strtof(v.c_str(), nullptr); }
                else if (k == "penalty_present") { pen_present = std::strtof(v.c_str(), nullptr); }
            }
            if (any_key) rq.greedy = false;   // any sampler key switches the request to the sampled path
            if (greedy_req) rq.greedy = true;
            // a repeat penalty of 0 or below (some clients' "off"; the logits would be divided by it) or a value that is
            // not a number counts as neutral
            if (!std::isfinite(pen_repeat) || pen_repeat <= 0.0f) pen_repeat = 1.0f;
            if (!std::isfinite(pen_freq)) pen_freq = 0.0f;
            if (!std::isfinite(pen_present)) pen_present = 0.0f;
            if (pen_n < 0) pen_n = INT_MAX;   // -1, llama.cpp's "the whole context": every token the window keeps (4096)
            // as llama.cpp: no window, or every penalty neutral, is no penalty - the request's picks are exactly as
            // without the keys (no history kept, nothing uploaded)
            const bool pen_on = pen_n > 0 && (pen_repeat != 1.0f || pen_freq != 0.0f || pen_present != 0.0f);
            if (pen_on) {
                rq.penalty_last_n = pen_n;
                rq.penalty_repeat = pen_repeat;
                rq.penalty_freq = pen_freq;
                rq.penalty_present = pen_present;
            }
            if (!bias_err.empty()) {
                if (multi && req_rid >= 0) std::printf("ERR %d %s\n", req_rid, bias_err.c_str());
                else std::printf("ERR %s\n", bias_err.c_str());
                std::fflush(stdout);
                continue;
            }
            if (!req_bias.empty() && (rq.greedy || rq.temperature <= 0.0f)) {
                // greedy with a bias: the sampled path's top_k = 1 is the same argmax (ties to the lower id) over the
                // biased logits - the greedy shortcuts (the argmax at the end of a forward, the windows' argmax) read
                // the unbiased ones.  (Greedy with penalties stays greedy: every shortcut leaves a request with
                // penalties to the sampler, whose greedy kernel counts them - the same pick, ~75 us less a token)
                rq.greedy = false;
                rq.temperature = 1.0f;
                rq.top_k = 1;
                rq.top_p = 1.0f;
                rq.min_p = 0.0f;
            }
            // the CPU lane for this request: what it names, else what the engine started with
            model.set_pcie_share(req_pcie >= 0.0 && req_pcie <= 1.0 ? req_pcie : -1.0);
            model.set_cpu_lane_threads(req_threads > 0 ? req_threads : 0);
            model.set_read_chunks(req_chunks > 0 ? req_chunks : 0);
            model.set_miss_skip(req_msw, req_msf);
            std::vector<int64_t> toks;
            std::string e;
            if (max_new <= 0 || ids.empty() || !parse_i64_list(ids.c_str(), toks, e)) {
                if (multi && req_rid >= 0) std::printf("ERR %d malformed GEN line\n", req_rid);
                else std::printf("ERR malformed GEN line\n");
                continue;
            }
            lp_ctx.clear();   // a generation saves its own state over LOGP's
            cur_img_pos.clear();
            cur_img_rows.clear();
            if (cmd == "GENI") {
                const int64_t E = model.geometry().n_embd;
                std::FILE* f = std::fopen(emb_path.c_str(), "rb");
                std::string ge = f ? "" : "cannot open " + emb_path;
                while (f && ge.empty()) {
                    int32_t hdr[5];
                    // the end of the records only between two of them: a header cut short is a broken file, and a
                    // record cannot hold more rows than the prompt has tokens (not a multi-GB allocation off one)
                    const size_t got = std::fread(hdr, 1, sizeof hdr, f);
                    if (got == 0 && std::feof(f)) break;
                    if (got != sizeof hdr || hdr[0] != 0x31455653 || hdr[1] <= 0 || hdr[4] != E ||
                        (size_t) hdr[1] > toks.size() - cur_img_rows.size() / (size_t) E) {
                        ge = "not a maya-vision record for this model";
                        break;
                    }
                    const size_t at = cur_img_rows.size();
                    cur_img_rows.resize(at + (size_t) hdr[1] * (size_t) E);
                    if (std::fread(cur_img_rows.data() + at, sizeof(float) * (size_t) E, (size_t) hdr[1], f) !=
                        (size_t) hdr[1])
                        ge = "short embeddings file";
                }
                if (f) std::fclose(f);
                for (size_t i = 0; ge.empty() && i < toks.size(); ++i)
                    if (toks[i] == kGlmImageToken) cur_img_pos.push_back((int64_t) i);
                if (ge.empty() && (int64_t) cur_img_pos.size() * E != (int64_t) cur_img_rows.size())
                    ge = std::to_string(cur_img_pos.size()) + " image tokens, " +
                         std::to_string(cur_img_rows.size() / (size_t) E) + " image rows";
                if (!ge.empty()) {
                    cur_img_pos.clear();
                    cur_img_rows.clear();
                    if (multi && req_rid >= 0) std::printf("ERR %d %s\n", req_rid, ge.c_str());
                    else std::printf("ERR %s\n", ge.c_str());
                    std::fflush(stdout);
                    continue;
                }
            }
            if (multi) {
                if (req_rid < 0) std::printf("ERR this engine runs several conversations: GEN needs rid=<id>\n");
                else gen_admit(req_rid, toks, max_new, rq);
                continue;
            }
            {
                std::string be;
                rq.logit_bias = model.logit_bias_table(0, req_bias, be);
                if (!be.empty()) {
                    std::printf("ERR %s\n", be.c_str());
                    std::fflush(stdout);
                    continue;
                }
            }
            model.set_image_rows(cur_img_pos, cur_img_rows);
            run_request(toks, max_new, rq);
            model.set_image_rows({}, {});
            continue;
        }
        if (cmd.empty()) continue;
        std::printf("ERR unsupported command '%s' (GEN, GENI, LOGP, VLEND, VRECLAIM, STOP and QUIT)\n", cmd.c_str());
    }
    // the engine ends (QUIT, or the server gone): the conversation in the model is kept for the next start
    if (multi) {   // the requests still running end here (their DONE lines), then every context's conversation
        for (const Pending& w : pending) std::printf("DONE %d 0 %zu 0.0 0.0 cancel 0 0 0 0 0\n", w.rid, w.ids.size());
        pending.clear();
        multi_drain();
        for (auto& gp : gens)
            if (!gp->done) {
                gp->finish = "cancel";
                gen_finish(*gp);
            }
        gen_sweep();
        if (slot_keep && !slot_ram)
            for (int c = 0; c < (int) ctxs.size(); ++c) {
                bind_ctx(c);
                slot_put(" on shutdown");
            }
        return 0;
    }
    if (slot_keep && !slot_ram) slot_put(" on shutdown");
    save_profile("quit");   // #477
    strata::core::Glm5Model::set_routing_dump("", err);   // --dump-routing: the trace closed whole
    return 0;
}

}  // namespace

#if defined(_WIN32)
extern "C" __declspec(dllexport) int glm_main(int argc, char** argv) {
#else
extern "C" __attribute__((visibility("default"))) int glm_main(int argc, char** argv) {
#endif
    strata::platform::env_aliases();   // (MAYA_* settings under their older names too)
    // **UNBUFFERED, BECAUSE THE INTERESTING OUTPUT IS THE OUTPUT BEFORE A CRASH.**  `stdout` redirected to a
    // pipe or a file is block-buffered, so a program that dies loses every line it had already printed - which
    // turns "it crashed at step 7" into "it crashed somewhere", and the difference is a debugging session.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // Load every CUDA kernel when the context is created, before the expert cache takes the free VRAM.  With the
    // default lazy loading, a kernel first used mid-prompt (MMQ for IQ3_XXS at 64K+ on a 12 GB card) found no VRAM
    // left for its code and the engine ended ("out of memory: cudaFuncSetAttribute").  Costs ~30 MB of VRAM.
    if (std::getenv("CUDA_MODULE_LOADING") == nullptr) {
#if defined(_WIN32)
        _putenv_s("CUDA_MODULE_LOADING", "EAGER");
#else
        setenv("CUDA_MODULE_LOADING", "EAGER", 0);
#endif
    }
#if defined(_WIN32)
    // Windows' power throttling (EcoQoS) runs a process whose windows are all in the background on a hybrid CPU's
    // efficiency cores at low clocks, and the CPU lane crawled there - an i7-13700: decode 9-12 tok/s with Maya's
    // window focused, ~2 tok/s behind other windows, the same either way once `powercfg /powerthrottling disable`
    // named the engine (#114).  The engine opts out itself.  Resolved at runtime as PrefetchVirtualMemory is
    // (expert_source.cpp): the struct is PROCESS_POWER_THROTTLING_STATE (version 1; EXECUTION_SPEED managed, and off)
    // and 4 is ProcessPowerThrottling - a Windows without them refuses the call and nothing changes
    {
        struct Throttle { ULONG version, control, state; } off{1u, 0x1u, 0u};
        using SetInfoFn = BOOL (WINAPI*)(HANDLE, int, LPVOID, DWORD);
        const HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        const SetInfoFn set_info = k32 ? reinterpret_cast<SetInfoFn>(GetProcAddress(k32, "SetProcessInformation")) : nullptr;
        if (set_info != nullptr) set_info(GetCurrentProcess(), 4, &off, (DWORD) sizeof off);
    }
#endif
    Options o;
    bool have_tokens = false;
    bool have_logits_stride = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", what); std::exit(2); }
            return argv[++i];
        };
        bool handled = true;
        if (a == "--help" || a == "-h") { usage(); return 0; }
        else if (a == "--pack") o.pack = next("--pack");
        else if (a == "--glm-pack") o.glm_pack = next("--glm-pack");
        else if (a == "--glm-ram-resident") {
#if defined(_WIN32)
            _putenv_s("STRATA_GLM_RAM_RESIDENT", "1");
#else
            setenv("STRATA_GLM_RAM_RESIDENT", "1", 1);
#endif
        }
        else if (a == "--parallel") {   // the sequence states (STRATA_GLM_SEQS): n conversations at once
            const int n = std::atoi(next("--parallel"));
            if (n < 1 || n > 8) { std::fprintf(stderr, "--parallel takes 1..8\n"); return 2; }
            set_env("STRATA_GLM_SEQS", std::to_string(n).c_str());
        }
        else if (a == "--glm-ram-slack") {
            const char* v = next("--glm-ram-slack");
#if defined(_WIN32)
            _putenv_s("STRATA_GLM_RAM_SLACK", v);
#else
            setenv("STRATA_GLM_RAM_SLACK", v, 1);
#endif
        }
        else if (a == "--tokens") {
            if (have_tokens) { std::fprintf(stderr, "supply one token input only\n"); return 2; }
            std::string e;
            if (!parse_i64_list(next("--tokens"), o.tokens, e)) { std::fprintf(stderr, "%s\n", e.c_str()); return 2; }
            have_tokens = true;
        }
        else if (a == "--tokens-file") {
            if (have_tokens) { std::fprintf(stderr, "supply one token input only\n"); return 2; }
            std::ifstream input(next("--tokens-file"));
            if (!input) { std::fprintf(stderr, "cannot open token file\n"); return 2; }
            std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            if (input.bad()) { std::fprintf(stderr, "cannot read token file\n"); return 2; }
            std::string error;
            if (text.find('\0') != std::string::npos || !parse_i64_list(text.c_str(), o.tokens, error)) {
                std::fprintf(stderr, "malformed token file: %s\n", error.c_str()); return 2;
            }
            have_tokens = true;
        }
        else if (a == "--max-new") o.max_new = std::atoll(next("--max-new"));
        else if (a == "--max-context") o.max_context = std::atoll(next("--max-context"));
        else if (a == "--greedy") o.greedy = true;
        else if (a == "--seed") { o.seed = (uint64_t) std::atoll(next("--seed")); o.greedy = false; }
        else if (a == "--top-k") o.top_k = std::atoi(next("--top-k"));
        else if (a == "--top-p") o.top_p = (float) std::atof(next("--top-p"));
        else if (a == "--temperature") o.temperature = (float) std::atof(next("--temperature"));
        else if (a == "--dump-logits") o.dump_logits = next("--dump-logits");
        else if (a == "--logits-stride") {
            if (have_logits_stride) { std::fprintf(stderr, "--logits-stride must be supplied only once\n"); return 2; }
            if (!strata::program::logits_selection::parse_stride(next("--logits-stride"), o.logits_stride)) {
                std::fprintf(stderr, "--logits-stride requires a positive decimal int64\n"); return 2;
            }
            have_logits_stride = true;
        }
        else if (a == "--dump-residual") o.dump_residual = next("--dump-residual");
        else if (a == "--dump-mixed") o.dump_mixed = next("--dump-mixed");
        else if (a == "--dump-layers") o.dump_layers = next("--dump-layers");
        else if (a == "--dump-halves") o.dump_halves = next("--dump-halves");
        else if (a == "--dump-routing") o.dump_routing = next("--dump-routing");
        else if (a == "--ple-gguf") o.ple_gguf = next("--ple-gguf");
        else if (a == "--no-ple") o.no_ple = true;
        else if (a == "--ple-io") o.ple_io = next("--ple-io");
        else if (a == "--ple-row-cache") o.ple_row_cache = std::atoll(next("--ple-row-cache"));
        else if (a == "--ple-inflight") o.ple_inflight = std::atoi(next("--ple-inflight"));
        else if (a == "--ple-delay-us") o.ple_delay_us = std::atof(next("--ple-delay-us"));
        else if (a == "--ple-sync-submit") o.ple_sync_submit = true;
        else if (a == "--kv") o.kv = next("--kv");
        else if (a == "--kv-resident") o.kv_resident = std::atoll(next("--kv-resident"));
        else if (a == "--kv-streaming") o.kv_streaming = next("--kv-streaming");
        else if (a == "--stream-token") o.stream_token = true;
        else if (a == "--check-logits") o.check_logits = true;
        else if (a == "--gr-fp32-activations") o.gr_fp32_activations = true;
        else if (a == "--gr-native-mmvf") o.gr_native_mmvf = true;
        else if (a == "--native-bf16") o.native_bf16 = true;
        else if (a == "--native-bf16-extra") o.native_bf16_extra = true;
        else if (a == "--native-ple-key") o.native_ple_key = true;
        else if (a == "--native-moe-combine") o.native_moe_combine = true;
        else if (a == "--native-gdn") o.native_gdn = true;
        else if (a == "--native-flash-attn-short") o.native_flash_attn_short = true;
        else if (a == "--native-qsa-indexer") o.native_qsa_indexer = true;
        else if (a == "--native-qsa") o.native_qsa = true;
        else if (a == "--native-rope") o.native_rope = true;
        else if (a == "--native-ple-postops") o.native_ple_postops = true;
        else if (a == "--native-router") o.native_router = true;
        else if (a == "--cpu-oracle-q8-0") o.cpu_oracle_q8_0 = true;
        else if (a == "--native") o.native_preset = next("--native");
        else if (a == "--native-head-gguf") o.native_head_gguf = next("--native-head-gguf");
        else if (a == "--native-dense-gguf") o.native_dense_gguf.push_back(next("--native-dense-gguf"));
        else if (a == "--no-capture") o.no_capture = true;
        else if (a == "--no-pool") o.no_pool = true;
        else if (a == "--sync-every-layer") o.sync_every_layer = true;
        else if (a == "--stage-timing") o.stage_timing = true;
        else if (a == "--graph-only") o.graph_only = true;
        else if (a == "--gpu-only-full") o.gpu_only_full = true;
        else if (a == "--pool-workers") o.pool_workers = std::atoi(next("--pool-workers"));
        else if (a == "--no-host-worker") o.no_host_worker = true;
        else if (a == "--no-ple-prefetch") o.no_ple_prefetch = true;
        else if (a == "--expert-cache") {
            const std::string v = next("--expert-cache");
            o.expert_cache = (v == "auto") ? -1 : std::atoi(v.c_str());
        }
        else handled = false;
        if (!handled) {   // (a second chain: MSVC stops an if-else chain this long, C1061 "blocks nested too deeply")
        if (a == "--expert-cache-device1") o.expert_cache_remote[0] = std::atoi(next("--expert-cache-device1"));
        else if (a == "--expert-cache-device2") o.expert_cache_remote[1] = std::atoi(next("--expert-cache-device2"));
        else if (a == "--expert-cache-device3") o.expert_cache_remote[2] = std::atoi(next("--expert-cache-device3"));
        else if (a == "--expert-cache-remote-placement")
            o.expert_cache_remote_placement = next("--expert-cache-remote-placement");
        else if (a == "--vram-reserve-mib") {
            o.vram_reserve_mib = std::atoi(next("--vram-reserve-mib"));
            o.vram_reserve_given = true;
        }
        else if (a == "--vram-reserve-later-mib") o.vram_reserve_later_mib = std::atoi(next("--vram-reserve-later-mib"));
        else if (a == "--prefill") {
            const std::string v = next("--prefill");
            o.prefill_arg = v;
            o.prefill_auto = v == "auto";
            o.prefill_chunk = o.prefill_auto ? 8192 : std::atoll(v.c_str());
        }
        else if (a == "--no-split-rows") o.no_split_rows = true;
        else if (a == "--no-prefill-borrow") o.no_prefill_borrow = true;
        else if (a == "--prefill-until") o.prefill_until = std::atoll(next("--prefill-until"));
        else if (a == "--dump-final-r") o.dump_final_r = next("--dump-final-r");
        else if (a == "--spec") o.spec = std::atoi(next("--spec"));
        else if (a == "--spec-oracle") o.spec_oracle = next("--spec-oracle");
        else if (a == "--spec-follow") o.spec_follow = next("--spec-follow");
        else if (a == "--spec-follow-save") o.spec_follow_save = next("--spec-follow-save");
        else if (a == "--spec-corrupt") o.spec_corrupt = std::atoi(next("--spec-corrupt"));
        else if (a == "--mtp") o.mtp = next("--mtp");
        else if (a == "--mtp-window") o.mtp_window = std::atoll(next("--mtp-window"));
        else if (a == "--pcie-frac") o.pcie_frac = std::atof(next("--pcie-frac"));
        else if (a == "--adapt-every") o.adapt_every = std::atoi(next("--adapt-every"));
        else if (a == "--spec-min-p") {
            o.spec_min_p = std::atof(next("--spec-min-p"));
            o.spec_min_p_set = true;
        }
        else if (a == "--stop-eos") { o.stop_eos = true; o.eos_given = true; }
        else if (a == "--spec-split") o.spec_split = true;
        else if (a == "--layer-split") o.layer_split = next("--layer-split");
        else if (a == "--dflash") o.dflash = next("--dflash");
        else if (a == "--split-device") o.split_device = next("--split-device");
        else if (a == "--pcie-mode") o.pcie_mode = next("--pcie-mode");
        else if (a == "--serve") o.serve = true;
        else if (a == "--vision") o.vision = true;
        else if (a == "--prompt-cache") o.prompt_cache = std::max(0, std::atoi(next("--prompt-cache")));
        else if (a == "--prompt-cache-every") o.prompt_cache_every = std::max(0LL, std::atoll(next("--prompt-cache-every")));
        else if (a == "--prompt-cache-root") o.prompt_cache_root = std::max(0LL, std::atoll(next("--prompt-cache-root")));
        else if (a == "--turn-token") o.turn_token = std::atoll(next("--turn-token"));
        else if (a == "--short-read") o.short_read = std::max(0LL, std::atoll(next("--short-read")));
        else if (a == "--suffix-draft") o.suffix_draft = std::max(0, std::atoi(next("--suffix-draft")));
        else if (a == "--mtp-max-t") o.mtp_max_t = std::max(0, std::atoi(next("--mtp-max-t")));
        else if (a == "--control-vector") o.cvec_files.push_back({next("--control-vector"), 1.0f});
        else if (a == "--control-vector-scaled") {
            // FILE:SCALE, comma-separated; the LAST colon splits, so a Windows path (C:\...) keeps its drive
            std::stringstream list(next("--control-vector-scaled"));
            std::string item;
            while (std::getline(list, item, ',')) {
                const size_t colon = item.rfind(':');
                char* end = nullptr;
                const float sc = colon == std::string::npos ? 0.0f : std::strtof(item.c_str() + colon + 1, &end);
                if (colon == std::string::npos || colon == 0 || end == item.c_str() + colon + 1 || *end != '\0') {
                    std::fprintf(stderr, "--control-vector-scaled: expected FILE:SCALE, got '%s'\n", item.c_str());
                    return 2;
                }
                o.cvec_files.push_back({item.substr(0, colon), sc});
            }
        }
        else if (a == "--control-vector-layer-range") {
            o.cvec_first = std::atoi(next("--control-vector-layer-range"));
            o.cvec_last = std::atoi(next("--control-vector-layer-range"));
        }
        else if (a == "--cvec-mode") {
            const std::string m = next("--cvec-mode");
            if (m == "project") o.cvec_mode = 0;
            else if (m == "add") o.cvec_mode = 1;
            else { std::fprintf(stderr, "--cvec-mode: add or project, got '%s'\n", m.c_str()); return 2; }
        }
        else if (a == "--cvec-dir") {
            const std::string d = next("--cvec-dir");
            if (d == "per-layer") o.cvec_single = -1;
            else if (d.rfind("single:", 0) == 0) o.cvec_single = std::atoi(d.c_str() + 7);
            else { std::fprintf(stderr, "--cvec-dir: per-layer or single:L, got '%s'\n", d.c_str()); return 2; }
        }
        else if (a == "--no-spec-split") o.spec_split = false;
        else if (a == "--eos-ids") {
            std::string e;
            if (!parse_i64_list(next("--eos-ids"), o.eos_ids, e)) { std::fprintf(stderr, "--eos-ids: %s\n", e.c_str()); return 2; }
            o.stop_eos = true;
            o.eos_given = true;
        }
        else if (a == "--adapt-swaps") o.adapt_swaps = std::atoi(next("--adapt-swaps"));
        else if (a == "--expert-cache-cpu-order") o.expert_cache_cpu_order = true;
        else if (a == "--expert-cache-per-layer") o.expert_cache_per_layer = true;
        else if (a == "--no-hit-poke") o.no_hit_poke = true;
        else if (a == "--expert-profile") o.expert_profile = next("--expert-profile");
        else if (a == "--expert-profile-save") o.expert_profile_save = next("--expert-profile-save");
        else if (a == "--expert-profile-save-every")
            o.expert_profile_save_min = std::atof(next("--expert-profile-save-every"));
        else if (a == "--gpu-stages") o.gpu_stages = true;
        else if (a == "--mmap-experts") o.mmap_experts = true;
        else if (a == "--resident-cpu-experts") o.resident_cpu_experts = true;
        else if (a == "--stats") o.stats = true;
        else if (a == "--shared-late") o.shared_late = true;
        else if (a == "--keep-canonical") o.keep_canonical = true;
        else if (a == "--no-token-graph") o.no_token_graph = true;
        else if (a == "--no-fused-gr") o.no_fused_gr = true;
        else if (a == "--no-fast-attn") o.no_fast_attn = true;
        else if (a == "--no-publish-kernel") o.no_publish_kernel = true;
        else if (a == "--no-fused-gdn") o.no_fused_gdn = true;
        else if (a == "--no-fast-select") o.no_fast_select = true;
        else {
            // An unknown flag is an ERROR and not a warning: a typo'd `--max-neww` that silently generated 16
            // tokens would look like a working run.
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            usage();
            return 2;
        }
        }
    }
    // ---- the glm5-next pack path (M3.1): BEFORE every qwen4exp-shaped check below (this driver's
    // session/graph machinery is qwen4exp's; a GLM pack runs the verified Glm5Model runner instead,
    // see glm_pack_generate).  --glm-pack with an explicit non-default --pack is almost certainly a
    // mistake worth saying out loud.
    // --kv-streaming on|off: on streams with --kv-resident's window (32768 when it gives none), off keeps every cell in
    // VRAM whatever --kv-resident says
    if (!o.kv_streaming.empty()) {
        if (o.kv_streaming == "on") {
            if (o.kv_resident <= 0) o.kv_resident = 32768;
        } else if (o.kv_streaming == "off") {
            o.kv_resident = 0;
        } else {
            std::fprintf(stderr, "maya generate: --kv-streaming takes on or off (not %s)\n", o.kv_streaming.c_str());
            return 2;
        }
    }
    if (!o.glm_pack.empty()) {
        // --kv on the GLM path, read at load: int8 glm5-next's INT8 latent cache (Glm5Model::lat_q8_); q8_0 / q4_0
        // raon1's quantized cache (Glm5Model::raon1_lat_type_, STRATA_GLM_KV_TYPE - int8 names q8_0 there, fp16 its
        // F32 cache)
        if (o.kv != "fp16" && o.kv != "f32" && o.kv != "int8" && o.kv != "q8_0" && o.kv != "q4_0" && o.kv != "q4") {
            std::fprintf(stderr, "maya generate: --kv takes fp16, int8, q8_0 or q4_0 (not %s)\n", o.kv.c_str());
            return 2;
        }
        if (o.kv == "int8" && std::getenv("STRATA_GLM_KV_INT8") == nullptr) set_env("STRATA_GLM_KV_INT8", "1");
        if (std::getenv("STRATA_GLM_KV_TYPE") == nullptr) set_env("STRATA_GLM_KV_TYPE", o.kv.c_str());
        // --kv-resident N: KV streaming of the DSA latent cache (Glm5Model::kv_resident_cells, read at load)
        if (o.kv_resident < 0) {
            std::fprintf(stderr, "maya generate: --kv-resident must be 0 or more\n");
            return 2;
        }
        if (o.kv_resident > 0 && std::getenv("STRATA_GLM_KV_RESIDENT") == nullptr)
            set_env("STRATA_GLM_KV_RESIDENT", std::to_string(o.kv_resident).c_str());
        if (o.kv_streaming == "off") set_env("STRATA_GLM_KV_RESIDENT", "0");   // (off whatever the environment says)
        // the expert pool's size, set in the config's args (the engine reads them as STRATA_GLM_EXPERT_CACHE,
        // STRATA_GLM_RESERVE_MB and STRATA_GLM_RESERVE_LATER_MB; given here, they win over the environment):
        // --expert-cache auto | N (0 = auto), --vram-reserve-mib N, --vram-reserve-later-mib N
        if (o.expert_cache < -1) {
            std::fprintf(stderr, "maya generate: --expert-cache takes auto or a number of experts\n");
            return 2;
        }
        if ((o.vram_reserve_given && o.vram_reserve_mib < 0) || o.vram_reserve_later_mib < -1) {
            std::fprintf(stderr, "maya generate: --vram-reserve-mib and --vram-reserve-later-mib take 0 or more MiB\n");
            return 2;
        }
        if (o.expert_cache != 0)
            set_env("STRATA_GLM_EXPERT_CACHE", o.expert_cache < 0 ? "auto" : std::to_string(o.expert_cache).c_str());
        if (o.vram_reserve_given) set_env("STRATA_GLM_RESERVE_MB", std::to_string(o.vram_reserve_mib).c_str());
        if (o.vram_reserve_later_mib >= 0)
            set_env("STRATA_GLM_RESERVE_LATER_MB", std::to_string(o.vram_reserve_later_mib).c_str());
        if (o.pack != "pack/full") {
            std::fprintf(stderr, "maya generate: note: --glm-pack supersedes --pack (%s ignored)\n", o.pack.c_str());
        }
        return glm_pack_generate(o);
    }
    // Phase G2 serves the GLM pack via --glm-pack (the native-family driver below the dispatch in Maya's
    // generate.cpp is intentionally not vendored into this family DLL).
    std::fprintf(stderr, "glm: the GLM family DLL runs a pack: pass --glm-pack <dir>.\n");
    return 2;
}
