#include "secure_frontend/launch_composition.hpp"

#include "secure_frontend/engine_arguments.hpp"
#include "secure_frontend/model_index_store.hpp"
#include "secure_frontend/model_inventory.hpp"
#include "secure_frontend/canonical_path.hpp"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace secure_frontend {
namespace {

// upstream setup.py's VISION["gpu"]["reserve_mib"]: the VRAM a GPU picture encoder keeps free beside the engine
constexpr std::int64_t kGpuEncoderReserveMib = 700;

// engine.vram_reserve_mib, else - with a GPU picture encoder - upstream's 700 MiB (serve/server.py #1322), given on the
// command line so the engine does not shrink it on a small card
std::optional<std::int64_t> engine_vram_reserve(const FrontendConfig& config) {
  if (config.engine.vram_reserve_mib.has_value() || !(config.vision.enabled && config.vision.gpu)) return config.engine.vram_reserve_mib;
  return kGpuEncoderReserveMib;
}

LaunchFailure config_failure(std::vector<std::string> messages) { return LaunchFailure{ExitCode::usage, std::move(messages)}; }

LaunchFailure store_failure(const AcquireError& error) { return LaunchFailure{exit_class(error.reason), {format_error(error)}}; }

bool is_weight_flag(const std::string& argument) {
  return std::any_of(kWeightFlags.begin(), kWeightFlags.end(), [&](const std::string_view flag) {
    return argument == flag || (argument.size() > flag.size() && argument.starts_with(flag) && argument[flag.size()] == '=');
  });
}

std::variant<ComposedLaunch, LaunchFailure> expert_launch(const FrontendConfig& config) {
  if (auto issues = launch_config_issues(config, true); !issues.empty()) return config_failure(std::move(issues));
  const EngineProfile profile = config.engine.profile.value_or(EngineProfile::qwen);
  auto models = enumerate_local_models(config.roots.models);
  if (const auto* failure = std::get_if<ModelInventoryError>(&models)) {
    return config_failure({"Unable to inventory local models (roots.models): " + failure->message});
  }
  const auto& list = std::get<std::vector<ModelManifest>>(models);
  const std::string& id = config.engine.model_id.value();
  const auto selected = std::find_if(list.begin(), list.end(), [&](const ModelManifest& m) { return m.id == id; });
  if (selected == list.end()) return config_failure({"Selected model is not present in roots.models."});
  const auto arguments_profile = arguments_profile_for(profile);
  // SP5 M1 Task 7 (engine.hip_ordinal, serve/server.py:3023-3029): the recorded HIP number
  // wins over a single (or absent) card — for the strata profile and the Gemma engine (Gemma
  // 4 plan 1 Task 3: --gpu as qwen); the GLM runner takes no --gpu at all. Same rule at the
  // managed path.
  std::vector<unsigned> gpus;
  switch (profile) {
    // qwen, gemma4 and flint select GPUs by argv --gpu (the engine renders CUDA/HIP from it), deepseek by maya-ds4's
    // own --gpus (build_engine_arguments); only the GLM runner takes none.
    case EngineProfile::qwen:
    case EngineProfile::gemma4:
    case EngineProfile::flint:
    case EngineProfile::deepseek:
    case EngineProfile::minimax: gpus = effective_gpu_list(config.engine.gpus, config.engine.hip_ordinal); break;
    // The glm_pack engine (glm53, raon1) takes no --gpu (GPUs come from the glm env mechanism).
    case EngineProfile::glm53:
    case EngineProfile::raon1: gpus = config.engine.gpus; break;
  }
  Glm53LaunchArgs glm{config.glm.max_context, config.replicas.parallel, config.glm.kv};
  if (profile == EngineProfile::glm53 || profile == EngineProfile::minimax) {
    // Maya's shipped expert profile, where the install put it beside the engine (engine/profiles), as Maya's setup adds
    // it to every GLM config (maya.py v1.1.30 write_config) and every MiniMax one (write_config_mm3's MM3_PROFILE) when it
    // has one; raon1's experts are another model's
    const auto shipped = config.engine.executable.path().parent_path() / "profiles" /
                         (profile == EngineProfile::glm53 ? "expert-profile-glm.bin" : "expert-profile-mm3.bin");
    std::error_code ec;
    if (std::filesystem::is_regular_file(shipped, ec)) glm.expert_profile = path_to_utf8(std::filesystem::path{shipped}.make_preferred());
  }
  auto args = build_engine_arguments(arguments_profile, *selected, config.roots.models, config.engine.args, gpus,
                                     config.engine.mtp, glm);
  if (const auto* failure = std::get_if<EngineArgumentsError>(&args)) {
    return config_failure({"Engine arguments rejected: " + failure->message});
  }
  auto argv = std::get<std::vector<std::string>>(std::move(args));
  // Gemma 4 plan 1 Task 3: the qwen engine's composed tails are qwen's only. The Gemma
  // engine's argv is its allowlisted engine.args, --gpu and --native (build_engine_arguments);
  // the config refuses the typed rows that would compose these tails on gemma4 by name.
  const bool qwen_tails = [profile] {
    switch (profile) {
      case EngineProfile::qwen: return true;
      // The qwen VRAM/elastic/expert-save/--batch tails are upstream strata's; deepseek (maya-ds4) composes its own
      // allowlist with --pack and --vision off, raon1 the GLM runner's base, like glm53/gemma4/flint their own.
      case EngineProfile::glm53:
      case EngineProfile::gemma4:
      case EngineProfile::deepseek:
      case EngineProfile::minimax:  // maya-mm3: its allowlist, --pack, --profile, --gpus, --vision off
      case EngineProfile::raon1:
      case EngineProfile::flint: return false;  // multiengine_flint: its allowlist + --gpu + --native
    }
    return false;
  }();
  if (qwen_tails) {  // Ruling T7-a: this family reaches the engine through argv only
    const VramGpuElasticArgs vram{.vram_reserve_mib = engine_vram_reserve(config),
                                  .vram_elastic = config.engine.vram_elastic,
                                  .vram_segment_mib = config.engine.vram_segment_mib,
                                  .layer_split = config.engine.layer_split,
                                  .split_skip_if_fits = config.engine.split_skip_if_fits};
    append_vram_gpu_elastic_arguments(argv, vram, config.engine.args, gpus);
    // SP5 M1 Task 9 (serve/server.py:3000-3005): the learned-profile save pair, guarded by
    // the pin's operator-owns check.
    append_expert_profile_save_arguments(
        argv, ExpertProfileSaveArgs{config.session.expert_profile_save, config.session.expert_profile_save_every},
        config.engine.args);
    // SP5 M1 Task 10 (serve/server.py:2955-2975): engine-internal parallelism renders --batch N.
    append_parallel_batch_arguments(argv, ParallelBatchArgs{config.replicas.parallel}, config.engine.args);
  }
  std::vector<std::string> served;  // the inventory ids; /v1/models lists only the served model (SP3 E9 settled it)
  served.reserve(list.size());
  for (const auto& model : list) served.push_back(model.id);
  // SP5 M1 Task 6 (parity row mtp): the removals can only come from the operator args.
  // The GLM profile keeps its arguments verbatim (its runner does not speak these flags).
  std::vector<std::string> mtp_removed;
  if (!config.engine.mtp && qwen_tails) mtp_removed = draft_layer_removed(config.engine.args);
  // Vision for model cards (spec 2026-10-28): the card's encoder file, held to the model folders' rule, and the model's
  // own GGUF for it; the engine is told it reads images (--vision, upstream's engine flag). qwen only: the families
  // upstream's engine reads pictures with.
  std::optional<std::string> vision_mmproj;
  std::optional<std::string> vision_model;
  if (config.vision.enabled) {
    if (profile != EngineProfile::qwen) return config_failure({"vision: only a qwen model reads images here"});
    const std::filesystem::path& mmproj = *config.vision.mmproj;  // set: compose_engine_launch refused otherwise
    auto folder = open_root(mmproj.parent_path(), config.roots.allow_untrusted_media ? RootPolicy::external : RootPolicy::existing);
    std::error_code ec;
    if (!std::holds_alternative<CanonicalRoot>(folder) || !std::filesystem::is_regular_file(mmproj, ec) ||
        std::filesystem::is_symlink(mmproj, ec)) {
      return config_failure({"vision.mmproj: the picture encoder file is missing, a link, or in a folder other users of this PC "
                             "can change"});
    }
    vision_mmproj = path_to_utf8(std::filesystem::path{mmproj}.make_preferred());
    vision_model = path_to_utf8((config.roots.models.path() / path_from_utf8(selected->relative_artifact_path)).make_preferred());
    if (std::find(argv.begin(), argv.end(), "--vision") == argv.end()) argv.emplace_back("--vision");
  }
  ComposedLaunch launch{profile, std::move(argv), config.roots.tokenizer_pack, id, std::move(served), false,
                        std::move(mtp_removed), std::move(vision_mmproj), std::move(vision_model)};
  if (!launch.tokenizer_root) {
    // launch_config_issues let this through only for a tokenizer_from_gguf profile: the selected model's file
    auto file = parse_relative_path(selected->relative_artifact_path);
    if (!std::holds_alternative<RelativePath>(file)) return config_failure({"engine.model_id: the model's file name is not usable"});
    launch.tokenizer_gguf = std::get<RelativePath>(std::move(file));
  }
  return launch;
}

// <models>/<version_dir>/<relative> through the P1 grammar; nullopt if the joined path is not P1-valid.
std::optional<std::filesystem::path> in_version(const CanonicalRoot& models, const RelativePath& version_dir,
                                                const std::string& relative) {
  const auto parsed = parse_relative_path(version_dir.generic() + "/" + relative);
  const auto* path = std::get_if<RelativePath>(&parsed);
  if (path == nullptr) return std::nullopt;
  return models.join(*path);
}

}  // namespace

