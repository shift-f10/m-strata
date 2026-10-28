// Model launcher (plan 2026-10-23 Task 1; spec §3, D9/D10): model card files, their refusals by
// rule, loading a list with a bad card in it, and the merge of the main config with a card.
#include "secure_frontend/config_file.hpp"
#include "secure_frontend/frontend_config.hpp"
#include "secure_frontend/model_cards.hpp"
#include "secure_frontend/model_options.hpp"
#include "support/config_json.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

int failures = 0;
void expect(const bool value, const std::string_view message) {
  if (!value) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

using secure_frontend::ConfigErrors;
using secure_frontend::ConfigIssueCode;
using secure_frontend::ModelCard;
using secure_frontend_test::ConfigFixture;
using secure_frontend_test::json_literal;

std::string issues_text(const ConfigErrors& errors) {
  std::string out;
  for (const auto& issue : errors) out += secure_frontend::format_issue(issue) + "; ";
  return out;
}

bool has_issue(const std::variant<ModelCard, ConfigErrors>& result, const std::string_view key, const ConfigIssueCode code) {
  const auto* errors = std::get_if<ConfigErrors>(&result);
  return errors != nullptr && std::any_of(errors->begin(), errors->end(), [&](const auto& issue) {
           return issue.key_path == key && issue.code == code;
         });
}

// The fixture's model rows, as a card's config object.
std::string card_config_object(const ConfigFixture& f, const std::map<std::string, std::string>& extra = {}) {
  std::map<std::string, std::string> rows{{"roots.models", f.literals.at("roots.models")},
                                          {"roots.tokenizer_pack", f.literals.at("roots.tokenizer_pack")},
                                          {"engine.executable", f.literals.at("engine.executable")},
                                          {"engine.model_id", f.literals.at("engine.model_id")}};
  for (const auto& [k, v] : extra) rows[k] = v;
  return secure_frontend::render_config_literals(rows);
}

std::string card_text(const std::string& name, const std::string& profile, const std::string& config,
                      const std::string& options = "{}", const std::string& extra_members = "") {
  return "{\"schema_version\": 1, \"name\": " + json_literal(name) + ", \"profile\": " + json_literal(profile) +
         ", \"config\": " + config + ", \"options\": " + options + extra_members + "}";
}

// The main config of launcher mode: the fixture's server rows without its model rows.
secure_frontend::FrontendConfigDraft main_draft(const ConfigFixture& f, const std::map<std::string, std::string>& extra = {}) {
  std::map<std::string, std::string> rows{{"schema_version", "1"}, {"roots.secret", f.literals.at("roots.secret")},
                                          {"models.cards", "[" + json_literal((f.dir / "cards" / "a.json").string()) + "]"}};
  for (const auto& [k, v] : extra) rows[k] = v;
  auto parsed = secure_frontend::parse_config_text(secure_frontend::render_config_literals(rows), f.dir);
  expect(std::holds_alternative<secure_frontend::FrontendConfigDraft>(parsed), "the main config parses");
  return std::holds_alternative<secure_frontend::FrontendConfigDraft>(parsed) ? std::get<secure_frontend::FrontendConfigDraft>(parsed)
                                                                              : secure_frontend::FrontendConfigDraft{};
}

void card_parses_and_merges_into_a_valid_config() {
  ConfigFixture f;
  const auto parsed = secure_frontend::parse_model_card(
      card_text("Qwen-Test", "qwen",
                card_config_object(f, {{"defaults.sampling.temperature", "0.7"}, {"reasoning.effort", "\"medium\""},
                                       {"engine.args", "[\"--prefill\", \"auto\"]"}})),
      f.dir);
  const auto* card = std::get_if<ModelCard>(&parsed);
  expect(card != nullptr, "a well-formed card parses: " + (card ? std::string{} : issues_text(std::get<ConfigErrors>(parsed))));
  if (card == nullptr) return;
  expect(card->name == "Qwen-Test" && card->profile == secure_frontend::EngineProfile::qwen && card->options.empty(),
         "its name and profile are kept; no options listed, none offered");
  expect(card->config.entries.contains("engine.executable") && card->config.entries.contains("defaults.sampling.temperature"),
         "its config rows are kept");
  const auto main = main_draft(f, {{"listener.port", "8091"}});
  expect(secure_frontend::main_config_card_issues(main).empty(), "the launcher's main config owns no card row");
  const auto result = secure_frontend::card_config(main, *card, secure_frontend::default_option_values(*card));
  const auto* validated = std::get_if<secure_frontend::ValidatedConfig>(&result);
  expect(validated != nullptr,
         "main + card validate: " + (validated ? std::string{} : issues_text(std::get<ConfigErrors>(result))));
  if (validated == nullptr) return;
  const auto& config = validated->get();
  expect(config.engine.served_name == std::optional<std::string>{"Qwen-Test"}, "the card's name is the served name");
  expect(config.engine.profile == std::optional<secure_frontend::EngineProfile>{secure_frontend::EngineProfile::qwen},
         "the card's profile is the engine profile");
  expect(config.listener.port == 8091, "the main config's server rows stay");
  expect(config.sampling.temperature == std::optional<double>{0.7}, "the card's sampling default reaches the config");
  expect(config.reasoning.effort == std::optional<secure_frontend::ReasoningEffort>{secure_frontend::ReasoningEffort::medium},
         "the card's thinking default reaches the config");
  expect(config.models.cards.size() == 1U, "the main config's models rows stay");
  const auto draft = secure_frontend::card_draft(main, *card, {});
  const auto profile = draft.entries.find("engine.profile");
  expect(profile != draft.entries.end() && std::get<std::string>(profile->second.value) == "qwen",
         "the draft carries engine.profile from the card");
}

void card_refusals_name_the_rule() {
  ConfigFixture f;
  const std::string good = card_config_object(f);
  const auto parse = [&f](const std::string& text) { return secure_frontend::parse_model_card(text, f.dir); };
  expect(std::holds_alternative<ModelCard>(parse(card_text("Good", "gemma4", good))), "the baseline card parses");
  expect(has_issue(parse("{\"schema_version\": 2, \"name\": \"A\", \"profile\": \"qwen\", \"config\": " + good + "}"),
                   "schema_version", ConfigIssueCode::invalid_value),
         "schema_version must be 1");
  expect(has_issue(parse("{\"schema_version\": 1, \"profile\": \"qwen\", \"config\": " + good + "}"), "name",
                   ConfigIssueCode::missing_required),
         "a card has a name");
  expect(has_issue(parse(card_text("has space", "qwen", good)), "name", ConfigIssueCode::invalid_value),
         "the name is a served name");
  expect(has_issue(parse(card_text(std::string(65, 'a'), "qwen", good)), "name", ConfigIssueCode::invalid_value),
         "at most 64 characters");
  // D10 met (owner 2026-10-27, item 46's fix: the page sends reasoning_effort only to a card whose profile takes it)
  const auto glm = parse(card_text("GLM-5.3-Flash", "glm53", good));
  expect(std::holds_alternative<ModelCard>(glm) && std::get<ModelCard>(glm).profile == secure_frontend::EngineProfile::glm53,
         "glm53 cards parse (Maya's GLM-5.3 engine, multiengine_maya)");
  expect(std::holds_alternative<ModelCard>(parse(card_text("A", "raon1", good))),
         "raon1 cards parse (a first-class glm_pack family)");
  const auto flint = parse(card_text("Flint", "flint", good));
  expect(std::holds_alternative<ModelCard>(flint) && std::get<ModelCard>(flint).profile == secure_frontend::EngineProfile::flint,
         "flint cards parse (multiengine_flint, the strata_0143 wire)");
  // 2026-10-27 (owner: "continue to the next pieces I asked for" - DeepSeek on Maya's maya-ds4)
  const auto deepseek = parse(card_text("DeepSeek-V4.1-Flash", "deepseek", good));
  expect(std::holds_alternative<ModelCard>(deepseek) &&
             std::get<ModelCard>(deepseek).profile == secure_frontend::EngineProfile::deepseek,
         "deepseek cards parse (Maya's DeepSeek engine, multiengine_maya_ds4)");
  // 2026-10-28 (owner: "add MiniMax-M3.1-Flash under the MiniMax Community License")
  const auto minimax = parse(card_text("MiniMax-M3.1-Flash", "minimax", good));
  expect(std::holds_alternative<ModelCard>(minimax) && std::get<ModelCard>(minimax).profile == secure_frontend::EngineProfile::minimax,
         "minimax cards parse (Maya's MiniMax engine, multiengine_maya_mm3)");
  expect(has_issue(parse(card_text("A", "llama", good)), "profile", ConfigIssueCode::invalid_value), "a known profile");
  expect(has_issue(parse("{\"schema_version\": 1, \"name\": \"A\", \"profile\": \"qwen\"}"), "config", ConfigIssueCode::missing_required),
         "a card has a config object");
  // a card the page wrote names where its model came from (the Download list hides a downloaded one)
  for (const std::string source : {"folder", "download:flint-q8_0", "download:gemma-4-26b.q8"}) {
    const auto sourced = parse(card_text("A", "flint", good, "{}", ", \"source\": " + json_literal(source)));
    expect(std::holds_alternative<ModelCard>(sourced) && std::get<ModelCard>(sourced).source == source, "source " + source);
  }
  for (const std::string& source : std::vector<std::string>{"", "download:", "download:Upper", "download:a/b", "Folder", "web",
                                                            "download:" + std::string(65, 'a')}) {
    expect(has_issue(parse(card_text("A", "flint", good, "{}", ", \"source\": " + json_literal(source))), "source",
                     ConfigIssueCode::invalid_value),
           "a bad source is refused: \"" + source + "\"");
  }
  expect(has_issue(parse(card_text("A", "flint", good, "{}", ", \"source\": 1")), "source", ConfigIssueCode::invalid_value),
         "a source is a string");
  expect(std::get<ModelCard>(parse(card_text("A", "flint", good))).source.empty(), "a hand-written card has none");
  expect(has_issue(parse(card_text("A", "qwen", good, "{}", ", \"engine\": {}")), "engine", ConfigIssueCode::unknown_key),
         "config rows sit inside config, not beside it");
  for (const auto& [row, literal] : std::vector<std::pair<std::string, std::string>>{{"listener.port", "8081"},
                                                                                     {"engine.lazy_load", "true"},
                                                                                     {"engine.idle_unload_s", "5"},
                                                                                     {"limits.chat.max_body_bytes", "1000"},
                                                                                     {"models.control", "true"},
                                                                                     {"engine.profile", "\"qwen\""},
                                                                                     {"model.name", "\"B\""}}) {
    expect(has_issue(parse(card_text("A", "qwen", card_config_object(f, {{row, literal}}))), row, ConfigIssueCode::invalid_value),
           "not a card row, refused by name: " + row);
  }
  expect(has_issue(parse(card_text("A", "qwen", card_config_object(f, {{"engine.bogus", "1"}}))), "engine.bogus",
                   ConfigIssueCode::unknown_key),
         "the config reader's own rules apply inside a card");
  expect(has_issue(parse("{\"schema_version\": 1, \"name\": \"A\", \"name\": \"B\", \"profile\": \"qwen\", \"config\": " + good + "}"),
                   "model card", ConfigIssueCode::duplicate_key),
         "strict JSON: a member once");
  for (const auto& [row, literal] : std::vector<std::pair<std::string, std::string>>{{"model.aliases", "[\"q\"]"},
                                                                                     {"engine.args", "[\"--prefill\", \"auto\"]"},
                                                                                     {"limits.chat.max_prompt_tokens", "65536"},
                                                                                     {"limits.chat.max_completion_tokens", "8192"},
                                                                                     {"limits.chat.default_max_tokens", "0"},
                                                                                     {"defaults.sampling.top_k", "40"},
                                                                                     {"reasoning.effort", "\"none\""},
                                                                                     // vision for cards (2026-10-28): the card's own encoder file
                                                                                     {"vision.enabled", "true"},
                                                                                     {"vision.gpu", "true"},
                                                                                     {"vision.mmproj", json_literal((f.dir / "mmproj.gguf").string())}}) {
    expect(std::holds_alternative<ModelCard>(parse(card_text("A", "qwen", card_config_object(f, {{row, literal}})))),
           "a card row is accepted: " + row);
  }
  // the main config may not set them while cards are in use: each card says whether its model reads images
  secure_frontend::FrontendConfigDraft main;
  for (const char* row : {"vision.enabled", "vision.gpu", "vision.mmproj"}) {
    main.entries[row] = secure_frontend::ConfigDraftEntry{true, secure_frontend::ConfigSource::file};
  }
  const auto issues = secure_frontend::main_config_card_issues(main);
  expect(issues.size() == 3U, "the vision rows are a card's, never the main config's under models.cards");
}

// D9: a bad card is refused by its place in the list (and its name when it had one); a second
// card with a name already loaded is refused; the others load.
void bad_card_is_refused_others_load() {
  ConfigFixture f;
  const auto dir = f.dir / "cards";
  secure_frontend_test::make_private_directory(dir);
  secure_frontend_test::write_file(dir / "a.json", card_text("Alpha", "qwen", card_config_object(f)), 0600);
  secure_frontend_test::write_file(dir / "b.json", "{\"schema_version\": 1,", 0600);
  secure_frontend_test::write_file(dir / "c.json", card_text("Alpha", "gemma4", card_config_object(f)), 0600);
  secure_frontend_test::write_file(dir / "d.json", card_text("Delta", "gemma4", card_config_object(f)), 0600);
  secure_frontend_test::write_file(dir / "e.json", card_text("Echo", "gemma4", card_config_object(f, {{"listener.port", "1"}})), 0600);
  const auto loaded = secure_frontend::load_model_cards(
      {dir / "a.json", dir / "b.json", dir / "c.json", dir / "missing.json", dir / "d.json", dir / "e.json"});
  expect(loaded.cards.size() == 2U && loaded.cards[0].name == "Alpha" && loaded.cards[1].name == "Delta",
         "the good cards load, in order");
  expect(loaded.refused.size() == 4U, "four refusals");
  if (loaded.refused.size() != 4U) return;
  expect(loaded.refused[3].position == 6U && loaded.refused[3].name == "Echo",
         "a card refused after its name was read is refused by that name (D9)");
  expect(loaded.refused[0].position == 2U && loaded.refused[0].name.empty() && !loaded.refused[0].issues.empty(),
         "the malformed card is refused by its place");
  expect(loaded.refused[1].position == 3U && loaded.refused[1].name == "Alpha" && loaded.refused[1].issues.size() == 1U &&
             loaded.refused[1].issues[0].code == ConfigIssueCode::duplicate_key,
         "a second card with a loaded name is refused by name");
  expect(loaded.refused[2].position == 4U && loaded.refused[2].issues.size() == 1U &&
             loaded.refused[2].issues[0].key_path == "model card" &&
             loaded.refused[2].issues[0].code == ConfigIssueCode::file_unavailable,
         "a missing card file is refused as unavailable: " + issues_text(loaded.refused[2].issues));
  for (const auto& refusal : loaded.refused) {
    for (const auto& issue : refusal.issues) {
      expect(issue.reason.find(f.dir.string()) == std::string::npos, "a refusal never echoes the card's path");
    }
  }
}

void main_config_may_not_set_card_keys() {
  ConfigFixture f;
  const auto main = main_draft(f, {{"engine.executable", f.literals.at("engine.executable")},
                                   {"roots.models", f.literals.at("roots.models")},
                                   {"model.name", "\"X\""},
                                   {"engine.profile", "\"qwen\""},
                                   {"defaults.sampling.top_p", "0.9"},
                                   {"reasoning.effort", "\"high\""},
                                   {"limits.chat.max_prompt_tokens", "4096"},
                                   {"listener.port", "8090"},
                                   {"limits.chat.max_body_bytes", "40000"}});
  const auto issues = secure_frontend::main_config_card_issues(main);
  for (const char* row : {"engine.executable", "roots.models", "model.name", "engine.profile", "defaults.sampling.top_p",
                          "reasoning.effort", "limits.chat.max_prompt_tokens"}) {
    expect(std::any_of(issues.begin(), issues.end(),
                       [row](const auto& issue) { return issue.key_path == row && issue.code == ConfigIssueCode::conflicting_values; }),
           std::string{"the main config may not set a card row: "} + row);
  }
  for (const char* row : {"listener.port", "limits.chat.max_body_bytes", "models.cards", "roots.secret"}) {
    expect(std::none_of(issues.begin(), issues.end(), [row](const auto& issue) { return issue.key_path == row; }),
           std::string{"a server row stays the main config's: "} + row);
  }
}

// The options a card lists reach the start: their flags follow the card's fixed arguments, and
// a fixed argument may not set what a listed option sets (the page's value is the only one).
void card_options_reach_the_engine_arguments() {
  ConfigFixture f;
  const std::string config = card_config_object(f, {{"engine.args", "[\"--prefill\", \"auto\"]"}});
  const auto parsed = secure_frontend::parse_model_card(
      card_text("Qwen-Test", "qwen", config,
                "{\"kv\": {\"default\": \"int8\", \"choices\": [\"int8\", \"k8v4\"]}, \"max_context\": {\"default\": 65536}}"),
      f.dir);
  const auto* card = std::get_if<ModelCard>(&parsed);
  expect(card != nullptr && card->options.size() == 2U,
         "a card's options parse: " + (card ? std::string{} : issues_text(std::get<ConfigErrors>(parsed))));
  if (card == nullptr) return;
  const auto draft = secure_frontend::card_draft(main_draft(f), *card, secure_frontend::default_option_values(*card));
  const auto args = draft.entries.find("engine.args");
  const auto* list = args == draft.entries.end() ? nullptr : std::get_if<std::vector<std::string>>(&args->second.value);
  expect(list != nullptr && list->size() >= 6U && (*list)[0] == "--prefill" && (*list)[1] == "auto" &&
             std::find(list->begin(), list->end(), "--kv") != list->end() &&
             std::find(list->begin(), list->end(), "65536") != list->end(),
         "the fixed arguments first, then the options' flags");
  const auto clash = secure_frontend::parse_model_card(
      card_text("Qwen-Test", "qwen", card_config_object(f, {{"engine.args", "[\"--kv\", \"fp16\"]"}}),
                "{\"kv\": {\"default\": \"int8\"}}"),
      f.dir);
  expect(has_issue(clash, "engine.args", ConfigIssueCode::conflicting_values), "a fixed --kv beside the kv option is refused");
  // The context option also writes the YaRN flags past Qwen's trained length: a card that offers
  // the context may not fix them.
  const auto rope = secure_frontend::parse_model_card(
      card_text("Qwen-Test", "qwen", card_config_object(f, {{"engine.args", "[\"--rope-scale\", \"2\"]"}}),
                "{\"max_context\": {\"default\": 65536}}"),
      f.dir);
  expect(has_issue(rope, "engine.args", ConfigIssueCode::conflicting_values), "a fixed --rope-scale beside the context option is refused");
  const auto unlisted = secure_frontend::parse_model_card(
      card_text("Qwen-Test", "qwen", card_config_object(f, {{"engine.args", "[\"--kv\", \"fp16\"]"}}), "{}"), f.dir);
  expect(std::holds_alternative<ModelCard>(unlisted), "a card that does not offer kv may fix --kv itself");
  const auto bad_option = secure_frontend::parse_model_card(
      card_text("Gemma", "gemma4", card_config_object(f), "{\"kv\": {\"default\": \"int8\"}}"), f.dir);
  expect(std::holds_alternative<ConfigErrors>(bad_option), "an option of another engine is refused");
}

// Model downloads (plan 2026-10-23-model-downloads Task 3): strata's own verbs (pull, hf-info,
// list) read a models.cards config with models.downloads_root as their store (roots.models), the
// server's settings from the main config and the first usable card; without the root they refuse.
void launcher_store_config_uses_the_downloads_root() {
  ConfigFixture f;
  const auto cards = f.dir / "cards";
  secure_frontend_test::make_private_directory(cards);
  secure_frontend_test::make_private_directory(f.dir / "downloads");
  secure_frontend_test::write_file(cards / "a.json", card_text("Alpha", "qwen", card_config_object(f)), 0600);
  const std::string listed = "[" + json_literal((cards / "a.json").string()) + "]";
  auto main = main_draft(f, {{"models.cards", listed}, {"models.downloads_root", json_literal((f.dir / "downloads").string())}});
  const auto result = secure_frontend::launcher_store_config(main);
  const auto* validated = std::get_if<secure_frontend::ValidatedConfig>(&result);
  expect(validated != nullptr, "a launcher config gives strata a config: " +
                                   (validated ? std::string{} : issues_text(std::get<ConfigErrors>(result))));
  if (validated != nullptr) {
    expect(std::filesystem::equivalent(validated->get().roots.models.path(), f.dir / "downloads"),
           "roots.models is the downloads root");
  }
  const auto without = secure_frontend::launcher_store_config(main_draft(f, {{"models.cards", listed}}));
  const auto* errors = std::get_if<ConfigErrors>(&without);
  expect(errors != nullptr && std::any_of(errors->begin(), errors->end(),
                                          [](const auto& issue) { return issue.key_path == "models.downloads_root"; }),
         "without a downloads root strata's verbs refuse by name");
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"card_parses_and_merges_into_a_valid_config", card_parses_and_merges_into_a_valid_config},
      {"card_refusals_name_the_rule", card_refusals_name_the_rule},
      {"bad_card_is_refused_others_load", bad_card_is_refused_others_load},
      {"main_config_may_not_set_card_keys", main_config_may_not_set_card_keys},
      {"card_options_reach_the_engine_arguments", card_options_reach_the_engine_arguments},
      {"launcher_store_config_uses_the_downloads_root", launcher_store_config_uses_the_downloads_root},
  }, failures);
}
