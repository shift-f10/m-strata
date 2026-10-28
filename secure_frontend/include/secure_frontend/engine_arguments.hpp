#pragma once

#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/model_inventory.hpp"
#include "secure_frontend/tokenizer_pack.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace secure_frontend {

// Which engine CLI the argv tail is built for. `unified` is the merged engine (deepseek, raon1):
// a closed allowlist of the merged engine's own flags, with --native/--gpu frontend-composed.
// strata_flint is multiengine_flint's own serve flags (engine/src/flint/f_cli.cpp), a closed allowlist like Gemma's.
enum class EngineArgumentsProfile { strata_0143, lighttransport_glm53, strata_gemma4, unified, strata_flint, maya_ds4, maya_mm3 };

enum class EngineArgumentsErrorCode { owned_flag, unsupported_mode, invalid_value, model_not_found, not_accepted, out_of_range };

// `message` is a fixed string; it never contains user-supplied argument text or paths.
struct EngineArgumentsError final {
  EngineArgumentsErrorCode code;
  std::string message;
};

using EngineArgumentsResult = std::variant<std::vector<std::string>, EngineArgumentsError>;

// SP5 M1 Task 5 (parity row `effort_position`): reasoning.effort_position "end" composes the
// engine's --tail-role-token flag with the tokenizer's single "system" token id
// (effort_end_args serve/server.py:2836-2856, appended to the engine argv at :7966/:7977). The
// probe rules are the pin's: the engine binary must contain the flag literal, and the tokenizer's
// plain encoding of "system" must be exactly one token; otherwise nothing composes and `notice`
// carries the pin's fixed fallback line (:2853). A composed pair comes with the pin's fixed
// success line (:2856). Neither notice names a path.
struct EffortEndArguments final {
  std::vector<std::string> arguments;
  std::string_view notice;
};

[[nodiscard]] EffortEndArguments compose_effort_end_arguments(const std::filesystem::path& engine_binary,
                                                              const TokenizerPack& tokenizer);

// SP5 M2 Task 7 (D-8): the typed values Maya's launcher composes into the GLM
// engine's argv. max_context is Maya's --max-context (its DEFAULT_CONTEXT is 32768,
// maya.py:3148; the 1-1048576 bound is added, C-1). parallel is replicas.parallel
// rendered --parallel N; Maya's launcher only sets the flag when a value is given
// (maya.py:2291) and its CLI admits 1-8 (choices=range(1,9), maya.py:3885).
// --expert-profile stays with a manifest role that schema 1 does not carry (ruling:
// the composition omits it until a glm53 manifest names one).
struct Glm53LaunchArgs final {
  std::int64_t max_context{32768};  // glm.max_context (Maya's DEFAULT_CONTEXT, maya.py:3148)
  std::int64_t parallel{1};         // replicas.parallel (1: the flag is not rendered)
  std::string kv{"fp16"};           // glm.kv: the engine's --kv (fp16, its default: not rendered)
  // Maya's shipped GLM expert profile (UTF-8; the composition sets it for glm53 when the install has it beside the
  // engine, maya.py write_config's rule): --expert-profile after the base
  std::optional<std::string> expert_profile{};
};

// Builds the engine argv tail (everything after `--serve`).
//
// strata_0143: the user's pass-through arguments, validated, followed by
// `--native <models_root/relative_artifact_path>` for the selected manifest,
// where models_root is the canonical root the inventory used (defect e).
// Rejects any user argument that names a flag the frontend owns (--native,
// --serve) or a mode whose protocol the frontend does not speak (--batch,
// --slots, --vision), in both `--flag value` and `--flag=value` spellings.
// `--pack DIR` (the expert directory, loaded unconditionally by the engine,
// default cwd-relative `pack/full`) and every other weight flag (--ple-gguf,
// --embd-gguf, --native-head-gguf, --native-dense-gguf, --mtp) pass through:
// they are operator-supplied until SP1/SP5 model the whole launch.
// `--gpu` is owned too (spec §3.9): GPUs are selected only by the typed `gpus`
// list (engine.gpu), rendered `--gpu i,j` before `--native`; the engine sets
// CUDA_DEVICE_ORDER/CUDA_VISIBLE_DEVICES (or HIP) itself (upstream
// generate.cpp:2357-2375, usage :801, at v0.2.2), so the strata
// family's environment carries no GPU variable.
//
// lighttransport_glm53 (SP5 M2 Task 7, D-8): the pass-through is retired — any
// user argument is refused (the parity table's mapped-away launcher-flag rows
// land here instead). The argv is composed from the selected model's directory
// (its pack) and the typed Glm53LaunchArgs, in Maya's launcher form:
// `--glm-pack <pack> --max-context <N> --prefill auto --kv-streaming on`
// (maya.py:2228-2230), with `--parallel <N>` last only for 2..8 (:2291, :3885).
// A non-empty `gpus` list stays refused (unsupported_mode): the GLM runner takes
// no --gpu; the typed glm.gpu list reaches the child's environment (Task 4).
//
// In both profiles, values containing NUL are rejected.
[[nodiscard]] EngineArgumentsResult build_engine_arguments(EngineArgumentsProfile profile,
                                                           const ModelManifest& selected,
                                                           const CanonicalRoot& models_root,
                                                           const std::vector<std::string>& user_arguments,
                                                           const std::vector<unsigned>& gpus = {},
                                                           bool mtp_on = true,
                                                           const Glm53LaunchArgs& glm = {});