bool tokenizer_from_gguf(const EngineProfile profile, const bool card) noexcept {
  // owner 2026-10-27: a Qwen card's too (upstream's GGUFs hold the same vocabulary, types and merges its
  // strata_tokenizer.py copies into a pack) - a model card's only: the operator's single-model --expert keeps its pack
  return profile == EngineProfile::flint || profile == EngineProfile::gemma4 || (card && profile == EngineProfile::qwen);
}

std::vector<std::string> launch_config_issues(const FrontendConfig& config, const bool expert) {
  std::vector<std::string> issues;
  if (expert) {
    if (!config.roots.tokenizer_pack && !tokenizer_from_gguf(config.engine.profile.value_or(EngineProfile::qwen), config.models.from_card)) {
      issues.emplace_back("roots.tokenizer_pack: required with --expert");
    }
    return issues;
  }
  if (config.engine.profile) {
    issues.emplace_back(
        "engine.profile: engine_profile_conflicts_with_manifest (the model's manifest sets the profile; only --expert takes it)");
  }
  if (config.roots.tokenizer_pack) {
    issues.emplace_back("roots.tokenizer_pack: a managed model brings its own tokenizer; only --expert takes this key");
  }
  if (std::any_of(config.engine.args.begin(), config.engine.args.end(), is_weight_flag)) {
    issues.emplace_back("engine.args: weight flags (--native, --pack, --ple-gguf, --embd-gguf, --native-head-gguf, "
                        "--native-dense-gguf, --mtp) come from the manifest; only --expert takes them");
  }
  if (!parse_model_reference(config.engine.model_id.value())) {
    issues.emplace_back("engine.model_id: must be name or name:tag of an installed model (lowercase, not index.json)");
  }
  return issues;
}

