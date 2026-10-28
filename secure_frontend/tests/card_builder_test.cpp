// Cards the page writes (docs/superpowers/specs/2026-10-27-gui-first-install-design.md §4.2-§4.5): a card made from a
// GGUF model file's header, its name and options, re-pointing its folder, and the danger zone's one-time codes.
#include "secure_frontend/card_builder.hpp"
#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/gguf_metadata.hpp"
#include "secure_frontend/model_cards.hpp"
#include "support/test_support.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <variant>

namespace {

int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

secure_frontend::GgufMetadata metadata(const std::string& arch) {
  secure_frontend::GgufMetadata md;
  md.version = 3U;
  md.values["general.architecture"] = std::string{arch};
  md.values["general.name"] = std::string{"Flint Preview"};
  md.values[arch + ".context_length"] = std::int64_t{262144};
  md.values["general.sampling.temp"] = 1.0;
  md.values["general.sampling.top_p"] = 0.949999988079071;
  md.values["general.sampling.top_k"] = std::int64_t{20};
  return md;
}

void a_flint_gguf_makes_a_flint_card() {
  const auto folder = secure_frontend_test::make_scratch_directory();
  const auto engine = folder / "multiengine.exe";
  const auto made = secure_frontend::card_for_gguf(metadata("flint"),
                                                   {.folder = folder, .file = "flint-Q8_0.gguf", .engine = engine, .source = "folder"});
  const auto* card = std::get_if<secure_frontend::CardText>(&made);
  expect(card != nullptr, "a flint GGUF makes a card");
  if (card == nullptr) return;
  expect(card->name == "flint-Q8_0" && card->profile == secure_frontend::EngineProfile::flint, "named after the file, profile flint");
  const auto parsed = secure_frontend::parse_model_card(card->json, folder);
  const auto* back = std::get_if<secure_frontend::ModelCard>(&parsed);
  std::string why;
  if (const auto* issues = std::get_if<secure_frontend::ConfigErrors>(&parsed)) {
    for (const auto& issue : *issues) why += secure_frontend::format_issue(issue) + "; ";
  }
  expect(back != nullptr, "the card is a valid card file: " + why);
  if (back == nullptr) return;
  expect(back->source == "folder", "it records where it came from (the Download list hides it)");
  const auto& rows = back->config.entries;
  expect(std::get<std::string>(rows.at("roots.models").value) == secure_frontend::path_to_utf8(folder) &&
             std::get<std::string>(rows.at("engine.model_id").value) == "flint-Q8_0" &&
             std::get<std::string>(rows.at("engine.executable").value) == secure_frontend::path_to_utf8(engine),
         "its folder, its model and the engine");
  expect(!rows.contains("roots.tokenizer_pack"), "no tokenizer pack: the tokenizer is read from the GGUF");
  const auto option = [&](const std::string_view name) -> const secure_frontend::ModelOptionLimits* {
    for (const auto& limit : back->options) {
      if (limit.spec != nullptr && limit.spec->name == name) return &limit;
    }
    return nullptr;
  };
  const auto* context = option("max_context");
  expect(context != nullptr && std::get<std::int64_t>(context->default_value) == 65536 && context->maximum == 262144,
         "a 64K context by default, the model's own as the most");
  const auto* top_k = option("top_k");
  const auto* top_p = option("top_p");
  expect(top_k != nullptr && std::get<std::int64_t>(top_k->default_value) == 20 && top_p != nullptr &&
             std::get<double>(top_p->default_value) == 0.95,
         "the GGUF's sampling (top_p rounded to two places from the file's float)");
  const auto* thinking = option("thinking");
  expect(thinking != nullptr && std::get<std::string>(thinking->default_value) == "on", "thinking on, as the template does");
}

// Owner 2026-10-27 ("Qwen 3.6 from the GGUF first"): a Qwen3.6 GGUF (architecture qwen35moe) runs on upstream's engine
// straight from the file (--native, upstream's OWN_PATH family), its tokenizer read from the GGUF - no prepare step. Its
// card is the one the download manager wrote after the Python step: profile qwen, "--mtp 2" when the file carries the
// MTP block (qwen35moe.nextn_predict_layers), 32K of the model's 256K by default, thinking high.
void a_qwen36_gguf_makes_a_qwen_card() {
  const auto folder = secure_frontend_test::make_scratch_directory();
  auto md = metadata("qwen35moe");
  md.values["qwen35moe.nextn_predict_layers"] = std::int64_t{1};
  const auto made = secure_frontend::card_for_gguf(md, {.folder = folder, .file = "Qwen3.6-35B-A3B-Q8_0.gguf",
                                                        .engine = folder / "multiengine.exe", .source = "download:qwen3.6-35b-a3b-q8_0"});
  const auto* card = std::get_if<secure_frontend::CardText>(&made);
  expect(card != nullptr && card->name == "Qwen3.6-35B-A3B-Q8_0" && card->profile == secure_frontend::EngineProfile::qwen,
         "a qwen35moe GGUF makes a qwen card");
  if (card == nullptr) return;
  expect(card->json.find("\"args\": [\"--mtp\", \"2\"]") != std::string::npos, "with the MTP draft layer (--mtp 2): " + card->json);
  const auto parsed = secure_frontend::parse_model_card(card->json, folder);
  const auto* back = std::get_if<secure_frontend::ModelCard>(&parsed);
  std::string why;
  if (const auto* issues = std::get_if<secure_frontend::ConfigErrors>(&parsed)) {
    for (const auto& issue : *issues) why += secure_frontend::format_issue(issue) + "; ";
  }
  expect(back != nullptr, "the card is a valid card file: " + why);
  if (back == nullptr) return;
  expect(!back->config.entries.contains("roots.tokenizer_pack") && back->source == "download:qwen3.6-35b-a3b-q8_0",
         "no tokenizer pack (read from the GGUF), and its source");
  const auto option = [&](const std::string_view name) -> const secure_frontend::ModelOptionLimits* {
    for (const auto& limit : back->options) {
      if (limit.spec != nullptr && limit.spec->name == name) return &limit;
    }
    return nullptr;
  };
  const auto* context = option("max_context");
  expect(context != nullptr && std::get<std::int64_t>(context->default_value) == 32768 && context->maximum == 262144,
         "32K by default, the model's trained 256K as the most");
  const auto* mtp = option("mtp");
  const auto* thinking = option("thinking");
  expect(mtp != nullptr && std::get<bool>(mtp->default_value) && thinking != nullptr &&
             std::get<std::string>(thinking->default_value) == "high",
         "the draft layer on, thinking high");
  // a file without the MTP block: no --mtp, the switch off
  const auto plain = secure_frontend::card_for_gguf(metadata("qwen35moe"), {.folder = folder, .file = "Qwen3.6-finetune.gguf",
                                                                           .engine = folder / "multiengine.exe", .source = "folder"});
  const auto* plain_card = std::get_if<secure_frontend::CardText>(&plain);
  expect(plain_card != nullptr && plain_card->json.find("--mtp") == std::string::npos &&
             plain_card->json.find("\"mtp\": {\"default\": false}") != std::string::npos,
         "no MTP block: no draft layer");
}

void names_are_made_valid_and_splits_named_once() {
  const auto folder = secure_frontend_test::make_scratch_directory();
  const auto split = secure_frontend::card_for_gguf(metadata("gemma4"), {.folder = folder,
      .file = "gemma 4 (26B) Q8-00001-of-00003.gguf", .engine = folder / "e.exe", .source = "folder"});
  const auto* card = std::get_if<secure_frontend::CardText>(&split);
  expect(card != nullptr && card->name == "gemma-4-26B-Q8" && card->profile == secure_frontend::EngineProfile::gemma4,
         "spaces and brackets become dashes, the shard suffix goes");
}

// Owner 2026-10-27: the families whose card needs a pack built first, and that card (as a download's, from a folder
// or a download): Qwen3.8 (qwen4exp) with its pack, PLE shard and profile; GLM-5.3 (both spellings) on Maya's pack.
void packed_families_and_their_cards() {
  using secure_frontend::PackedFamily;
  expect(secure_frontend::packed_family(metadata("qwen4exp")) == std::optional<PackedFamily>{PackedFamily::qwen38} &&
             secure_frontend::packed_family(metadata("glm5-next")) == std::optional<PackedFamily>{PackedFamily::glm53} &&
             secure_frontend::packed_family(metadata("glm5next")) == std::optional<PackedFamily>{PackedFamily::glm53} &&
             !secure_frontend::packed_family(metadata("gemma4")) && !secure_frontend::packed_family(metadata("qwen35moe")),
         "Qwen3.8 and GLM-5.3 need a pack; the families read from the GGUF do not");
  expect(secure_frontend::first_shard_or_whole("m") && secure_frontend::first_shard_or_whole("m-00001-of-00003") &&
             !secure_frontend::first_shard_or_whole("m-00002-of-00003"),
         "a whole model or its first shard");
  expect(secure_frontend::card_name_for_file("GLM-5.3-Flash Maya-S-00001-of-00003") == "GLM-5.3-Flash-Maya-S", "the card name a file gives");
  const auto folder = secure_frontend_test::make_scratch_directory();
  const auto parse = [&folder](const secure_frontend::CardText& text) {
    auto parsed = secure_frontend::parse_model_card(text.json, folder);
    std::string why;
    if (const auto* issues = std::get_if<secure_frontend::ConfigErrors>(&parsed)) {
      for (const auto& issue : *issues) why += secure_frontend::format_issue(issue) + "; ";
    }
    expect(std::holds_alternative<secure_frontend::ModelCard>(parsed), text.name + " is a valid card file: " + why);
    return parsed;
  };
  const auto qwen = secure_frontend::card_for_pack({.family = PackedFamily::qwen38, .name = "Q", .engine = folder / "multiengine.exe",
                                                    .source = "folder", .pack = folder / "packs" / "Q", .folder = folder / "models",
                                                    .model_id = "q-00001-of-00002", .ple_gguf = std::string{"/m/q-00002-of-00002.gguf"},
                                                    .expert_profile = folder / "profiles" / "expert-profile.bin"});
  const auto parsed_qwen = parse(qwen);
  if (const auto* card = std::get_if<secure_frontend::ModelCard>(&parsed_qwen)) {
    expect(card->profile == secure_frontend::EngineProfile::qwen && card->source == "folder" &&
               std::get<std::vector<std::string>>(card->config.entries.at("engine.args").value) ==
                   std::vector<std::string>{"--pack", secure_frontend::path_to_utf8(folder / "packs" / "Q"), "--ple-gguf", "/m/q-00002-of-00002.gguf",
                                            "--expert-profile", secure_frontend::path_to_utf8(folder / "profiles" / "expert-profile.bin")} &&
               std::get<std::string>(card->config.entries.at("engine.model_id").value) == "q-00001-of-00002",
           "the Qwen3.8 card: --pack, --ple-gguf, --expert-profile, the first shard as its model");
  }
  const auto glm = secure_frontend::card_for_pack({.family = PackedFamily::glm53, .name = "G", .engine = folder / "multiengine.exe",
                                                   .source = "download:glm-x", .pack = folder / "g" / "pack"});
  const auto parsed_glm = parse(glm);
  if (const auto* card = std::get_if<secure_frontend::ModelCard>(&parsed_glm)) {
    expect(card->profile == secure_frontend::EngineProfile::glm53 && card->source == "download:glm-x" &&
               !card->config.entries.contains("engine.args") &&
               std::get<std::string>(card->config.entries.at("roots.models").value) == secure_frontend::path_to_utf8(folder / "g" / "pack") &&
               std::get<std::string>(card->config.entries.at("engine.model_id").value) == "glm-5.3-flash" && card->options.size() == 4U,
           "the GLM card: the pack, Maya's model name, Maya's four options");
  }
}

// Owner 2026-10-28 ("I don't see MTP settings in the cards"): a Qwen3.8 pack card runs the MTP draft layer when one is
// found (upstream's mtp/rt), as upstream's setup writes it (--mtp <rt>, --spec 4, --spec-min-p 0.5), and lists its three
// settings; without one it keeps prompt lookup's --spec only.
void a_qwen38_pack_with_its_draft_layer_offers_mtp() {
  using secure_frontend::PackedFamily;
  const auto folder = secure_frontend_test::make_scratch_directory();
  secure_frontend::PackedCardRequest request{.family = PackedFamily::qwen38, .name = "Q", .engine = folder / "multiengine.exe",
                                             .source = "folder", .pack = folder / "packs" / "Q", .folder = folder / "models",
                                             .model_id = "q-00001-of-00002"};
  const auto options_of = [&folder](const secure_frontend::CardText& text) {
    std::map<std::string, secure_frontend::ModelOptionValue> out;
    auto parsed = secure_frontend::parse_model_card(text.json, folder);
    if (const auto* card = std::get_if<secure_frontend::ModelCard>(&parsed)) {
      for (const auto& limit : card->options) out[std::string{limit.spec->name}] = limit.default_value;
      const auto args = card->config.entries.find("engine.args");
      out["args"] = args == card->config.entries.end() ? std::string{} : [&] {
        std::string all;
        for (const auto& a : std::get<std::vector<std::string>>(args->second.value)) all += " " + a;
        return all;
      }();
    }
    return out;
  };
  const auto plain = options_of(secure_frontend::card_for_pack(request));
  expect(!plain.empty() && !plain.contains("mtp") && !plain.contains("spec_min_p") && plain.contains("spec") &&
             std::get<std::string>(plain.at("args")).find("--mtp") == std::string::npos,
         "without a draft layer: prompt lookup's --spec, no MTP settings");
  request.mtp = folder / "mtp" / "rt";
  const auto with = options_of(secure_frontend::card_for_pack(request));
  expect(with.contains("mtp") && std::get<bool>(with.at("mtp")) && with.contains("spec") && std::get<std::int64_t>(with.at("spec")) == 4 &&
             with.contains("spec_min_p") && std::get<double>(with.at("spec_min_p")) == 0.5,
         "with one: the MTP draft layer on, a window of 4, a confidence of 0.5");
  expect(std::get<std::string>(with.at("args")).find(" --mtp " + secure_frontend::path_to_utf8(folder / "mtp" / "rt")) != std::string::npos,
         "and --mtp names its runtime folder: " + std::get<std::string>(with.at("args")));
}

// DeepSeek-V4.1 (2026-10-27, spec 2026-10-27-strata-prepare-design §7.2): its GGUF needs Maya's pack (maya-ds4), and the
// card runs it on multiengine --pack as Maya's write_config_ds4 does for one GPU (--tp 1 --threads on; the launch adds
// --pack and --vision off): the pack as roots.models with its marker (Maya's model name) and tokenizer, Maya's DeepSeek
// options (64K of 32K-1M, thinking high, temperature 1.0, top_p 1.0).
void a_deepseek_pack_makes_a_deepseek_card() {
  using secure_frontend::PackedFamily;
  expect(secure_frontend::packed_family(metadata("deepseek41")) == std::optional<PackedFamily>{PackedFamily::deepseek},
         "a DeepSeek-V4.1 GGUF needs Maya's pack");
  const auto folder = secure_frontend_test::make_scratch_directory();
  const auto pack = folder / "m" / "maya-ds4";
  const auto made = secure_frontend::card_for_pack({.family = PackedFamily::deepseek, .name = "D", .engine = folder / "multiengine.exe",
                                                    .source = "folder", .pack = pack});
  auto parsed = secure_frontend::parse_model_card(made.json, folder);
  const auto* card = std::get_if<secure_frontend::ModelCard>(&parsed);
  expect(card != nullptr, "the DeepSeek card is a valid card file");
  if (card == nullptr) return;
  expect(card->profile == secure_frontend::EngineProfile::deepseek && made.profile == secure_frontend::EngineProfile::deepseek &&
             std::get<std::vector<std::string>>(card->config.entries.at("engine.args").value) ==
                 std::vector<std::string>{"--tp", "1", "--threads", "on"} &&
             std::get<std::string>(card->config.entries.at("roots.models").value) == secure_frontend::path_to_utf8(pack) &&
             std::get<std::string>(card->config.entries.at("roots.tokenizer_pack").value) == secure_frontend::path_to_utf8(pack / "tokenizer") &&
             std::get<std::string>(card->config.entries.at("engine.model_id").value) == "deepseek-v4.1-flash",
         "the DeepSeek card: Maya's one-GPU flags, the pack and its tokenizer, Maya's model name");
  std::vector<std::string> names;
  for (const auto& option : card->options) names.emplace_back(option.spec != nullptr ? option.spec->name : "");
  expect(names == std::vector<std::string>{"max_context", "thinking", "temperature", "top_p"}, "Maya's DeepSeek options");
  std::filesystem::create_directories(pack);
  expect(secure_frontend::write_pack_marker(pack, secure_frontend::kDeepseekModelName) &&
             std::filesystem::is_regular_file(pack / "deepseek-v4.1-flash.gguf"),
         "the pack's marker is Maya's model name");
}

// MiniMax-M3.1-Flash (owner 2026-10-28): its GGUF needs Maya's pack (maya-mm3), and the card runs it on multiengine --pack
// as Maya's write_config_mm3 does for a GGUF quant (the launch adds --pack, KV streaming, the shipped profile and
// --vision off): the pack as roots.models with its marker (Maya's MM3_MODEL_NAME) and tokenizer, Maya's MiniMax options
// (64K of 32K-1M, the KV cache int8, thinking auto, temperature 1.0, top_p 0.95).
void a_minimax_pack_makes_a_minimax_card() {
  using secure_frontend::PackedFamily;
  expect(secure_frontend::packed_family(metadata("minimax-m3")) == std::optional<PackedFamily>{PackedFamily::minimax},
         "a MiniMax-M3 GGUF needs Maya's pack");
  const auto folder = secure_frontend_test::make_scratch_directory();
  const auto pack = folder / "m" / "maya-mm3";
  const auto made = secure_frontend::card_for_pack({.family = PackedFamily::minimax, .name = "M", .engine = folder / "multiengine.exe",
                                                    .source = "folder", .pack = pack});
  auto parsed = secure_frontend::parse_model_card(made.json, folder);
  const auto* card = std::get_if<secure_frontend::ModelCard>(&parsed);
  expect(card != nullptr, "the MiniMax card is a valid card file");
  if (card == nullptr) return;
  expect(card->profile == secure_frontend::EngineProfile::minimax && made.profile == secure_frontend::EngineProfile::minimax &&
             !card->config.entries.contains("engine.args") &&
             std::get<std::string>(card->config.entries.at("roots.models").value) == secure_frontend::path_to_utf8(pack) &&
             std::get<std::string>(card->config.entries.at("roots.tokenizer_pack").value) == secure_frontend::path_to_utf8(pack / "tokenizer") &&
             std::get<std::string>(card->config.entries.at("engine.model_id").value) == "minimax-m3.1-flash",
         "the MiniMax card: no fixed flags, the pack and its tokenizer, Maya's model name");
  std::vector<std::string> names;
  for (const auto& option : card->options) names.emplace_back(option.spec != nullptr ? option.spec->name : "");
  expect(names == std::vector<std::string>{"max_context", "kv", "thinking", "temperature", "top_p"}, "Maya's MiniMax options");
  const auto default_of = [&card](const std::string_view name) -> std::string {
    for (const auto& option : card->options) {
      if (option.spec == nullptr || option.spec->name != name) continue;
      if (const auto* i = std::get_if<std::int64_t>(&option.default_value)) return std::to_string(*i);
      if (const auto* s = std::get_if<std::string>(&option.default_value)) return *s;
    }
    return {};
  };
  expect(default_of("max_context") == "65536" && default_of("kv") == "int8" && default_of("thinking") == "auto",
         "64K, int8, auto by default");
  std::filesystem::create_directories(pack);
  expect(secure_frontend::write_pack_marker(pack, secure_frontend::kMinimaxModelName) &&
             std::filesystem::is_regular_file(pack / "minimax-m3.1-flash.gguf"),
         "the pack's marker is Maya's model name");
}

// raon-1-mini (owner 2026-10-27: its card from its files): its GGUF needs a pack (strata-prepare's raon1 port, beside the
// file as for GLM-5.3), and the card runs that pack on the GLM engine as the managed raon1 card did - the pack as
// roots.models with its marker (raon1-mini) and tokenizer, KRAFTON's own sampling (temperature 1.0, top_p 0.95, top_k 20,
// no presence penalty over the last 2048 tokens, 32K answers by default), and the GLM engine's typed menus: contexts
// 8K-256K (64K by default), the KV cache (q8_0 by default, as it was served).
void a_raon1_pack_makes_a_raon1_card() {
  using secure_frontend::PackedFamily;
  expect(secure_frontend::packed_family(metadata("raon1")) == std::optional<PackedFamily>{PackedFamily::raon1},
         "a raon1 GGUF needs a pack");
  const auto folder = secure_frontend_test::make_scratch_directory();
  const auto pack = folder / "raon1_q8" / "pack";
  const auto made = secure_frontend::card_for_pack({.family = PackedFamily::raon1, .name = "raon-1-mini-preview-Q8_0",
                                                    .engine = folder / "multiengine.exe", .source = "folder", .pack = pack});
  auto parsed = secure_frontend::parse_model_card(made.json, folder);
  std::string why;
  if (const auto* issues = std::get_if<secure_frontend::ConfigErrors>(&parsed)) {
    for (const auto& issue : *issues) why += secure_frontend::format_issue(issue) + "; ";
  }
  const auto* card = std::get_if<secure_frontend::ModelCard>(&parsed);
  expect(card != nullptr, "the raon1 card is a valid card file: " + why);
  if (card == nullptr) return;
  const auto number = [card](const std::string& key) {
    const auto found = card->config.entries.find(key);
    if (found == card->config.entries.end()) return -1.0;
    if (const auto* d = std::get_if<double>(&found->second.value)) return *d;
    if (const auto* i = std::get_if<std::int64_t>(&found->second.value)) return static_cast<double>(*i);
    return -1.0;
  };
  expect(card->profile == secure_frontend::EngineProfile::raon1 && made.profile == secure_frontend::EngineProfile::raon1 &&
             !card->config.entries.contains("engine.args") &&
             std::get<std::string>(card->config.entries.at("roots.models").value) == secure_frontend::path_to_utf8(pack) &&
             std::get<std::string>(card->config.entries.at("roots.tokenizer_pack").value) == secure_frontend::path_to_utf8(pack / "tokenizer") &&
             std::get<std::string>(card->config.entries.at("engine.model_id").value) == "raon1-mini",
         "the raon1 card: the pack and its tokenizer, the marker's model name, no engine.args");
  expect(number("defaults.sampling.presence_penalty") == 0.0 && number("defaults.sampling.penalty_last_n") == 2048.0 &&
             number("limits.chat.default_max_tokens") == 32768.0,
         "KRAFTON's penalties and answer length, as the managed card had them");
  std::vector<std::string> names;
  for (const auto& option : card->options) names.emplace_back(option.spec != nullptr ? option.spec->name : "");
  expect(names == std::vector<std::string>{"max_context", "kv", "temperature", "top_p", "top_k"}, "the GLM engine's menus and the sampling");
  const auto option = [card](const std::string& name) -> const secure_frontend::ModelOptionLimits* {
    for (const auto& o : card->options) {
      if (o.spec != nullptr && o.spec->name == name) return &o;
    }
    return nullptr;
  };
  const auto* context = option("max_context");
  const auto* kv = option("kv");
  const auto* temperature = option("temperature");
  expect(context != nullptr && std::get<std::int64_t>(context->default_value) == 65536 && kv != nullptr &&
             std::get<std::string>(kv->default_value) == "q8_0" && temperature != nullptr &&
             std::get<double>(temperature->default_value) == 1.0,
         "64K of context, the q8_0 cache, temperature 1.0 by default");
  // the options land on the GLM engine's typed rows, as glm53's do (never engine.args)
  secure_frontend::FrontendConfigDraft draft;
  secure_frontend::apply_option_values(secure_frontend::EngineProfile::raon1,
                                       {{"max_context", std::int64_t{262144}}, {"kv", std::string{"q4_0"}}}, draft);
  expect(draft.entries.contains("glm.max_context") && std::get<std::int64_t>(draft.entries.at("glm.max_context").value) == 262144 &&
             draft.entries.contains("glm.kv") && std::get<std::string>(draft.entries.at("glm.kv").value) == "q4_0" &&
             !draft.entries.contains("engine.args"),
         "the context and the cache as glm.max_context and glm.kv");
  std::filesystem::create_directories(pack);
  expect(secure_frontend::write_pack_marker(pack, secure_frontend::kRaon1ModelName) && std::filesystem::is_regular_file(pack / "raon1-mini.gguf"),
         "the pack's marker is the served model name");
}

void other_architectures_are_not_addable_yet() {
  const auto folder = secure_frontend_test::make_scratch_directory();
  for (const std::string arch : {"qwen4exp", "glm5-next", "raon1", "llama"}) {
    const auto made = secure_frontend::card_for_gguf(metadata(arch), {.folder = folder, .file = "m.gguf", .engine = folder / "e",
                                                                       .source = "folder"});
    const auto* refused = std::get_if<secure_frontend::CardError>(&made);
    expect(refused != nullptr && refused->message.find(arch) != std::string::npos, arch + " is refused by name");
  }
  secure_frontend::GgufMetadata none;
  expect(std::holds_alternative<secure_frontend::CardError>(
             secure_frontend::card_for_gguf(none, {.folder = folder, .file = "m.gguf", .engine = folder / "e", .source = "folder"})),
         "a GGUF with no architecture is refused");
}

void repointing_rewrites_only_the_folder() {
  const auto folder = secure_frontend_test::make_scratch_directory();
  const auto moved = secure_frontend_test::make_scratch_directory();
  const auto made = std::get<secure_frontend::CardText>(secure_frontend::card_for_gguf(
      metadata("flint"), {.folder = folder, .file = "flint-Q8_0.gguf", .engine = folder / "e.exe", .source = "folder"}));
  const auto repointed = secure_frontend::card_with_folder(made.json, moved);
  const auto* text = std::get_if<std::string>(&repointed);
  expect(text != nullptr, "the card is rewritten");
  if (text == nullptr) return;
  const auto parsed = secure_frontend::parse_model_card(*text, folder);
  const auto* card = std::get_if<secure_frontend::ModelCard>(&parsed);
  expect(card != nullptr && std::get<std::string>(card->config.entries.at("roots.models").value) == secure_frontend::path_to_utf8(moved) &&
             std::get<std::string>(card->config.entries.at("engine.model_id").value) == "flint-Q8_0" && card->name == "flint-Q8_0",
         "only roots.models changes");
}

void delete_codes_are_one_time() {
  using Clock = secure_frontend::DeleteCodes::Clock;
  secure_frontend::DeleteCodes codes;
  const auto now = Clock::now();
  const auto code = codes.issue("flint-Q8_0", now);
  expect(code.has_value() && code->size() == 9U && (*code)[4] == '-', "an XXXX-XXXX code");
  if (!code) return;
  for (const char c : *code) {
    expect(c == '-' || (std::string_view{"ABCDEFGHJKMNPQRSTUVWXYZ23456789"}.find(c) != std::string_view::npos),
           "from an unambiguous alphabet (no 0/O, 1/I/L)");
  }
  expect(!codes.claim("other-card", *code, now), "a code is for its card only");
  const auto second = codes.issue("flint-Q8_0", now);
  expect(second && !codes.claim("flint-Q8_0", "WRONG-CODE", now) && !codes.claim("flint-Q8_0", *second, now),
         "a wrong try spends the code");
  const auto third = codes.issue("flint-Q8_0", now);
  expect(third && codes.claim("flint-Q8_0", *third, now + std::chrono::minutes(4)) && !codes.claim("flint-Q8_0", *third, now),
         "a right code works once");
  const auto late = codes.issue("flint-Q8_0", now);
  expect(late && !codes.claim("flint-Q8_0", *late, now + secure_frontend::DeleteCodes::kLifetime + std::chrono::seconds(1)),
         "a code lasts five minutes");
}

// The danger zone's delete: the card's model file(s) - every shard of a split model - and the folder only when nothing
// else is left in it. Nothing else is touched; a link among the files stops it before anything is removed.
void deleting_removes_only_the_model_files() {
  const auto shared = secure_frontend_test::make_scratch_directory();
  secure_frontend_test::write_file(shared / "flint-Q8_0.gguf", "GGUF model");
  secure_frontend_test::write_file(shared / "notes.txt", "mine");
  secure_frontend_test::write_file(shared / "other-model.gguf", "GGUF other");
  const auto one = secure_frontend::delete_gguf_model_files(shared, "flint-Q8_0");
  const auto* done = std::get_if<secure_frontend::DeletedFiles>(&one);
  expect(done != nullptr && done->files == 1U && done->bytes == 10U && !done->folder_removed, "one model file, 10 bytes");
  expect(!std::filesystem::exists(shared / "flint-Q8_0.gguf") && std::filesystem::exists(shared / "notes.txt") &&
             std::filesystem::exists(shared / "other-model.gguf"),
         "only the model's file goes; the folder keeps what else it holds");

  const auto split = secure_frontend_test::make_scratch_directory() / "glm";
  secure_frontend_test::make_private_directory(split);
  for (const char* name : {"GLM-Q4-00001-of-00003.gguf", "GLM-Q4-00002-of-00003.gguf", "GLM-Q4-00003-of-00003.gguf"}) {
    secure_frontend_test::write_file(split / name, "GGUF");
  }
  const auto all = secure_frontend::delete_gguf_model_files(split, "GLM-Q4-00001-of-00003");
  const auto* gone = std::get_if<secure_frontend::DeletedFiles>(&all);
  expect(gone != nullptr && gone->files == 3U && gone->folder_removed && !std::filesystem::exists(split),
         "every shard of a split model, then the empty folder");

  const auto missing = secure_frontend::delete_gguf_model_files(shared, "flint-Q8_0");
  expect(std::holds_alternative<secure_frontend::CardError>(missing), "a model file that is not there is an error, not a no-op");
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"a_flint_gguf_makes_a_flint_card", a_flint_gguf_makes_a_flint_card},
      {"a_qwen36_gguf_makes_a_qwen_card", a_qwen36_gguf_makes_a_qwen_card},
      {"names_are_made_valid_and_splits_named_once", names_are_made_valid_and_splits_named_once},
      {"other_architectures_are_not_addable_yet", other_architectures_are_not_addable_yet},
      {"packed_families_and_their_cards", packed_families_and_their_cards},
      {"a_qwen38_pack_with_its_draft_layer_offers_mtp", a_qwen38_pack_with_its_draft_layer_offers_mtp},
      {"a_deepseek_pack_makes_a_deepseek_card", a_deepseek_pack_makes_a_deepseek_card},
      {"a_minimax_pack_makes_a_minimax_card", a_minimax_pack_makes_a_minimax_card},
      {"a_raon1_pack_makes_a_raon1_card", a_raon1_pack_makes_a_raon1_card},
      {"repointing_rewrites_only_the_folder", repointing_rewrites_only_the_folder},
      {"delete_codes_are_one_time", delete_codes_are_one_time},
      {"deleting_removes_only_the_model_files", deleting_removes_only_the_model_files},
  }, failures);
}
