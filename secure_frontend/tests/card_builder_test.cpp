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
#include <optional>
#include <string>
#include <utility>
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

// Vision for cards (spec 2026-10-28 point 3): a qwen card made with the model's picture encoder (a Qwen3.6 GGUF's or a
// Qwen3.8 pack's) names it (vision.mmproj) and lists the Pictures option, on the GPU by default, as upstream's setup
// runs it; without one, or for a family that reads no pictures here, neither. find_mmproj: the folder's one
// mmproj*.gguf file (none or two: none, so a card never guesses between encoders).
void cards_name_their_picture_encoder() {
  using secure_frontend::PackedFamily;
  const auto folder = secure_frontend_test::make_scratch_directory();
  const auto encoder = folder / "mmproj-F16.gguf";
  const auto rows_and_option = [&folder](const std::string& json) {
    std::pair<std::optional<std::string>, std::optional<std::string>> out;
    auto parsed = secure_frontend::parse_model_card(json, folder);
    const auto* card = std::get_if<secure_frontend::ModelCard>(&parsed);
    std::string why;
    if (const auto* issues = std::get_if<secure_frontend::ConfigErrors>(&parsed)) {
      for (const auto& issue : *issues) why += secure_frontend::format_issue(issue) + "; ";
    }
    expect(card != nullptr, "a valid card file: " + why);
    if (card == nullptr) return out;
    if (const auto row = card->config.entries.find("vision.mmproj"); row != card->config.entries.end()) out.first = std::get<std::string>(row->second.value);
    for (const auto& limit : card->options) {
      if (limit.spec != nullptr && limit.spec->name == "vision") out.second = std::get<std::string>(limit.default_value);
    }
    return out;
  };
  const auto qwen36 = secure_frontend::card_for_gguf(metadata("qwen35moe"), {.folder = folder, .file = "Qwen3.6-Q8_0.gguf",
                                                                            .engine = folder / "multiengine.exe", .source = "folder",
                                                                            .mmproj = encoder});
  const auto* qwen36_card = std::get_if<secure_frontend::CardText>(&qwen36);
  expect(qwen36_card != nullptr, "a Qwen3.6 card with its encoder is made");
  if (qwen36_card != nullptr) {
    const auto [row, option] = rows_and_option(qwen36_card->json);
    expect(row == std::optional<std::string>{secure_frontend::path_to_utf8(encoder)} && option == std::optional<std::string>{"gpu"},
           "the Qwen3.6 card names its encoder and reads pictures on the GPU by default: " + qwen36_card->json);
  }
  const auto gemma = secure_frontend::card_for_gguf(metadata("gemma4"), {.folder = folder, .file = "gemma-4.gguf", .engine = folder / "e.exe",
                                                                        .source = "folder", .mmproj = encoder});
  const auto* gemma_card = std::get_if<secure_frontend::CardText>(&gemma);
  expect(gemma_card != nullptr && rows_and_option(gemma_card->json) == decltype(rows_and_option(gemma_card->json)){},
         "Gemma 4 reads no pictures here: no encoder row, no Pictures option");
  secure_frontend::PackedCardRequest request{.family = PackedFamily::qwen38, .name = "Q", .engine = folder / "multiengine.exe",
                                             .source = "folder", .pack = folder / "packs" / "Q", .folder = folder / "models",
                                             .model_id = "q-00001-of-00002"};
  expect(rows_and_option(secure_frontend::card_for_pack(request).json) == decltype(rows_and_option(std::string{})){},
         "a pack card without an encoder reads no pictures");
  request.mmproj = encoder;
  const auto [pack_row, pack_option] = rows_and_option(secure_frontend::card_for_pack(request).json);
  expect(pack_row == std::optional<std::string>{secure_frontend::path_to_utf8(encoder)} && pack_option == std::optional<std::string>{"gpu"},
         "a Qwen3.8 pack card names its encoder and lists Pictures");
  // find_mmproj
  const auto models = folder / "models";
  secure_frontend_test::make_private_directory(models);
  secure_frontend_test::write_file(models / "q-00001-of-00002.gguf", "GGUF");
  expect(!secure_frontend::find_mmproj(models), "no encoder in the folder: none");
  secure_frontend_test::write_file(models / "mmproj-Qwen3.8-Flash-Next-BF16.gguf", "GGUF");
  expect(secure_frontend::find_mmproj(models) == std::optional<std::filesystem::path>{models / "mmproj-Qwen3.8-Flash-Next-BF16.gguf"},
         "the folder's one mmproj*.gguf");
  secure_frontend_test::write_file(models / "MMPROJ-F16.gguf", "GGUF");
  expect(!secure_frontend::find_mmproj(models), "two encoders: none (never a guess)");
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
             !card->config.entries.contains("engine.args") &&   // --tp and --threads follow the GPUs at each launch
             std::get<std::string>(card->config.entries.at("roots.models").value) == secure_frontend::path_to_utf8(pack) &&
             std::get<std::string>(card->config.entries.at("roots.tokenizer_pack").value) == secure_frontend::path_to_utf8(pack / "tokenizer") &&
             std::get<std::string>(card->config.entries.at("engine.model_id").value) == "deepseek-v4.1-flash",
         "the DeepSeek card: no fixed --tp or --threads (the launch gives them by the GPUs), the pack and its tokenizer, Maya's model name");
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

