#pragma once

#include "secure_frontend/acquire_manifest.hpp"
#include "secure_frontend/acquire_reason.hpp"
#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/engine_gate.hpp"  // engine_gate (F-58: one gate, in secure_frontend_model_store)
#include "secure_frontend/frontend_config.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace secure_frontend {

// Spec §4.9 delta c: engine weight flags a managed model's manifest owns.
inline constexpr std::array<std::string_view, 7> kWeightFlags{"--native", "--pack", "--ple-gguf", "--embd-gguf",
                                                              "--native-head-gguf", "--native-dense-gguf", "--mtp"};

struct ComposedLaunch final {
  EngineProfile profile;
  std::vector<std::string> engine_args;         // argv tail after --serve; paths are UTF-8 (never path::string())
  std::optional<CanonicalRoot> tokenizer_root;  // opened once; set on success (never re-opened by path)
  std::string model_id;                         // the model chat requests must name
  std::vector<std::string> served_models;       // inventory ids under --expert, else {model_id}; not what /v1/models
                                                // lists: that is model_id + model.aliases only (SP3 E9, server_bootstrap)
  bool managed;                                 // false only with --expert
  // SP5 M1 Task 6 (parity row mtp): non-empty when engine.mtp=false dropped the draft
  // layer's own flags; the startup line reports them once, like the pin (serve/server.py:2925).
  std::vector<std::string> mtp_removed;
  // SP5 M1 Task 11 (D-9): when the manifest declares launch.mmproj, these carry the encoder
  // artifact and the weights it reads (absolute UTF-8, joined exactly like the engine args).
  std::optional<std::string> vision_mmproj;
  std::optional<std::string> vision_model;
  // GUI-first start-up (spec 2026-10-27 §4.2): no tokenizer pack (a card the page built from a GGUF) - the model file
  // beneath roots.models whose header holds the tokenizer (tokenizer_from_gguf profiles only).
  std::optional<RelativePath> tokenizer_gguf{};
};

// The profiles whose configs may read the tokenizer from the model's GGUF instead of a pack (their engines run the GGUF
// with --native; load_tokenizer_from_gguf reads it in the profile's mode): flint and gemma4, and qwen for a model
// card (`card`: ModelsConfig::from_card) - the operator's single-model --expert qwen keeps its roots.tokenizer_pack.
[[nodiscard]] bool tokenizer_from_gguf(EngineProfile profile, bool card) noexcept;

struct LaunchFailure final {
  ExitCode exit;
  std::vector<std::string> messages;  // key paths and fixed reasons; never configured values
};

// Spec §4.9 c/d: the launch-related config errors, without touching roots.models.
// Managed (expert == false): engine.profile set, roots.tokenizer_pack set, a weight
// flag in engine.args (`--flag value` and `--flag=value`), and an engine.model_id
// that is not a name[:tag] reference. --expert: roots.tokenizer_pack must be set.
// Each message starts with the key path. Shared by compose_engine_launch and
// strata-secure-server --check (F-8), so --check reports what serve refuses.
[[nodiscard]] std::vector<std::string> launch_config_issues(const FrontendConfig& config, bool expert);
// launch_config_issues(config, false).
[[nodiscard]] std::vector<std::string> managed_config_issues(const FrontendConfig& config);

// Managed (default): engine.model_id is name[:tag] in the index; profile, weights and
// tokenizer come from the installed manifest, gated by engine_gate (exit 11).
// --expert: SP1's operator path unchanged (inventory, roots.tokenizer_pack, engine.profile).
[[nodiscard]] std::variant<ComposedLaunch, LaunchFailure> compose_engine_launch(const FrontendConfig& config, bool expert);

}  // namespace secure_frontend
