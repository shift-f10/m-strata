#pragma once

// Model launcher (plan docs/superpowers/plans/2026-10-23-model-launcher.md Task 1; spec §3):
// a model card is one operator file per model, listed by the main config's models.cards. It
// names the model (the served name), its engine profile, a `config` object holding the
// card-owned rows of the main config schema (the engine, the weights, the tokenizer, the
// sampling and thinking defaults, the chat token limits) read by the main config's own reader,
// and an `options` object: what the page may change before a start, within which bounds
// (model_options.hpp). The main config then holds the server and may not set a card-owned row.
//
//   {"schema_version": 1, "name": "Gemma-4-26B-A4B-it", "profile": "gemma4",
//    "config": {"engine": {"executable": "...", "model_id": "..."}, "roots": {...}},
//    "options": {"max_context": {"default": 32768, "max": 262144}}}

#include "secure_frontend/frontend_config.hpp"
#include "secure_frontend/model_options.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace secure_frontend {

inline constexpr std::size_t kMaxModelCards = 16;

struct ModelCard final {
  std::string name;                        // the served name (the config's model.name)
  EngineProfile profile{EngineProfile::qwen};  // qwen, gemma4, glm53, deepseek, raon1 or flint
  FrontendConfigDraft config;              // the card-owned rows of its "config" object
  std::vector<ModelOptionLimits> options;  // the page-settable options, in the profile's order
  // A card the page wrote (card_builder.hpp): "folder" (the user's own files) or "download:<catalog id>" (the Download
  // list hides that entry). Empty for a hand-written card.
  std::string source;
};

// True for a row a card owns: engine.* (but engine.profile, engine.lazy_load and
// engine.idle_unload_s), model.aliases, roots.models, roots.tokenizer_pack, defaults.sampling.*,
// reasoning.* and limits.chat.{max_prompt_tokens,max_completion_tokens,default_max_tokens}.
[[nodiscard]] bool is_card_owned_key(std::string_view path) noexcept;

// A card's text. Strict JSON within the config file's bounds; schema_version 1; name in the
// served-name grammar; profile qwen, gemma4, raon1 or flint; source (optional) "folder" or "download:" and a catalog
// id ([a-z0-9._-]{1,64}); config an object of card-owned rows only (issues
// keep the row's own path, e.g. "engine.args"); options per parse_card_options. A card's
// engine.args may not hold a flag one of its listed options sets. Reasons are fixed text.
// `name_read` (optional) receives the card's name once it was read, also when a later rule
// refuses the card (D9 refuses a card by name).
[[nodiscard]] std::variant<ModelCard, ConfigErrors> parse_model_card(std::string_view text,
                                                                     const std::filesystem::path& directory,
                                                                     std::string* name_read = nullptr);
// Read the way the config file is read (read_trusted_config_text), then parsed.
[[nodiscard]] std::variant<ModelCard, ConfigErrors> read_model_card(const std::filesystem::path& file,
                                                                    std::string* name_read = nullptr);

// D9: every listed card is read; a bad one (or a second card with a name already loaded) is
// refused by its position and, when it got that far, its name, and the others load.
struct CardRefusal final {
  std::size_t position{0};  // 1-based, in models.cards order
  std::string name;         // empty when the card's name could not be read
  ConfigErrors issues;
};
struct LoadedCards final {
  std::vector<ModelCard> cards;
  std::vector<CardRefusal> refused;
};
[[nodiscard]] LoadedCards load_model_cards(const std::vector<std::filesystem::path>& files);

// The main config's card-owned rows (and engine.profile, model.name), from the file or the
// command line, refused by name: with models.cards each card sets them.
[[nodiscard]] ConfigErrors main_config_card_issues(const FrontendConfigDraft& main);

// The draft one start validates: the main config's rows, the card's rows over them, the card's
// profile and name (engine.profile, model.name), then the option values (apply_option_values).
[[nodiscard]] FrontendConfigDraft card_draft(const FrontendConfigDraft& main, const ModelCard& card,
                                             const ModelOptionValues& values);
[[nodiscard]] ConfigValidationResult card_config(const FrontendConfigDraft& main, const ModelCard& card,
                                                 const ModelOptionValues& values);
// GUI-first start-up (docs/superpowers/specs/2026-10-27-gui-first-install-design.md §3): the server's config while it
// has no usable card - the main config's rows, with models.default_engine as the engine and models.default_root (else
// models.downloads_root, else roots.state) as roots.models, validated and never launched. Without
// models.default_engine there is nothing to serve (refused by name).
[[nodiscard]] ConfigValidationResult card_less_server_config(const FrontendConfigDraft& main);
// The card's own defaults (each listed option's default), the values of a start that names none.
[[nodiscard]] ModelOptionValues default_option_values(const ModelCard& card);

[[nodiscard]] std::string_view profile_name(EngineProfile profile) noexcept;

// Model downloads (plan 2026-10-23-model-downloads Task 3): the config strata's own verbs (pull,
// hf-info, list, ...) read from a models.cards config: the main config and the first usable card
// at its defaults, with models.downloads_root as roots.models (the downloads' store). Without
// models.downloads_root, refused by name.
[[nodiscard]] ConfigValidationResult launcher_store_config(const FrontendConfigDraft& main);

}  // namespace secure_frontend