// Owner 2026-10-28 ("gemma4 has an mtp ... make sure we work that mtp in our gemma engine"): a Gemma 4 card made with
// its MTP draft file runs it (engine.args --mtp <the file>) and lists the MTP option, on by default (off drops --mtp);
// without one, or for flint (its engine runs none), neither. find_mtp_gguf: the model folder's one mtp-*.gguf, beside
// the model or in its MTP/ folder (unsloth's layout), a regular file; none or two: none (never a guess).
void a_gemma4_card_runs_its_mtp_file() {
  const auto folder = secure_frontend_test::make_scratch_directory();
  const auto mtp = folder / "MTP" / "mtp-gemma-4-Q8_0.gguf";
  const auto card_of = [&folder](const secure_frontend::CardText* text) -> std::optional<secure_frontend::ModelCard> {
    if (text == nullptr) return std::nullopt;
    auto parsed = secure_frontend::parse_model_card(text->json, folder);
    std::string why;
    if (const auto* issues = std::get_if<secure_frontend::ConfigErrors>(&parsed)) {
      for (const auto& issue : *issues) why += secure_frontend::format_issue(issue) + "; ";
    }
    expect(std::holds_alternative<secure_frontend::ModelCard>(parsed), "a valid card file: " + why);
    if (!std::holds_alternative<secure_frontend::ModelCard>(parsed)) return std::nullopt;
    return std::get<secure_frontend::ModelCard>(std::move(parsed));
  };
  const auto args_of = [](const secure_frontend::ModelCard& card) {
    const auto found = card.config.entries.find("engine.args");
    return found == card.config.entries.end() ? std::vector<std::string>{} : std::get<std::vector<std::string>>(found->second.value);
  };
  const auto mtp_default = [](const secure_frontend::ModelCard& card) -> std::optional<bool> {
    for (const auto& limit : card.options) {
      if (limit.spec != nullptr && limit.spec->name == "mtp") return std::get<bool>(limit.default_value);
    }
    return std::nullopt;
  };
  const auto with = secure_frontend::card_for_gguf(metadata("gemma4"), {.folder = folder, .file = "gemma-4-Q8_0.gguf", .engine = folder / "e.exe",
                                                                       .source = "folder", .mtp = mtp});
  const auto card = card_of(std::get_if<secure_frontend::CardText>(&with));
  expect(card && args_of(*card) == std::vector<std::string>{"--mtp", secure_frontend::path_to_utf8(mtp)} &&
             mtp_default(*card) == std::optional<bool>{true},
         "the Gemma 4 card drafts with its MTP file (--mtp <file>), the MTP option on");
  // and lists the drafter's window and confidence: a window of 4 (llama.cpp's 3 drafts) and 0.6, the floor measured
  // fastest on the 4080 SUPER (86.5-91.9 tok/s against 81.2 at llama.cpp's 0 and 74-76 without the drafter)
  const auto listed = [](const secure_frontend::ModelCard& c, const std::string_view name) {
    for (const auto& limit : c.options) {
      if (limit.spec != nullptr && limit.spec->name == name) return std::optional<secure_frontend::ModelOptionValue>{limit.default_value};
    }
    return std::optional<secure_frontend::ModelOptionValue>{};
  };
  const auto window = card ? listed(*card, "spec") : std::nullopt;
  const auto confidence = card ? listed(*card, "spec_min_p") : std::nullopt;
  expect(window && std::get<std::int64_t>(*window) == 4 && confidence && std::get<double>(*confidence) == 0.6,
         "its draft window (4) and draft confidence (0.6) are on the card");
  const auto plain = secure_frontend::card_for_gguf(metadata("gemma4"), {.folder = folder, .file = "gemma-4-Q8_0.gguf", .engine = folder / "e.exe",
                                                                        .source = "folder"});
  const auto plain_card = card_of(std::get_if<secure_frontend::CardText>(&plain));
  expect(plain_card && args_of(*plain_card).empty() && !mtp_default(*plain_card) && !listed(*plain_card, "spec") &&
             !listed(*plain_card, "spec_min_p"),
         "without one: no --mtp, no MTP option, no draft window or confidence");
  const auto flint = secure_frontend::card_for_gguf(metadata("flint"), {.folder = folder, .file = "flint-Q8_0.gguf", .engine = folder / "e.exe",
                                                                       .source = "folder", .mtp = mtp});
  const auto flint_card = card_of(std::get_if<secure_frontend::CardText>(&flint));
  expect(flint_card && args_of(*flint_card).empty() && !mtp_default(*flint_card), "flint's engine runs no MTP file: none on its card");

  // find_mtp_gguf: the one named for the model, else the folder's only one when it holds this model alone
  using Found = std::optional<std::filesystem::path>;
  const auto models = folder / "models";
  secure_frontend_test::make_private_directory(models);
  secure_frontend_test::write_file(models / "gemma-4-Q8_0.gguf", "GGUF");
  secure_frontend_test::write_file(models / "mmproj-F16.gguf", "GGUF");   // a picture encoder is not another model
  secure_frontend_test::write_file(models / "mtp-notes.txt", "not a GGUF");
  expect(!secure_frontend::find_mtp_gguf(models, "gemma-4-Q8_0.gguf"), "no MTP file in the folder: none");
  secure_frontend_test::make_private_directory(models / "MTP");
  secure_frontend_test::write_file(models / "MTP" / "mtp-gemma-4-Q8_0.gguf", "GGUF");
  expect(secure_frontend::find_mtp_gguf(models, "gemma-4-Q8_0.gguf") == Found{models / "MTP" / "mtp-gemma-4-Q8_0.gguf"},
         "the one in its MTP/ folder named for the model (unsloth's layout)");
  secure_frontend_test::write_file(models / "MTP-gemma-4-Q4.gguf", "GGUF");
  expect(secure_frontend::find_mtp_gguf(models, "gemma-4-Q8_0.gguf") == Found{models / "MTP" / "mtp-gemma-4-Q8_0.gguf"},
         "the one named for the model wins over another");
  expect(!secure_frontend::find_mtp_gguf(models, "gemma-4-Q4_K_M.gguf"), "two MTP files, none named for the model: none (never a guess)");
  std::filesystem::remove(models / "MTP" / "mtp-gemma-4-Q8_0.gguf");
  expect(secure_frontend::find_mtp_gguf(models, "gemma-4-Q8_0.gguf") == Found{models / "MTP-gemma-4-Q4.gguf"},
         "the folder's only MTP file, beside its only model (names in any case)");
  secure_frontend_test::write_file(models / "gemma-4-31B-Q8_0.gguf", "GGUF");
  expect(!secure_frontend::find_mtp_gguf(models, "gemma-4-Q8_0.gguf"),
         "a folder of two models: an MTP file named for neither is never guessed");
  secure_frontend_test::write_file(models / "mtp-gemma-4-31B-Q8_0.gguf", "GGUF");
  expect(secure_frontend::find_mtp_gguf(models, "gemma-4-31B-Q8_0.gguf") == Found{models / "mtp-gemma-4-31B-Q8_0.gguf"},
         "but the one named for the model is");
}

