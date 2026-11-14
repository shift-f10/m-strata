// The engine families (plugin spec 2026-10-30 section 5, phase 2): what the server's switches on EngineProfile decided,
// one record a family, and the card options each lists (model_options.cpp reads and writes them). family_descriptor_test
// pins every value.
#include "secure_frontend/family_descriptor.hpp"

#include "secure_frontend/plugin_family.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>

namespace secure_frontend {
namespace {

using Kind = ModelOptionKind;

// --- The option tables (spec §4) -----------------------------------------------------------

constexpr ModelOptionSpec make_spec(const std::string_view name, const Kind kind, const std::string_view label,
                                    const std::string_view help) {
  ModelOptionSpec spec{};
  spec.name = name;
  spec.kind = kind;
  spec.label = label;
  spec.help = help;
  return spec;
}

constexpr ModelOptionSpec boolean_option(const std::string_view name, const std::string_view label, const std::string_view help) {
  return make_spec(name, Kind::boolean, label, help);
}

constexpr ModelOptionSpec integer_option(const std::string_view name, const std::string_view label, const std::string_view help,
                                         const std::int64_t minimum, const std::int64_t maximum, const Kind kind = Kind::integer) {
  ModelOptionSpec spec = make_spec(name, kind, label, help);
  spec.minimum = minimum;
  spec.maximum = maximum;
  return spec;
}

constexpr ModelOptionSpec decimal_option(const std::string_view name, const std::string_view label, const std::string_view help,
                                         const double minimum, const double maximum) {
  ModelOptionSpec spec = make_spec(name, Kind::decimal, label, help);
  spec.minimum_decimal = minimum;
  spec.maximum_decimal = maximum;
  return spec;
}

constexpr ModelOptionSpec choice_option(const std::string_view name, const std::string_view label, const std::string_view help,
                                        const std::span<const std::string_view> choices) {
  ModelOptionSpec spec = make_spec(name, Kind::choice, label, help);
  spec.choices = choices;
  return spec;
}

// Multi-GPU like upstream (owner 2026-10-28): the GPUs a card runs on, as nvidia-smi numbers them - upstream's "gpu"
// (serve/server.py gpu_list: several GPUs split the model's layers). The engines that split take up to 16; our Gemma 4
// and flint engines run on one (their --gpu uses the first).
constexpr ModelOptionSpec devices_option(const std::string_view help, const std::int64_t most) {
  ModelOptionSpec spec = make_spec("gpus", Kind::devices, "GPUs", help);
  spec.minimum = 1;
  spec.maximum = most;
  return spec;
}
constexpr ModelOptionSpec kGpus =
    devices_option("The graphics cards this model runs on (auto: the first); two or more split its layers across them", 16);
constexpr ModelOptionSpec kOneGpu = devices_option("The graphics card this model runs on (auto: the first); this engine uses one", 1);

// upstream/src/program/generate.cpp:3148-3151 (--kv; "q4" is only an alias of q4_0).
constexpr std::array<std::string_view, 4> kQwenKv{"fp16", "int8", "q4_0", "k8v4"};
// reasoning.effort's levels (frontend_config.cpp, the reasoning.effort row and its reader).
constexpr std::array<std::string_view, 4> kQwenThinking{"none", "low", "medium", "high"};
// Upstream's low-RAM mode (owner 2026-10-31; upstream setup.py --low-ram): auto, on, resident, mmap, off (low_ram.hpp).
constexpr std::array<std::string_view, 5> kQwenLowRam{"auto", "on", "resident", "mmap", "off"};
// Vision for cards (spec 2026-10-28-vision-for-cards-design.md): upstream setup's --vision gpu|cpu|no, as card rows.
constexpr std::array<std::string_view, 3> kVisionModes{"off", "gpu", "cpu"};
// The Gemma spec's D10: thinking off or on (reasoning.effort none or high).
constexpr std::array<std::string_view, 2> kGemmaThinking{"off", "on"};

// The page's dropdowns (owner request 2026-10-23: "make the context length a drop down menu that
// tops out to 1M, and make the kv streaming into a dropdown selection also"). Qwen's contexts run
// to the 1M upstream measured end to end (docs/DETAILS.md "Context extension past 262K"), with
// upstream's setup steps 384K and 768K (docs/INSTALL.md); Gemma's stop at its trained 256K (the
// strata_gemma4 allowlist). KV streaming: off, then resident cells from the engine's floor.
constexpr std::array<std::int64_t, 11> kQwenContexts{4096, 8192, 16384, 32768, 65536, 131072,
                                                     262144, 393216, 524288, 786432, 1048576};
constexpr std::array<std::int64_t, 7> kGemmaContexts{4096, 8192, 16384, 32768, 65536, 131072, 262144};
constexpr std::array<std::int64_t, 7> kKvResidentCells{0, 20480, 32768, 65536, 131072, 262144, 524288};
// VRAM kept free (MiB, upstream setup's --vram-reserve-mib): Off, then steps up to 8 GiB (a GPU picture encoder takes
// upstream's 700, a desktop or another program a few GiB)
constexpr std::array<std::int64_t, 9> kVramReserveMib{0, 512, 1024, 1536, 2048, 3072, 4096, 6144, 8192};

constexpr ModelOptionSpec listed(ModelOptionSpec spec, const std::span<const std::int64_t> values,
                                 const std::string_view zero_label = {}, const std::string_view at_most = {}) {
  spec.values = values;
  spec.zero_label = zero_label;
  spec.at_most = at_most;
  return spec;
}

// The sampling options carry the defaults.sampling.* rows' bounds (frontend_config.cpp, SP5 M1
// Task 4). top_p's row is 0 < top_p <= 1 (minimum_exclusive): the smallest double above zero
// makes the same set an inclusive bound, so the card's and the page's checks stay one compare.
constexpr ModelOptionSpec kTemperature =
    decimal_option("temperature", "Temperature", "Default sampling temperature for requests that set none (0-2)", 0.0, 2.0);
constexpr ModelOptionSpec kTopP = decimal_option("top_p", "Top-p", "Default nucleus mass for requests that set none (above 0, at most 1)",
                                                 std::numeric_limits<double>::denorm_min(), 1.0);
constexpr ModelOptionSpec kTopK =
    integer_option("top_k", "Top-k", "Default number of candidate tokens for requests that set none (1-64)", 1, 64);
constexpr ModelOptionSpec kMinP =
    decimal_option("min_p", "Min-p", "Default minimum token probability, relative to the likeliest, for requests that set none (0 = off)",
                   0.0, 1.0);
// Owner 2026-10-28 ("a toggle in the card that can enable search"): the profiles with the server's tool (the chat
// route's tool formats). It writes no flag and no row; the launcher reads it at a start (ModelLauncher::run_start).
constexpr ModelOptionSpec kWebSearch =
    boolean_option("web_search", "Web search", "Let this model search the web through the server's SearXNG while it answers");

// Owner 2026-10-29 ("make the default timeout configurable per model on the card"): the longest one answer may take,
// in minutes, as the card's limits.engine.generation_timeout_s (its bounds: up to 86,400 s, a day); the launcher offers
// it at the server's own row. It writes no flag.
constexpr std::array<std::int64_t, 9> kAnswerMinutesValues{2, 5, 10, 15, 30, 60, 120, 240, 1440};
constexpr ModelOptionSpec kAnswerMinutes = listed(
    integer_option("answer_minutes", "Longest answer (minutes)",
                   "How long one answer may take before the server stops it (it then ends as length); a thinking model's "
                   "long answers need more",
                   1, 1440),
    kAnswerMinutesValues);

// Pinned and hot models (owner 2026-10-31, spec part 2): Keep loaded - a card kept hot never unloads when it idles (a
// GLM-engine card's glm.idle_unload_s is 0 for its start; no other card idle-unloads). Every engine's, off unless its
// card's file lists it on.
constexpr ModelOptionSpec kHot =
    boolean_option("hot", "Keep loaded", "Never unload this model when it idles, so it is always ready");

// The accuracy trade-offs (main, 2026-11): the speed options that may change results, each with the value that
// restores the accuracy baseline; in every table after its GPUs.
constexpr ModelOptionSpec tradeoff(ModelOptionSpec spec, const std::string_view baseline, const std::string_view at_most = {}) {
  spec.accuracy_tradeoff = true;
  spec.accuracy_default = baseline;
  spec.at_most = at_most;
  return spec;
}
constexpr ModelOptionSpec kTailSkip = tradeoff(integer_option("speed_route_tail_skip", "Skip uncached tail experts",
    "May reduce accuracy: omit uncached experts at this zero-based route rank or later; 0 computes every selected expert", 0, 9), "0");
constexpr ModelOptionSpec kRouteResident = tradeoff(decimal_option("speed_route_resident", "Residency routing margin",
    "May reduce accuracy: replace model-selected tail experts with cached experts within this logit margin; only 512-expert top-10 routers; 0 disables", 0, 1), "0");
constexpr ModelOptionSpec kRoutePrior = tradeoff(decimal_option("speed_route_prior", "Cached-expert routing bonus",
    "May reduce accuracy: bias every routing rank toward cached experts; only 512-expert top-10 routers; combines with residency margin; 0 disables", 0, 1), "0");
constexpr ModelOptionSpec kResidentMtp = tradeoff(boolean_option("speed_route_resident_mtp", "Bias draft routing toward cache",
    "Experimental: uses the final layer's cache for draft routing; requires positive residency margin, an active MTP drafter and one GPU; may lower acceptance"), "false");
constexpr ModelOptionSpec kHcReq8 = tradeoff(boolean_option("speed_hc_req8", "Requantize hyper-connections to INT8",
    "May reduce accuracy: requantize BF16 hyper-connection weights to INT8 to free expert-cache memory; only hyper-connection models on one GPU"), "false");
constexpr ModelOptionSpec kSpecProb = tradeoff(boolean_option("speed_spec_prob", "Experimental sampled draft acceptance",
    "May change sampling probabilities: confidence-gated sampled drafts are not distribution-preserving; requires sampled requests and an active MTP drafter"), "false");
constexpr ModelOptionSpec kTrunkKeep = tradeoff(integer_option("speed_glm_trunk_keep", "Uncached trunk experts to keep",
    "May reduce accuracy: omit uncached experts outside these leading routing ranks; 8 computes every selected expert", 1, 8), "8");
constexpr ModelOptionSpec kDiskFrom = tradeoff(integer_option("speed_glm_disk_skip_from", "Skip disk experts from rank",
    "May reduce accuracy: omit disk-only trunk experts from this zero-based routing rank onward; 8 disables", 0, 8), "8");
constexpr ModelOptionSpec kDiskWeight = tradeoff(decimal_option("speed_glm_disk_skip_w", "Disk-expert weight cutoff",
    "May reduce accuracy: omit disk-only trunk experts below this fraction of route weight; 0 disables", 0, 1), "0");
constexpr ModelOptionSpec kMissWeight = tradeoff(decimal_option("speed_glm_miss_skip_w", "Uncached-expert weight cutoff",
    "May reduce accuracy: omit every non-VRAM trunk expert below this fraction of route weight, including RAM experts; 0 disables", 0, 1), "0");
constexpr ModelOptionSpec kMtpKeep = tradeoff(integer_option("speed_glm_mtp_keep", "Uncached draft experts to keep",
    "Omit draft-only experts outside these leading ranks; verified output stays with the target but acceptance and speed may worsen; 8 keeps all", 0, 8), "8");
constexpr ModelOptionSpec kDeferCpu = tradeoff(boolean_option("speed_glm_defer_cpu", "Defer CPU expert results",
    "May reduce accuracy: add CPU expert results after the next layer's attention; requires a CPU expert lane and uncached experts"), "false");
constexpr ModelOptionSpec kDsDisk = tradeoff(decimal_option("speed_ds4_disk_skip_w", "Disk-expert weight cutoff",
    "May reduce accuracy: omit disk-only experts below this route-weight fraction during decode and verification, without renormalizing; 0 disables", 0, 1), "0");
constexpr ModelOptionSpec kDsRam = tradeoff(decimal_option("speed_ds4_ram_skip_w", "Uncached-expert weight cutoff",
    "May reduce accuracy: omit non-VRAM experts below this route-weight fraction, even when RAM holds them; 0 disables", 0, 1), "0");
constexpr ModelOptionSpec kDsSample = tradeoff(boolean_option("speed_ds4_dspark_sample", "Experimental sampled DSpark drafts",
    "May change sampling probabilities: confidence-gated sampled proposals are not distribution-preserving; requires a loaded DSpark drafter and sampled requests"), "false");
constexpr ModelOptionSpec kDsYoco = tradeoff(integer_option("speed_ds4_yoco", "Prompt replay mode",
    "0 disables pruning; 1 uses exact dependency-based pruning; 2 replays only the attention window and may reduce accuracy on long prompts", 0, 2), "1");
constexpr ModelOptionSpec kDsKeep = tradeoff(integer_option("speed_ds4_yoco_keep", "Prompt replay rows",
    "May reduce accuracy: positive values override exact replay with bounded history, at least one attention window and at most context; 0 uses the selected replay mode", 0, 1048576), "0", "max_context");
constexpr ModelOptionSpec kFastMath = tradeoff(boolean_option("speed_fast_math", "CUDA fast math",
    "May change numerical accuracy: selects the bundled fast-math engine on the next model start; model weights and INT8/Q8 cache settings stay unchanged"), "false");

// The upstream Strata engine (strata_0143). Flags, units and limits as its argument parser reads
// them (upstream/src/program/generate.cpp): --max-context :2428 (positive, below 2^31 :3254-3258;
// the frontend's cap 1,048,576 is glm.max_context's); --kv-resident :2464 (>= 0 :3162); --kv-grow
// :2465; --spec :2591 (the verify window: below 2 the draft layer is ignored :5403-5404, above
// kVerifyMaxT = 8 the drafter refuses, core/mtp.cpp:266); --spec-min-p :2613 (a probability the
// draft must reach, core/mtp.cpp:1689); --expert-cache :2541-2544 (auto or a count of expert blobs
// in VRAM, >= -1 :3304; at most the 48 x 512 experts there are, core/geometry.hpp:25/:53);
// --prefill :2558-2565 (auto or a chunk of N prompt tokens; auto's largest is 32,768, :2564).
constexpr std::array<ModelOptionSpec, 28> kQwenOptions{{
    listed(integer_option("max_context", "Context length",
                          "Tokens of context the engine allocates; past the trained 256K the engine stretches the model with "
                          "YaRN rope scaling (experimental, upstream's own rule)",
                          1, 1048576),
           kQwenContexts),
    choice_option("kv", "KV cache precision",
                  "Attention cache: fp16 preserves precision; int8, q4_0 and k8v4 save memory and can change results", kQwenKv),
    listed(integer_option("kv_resident", "KV streaming",
                          "Attention-cache tokens kept in VRAM, the rest streamed from RAM (Off: all of it in VRAM); at most "
                          "the context",
                          0, 1048576),
           kKvResidentCells, "Off", "max_context"),
    boolean_option("kv_grow", "Grow the KV cache", "Take VRAM for the attention cache as the context fills instead of all at start"),
    boolean_option("mtp", "MTP draft layer", "Guess tokens ahead with the model's draft layer"),
    integer_option("spec", "Draft window", "Tokens the engine proposes per verify step (2-8)", 2, 8),
    decimal_option("spec_min_p", "Draft confidence", "How sure the draft layer must be to extend a window by another guess (0-1)",
                   0.0, 1.0),
    integer_option("expert_cache", "Expert cache", "Expert blobs kept in VRAM, or auto to fill what fits", 0, 24576,
                   Kind::integer_or_auto),
    integer_option("prefill", "Prompt chunk", "Prompt tokens processed per batch, or auto for the largest that fits", 1, 32768,
                   Kind::integer_or_auto),
    choice_option("low_ram", "Low-RAM mode",
                  "When this PC's RAM cannot hold the model's experts with 10 GB beside them, read them from the model's files: "
                  "auto decides by this PC's RAM and GPU (upstream's rule), on always, resident keeps the ones the GPU does not "
                  "hold in RAM, mmap reads them through the file cache, off keeps them all in RAM",
                  kQwenLowRam),
    // owner 2026-11-01 (the Azure VM's T4: "it also may need low vram mode"): upstream setup's --vram-reserve-mib - the
    // strata core's own flag for a pack; a Qwen3.6 card's engine has none, so the launcher gives its experts the GPU's
    // VRAM less 3 GiB and this (--expert-vram), setup.py's rule for that family
    listed(integer_option("vram_reserve", "VRAM kept free",
                          "MiB of VRAM left for other programs, so the model takes that much less (Off: the engine uses what "
                          "it finds free); a Qwen3.6 model's experts then get the card's VRAM less 3 GiB and this, as upstream's "
                          "setup gives them",
                          0, 65536),
           kVramReserveMib, "Off"),
    choice_option("thinking", "Thinking", "The thinking level for requests that name none", kQwenThinking),
    kTemperature,
    kTopP,
    kTopK,
    kMinP,
    choice_option("vision", "Pictures",
                  "Read pictures with the model's picture encoder: on the GPU (faster, takes about 1 GB of VRAM), on the CPU, or off",
                  kVisionModes),
    kAnswerMinutes,
    kHot,
    kWebSearch,
    kGpus,
    kTailSkip, kRouteResident, kRoutePrior, kResidentMtp, kHcReq8, kSpecProb,
    kFastMath,
}};

// Our strata-gemma4-engine (strata_gemma4): the bounds are kGemma4Flags (engine_arguments.cpp). Web search since
// 2026-10-28 (Gemma 4's own tool form). The MTP switch since 2026-10-28 (the owner: "gemma4 has an mtp ... make sure we
// work that mtp in our gemma engine"): a card that has the model's MTP draft file lists it; off drops the card's
// --mtp <file> for that start (write_option); its window and confidence are the engine's --spec and --spec-min-p (the
// latter only while the switch is on).
constexpr std::array<ModelOptionSpec, 17> kGemmaOptions{{
    listed(integer_option("max_context", "Context length", "Tokens of context the engine allocates (up to the model's trained 256K)",
                          1, 262144),
           kGemmaContexts),
    integer_option("chunk", "Prompt chunk", "Prompt tokens processed per batch", 1, 8192),
    integer_option("expert_vram", "Expert VRAM", "GiB of VRAM for the experts, or auto to let the engine decide", 0, 1024,
                   Kind::integer_or_auto),
    boolean_option("mtp", "MTP draft model", "Guess tokens ahead with the model's MTP draft file (off: the engine runs without it)"),
    integer_option("spec", "Draft window", "Tokens checked per step with the MTP draft model: its guesses plus one (2-8)", 2, 8),
    decimal_option("spec_min_p", "Draft confidence", "How sure the MTP draft model must be to guess another token (0-1; 0 always guesses)",
                   0.0, 1.0),
    choice_option("thinking", "Thinking", "Whether requests that name no thinking level think", kGemmaThinking),
    kTemperature,
    kTopP,
    kTopK,
    kMinP,
    kAnswerMinutes,
    kHot,
    kWebSearch,
    kOneGpu,
    kTailSkip,
    kFastMath,
}};

// multiengine_flint (strata_flint): the Gemma engine's options, bounded by kFlintFlags (engine_arguments.cpp); its
// trained context is 256K too.
constexpr std::array<ModelOptionSpec, 14> kFlintOptions{{
    listed(integer_option("max_context", "Context length", "Tokens of context the engine allocates (up to the model's trained 256K)",
                          1, 262144),
           kGemmaContexts),
    integer_option("chunk", "Prompt chunk", "Prompt tokens processed per batch", 1, 8192),
    integer_option("expert_vram", "Expert VRAM", "GiB of VRAM for the experts, or auto to let the engine decide", 0, 1024,
                   Kind::integer_or_auto),
    choice_option("thinking", "Thinking", "Whether requests that name no thinking level think", kGemmaThinking),
    kTemperature,
    kTopP,
    kTopK,
    kMinP,
    kAnswerMinutes,
    kHot,
    kWebSearch,
    kOneGpu,
    kTailSkip,
    kFastMath,
}};

// Maya's GLM-5.3 engine (multiengine_maya, the glm53 profile, launcher spec D10 met 2026-10-27): its launch is
// composed from typed rows, never engine.args, so each option writes the row the composition reads (write_option).
// Contexts: Maya's menu (maya.py v1.1.30 CONTEXTS, DEFAULT_CONTEXT 32768) inside glm.max_context's bounds; --kv: the
// GLM engine's vocabulary (glm.kv, glm_driver.cpp); no thinking option (the GLM prompt opens its think block always).
constexpr std::array<std::int64_t, 4> kGlmContexts{8192, 32768, 65536, 131072};
constexpr std::array<std::string_view, 4> kGlmKv{"fp16", "int8", "q8_0", "q4_0"};
// The RAM the GLM engine leaves free when it fills RAM with experts (owner 2026-10-29: "make it an option in the card
// setting"): glm.ram_headroom_gb's bounds (STRATA_GLM_RAM_HEADROOM_GB, glm_fast_path.cu); Maya's own is 6 GB with a
// separate GPU - measured twice as fast on GLM-5.3 here as the server's 16, which keeps a desktop responsive.
constexpr std::array<std::int64_t, 8> kRamHeadroomValues{6, 8, 12, 16, 24, 32, 48, 64};
constexpr ModelOptionSpec kRamHeadroom = listed(
    integer_option("ram_headroom", "RAM left free (GB)",
                   "RAM the engine leaves for the rest of the PC when it fills RAM with experts: less is faster (6 is Maya's own "
                   "with a separate GPU), more keeps the desktop responsive",
                   6, 1024),
    kRamHeadroomValues);
constexpr std::array<ModelOptionSpec, 18> kGlmOptions{{
    listed(integer_option("max_context", "Context length",
                          "Tokens of context the engine allocates (Maya's choices; past 32K the attention cache streams from RAM)",
                          1, 1048576),
           kGlmContexts),
    choice_option("kv", "KV cache precision", "Attention cache: fp16 preserves precision; int8, q8_0 and q4_0 save memory and can change results",
                  kGlmKv),
    kRamHeadroom,
    kTemperature,
    kTopP,
    kTopK,
    kMinP,
    kAnswerMinutes,
    kHot,
    kWebSearch,
    kGpus,
    kTrunkKeep, kDiskFrom, kDiskWeight, kMissWeight, kMtpKeep, kDeferCpu,
    kFastMath,
}};

// raon-1-mini on our port of Maya's GLM engine (multiengine_raon1, the raon1 profile; owner 2026-10-27: its card from
// its files): the same typed rows as glm53 (glm.max_context, glm.kv) up to the 256K it was served at; the cache in the
// GLM engine's vocabulary (fp16 keeps raon1's exact F32 cache); KRAFTON's sampling as the card's defaults.
constexpr std::array<std::int64_t, 5> kRaon1Contexts{8192, 32768, 65536, 131072, 262144};
constexpr std::array<ModelOptionSpec, 18> kRaon1Options{{
    listed(integer_option("max_context", "Context length", "Tokens of context the engine allocates (its full attention keeps them all)",
                          1, 1048576),
           kRaon1Contexts),
    choice_option("kv", "KV cache precision", "Attention cache: fp16 uses raon1's exact cache; int8, q8_0 and q4_0 save memory and can change results",
                  kGlmKv),
    kRamHeadroom,
    kTemperature,
    kTopP,
    kTopK,
    kMinP,
    kAnswerMinutes,
    kHot,
    kWebSearch,
    kGpus,
    kTrunkKeep, kDiskFrom, kDiskWeight, kMissWeight, kMtpKeep, kDeferCpu,
    kFastMath,
}};

// DeepSeek-V4.1 on Maya's maya-ds4 (2026-10-27, spec 2026-10-27-strata-prepare-design §7.2): Maya's own choices - its
// context menu (maya.py v1.1.30 DS4_CONTEXTS; maya-ds4's own --max-context, in engine.args by the maya_ds4 allowlist),
// the thinking level (DS4_EFFORT "high"; DeepSeek's effort line: low 50, medium and high 75, none chat mode), sampling.
constexpr std::array<std::int64_t, 6> kDeepseekContexts{32768, 65536, 131072, 262144, 524288, 1048576};
constexpr std::array<ModelOptionSpec, 16> kDeepseekOptions{{
    listed(integer_option("max_context", "Context length", "Tokens of context the engine allocates (Maya's choices)", 1, 1048576),
           kDeepseekContexts),
    choice_option("thinking", "Thinking", "The thinking level for requests that name none (DeepSeek's reasoning effort)",
                  kQwenThinking),
    kTemperature,
    kTopP,
    kTopK,
    kMinP,
    kAnswerMinutes,
    kHot,
    kWebSearch,  // DSML calls, as Maya renders and reads them (owner 2026-10-28)
    kGpus,
    kDsDisk, kDsRam, kDsSample, kDsYoco, kDsKeep,
    kFastMath,
}};

// MiniMax-M3.1-Flash on Maya's maya-mm3 (owner 2026-10-28): Maya's own choices - its context menu (maya.py v1.1.30
// MM3_CONTEXTS, MM3_DEFAULT_CONTEXT 64K), the KV cache formats Maya's setup sizes (MM3_KV_BYTES; MM3_KV_FORMAT int8),
// the thinking level (MM3_EFFORT "auto": MiniMax's adaptive thinking, no effort named), MiniMax's sampling (its model
// card's temperature and top_p, no top-k or min-p). The context and the cache are maya-mm3's own flags in engine.args.
constexpr std::array<std::int64_t, 6> kMinimaxContexts{32768, 65536, 131072, 262144, 524288, 1048576};
constexpr std::array<std::string_view, 4> kMinimaxKv{"bf16", "int8", "fp8", "fp4"};
constexpr std::array<std::string_view, 5> kMinimaxThinking{"auto", "none", "low", "medium", "high"};
constexpr std::array<ModelOptionSpec, 10> kMinimaxOptions{{
    listed(integer_option("max_context", "Context length",
                          "Tokens of context the engine allocates (Maya's choices; past 256K not yet run end to end)", 1, 1048576),
           kMinimaxContexts),
    choice_option("kv", "KV cache precision", "Attention cache: bf16 preserves precision; int8, fp8 and fp4 save memory and can change results", kMinimaxKv),
    choice_option("thinking", "Thinking",
                  "The thinking level for requests that name none (auto: MiniMax decides when to think)", kMinimaxThinking),
    kTemperature,
    kTopP,
    kAnswerMinutes,
    kHot,
    kWebSearch,  // its namespaced XML calls, as Maya renders and reads them (owner 2026-10-28)
    kGpus,
    kFastMath,
}};

constexpr std::array<std::string_view, 1> kDeepseekFlags{"--max-context"};
constexpr std::array<std::string_view, 2> kMinimaxFlags{"--max-context", "--kv-cache"};
constexpr std::array<std::string_view, 12> kQwenFlags{"--max-context", "--rope-scaling", "--rope-scale",   "--kv",
                                                      "--kv-resident", "--kv-grow",      "--spec",         "--spec-min-p",
                                                      "--expert-cache", "--prefill",     "--vram-reserve-mib", "--expert-vram"};
constexpr std::array<std::string_view, 3> kGemmaFlags{"--max-context", "--chunk", "--expert-vram"};   // flint's too
// Gemma 4's: flint's, and its MTP drafter's window and confidence (owner 2026-10-28)
constexpr std::array<std::string_view, 5> kGemma4Flags{"--max-context", "--chunk", "--expert-vram", "--spec", "--spec-min-p"};

// Each family from the defaults (FamilyDescriptor's), setting what differs.
constexpr FamilyDescriptor qwen() {
  FamilyDescriptor family{};
  family.profile = EngineProfile::qwen;
  family.id = "qwen";
  family.chat_profile = NativeChatProfile::qwen;
  family.protocol_flavor = EngineProtocolFlavor::strata_0143;
  family.environment_family = EngineFamily::strata;
  family.arguments_profile = EngineArgumentsProfile::strata_0143;
  family.tokenizer_mode = TokenizerMode::byte_bpe;
  family.tokenizer_source = TokenizerSource::gguf_for_cards;  // a managed launch reads its pack
  family.composes_strata_tails = true;
  family.reads_images = true;
  family.effort_end_tail = true;
  family.reads_conversation_cache = true;
  family.download_template_card = true;
  family.options = kQwenOptions;
  family.option_controlled_flags = kQwenFlags;
  family.sampling.top_k = 20;
  // Qwen's trained positions: past them a start adds YaRN with factor context / 262,144, upstream's
  // own setup rule (docs/INSTALL.md "How much context?"; generate.cpp's --yarn-orig-ctx default).
  family.yarn_trained_context = 262144;
  family.draft_layer = DraftLayerKind::in_model;
  return family;
}

constexpr FamilyDescriptor glm53() {
  FamilyDescriptor family{};
  family.profile = EngineProfile::glm53;
  family.id = "glm53";
  family.chat_profile = NativeChatProfile::glm53;
  family.protocol_flavor = EngineProtocolFlavor::maya_118;
  family.environment_family = EngineFamily::glm;
  family.arguments_profile = EngineArgumentsProfile::lighttransport_glm53;
  family.tokenizer_mode = TokenizerMode::glm_bpe;  // byte_bpe's files, Maya's glm4 split and ignore_merges (byte_bpe's qwen35 split is not GLM's)
  family.per_request_reasoning_effort = false;  // glm53 refuses the field: GLM-5.3's template has no effort levels
  family.glm_pack_rows = true;
  family.shipped_expert_profile = "expert-profile-glm.bin";
  family.options = kGlmOptions;  // (its options set typed rows, never flags)
  family.maya_pack_model_file = true;
  return family;
}

constexpr FamilyDescriptor gemma4() {
  FamilyDescriptor family{};
  family.profile = EngineProfile::gemma4;
  family.id = "gemma4";
  family.chat_profile = NativeChatProfile::gemma4;
  family.protocol_flavor = EngineProtocolFlavor::strata_0143;  // strata-gemma4-engine speaks upstream's strata_0143 serve protocol (Gemma 4 spec 4.7)
  family.environment_family = EngineFamily::strata;  // the strata family's closed allowlist (Gemma 4 spec 6.1, 8)
  family.arguments_profile = EngineArgumentsProfile::strata_gemma4;
  family.tokenizer_mode = TokenizerMode::spm_bpe;
  family.tokenizer_source = TokenizerSource::gguf;
  family.per_request_reasoning_effort = true;  // none is thinking off, low/medium/high thinking on (Gemma 4 spec 6.6)
  family.refuses_qwen_launch_rows = true;
  family.engine_words = "the Gemma engine";
  family.template_words = "Gemma's template";
  family.checks_mtp_side_file = true;
  family.calibrates_before_first_start = true;
  family.options = kGemmaOptions;
  family.option_controlled_flags = kGemma4Flags;
  family.sampling.top_k = 64;
  family.sampling.min_p = 0.05;
  family.thinking_write = ThinkingWrite::on_off;  // the Gemma spec's D10: off or on (reasoning.effort none or high)
  family.mtp_off = MtpOff::drop_card_flag;         // the Gemma engine has no engine.mtp row
  family.spec_min_p_places = 3;                    // the Gemma engine's allowlist takes at most three places
  family.draft_layer = DraftLayerKind::file;       // its MTP drafter (owner 2026-10-28)
  family.draft_spec = 4;
  family.draft_spec_min_p = 0.6;
  return family;
}

constexpr FamilyDescriptor deepseek() {
  FamilyDescriptor family{};
  family.profile = EngineProfile::deepseek;
  family.id = "deepseek";
  family.chat_profile = NativeChatProfile::deepseek;
  family.protocol_flavor = EngineProtocolFlavor::maya_118;  // Maya's maya-ds4: GLM's maya_118 dialect with READY <ctx> stop
  family.environment_family = EngineFamily::strata;  // it takes its GPUs on the command line (--gpus)
  family.arguments_profile = EngineArgumentsProfile::maya_ds4;
  family.tokenizer_mode = TokenizerMode::deepseek_bpe;  // byte_bpe's files, Maya's deepseek3 splits (strata_tokenizer.py)
  family.per_request_reasoning_effort = true;  // as DeepSeek's encoder: none is chat mode, a level the effort line
  family.sweep_keep_margin = 0.05;
  family.options = kDeepseekOptions;
  family.option_controlled_flags = kDeepseekFlags;  // maya-ds4's --max-context
  family.sampling.top_p = 1.0;                       // upstream's MODEL_CARD_SAMPLING: DeepSeek-V4.1 1.0 / 1.0
  family.maya_pack_model_file = true;
  return family;
}

constexpr FamilyDescriptor raon1() {
  FamilyDescriptor family{};
  family.profile = EngineProfile::raon1;
  family.id = "raon1";
  family.chat_profile = NativeChatProfile::raon1;
  family.protocol_flavor = EngineProtocolFlavor::maya_118;  // it rides the glm_pack engine (strata_glm, arch-detected): glm53's serve protocol
  family.environment_family = EngineFamily::glm;  // the glm env-variable mechanism, like glm53
  family.arguments_profile = EngineArgumentsProfile::lighttransport_glm53;
  family.per_request_reasoning_effort = true;  // its <think> block: none closes it (a direct answer), a level opens it
  family.tokenizer_mode = TokenizerMode::raon_bpe;  // byte_bpe's files, raon-1-mini's NFC + pre-tokenizer (byte_bpe's split gave ~46% more tokens)
  family.glm_pack_rows = true;
  family.options = kRaon1Options;  // (its options set typed rows, never flags)
  family.sampling.top_k = 20;
  family.maya_pack_model_file = true;
  return family;
}

constexpr FamilyDescriptor flint() {
  FamilyDescriptor family{};
  family.profile = EngineProfile::flint;
  family.id = "flint";
  family.chat_profile = NativeChatProfile::flint;
  family.protocol_flavor = EngineProtocolFlavor::strata_0143;  // multiengine_flint: Gemma 4's serve loop
  family.environment_family = EngineFamily::strata;  // argv --gpu, like gemma4
  family.arguments_profile = EngineArgumentsProfile::strata_flint;
  family.tokenizer_mode = TokenizerMode::flint_bpe;  // byte_bpe's files, llama.cpp's flint pre-tokenizer (three regexes over its collapsed text)
  family.tokenizer_source = TokenizerSource::gguf;
  family.per_request_reasoning_effort = true;  // none closes the think block, a level leaves it to the model
  family.refuses_qwen_launch_rows = true;
  family.engine_words = "the flint engine";
  family.template_words = "flint's template";
  family.calibrates_before_first_start = true;
  family.options = kFlintOptions;
  family.option_controlled_flags = kGemmaFlags;  // --max-context, --chunk, --expert-vram
  family.sampling.top_k = 20;
  family.thinking_write = ThinkingWrite::on_off;
  return family;
}

constexpr FamilyDescriptor minimax() {
  FamilyDescriptor family{};
  family.profile = EngineProfile::minimax;
  family.id = "minimax";
  family.chat_profile = NativeChatProfile::minimax;
  family.protocol_flavor = EngineProtocolFlavor::maya_118;  // Maya's maya-mm3: maya-ds4's wire
  family.environment_family = EngineFamily::strata;  // maya-mm3's --gpus, like maya-ds4
  family.arguments_profile = EngineArgumentsProfile::maya_mm3;
  family.tokenizer_mode = TokenizerMode::minimax_bpe;  // byte_bpe's files, NFC and Maya's minimax split (strata_tokenizer.py MINIMAX_PATTERN)
  family.per_request_reasoning_effort = true;  // none is thinking off, a level MiniMax's <effort>
  family.shipped_expert_profile = "expert-profile-mm3.bin";
  family.sweep_keep_margin = 0.05;
  family.prefill_calibration = true;
  family.options = kMinimaxOptions;
  family.option_controlled_flags = kMinimaxFlags;  // maya-mm3's --max-context, --kv-cache
  family.kv_flag = "--kv-cache";                    // maya-mm3's own
  family.thinking_write = ThinkingWrite::verbatim_auto_unset;  // "auto": MiniMax's adaptive thinking, no level named
  family.maya_pack_model_file = true;
  return family;
}

constexpr std::array<FamilyDescriptor, 7> kFamilies{{qwen(), glm53(), gemma4(), deepseek(), raon1(), flint(), minimax()}};

// (the table is in EngineProfile's order: family_descriptor indexes it)
constexpr bool in_order() {
  for (std::size_t i = 0; i < kFamilies.size(); ++i) {
    if (static_cast<std::size_t>(kFamilies[i].profile) != i) return false;
  }
  return kFamilies.size() == static_cast<std::size_t>(EngineProfile::minimax) + 1U;
}
static_assert(in_order(), "one descriptor a profile, in EngineProfile's order");

}  // namespace

std::span<const FamilyDescriptor> family_descriptors() noexcept { return kFamilies; }

const FamilyDescriptor& family_descriptor(const EngineProfile profile) noexcept {
  if (static_cast<int>(profile) < kFirstPluginProfile) return kFamilies[static_cast<std::size_t>(profile)];
  const auto* plugin = plugin_family(profile);
  // (a plugin's profile comes only from family_named, so its family is there; were it not, nothing may run as another)
  if (plugin == nullptr) std::abort();
  return plugin->descriptor();
}

std::vector<const FamilyDescriptor*> all_families() {
  std::vector<const FamilyDescriptor*> out;
  for (const auto& family : kFamilies) out.push_back(&family);
  for (const auto* plugin : plugin_families()) out.push_back(&plugin->descriptor());
  return out;
}

std::string family_choices() {
  const auto families = all_families();
  std::string words;
  for (std::size_t i = 0; i < families.size(); ++i) {
    if (i > 0) words += i + 1 == families.size() ? " or " : ", ";
    words += families[i]->id;
  }
  return words;
}

const FamilyDescriptor* family_named(const std::string_view id) noexcept {
  for (const auto& family : kFamilies) {
    if (family.id == id) return &family;
  }
  const auto* plugin = plugin_family_named(id);
  return plugin == nullptr ? nullptr : &plugin->descriptor();
}

const ModelOptionSpec* server_card_option(const std::string_view name) noexcept {
  for (const auto* spec : {&kTemperature, &kTopP, &kTopK, &kMinP, &kAnswerMinutes, &kHot, &kWebSearch}) {
    if (spec->name == name) return spec;
  }
  return nullptr;
}

}  // namespace secure_frontend