std::vector<std::string> managed_config_issues(const FrontendConfig& config) { return launch_config_issues(config, false); }

std::variant<ComposedLaunch, LaunchFailure> compose_engine_launch(const FrontendConfig& config, const bool expert) {
  // SP5 M1 Task 11 (Ruling T11-aa): --expert is the operator path — no manifest, no mmproj
  // artifact — so vision fails there by name before anything else is composed.
  // Vision for model cards (spec 2026-10-28-vision-for-cards-design.md): an expert launch - every card's - reads images
  // with the card's own encoder file, vision.mmproj.
  if (expert && config.vision.enabled && !config.vision.mmproj) {
    return LaunchFailure{ExitCode::usage,
                         {"vision: the supervised encoder needs a managed model (its mmproj comes from the "
                          "manifest; --expert has none) or a card's vision.mmproj"}};
  }
  if (expert) return expert_launch(config);
  if (auto issues = managed_config_issues(config); !issues.empty()) return config_failure(std::move(issues));
  const auto reference = parse_model_reference(config.engine.model_id.value());  // valid: checked above
  auto read = read_model_index(config.roots.models);
  if (const auto* failure = std::get_if<AcquireError>(&read)) return store_failure(*failure);
  auto found = find_installed(std::get<ModelIndex>(read), *reference);  // F-58: the one lookup
  if (const auto* failure = std::get_if<AcquireError>(&found)) {
    if (failure->reason == AcquireReason::model_unknown) {
      return LaunchFailure{exit_class(failure->reason), {format_error(*failure) + "; install it with strata pull or strata import"}};
    }
    return LaunchFailure{ExitCode::usage, {"engine.model_id: " + format_error(*failure)}};
  }
  const auto& entry = std::get<IndexEntry>(found);
  auto loaded = load_installed(config.roots.models, entry);
  if (const auto* failure = std::get_if<AcquireError>(&loaded)) return store_failure(*failure);
  const auto& model = std::get<InstalledModel>(loaded);
  const auto& m = model.manifest.manifest;
  if (auto gate = engine_gate(m)) return LaunchFailure{ExitCode::engine, {format_error(*gate)}};
  // SP5 M2 Task 7 (R-T3-a): the config could not see a managed model's profile (the manifest
  // decides it), so glm.* rows validated at load; here the manifest's own profile refuses them.
  if (config.glm.rows_present && m.profile != ManifestProfile::glm53) {
    return LaunchFailure{ExitCode::usage,
                         {"glm.*: only a glm53 engine takes these rows; this managed model's manifest profile is " +
                          std::string{profile_name(m.profile)}}};
  }
  // SP5 M1 Task 11 (D-9, Ruling T11-aa): vision is fail-closed on the manifest artifact.
  if (config.vision.enabled && !m.launch.mmproj) {
    return LaunchFailure{ExitCode::manifest,
                         {"vision: this model's manifest declares no mmproj artifact (turn vision.enabled off, "
                          "or install a model that reads images)"}};
  }
  // The pin's config conflict, runconfig.py:97-98: lazy loading is text-only.
  if (m.launch.mmproj && config.engine.lazy_load) {
    return LaunchFailure{ExitCode::usage,
                         {"vision: lazy loading is text-only, and this model reads images "
                          "(engine.lazy_load with a vision model)"}};
  }
  const std::string ref = model_ref_text(entry.name, entry.tag);
  const auto inconsistent = [&ref] {
    return store_failure(AcquireError{AcquireReason::index_inconsistent, ref + ": launch paths"});
  };
  // qwen manifests always carry native, pack and tokenizer_pack (manifest rules); fail closed anyway.
  if (!m.launch.native || !m.launch.pack || !m.launch.tokenizer_pack) return inconsistent();
  const auto native = in_version(config.roots.models, model.version_dir, *m.launch.native);
  const auto pack = in_version(config.roots.models, model.version_dir, *m.launch.pack);
  const auto tokenizer = in_version(config.roots.models, model.version_dir, *m.launch.tokenizer_pack);
  const auto ple = m.launch.ple_gguf ? in_version(config.roots.models, model.version_dir, *m.launch.ple_gguf) : std::nullopt;
  if (!native || !pack || !tokenizer || (m.launch.ple_gguf && !ple)) return inconsistent();
  // SP5 M1 Task 11 (D-9): the encoder artifact and the weights it reads, joined exactly
  // like --native; carried whether or not vision.enabled (doctor and the boot read them).
  std::optional<std::string> vision_mmproj;
  std::optional<std::string> vision_model;
  if (m.launch.mmproj) {
    const auto mmproj = in_version(config.roots.models, model.version_dir, *m.launch.mmproj);
    if (!mmproj) return inconsistent();
    vision_mmproj = path_to_utf8(*mmproj);
    vision_model = path_to_utf8(*native);
  }
  // UTF-8, never path::string(): on Windows that narrows through the ANSI code
  // page, and the engine launcher decodes every argument as UTF-8 (SP1 rule, F-7).
  std::vector<std::string> args{"--native", path_to_utf8(*native), "--pack", path_to_utf8(*pack)};
  if (ple) {
    args.emplace_back("--ple-gguf");
    args.push_back(path_to_utf8(*ple));
  }
  if (const auto rejected = validate_engine_user_arguments(EngineArgumentsProfile::strata_0143, config.engine.args)) {
    return config_failure({"engine.args: " + rejected->message});
  }
  args.insert(args.end(), config.engine.args.begin(), config.engine.args.end());
  // SP5 M1 Task 6 (parity row mtp; pin :2922 "applied at every start"): an explicit
  // false drops the draft layer's own options from the composed argv, and the removed
  // flags are reported once at startup.
  std::vector<std::string> mtp_removed;
  if (!config.engine.mtp) mtp_removed = strip_draft_layer_arguments(args);
  // SP2 spec §3.9: GPUs come only from the typed engine.gpu list (--gpu is
  // frontend-owned, so validate_engine_user_arguments refused it in engine.args).
  // SP5 M1 Task 7: engine.hip_ordinal wins over a single (or absent) card (:3025).
  const std::vector<unsigned> gpus = effective_gpu_list(config.engine.gpus, config.engine.hip_ordinal);
  if (!gpus.empty()) {
    std::string list;
    for (const unsigned device : gpus) {
      if (!list.empty()) list += ',';
      list += std::to_string(device);
    }
    args.emplace_back("--gpu");
    args.push_back(std::move(list));
  }
  // SP5 M1 Task 7 (parity rows vram_reserve_mib / vram_elastic / vram_segment_mib /
  // layer_split / split_skip_if_fits): the family renders after the composed tail, gated
  // per the pin (several GPUs for the split pair, the elastic branch nesting the size).
  const VramGpuElasticArgs vram{.vram_reserve_mib = engine_vram_reserve(config),
                                .vram_elastic = config.engine.vram_elastic,
                                .vram_segment_mib = config.engine.vram_segment_mib,
                                .layer_split = config.engine.layer_split,
                                .split_skip_if_fits = config.engine.split_skip_if_fits};
  append_vram_gpu_elastic_arguments(args, vram, config.engine.args, gpus);
  // SP5 M1 Task 9 (serve/server.py:3000-3005): the learned-profile save pair, guarded by
  // the pin's operator-owns check.
  append_expert_profile_save_arguments(
      args, ExpertProfileSaveArgs{config.session.expert_profile_save, config.session.expert_profile_save_every},
      config.engine.args);
  // SP5 M1 Task 10 (serve/server.py:2955-2975): engine-internal parallelism renders --batch N.
  append_parallel_batch_arguments(args, ParallelBatchArgs{config.replicas.parallel}, config.engine.args);
  // SP5 M1 Task 11 (D-9; pin :2926-2931, #1322): an enabled vision family drives an engine
  // that advertises images; an operator-owned --vision in the saved args stays untouched.
  if (config.vision.enabled &&
      std::find(config.engine.args.begin(), config.engine.args.end(), "--vision") == config.engine.args.end()) {
    args.emplace_back("--vision");
  }
  // Opened once, on the path (F-7), and only if it stays below roots.models.
  auto opened = open_root(*tokenizer,
                          config.roots.allow_untrusted_media ? RootPolicy::external : RootPolicy::existing);
  auto* root = std::get_if<CanonicalRoot>(&opened);
  if (root == nullptr || !is_within(config.roots.models.path(), root->path())) {
    return LaunchFailure{ExitCode::verification,
                         {"Unable to load local tokenizer pack: the model's tokenizer directory cannot be opened safely"}};
  }
  return ComposedLaunch{EngineProfile::qwen, std::move(args), std::move(*root), ref, std::vector<std::string>{ref}, true,
                        std::move(mtp_removed), std::move(vision_mmproj), std::move(vision_model)};
}

}  // namespace secure_frontend