// A Gemma 4 card's MTP file in its model folder moves with the folder when the new folder holds it at the same place
// (spec section 4.4: the files were moved together); one the new folder lacks (the model alone moved), or one kept elsewhere, stays
// as it was.
void repointing_moves_the_mtp_file_with_the_folder() {
  const auto folder = secure_frontend_test::make_scratch_directory();
  const auto moved = secure_frontend_test::make_scratch_directory();
  const auto elsewhere = secure_frontend_test::make_scratch_directory();
  secure_frontend_test::make_private_directory(moved / "MTP");
  secure_frontend_test::write_file(moved / "MTP" / "mtp-g.gguf", "GGUF");
  secure_frontend_test::write_file(moved / "mtp-g.gguf", "GGUF");
  const auto args_after = [&](const std::filesystem::path& mtp) {
    const auto made = std::get<secure_frontend::CardText>(secure_frontend::card_for_gguf(
        metadata("gemma4"), {.folder = folder, .file = "g.gguf", .engine = folder / "e.exe", .source = "folder", .mtp = mtp}));
    const auto repointed = secure_frontend::card_with_folder(made.json, moved);
    const auto* text = std::get_if<std::string>(&repointed);
    if (text == nullptr) return std::vector<std::string>{"<not rewritten>"};
    const auto parsed = secure_frontend::parse_model_card(*text, folder);
    const auto* card = std::get_if<secure_frontend::ModelCard>(&parsed);
    if (card == nullptr || std::get<std::string>(card->config.entries.at("roots.models").value) != secure_frontend::path_to_utf8(moved)) {
      return std::vector<std::string>{"<not moved>"};
    }
    return std::get<std::vector<std::string>>(card->config.entries.at("engine.args").value);
  };
  expect(args_after(folder / "MTP" / "mtp-g.gguf") ==
             std::vector<std::string>{"--mtp", secure_frontend::path_to_utf8(moved / "MTP" / "mtp-g.gguf")},
         "the MTP file in the model folder follows it");
  expect(args_after(folder / "mtp-g.gguf") == std::vector<std::string>{"--mtp", secure_frontend::path_to_utf8(moved / "mtp-g.gguf")},
         "beside the model too");
  expect(args_after(elsewhere / "mtp-g.gguf") == std::vector<std::string>{"--mtp", secure_frontend::path_to_utf8(elsewhere / "mtp-g.gguf")},
         "one outside the model folder stays where it is");
  expect(args_after(folder / "MTP" / "mtp-other.gguf") ==
             std::vector<std::string>{"--mtp", secure_frontend::path_to_utf8(folder / "MTP" / "mtp-other.gguf")},
         "one the new folder does not hold stays where it was (the model alone moved)");
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

// Owner 2026-10-28 ("when we point it to a local folder with the model, if it match the card of the model that is
// downloadable, we should change the banner from available to Locally Provided vs Downloaded"): the model file a card
// runs, which the Download list matches to a catalog entry. A card from a GGUF and a Qwen3.8 pack card name it
// (roots.models / engine.model_id .gguf); a card on a Maya pack (GLM-5.3, raon-1-mini, DeepSeek, MiniMax) names the pack
// and Maya's model name (its marker), so its model is the first shard the pack was built from, beside the pack - the one
// native_experts.txt indexes, else the first sources.json lists (MiniMax's indexers file after the shards is not it).
void a_cards_model_file_is_the_gguf_it_runs() {
  using secure_frontend::PackedFamily;
  const auto folder = secure_frontend_test::make_scratch_directory();
  const auto engine = folder / "multiengine.exe";
  const auto card_of = [&folder](const std::string& json) -> std::optional<secure_frontend::ModelCard> {
    auto parsed = secure_frontend::parse_model_card(json, folder);
    std::string why;
    if (const auto* issues = std::get_if<secure_frontend::ConfigErrors>(&parsed)) {
      for (const auto& issue : *issues) why += secure_frontend::format_issue(issue) + "; ";
    }
    expect(std::holds_alternative<secure_frontend::ModelCard>(parsed), "a valid card file: " + why);
    if (auto* card = std::get_if<secure_frontend::ModelCard>(&parsed)) return std::move(*card);
    return std::nullopt;
  };
  const auto model_file = [](const std::optional<secure_frontend::ModelCard>& card) -> std::string {
    if (!card) return "no card";
    const auto file = secure_frontend::card_model_file(*card);
    return file ? secure_frontend::path_to_utf8(*file) : std::string{"none"};
  };
  const auto at = [](const std::filesystem::path& path) { return secure_frontend::path_to_utf8(path); };
  const auto nested = [](const std::filesystem::path& path) {
    secure_frontend_test::make_private_directory(path.parent_path());
    secure_frontend_test::make_private_directory(path);
  };

  // from a GGUF: the file picked (a split model's first shard)
  const auto flint = secure_frontend::card_for_gguf(metadata("flint"), {.folder = folder / "f", .file = "flint-Q8_0-00001-of-00002.gguf",
                                                                        .engine = engine, .source = "folder"});
  const auto* flint_text = std::get_if<secure_frontend::CardText>(&flint);
  expect(flint_text != nullptr && model_file(card_of(flint_text->json)) == at(folder / "f" / "flint-Q8_0-00001-of-00002.gguf"),
         "a card from a GGUF runs that file: " + (flint_text != nullptr ? model_file(card_of(flint_text->json)) : std::string{}));

  // Qwen3.8: the model folder and the first shard, the pack elsewhere
  const auto qwen = secure_frontend::card_for_pack({.family = PackedFamily::qwen38, .name = "Q", .engine = engine, .source = "folder",
                                                    .pack = folder / "packs" / "Q", .folder = folder / "models",
                                                    .model_id = "q-00001-of-00002"});
  expect(model_file(card_of(qwen.json)) == at(folder / "models" / "q-00001-of-00002.gguf"),
         "a Qwen3.8 card runs its first shard: " + model_file(card_of(qwen.json)));

  // GLM-5.3 and raon-1-mini on pack/: the shard native_experts.txt indexes, beside the pack
  const auto glm_folder = folder / "glm";
  nested(glm_folder / "pack");
  secure_frontend_test::write_file(glm_folder / "pack" / "native_experts.txt",
                                   "# strata native experts v3: ... absolute offsets in GLM-Test-00001-of-00002.gguf, or in the named shard(s) beside it)\n1 2 3\n");
  const auto glm = card_of(secure_frontend::card_for_pack({.family = PackedFamily::glm53, .name = "G", .engine = engine, .source = "folder",
                                                           .pack = glm_folder / "pack"}).json);
  expect(model_file(glm) == at(glm_folder / "GLM-Test-00001-of-00002.gguf"), "a GLM card runs the shards its pack indexes: " + model_file(glm));
  const auto raon_folder = folder / "raon";
  nested(raon_folder / "pack-raon");
  secure_frontend_test::write_file(raon_folder / "pack-raon" / "native_experts.txt",
                                   "# strata native experts v3: ... absolute offsets in raon-1-mini-preview-Q8_0.gguf, or in the named shard(s) beside it)\r\n");
  const auto raon = card_of(secure_frontend::card_for_pack({.family = PackedFamily::raon1, .name = "R", .engine = engine, .source = "folder",
                                                            .pack = raon_folder / "pack-raon"}).json);
  expect(model_file(raon) == at(raon_folder / "raon-1-mini-preview-Q8_0.gguf"), "a raon1 card likewise (a CRLF line too): " + model_file(raon));

  // DeepSeek-V4.1 and MiniMax-M3.1: no native_experts.txt; the first model file sources.json lists
  const auto ds_folder = folder / "ds";
  nested(ds_folder / "maya-ds4");
  secure_frontend_test::write_file(ds_folder / "maya-ds4" / "sources.json",
                                   "{\n \"files\": [\n  {\n   \"name\": \"DS-Test-00001-of-00002.gguf\",\n   \"size\": 10,\n   \"mtime_ns\": 1\n  },\n"
                                   "  {\n   \"name\": \"DS-Test-00002-of-00002.gguf\",\n   \"size\": 10,\n   \"mtime_ns\": 1\n  }\n ],\n \"tools\": \"x\"\n}");
  const auto ds = card_of(secure_frontend::card_for_pack({.family = PackedFamily::deepseek, .name = "D", .engine = engine, .source = "folder",
                                                          .pack = ds_folder / "maya-ds4"}).json);
  expect(model_file(ds) == at(ds_folder / "DS-Test-00001-of-00002.gguf"), "a DeepSeek card runs the shards sources.json lists: " + model_file(ds));
  const auto mm_folder = folder / "mm";
  nested(mm_folder / "maya-mm3");
  secure_frontend_test::write_file(mm_folder / "maya-mm3" / "sources.json",
                                   "{\"files\": [{\"name\": \"MM-Test-maya-indexers.gguf\", \"size\": 1, \"mtime_ns\": 1}, "
                                   "{\"name\": \"MM-Test-IQ1_S-00002-of-00002.gguf\", \"size\": 1, \"mtime_ns\": 1}, "
                                   "{\"name\": \"MM-Test-IQ1_S-00001-of-00002.gguf\", \"size\": 1, \"mtime_ns\": 1}]}");
  const auto mm = card_of(secure_frontend::card_for_pack({.family = PackedFamily::minimax, .name = "M", .engine = engine, .source = "folder",
                                                          .pack = mm_folder / "maya-mm3"}).json);
  expect(model_file(mm) == at(mm_folder / "MM-Test-IQ1_S-00001-of-00002.gguf"),
         "a MiniMax card runs its first shard, never its indexers file nor a later shard: " + model_file(mm));

  // a pack that records no source, a name that is a path, and a card naming no model folder: nothing is guessed
  const auto bare = folder / "bare";
  nested(bare / "pack");
  const auto unknown = card_of(secure_frontend::card_for_pack({.family = PackedFamily::glm53, .name = "U", .engine = engine, .source = "folder",
                                                               .pack = bare / "pack"}).json);
  expect(model_file(unknown) == "none", "a pack that names no source: none: " + model_file(unknown));
  const auto away = folder / "away";
  nested(away / "pack");
  secure_frontend_test::write_file(away / "pack" / "native_experts.txt", "# ... absolute offsets in ../elsewhere/GLM-00001-of-00002.gguf, or ...\n");
  const auto outside = card_of(secure_frontend::card_for_pack({.family = PackedFamily::glm53, .name = "W", .engine = engine, .source = "folder",
                                                               .pack = away / "pack"}).json);
  expect(model_file(outside) == "none", "a pack's source is a file name beside the pack, never a path: " + model_file(outside));
  const auto handwritten = card_of(R"({"schema_version":1,"name":"H","profile":"gemma4","config":{"engine":{"executable":")" +
                                   engine.generic_string() + R"("}},"options":{}})");
  expect(model_file(handwritten) == "none", "a card that names no model folder or model: none: " + model_file(handwritten));
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"a_flint_gguf_makes_a_flint_card", a_flint_gguf_makes_a_flint_card},
      {"a_qwen36_gguf_makes_a_qwen_card", a_qwen36_gguf_makes_a_qwen_card},
      {"names_are_made_valid_and_splits_named_once", names_are_made_valid_and_splits_named_once},
      {"cards_name_their_picture_encoder", cards_name_their_picture_encoder},
      {"other_architectures_are_not_addable_yet", other_architectures_are_not_addable_yet},
      {"packed_families_and_their_cards", packed_families_and_their_cards},
      {"a_qwen38_pack_with_its_draft_layer_offers_mtp", a_qwen38_pack_with_its_draft_layer_offers_mtp},
      {"a_deepseek_pack_makes_a_deepseek_card", a_deepseek_pack_makes_a_deepseek_card},
      {"a_minimax_pack_makes_a_minimax_card", a_minimax_pack_makes_a_minimax_card},
      {"a_raon1_pack_makes_a_raon1_card", a_raon1_pack_makes_a_raon1_card},
      {"repointing_rewrites_only_the_folder", repointing_rewrites_only_the_folder},
      {"a_gemma4_card_runs_its_mtp_file", a_gemma4_card_runs_its_mtp_file},
      {"repointing_moves_the_mtp_file_with_the_folder", repointing_moves_the_mtp_file_with_the_folder},
      {"delete_codes_are_one_time", delete_codes_are_one_time},
      {"deleting_removes_only_the_model_files", deleting_removes_only_the_model_files},
      {"a_cards_model_file_is_the_gguf_it_runs", a_cards_model_file_is_the_gguf_it_runs},
  }, failures);
}