// SP5 M1 Task 6 (parity row mtp; strata_control/draft_layer.py): MTP_FLAGS verbatim —
// {"--mtp": value, "--spec-min-p": value, "--batch-mtp": no value} — and the pin's
// strip() rule: a value is consumed only when the next token is not another flag.
// Returns the draft layer's own flags the composition dropped (in order), for the pin's
// fixed startup line (serve/server.py:2925). `--spec` (prompt lookup) stays.
[[nodiscard]] std::vector<std::string> draft_layer_removed(const std::vector<std::string>& arguments);

// The same strip applied in place; returns the flags removed. The managed composition
// uses this directly (its argv tail is composed, not passed through build_engine_arguments).
[[nodiscard]] std::vector<std::string> strip_draft_layer_arguments(std::vector<std::string>& arguments);

// SP5 M1 Task 7 (parity rows vram_reserve_mib / vram_elastic / vram_segment_mib /
// layer_split / split_skip_if_fits): the typed values the pin renders into the engine's
// argv (runconfig.py:42-43; serve/server.py:2938-2948, 2896-2899). `user_arguments` is
// the operator's args: every rendered flag is skipped when the operator already owns it
// (the pin's "not in args" guards), and the layer-split gate needs the typed GPU count.
// Ruling T7-a: this family reaches the engine through argv only — no GPU/elastic
// environment variables (SP2 §3.9; the engine renders CUDA/HIP from --gpu itself).
struct VramGpuElasticArgs final {
  std::optional<std::int64_t> vram_reserve_mib;  // unset: the engine's own 700 stays
  bool vram_elastic{false};
  std::optional<std::int64_t> vram_segment_mib;  // unset: the engine's own 512 stays
  std::string layer_split{"auto"};               // already validated (layer_split_value grammar)
  bool split_skip_if_fits{false};
};

void append_vram_gpu_elastic_arguments(std::vector<std::string>& arguments, const VramGpuElasticArgs& vram,
                                       const std::vector<std::string>& user_arguments, const std::vector<unsigned>& gpus);

// engine.hip_ordinal (serve/server.py:3016-3030): the recorded HIP number wins over one
// (or zero) configured GPUs, never over several; it reaches the engine through --gpu
// (Ruling T7-f — our design owns the device list, the engine renders HIP from it).
[[nodiscard]] std::vector<unsigned> effective_gpu_list(const std::vector<unsigned>& gpus,
                                                       std::optional<std::int64_t> hip_ordinal);

// SP5 M1 Task 9 (parity rows expert_profile_save / expert_profile_save_every;
// serve/server.py:3000-3005): a configured learned-profile save path composes
// --expert-profile-save and the every-minutes re-save (the pin's default 10, 0 = at QUIT
// only, :2996). The pin's own guard is kept: no path, or an operator-owned
// --expert-profile-save in argv, leaves the arguments unchanged — not even the every (:3001).
struct ExpertProfileSaveArgs final {
  std::string path;                 // session.expert_profile_save (empty: no flags)
  std::int64_t every{10};           // session.expert_profile_save_every (minutes; 0 = at QUIT only)
};

void append_expert_profile_save_arguments(std::vector<std::string>& arguments, const ExpertProfileSaveArgs& save,
                                          const std::vector<std::string>& user_arguments);

// SP5 M1 Task 10 (parity row parallel; serve/replicas.py + serve/server.py:2955-2975, the
// C-4 narrowing): replicas.parallel 2..N decodes together INSIDE the one engine child —
// the pin renders it as --batch N (llama.cpp's historical name, :2975). <=1: nothing (the
// pin's "1 or absent: one at a time"); an operator --batch or --slots wins (:2961). The
// value is forwarded even past PARALLEL_MAX 8 (:2954): the engine warns and adjusts,
// recommend never force (:2969-2974).
struct ParallelBatchArgs final {
  std::int64_t parallel{1};  // replicas.parallel (1: one request at a time)
};

void append_parallel_batch_arguments(std::vector<std::string>& arguments, const ParallelBatchArgs& parallel,
                                     const std::vector<std::string>& user_arguments);

// Parses the operator's --gpu value: comma-separated decimal device indices
// 0-255, no sign, spaces or leading zeros, distinct, 1-16 entries.
[[nodiscard]] std::optional<std::vector<unsigned>> parse_gpu_list(std::string_view value);

// Checks only the operator's pass-through arguments (no model needed): NUL,
// frontend-owned flags and unsupported modes for `profile`. build_engine_arguments
// calls it; the config validator calls it so a bad engine.args fails before startup.
[[nodiscard]] std::optional<EngineArgumentsError> validate_engine_user_arguments(
    EngineArgumentsProfile profile, const std::vector<std::string>& user_arguments);

}  // namespace secure_frontend
