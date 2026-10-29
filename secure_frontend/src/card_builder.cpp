#include "secure_frontend/card_builder.hpp"

#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/json.hpp"
#include "secure_frontend/secret_bytes.hpp"
#include "secure_frontend/token_generator.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

namespace secure_frontend {
namespace {

// The page's context dropdown for these engines (model_options.cpp kGemmaContexts).
constexpr std::array<std::int64_t, 7> kContexts{4096, 8192, 16384, 32768, 65536, 131072, 262144};
constexpr std::int64_t kDefaultContext = 65536;
// Qwen3.6 (upstream's OWN_PATH family): 32K by default as the download card had it (download_manager.cpp
// kCustomProfiles qwen36), the model's own context the most (its dropdown runs past 256K with YaRN; never offered here).
constexpr std::int64_t kQwen36DefaultContext = 32768;
constexpr std::string_view kCodeAlphabet = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
// the MTP draft layer's runtime files as upstream's setup leaves them in mtp/rt (tools/mtp_rt.py, refresh_draft_vocab)
constexpr std::array<std::string_view, 3> kMtpRuntimeFiles{"dense.bin", "experts.bin", "draft_vocab.bin"};

std::string decimal(const double value) {
  char buffer[32];
  const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), value);
  std::string text = error == std::errc{} ? std::string{buffer, end} : std::string{"0"};
  if (text.find_first_of(".eE") == std::string::npos) text += ".0";
  return text;
}

bool card_name_character(const char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

// "<prefix>-00001-of-00003" -> {prefix, 3}; nullopt for a name that is not a split model's shard.
std::optional<std::pair<std::string, int>> split_shard(const std::string_view stem) {
  constexpr std::size_t kTail = 15U;   // "-NNNNN-of-NNNNN"
  if (stem.size() <= kTail) return std::nullopt;
  const std::string_view tail = stem.substr(stem.size() - kTail);
  const auto digits = [](const std::string_view text) {
    return std::all_of(text.begin(), text.end(), [](const char c) { return c >= '0' && c <= '9'; });
  };
  if (tail[0] != '-' || !digits(tail.substr(1, 5)) || tail.substr(6, 4) != "-of-" || !digits(tail.substr(10, 5))) return std::nullopt;
  int count = 0;
  for (const char c : tail.substr(10, 5)) count = count * 10 + (c - '0');
  return std::pair<std::string, int>{std::string{stem.substr(0, stem.size() - kTail)}, count};
}

// The JSON string literal whose opening quote is text[at] (as this module writes them): its closing quote's index and
// its value. nullopt: not one.
std::optional<std::pair<std::size_t, std::string>> json_string_at(const std::string_view text, const std::size_t at) {
  if (at >= text.size() || text[at] != '"') return std::nullopt;
  std::size_t end = at + 1U;
  while (end < text.size() && text[end] != '"') end += text[end] == '\\' ? 2U : 1U;
  if (end >= text.size()) return std::nullopt;
  const auto parsed = parse_json(text.substr(at, end - at + 1U), JsonLimits{.max_bytes = 1U << 16U, .max_depth = 2U, .max_container_entries = 1U,
                                                                           .max_string_bytes = 1U << 15U, .allow_fractions = false});
  const auto* value = std::get_if<JsonValue>(&parsed);
  if (value == nullptr || value->if_string() == nullptr) return std::nullopt;
  return std::pair<std::size_t, std::string>{end, *value->if_string()};
}

std::string card_name_from(const std::string_view stem) {
  std::string base{stem};
  if (const auto split = split_shard(stem)) base = split->first;
  std::string name;
  for (const char c : base) {
    const char kept = card_name_character(c) ? c : '-';
    if (kept == '-' && !name.empty() && name.back() == '-') continue;
    name.push_back(kept);
  }
  while (!name.empty() && !std::isalnum(static_cast<unsigned char>(name.front()))) name.erase(name.begin());
  while (!name.empty() && name.back() == '-') name.pop_back();
  if (name.size() > 64U) name.resize(64U);
  while (!name.empty() && name.back() == '-') name.pop_back();
  return name;
}


// The three runtime files of upstream's MTP draft layer in `rt`, each a regular file and not a link.
bool holds_mtp_runtime(const std::filesystem::path& rt) {
  std::error_code ec;
  return std::all_of(kMtpRuntimeFiles.begin(), kMtpRuntimeFiles.end(), [&rt, &ec](const std::string_view file) {
    return std::filesystem::is_regular_file(rt / file, ec) && !std::filesystem::is_symlink(rt / file, ec);
  });
}

const JsonValue* object_member(const JsonValue& object, const std::string_view key) {
  const auto* members = object.if_object();
  if (members == nullptr) return nullptr;
  const auto it = std::find_if(members->begin(), members->end(), [key](const JsonMember& member) { return member.key == key; });
  return it == members->end() ? nullptr : &it->value;
}

// `object` with `key` set to `value`: in its place when there, else last.
JsonValue with_member(const JsonValue& object, const std::string& key, JsonValue value) {
  std::vector<JsonMember> members = object.if_object() != nullptr ? *object.if_object() : std::vector<JsonMember>{};
  const auto it = std::find_if(members.begin(), members.end(), [&key](const JsonMember& member) { return member.key == key; });
  if (it != members.end()) {
    it->value = std::move(value);
  } else {
    members.push_back(JsonMember{key, std::move(value)});
  }
  return JsonValue::make_object(std::move(members));
}
}  // namespace

std::variant<CardText, CardError> card_for_gguf(const GgufMetadata& metadata, const GgufCardRequest& request) {
  const auto* arch = metadata.string("general.architecture");
  if (arch == nullptr || arch->empty()) return CardError{"the GGUF names no model architecture (general.architecture)"};
  EngineProfile profile{};
  bool thinking = false;
  if (*arch == "flint") {
    profile = EngineProfile::flint;
    thinking = true;   // the template always lets the model think
  } else if (*arch == "gemma4") {
    profile = EngineProfile::gemma4;   // the template's default: thinking off
  } else if (*arch == "qwen35moe") {
    profile = EngineProfile::qwen;     // Qwen3.6: upstream's engine reads the GGUF itself (--native)
  } else {
    std::string shown;
    for (const char c : arch->substr(0, 64)) shown.push_back(card_name_character(c) ? c : '?');
    return CardError{"a " + shown + " model can't be added from its files yet (flint, Gemma 4 and Qwen3.6 can)"};
  }
  const bool qwen = profile == EngineProfile::qwen;
  // the MTP draft layer the file carries (Unsloth's Qwen3.6 GGUFs do): upstream runs it with two drafts
  const bool mtp = qwen && metadata.integer(*arch + ".nextn_predict_layers").value_or(0) > 0;
  // Gemma 4's MTP draft file (owner 2026-10-28), a GGUF of its own: the Gemma engine's --mtp <file>
  const bool drafts = profile == EngineProfile::gemma4 && request.mtp.has_value();
  const std::string_view file = request.file;
  if (file.size() <= 5U || file.substr(file.size() - 5U) != ".gguf") return CardError{"the model file must be a .gguf file"};
  const std::string stem{file.substr(0, file.size() - 5U)};
  const std::string name = card_name_from(stem);
  if (name.empty()) return CardError{"the model file's name gives no usable card name"};

  // the context: the model's own, at most the dropdown's largest; 64K by default (or the largest that fits)
  const std::int64_t trained = metadata.integer(*arch + ".context_length").value_or(kContexts.back());
  std::int64_t maximum = kContexts.front();
  for (const auto value : kContexts) {
    if (value <= trained) maximum = value;
  }
  const std::int64_t context = std::min(qwen ? kQwen36DefaultContext : kDefaultContext, maximum);

  std::string json = "{\n  \"schema_version\": 1,\n  \"name\": ";
  append_json_string(&json, name);
  json += ",\n  \"profile\": ";
  append_json_string(&json, profile == EngineProfile::flint ? "flint" : qwen ? "qwen" : "gemma4");
  json += ",\n  \"source\": ";
  append_json_string(&json, request.source);
  json += ",\n  \"config\": {\n    \"engine\": {\"executable\": ";
  append_json_string(&json, path_to_utf8(request.engine));
  json += ", \"model_id\": ";
  append_json_string(&json, stem);
  if (mtp) json += ", \"args\": [\"--mtp\", \"2\"]";
  if (drafts) {
    json += ", \"args\": [\"--mtp\", ";
    append_json_string(&json, path_to_utf8(*request.mtp));
    json += "]";
  }
  json += "},\n    \"roots\": {\"models\": ";
  append_json_string(&json, path_to_utf8(request.folder));
  json += "}";
  const bool pictures = qwen && request.mmproj.has_value();   // the families upstream's engine reads pictures with
  if (pictures) {
    json += ",\n    \"vision\": {\"mmproj\": ";
    append_json_string(&json, path_to_utf8(*request.mmproj));
    json += "}";
  }
  // Qwen's chat rows, as the download card had them: long prompts and answers (the engine's own context bounds them)
  if (qwen) json += ",\n    \"limits\": {\"chat\": {\"max_prompt_tokens\": 1048576, \"max_completion_tokens\": 1048576}}";
  json += "\n  },\n  \"options\": {\n";
  json += "    \"max_context\": {\"default\": " + std::to_string(context) + ", \"min\": 4096, \"max\": " + std::to_string(maximum) +
          "},\n";
  if (qwen) {
    json += std::string{"    \"mtp\": {\"default\": "} + (mtp ? "true" : "false") + "},\n";
    json += "    \"prefill\": {\"default\": \"auto\"},\n";
    if (pictures) json += "    \"vision\": {\"default\": \"gpu\"},\n";   // upstream setup's --vision gpu
    json += "    \"thinking\": {\"default\": \"high\"}";
  } else {
    json += "    \"expert_vram\": {\"default\": \"auto\"},\n";
    if (drafts) {
      json += "    \"mtp\": {\"default\": true},\n";   // the MTP switch: off drops --mtp <file> for a start
      // its window and confidence: a window of 4 (llama.cpp's 3 drafts) and a 0.6 floor, measured fastest on a 4080 SUPER
      // (86.5-91.9 tok/s served; 81.2 at llama.cpp's p_min 0; 74-76 without the drafter)
      json += "    \"spec\": {\"default\": 4},\n    \"spec_min_p\": {\"default\": 0.6},\n";
    }
    json += std::string{"    \"thinking\": {\"default\": \""} + (thinking ? "on" : "off") + "\"}";
  }
  if (const auto temperature = metadata.real("general.sampling.temp"); temperature && *temperature >= 0.0 && *temperature <= 2.0) {
    json += ",\n    \"temperature\": {\"default\": " + decimal(std::round(*temperature * 100.0) / 100.0) + "}";
  }
  if (const auto top_p = metadata.real("general.sampling.top_p"); top_p && *top_p > 0.0 && *top_p <= 1.0) {
    const double rounded = std::round(*top_p * 100.0) / 100.0;
    if (rounded > 0.0) json += ",\n    \"top_p\": {\"default\": " + decimal(rounded) + "}";
  }
  if (const auto top_k = metadata.integer("general.sampling.top_k"); top_k && *top_k >= 1 && *top_k <= 64) {
    json += ",\n    \"top_k\": {\"default\": " + std::to_string(*top_k) + "}";
  }
  if (profile == EngineProfile::gemma4) json += ",\n    \"min_p\": {\"default\": 0.05}";   // README: no repetition loops
  json += "\n  }\n}\n";
  return CardText{name, profile, std::move(json)};
}

std::optional<PackedFamily> packed_family(const GgufMetadata& metadata) {
  const auto* arch = metadata.string("general.architecture");
  if (arch == nullptr) return std::nullopt;
  if (*arch == "qwen4exp") return PackedFamily::qwen38;
  if (*arch == "glm5-next" || *arch == "glm5next") return PackedFamily::glm53;
  if (*arch == "deepseek41") return PackedFamily::deepseek;
  if (*arch == "raon1") return PackedFamily::raon1;
  if (*arch == "minimax-m3") return PackedFamily::minimax;
  return std::nullopt;
}

CardText card_for_pack(const PackedCardRequest& request) {
  const auto text = [](const std::string_view value) {
    std::string out;
    append_json_string(&out, value);
    return out;
  };
  const char* const profile = request.family == PackedFamily::glm53      ? "glm53"
                              : request.family == PackedFamily::deepseek ? "deepseek"
                              : request.family == PackedFamily::raon1    ? "raon1"
                              : request.family == PackedFamily::minimax  ? "minimax"
                                                                         : "qwen";
  const std::string head = "{\"schema_version\":1,\"name\":" + text(request.name) + ",\"profile\":\"" + profile +
                           "\",\"source\":" + text(request.source) +
                           ",\"config\":{\"engine\":{\"executable\":" + text(path_to_utf8(request.engine)) + ",\"model_id\":";
  if (request.family == PackedFamily::deepseek) {
    // Maya's write_config_ds4 (maya.py v1.1.30 :4136-4138; the launch adds --pack, --tp and --threads by the card's GPUs,
    // and --vision off), the pack as the model folder and its tokenizer; Maya's DeepSeek menus (DS4_CONTEXTS, 64K;
    // DS4_EFFORT; DS4_SAMPLING)
    const std::string json =
        head + text(kDeepseekModelName) + "},\"roots\":{\"models\":" +
        text(path_to_utf8(request.pack)) + ",\"tokenizer_pack\":" + text(path_to_utf8(request.pack / "tokenizer")) +
        "},\"limits\":{\"chat\":{\"max_prompt_tokens\":1048576,\"max_completion_tokens\":1048576}}}," +
        "\"options\":{\"max_context\":{\"default\":65536,\"min\":32768,\"max\":1048576},\"thinking\":{\"default\":\"high\"}," +
        "\"temperature\":{\"default\":1.0},\"top_p\":{\"default\":1.0}}}\n";
    return CardText{request.name, EngineProfile::deepseek, json};
  }
  if (request.family == PackedFamily::minimax) {
    // Maya's write_config_mm3 for a GGUF quant (maya.py v1.1.30 :4536-4634; the launch adds --pack, --kv-streaming on,
    // the shipped profile and --vision off), the pack as the model folder and its tokenizer; Maya's MiniMax menus
    // (MM3_CONTEXTS, 64K; MM3_KV_FORMAT int8; MM3_EFFORT auto; MM3_SAMPLING)
    const std::string json =
        head + text(kMinimaxModelName) + "},\"roots\":{\"models\":" + text(path_to_utf8(request.pack)) + ",\"tokenizer_pack\":" +
        text(path_to_utf8(request.pack / "tokenizer")) +
        "},\"limits\":{\"chat\":{\"max_prompt_tokens\":1048576,\"max_completion_tokens\":1048576}}}," +
        "\"options\":{\"max_context\":{\"default\":65536,\"min\":32768,\"max\":1048576},\"kv\":{\"default\":\"int8\"}," +
        "\"thinking\":{\"default\":\"auto\"},\"temperature\":{\"default\":1.0},\"top_p\":{\"default\":0.95}}}\n";
    return CardText{request.name, EngineProfile::minimax, json};
  }
  if (request.family == PackedFamily::glm53) {
    // the pack as the model folder (the launch passes it as --glm-pack), its tokenizer; Maya's GLM menus
    const std::string json =
        head + text(kGlmModelName) + "},\"roots\":{\"models\":" + text(path_to_utf8(request.pack)) + ",\"tokenizer_pack\":" +
        text(path_to_utf8(request.pack / "tokenizer")) + "},\"limits\":{\"chat\":{\"max_prompt_tokens\":131072,\"max_completion_tokens\":131072}}}," +
        "\"options\":{\"max_context\":{\"default\":32768,\"min\":8192,\"max\":131072},\"kv\":{\"default\":\"fp16\"}," +
        "\"temperature\":{\"default\":1.0},\"top_p\":{\"default\":0.95}}}\n";
    return CardText{request.name, EngineProfile::glm53, json};
  }
  if (request.family == PackedFamily::raon1) {
    // the managed raon1 card's (engine/fe-config/cards/raon1.json): the pack as the model folder (--glm-pack), its
    // tokenizer, KRAFTON's coding settings; the GLM engine's menus up to the 256K it was served at
    const std::string json =
        head + text(kRaon1ModelName) + "},\"roots\":{\"models\":" + text(path_to_utf8(request.pack)) + ",\"tokenizer_pack\":" +
        text(path_to_utf8(request.pack / "tokenizer")) + "},\"defaults\":{\"sampling\":{\"presence_penalty\":0.0,\"penalty_last_n\":2048}}," +
        "\"limits\":{\"chat\":{\"default_max_tokens\":32768,\"max_prompt_tokens\":262144,\"max_completion_tokens\":262144}}}," +
        "\"options\":{\"max_context\":{\"default\":65536,\"min\":8192,\"max\":262144},\"kv\":{\"default\":\"q8_0\"}," +
        "\"temperature\":{\"default\":1.0},\"top_p\":{\"default\":0.95},\"top_k\":{\"default\":20}}}\n";
    return CardText{request.name, EngineProfile::raon1, json};
  }
  std::string args = "[\"--pack\"," + text(path_to_utf8(request.pack));
  if (request.ple_gguf) args += ",\"--ple-gguf\"," + text(*request.ple_gguf);
  if (request.expert_profile) args += ",\"--expert-profile\"," + text(path_to_utf8(*request.expert_profile));
  // the MTP draft layer as upstream's setup runs it (--spec 4 --spec-min-p 0.5 --mtp <rt>; the options set the first two)
  if (request.mtp) args += ",\"--mtp\"," + text(path_to_utf8(*request.mtp));
  const std::string mtp_options = request.mtp ? "\"mtp\":{\"default\":true},\"spec_min_p\":{\"default\":0.5}," : "";
  // the picture encoder (spec 2026-10-28): vision.mmproj, and Pictures on the GPU by default (upstream setup's --vision gpu)
  const std::string vision_row = request.mmproj ? ",\"vision\":{\"mmproj\":" + text(path_to_utf8(*request.mmproj)) + "}" : "";
  const std::string vision_option = request.mmproj ? "\"vision\":{\"default\":\"gpu\"}," : "";
  const std::string json =
      head + text(request.model_id) + ",\"args\":" + args + "]},\"roots\":{\"models\":" + text(path_to_utf8(request.folder)) +
      "}" + vision_row + ",\"limits\":{\"chat\":{\"max_prompt_tokens\":1048576,\"max_completion_tokens\":1048576}}}," +
      "\"options\":{\"max_context\":{\"default\":131072,\"min\":4096,\"max\":1048576},\"kv\":{\"default\":\"int8\"}," +
      "\"kv_resident\":{\"default\":32768,\"max\":262144}," + mtp_options + vision_option + "\"spec\":{\"default\":4},\"expert_cache\":{\"default\":\"auto\"}," +
      "\"prefill\":{\"default\":\"auto\"},\"thinking\":{\"default\":\"high\"}," +
      // Qwen3.8's own sampling for thinking (upstream's MODEL_CARD_SAMPLING; owner 2026-10-29: the recommended)
      "\"temperature\":{\"default\":1.0},\"top_p\":{\"default\":0.95},\"top_k\":{\"default\":20}}}\n";
  return CardText{request.name, EngineProfile::qwen, json};
}

std::optional<std::filesystem::path> find_mmproj(const std::filesystem::path& folder) {
  std::optional<std::filesystem::path> found;
  std::error_code ec;
  for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = path_to_utf8(it->path().filename());
    if (name.size() < 11U || name.compare(name.size() - 5U, 5U, ".gguf") != 0) continue;
    std::string start = name.substr(0, 6U);
    for (auto& c : start) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::error_code type_ec;
    if (start != "mmproj" || it->is_symlink(type_ec) || !it->is_regular_file(type_ec)) continue;
    if (found) return std::nullopt;   // two encoders: never a guess
    found = it->path();
  }
  return found;
}

std::optional<std::filesystem::path> find_mtp_gguf(const std::filesystem::path& folder, const std::string_view model_file) {
  const auto lower = [](std::string text) {
    for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
  };
  const auto gguf = [](const std::string& name) { return name.size() > 5U && name.compare(name.size() - 5U, 5U, ".gguf") == 0; };
  const std::string named_for = "mtp-" + lower(std::string{model_file});
  std::vector<std::filesystem::path> found;        // every mtp-*.gguf
  std::vector<std::filesystem::path> subfolders;   // MTP/ (unsloth's layout), in any case, never a link
  std::size_t models = 0;                          // the folder's other model files (a split model's first shard)
  const auto scan = [&](const std::filesystem::path& dir, const bool top) {
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
      const std::string name = lower(path_to_utf8(it->path().filename()));
      std::error_code type_ec;
      if (it->is_symlink(type_ec)) continue;
      if (top && name == "mtp" && it->is_directory(type_ec)) {
        subfolders.push_back(it->path());
        continue;
      }
      if (!gguf(name) || !it->is_regular_file(type_ec)) continue;
      if (name.size() > 9U && name.compare(0, 4, "mtp-") == 0) found.push_back(it->path());
      else if (top && name.compare(0, 6, "mmproj") != 0 && first_shard_or_whole(name.substr(0, name.size() - 5U))) ++models;
    }
  };
  scan(folder, true);
  for (const auto& sub : subfolders) scan(sub, false);
  // the one named for the model (mtp-<its file>, unsloth's naming); else the folder's one MTP file, but only in a folder
  // that holds this model alone (another model's drafter is never guessed)
  std::vector<std::filesystem::path> named;
  for (const auto& file : found) {
    if (lower(path_to_utf8(file.filename())) == named_for) named.push_back(file);
  }
  if (named.size() == 1U) return named.front();
  if (named.empty() && found.size() == 1U && models <= 1U) return found.front();
  return std::nullopt;
}

std::optional<std::filesystem::path> find_mtp_runtime(const std::filesystem::path& folder) {
  std::filesystem::path at = folder;
  for (int up = 0; up < 3 && !at.empty(); ++up) {
    if (const auto rt = at / "mtp" / "rt"; holds_mtp_runtime(rt)) return rt;
    if (at == at.parent_path()) break;
    at = at.parent_path();
  }
  return std::nullopt;
}

std::optional<std::filesystem::path> mtp_runtime_at(const std::filesystem::path& folder) {
  for (const auto& rt : {folder, folder / "rt", folder / "mtp" / "rt"}) {
    if (holds_mtp_runtime(rt)) return rt;
  }
  return std::nullopt;
}

std::optional<DraftLayerSlot> draft_layer_slot(const ModelCard& card) {
  const auto found = card.config.entries.find("engine.args");
  const auto* args = found == card.config.entries.end() ? nullptr : std::get_if<std::vector<std::string>>(&found->second.value);
  const auto has = [args](const std::string_view flag) {
    return args != nullptr && std::find(args->begin(), args->end(), flag) != args->end();
  };
  // a Qwen3.8 pack's card runs --pack; a Qwen3.6 GGUF's (--mtp 2: the model's own layer) does not
  if (!(card.profile == EngineProfile::qwen && has("--pack")) && card.profile != EngineProfile::gemma4) return std::nullopt;
  DraftLayerSlot slot{.folder = card.profile == EngineProfile::qwen, .path = std::nullopt};
  if (args != nullptr) {
    const auto mtp = std::find(args->begin(), args->end(), "--mtp");
    if (mtp != args->end() && mtp + 1 != args->end()) slot.path = *(mtp + 1);
  }
  return slot;
}

std::string draft_layer_json(const DraftLayerSlot& slot) {
  std::string out = std::string{"{\"kind\":\""} + (slot.folder ? "folder" : "file") + "\",\"path\":";
  if (slot.path) {
    append_json_string(&out, *slot.path);
  } else {
    out += "null";
  }
  return out + "}";
}

std::variant<std::string, CardError> card_with_draft_layer(const std::string_view card_json, const EngineProfile profile,
                                                           const std::filesystem::path& draft) {
  const CardError foreign{"the card is not one the page wrote"};
  const auto parsed = parse_json(card_json, JsonLimits{.max_bytes = 1U << 20U, .max_depth = 16U, .max_container_entries = 4096U,
                                                       .max_string_bytes = 1U << 16U, .allow_fractions = true});
  const auto* card = std::get_if<JsonValue>(&parsed);
  const auto* config = card != nullptr ? object_member(*card, "config") : nullptr;
  const auto* engine = config != nullptr ? object_member(*config, "engine") : nullptr;
  if (engine == nullptr || engine->if_object() == nullptr) return foreign;
  // the card's arguments without an earlier --mtp and its value, then --mtp <draft>
  std::vector<JsonValue> args;
  if (const auto* listed = object_member(*engine, "args")) {
    const auto* items = listed->if_array();
    if (items == nullptr) return foreign;
    for (std::size_t i = 0; i < items->size(); ++i) {
      const auto* text = (*items)[i].if_string();
      if (text != nullptr && *text == "--mtp") {
        ++i;
        continue;
      }
      args.push_back((*items)[i]);
    }
  }
  args.push_back(JsonValue::make_string("--mtp"));
  args.push_back(JsonValue::make_string(path_to_utf8(draft)));
  JsonValue out = with_member(*card, "config", with_member(*config, "engine", with_member(*engine, "args", JsonValue::make_array(std::move(args)))));
  // the MTP options as a card made with the draft layer lists them (card_for_pack, card_for_gguf): the switch on, and the
  // window and confidence the card lacks
  const auto* listed_options = object_member(*card, "options");
  JsonValue options = listed_options != nullptr ? *listed_options : JsonValue::make_object({});
  if (options.if_object() == nullptr) return foreign;
  const auto with_default = [&options](const std::string& key, JsonValue value, const bool replace) {
    if (!replace && object_member(options, key) != nullptr) return;
    options = with_member(options, key, JsonValue::make_object({JsonMember{"default", std::move(value)}}));
  };
  with_default("mtp", JsonValue::make_boolean(true), true);
  if (profile == EngineProfile::gemma4) with_default("spec", JsonValue::make_integer(4), false);
  with_default("spec_min_p", JsonValue::make_decimal(profile == EngineProfile::gemma4 ? 0.6 : 0.5), false);
  return python_json_text(with_member(out, "options", std::move(options))) + "\n";
}

bool write_glm_pack_marker(const std::filesystem::path& pack) { return write_pack_marker(pack, kGlmModelName); }

bool write_pack_marker(const std::filesystem::path& pack, const std::string_view model_name) {
  const auto marker = pack / path_from_utf8(std::string{model_name} + ".gguf");
  std::error_code ec;
  if (std::filesystem::is_regular_file(marker, ec)) return true;
  std::ofstream out(marker, std::ios::binary | std::ios::trunc);
  out << "GGUF-pack-marker\n";
  return static_cast<bool>(out);
}

std::optional<std::string> pack_indexed_gguf(const std::filesystem::path& pack) {
  // its header line only (the first 4 KiB), of a regular file (never a pipe left in its place)
  const auto file = pack / "native_experts.txt";
  std::error_code ec;
  if (!std::filesystem::is_regular_file(file, ec)) return std::nullopt;
  std::ifstream in(file, std::ios::binary);
  std::string head(4096, '\0');
  in.read(head.data(), static_cast<std::streamsize>(head.size()));
  head.resize(static_cast<std::size_t>(in.gcount()));
  const std::string line = head.substr(0, head.find('\n'));
  if (line.empty()) return std::nullopt;
  constexpr std::string_view kKey = "absolute offsets in ";
  const auto at = line.find(kKey);
  if (at == std::string::npos) return std::nullopt;
  const auto from = at + kKey.size();
  const auto end = line.find_first_of(",)\r", from);
  return line.substr(from, end == std::string::npos ? std::string::npos : end - from);
}

std::optional<std::vector<std::string>> pack_recorded_files(const std::filesystem::path& pack) {
  // a regular file within the parse's own bound, read only then
  const auto file = pack / "sources.json";
  std::error_code ec;
  if (!std::filesystem::is_regular_file(file, ec)) return std::nullopt;
  const auto size = std::filesystem::file_size(file, ec);
  if (ec || size > (1U << 20U)) return std::nullopt;
  std::ifstream in(file, std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream text;
  text << in.rdbuf();
  const auto parsed = parse_json(text.str(), JsonLimits{.max_bytes = 1U << 20U, .max_depth = 4U, .max_container_entries = 4096U,
                                                        .max_string_bytes = 4096U, .allow_fractions = false});
  const auto* record = std::get_if<JsonValue>(&parsed);
  const auto* members = record != nullptr ? record->if_object() : nullptr;
  if (members == nullptr) return std::nullopt;
  std::vector<std::string> names;
  for (const auto& member : *members) {
    const auto* files = member.key == "files" ? member.value.if_array() : nullptr;
    if (files == nullptr) continue;
    for (const auto& entry : *files) {
      const auto* fields = entry.if_object();
      if (fields == nullptr) continue;
      for (const auto& field : *fields) {
        if (field.key == "name" && field.value.if_string() != nullptr) names.push_back(*field.value.if_string());
      }
    }
  }
  return names;
}

std::optional<std::filesystem::path> card_model_file(const ModelCard& card) {
  const auto row = [&card](const std::string_view key) -> const std::string* {
    const auto found = card.config.entries.find(std::string{key});
    return found == card.config.entries.end() ? nullptr : std::get_if<std::string>(&found->second.value);
  };
  const auto* folder = row("roots.models");
  const auto* model = row("engine.model_id");
  if (folder == nullptr || model == nullptr || folder->empty() || model->empty()) return std::nullopt;
  const auto models = path_from_utf8(*folder);
  const bool maya_pack = card.profile == EngineProfile::glm53 || card.profile == EngineProfile::raon1 ||
                         card.profile == EngineProfile::deepseek || card.profile == EngineProfile::minimax;
  if (!maya_pack) return models / path_from_utf8(*model + ".gguf");
  // a model file's name beside the pack: a GGUF, a whole model or a split model's first shard, never a path
  const auto usable = [](const std::string& name) {
    constexpr std::string_view kIndexers = "-maya-indexers.gguf";
    if (name.size() <= 5U || name.compare(name.size() - 5U, 5U, ".gguf") != 0 || name.find_first_of("/\\:") != std::string::npos) return false;
    if (name.size() >= kIndexers.size() && name.compare(name.size() - kIndexers.size(), kIndexers.size(), kIndexers) == 0) return false;
    return first_shard_or_whole(std::string_view{name}.substr(0, name.size() - 5U));
  };
  std::optional<std::string> source = pack_indexed_gguf(models);
  if (source && !usable(*source)) return std::nullopt;
  if (!source) {
    const auto recorded = pack_recorded_files(models);
    if (!recorded) return std::nullopt;
    const auto first = std::find_if(recorded->begin(), recorded->end(), usable);
    if (first == recorded->end()) return std::nullopt;
    source = *first;
  }
  return models.parent_path() / path_from_utf8(*source);
}

std::string card_name_for_file(const std::string_view stem) { return card_name_from(stem); }

bool first_shard_or_whole(const std::string_view stem) {
  const auto split = split_shard(stem);
  return !split || stem.substr(stem.size() - 15U, 6U) == "-00001";
}

std::variant<std::string, CardError> card_with_folder(const std::string_view card_json, const std::filesystem::path& folder) {
  // The card is this module's own rendering: "roots": {"models": "<folder>"} appears once.
  constexpr std::string_view kKey = "\"roots\": {\"models\": ";
  const auto at = card_json.find(kKey);
  if (at == std::string_view::npos || card_json.find(kKey, at + 1U) != std::string_view::npos) {
    return CardError{"the card is not one the page wrote"};
  }
  const auto start = at + kKey.size();
  // the old value: a JSON string ending at the first unescaped quote
  if (start >= card_json.size() || card_json[start] != '"') return CardError{"the card is not one the page wrote"};
  std::size_t end = start + 1U;
  while (end < card_json.size() && card_json[end] != '"') end += card_json[end] == '\\' ? 2U : 1U;
  if (end >= card_json.size()) return CardError{"the card is not one the page wrote"};
  std::string value;
  append_json_string(&value, path_to_utf8(folder));
  std::string out = std::string{card_json.substr(0, start)} + value + std::string{card_json.substr(end + 1U)};
  // Gemma 4's MTP file ("--mtp", "<file>", as card_for_gguf writes it) inside the old folder moves with it when the new
  // folder holds it at the same place; otherwise it stays where it is (the model alone moved, or it is kept elsewhere).
  const auto old_folder = json_string_at(card_json, start);
  constexpr std::string_view kMtp = "\"--mtp\", ";
  const auto mtp_at = out.find(kMtp);
  if (!old_folder || mtp_at == std::string::npos) return out;
  const std::size_t literal_at = mtp_at + kMtp.size();
  const auto mtp = json_string_at(out, literal_at);
  if (!mtp) return out;
  const auto relative = path_from_utf8(mtp->second).lexically_relative(path_from_utf8(old_folder->second));
  if (relative.empty() || relative == "." || *relative.begin() == "..") return out;   // not inside the old folder
  const auto moved = folder / relative;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(moved, ec)) return out;
  std::string moved_text;
  append_json_string(&moved_text, path_to_utf8(moved));
  out.replace(literal_at, mtp->first - literal_at + 1U, moved_text);
  return out;
}

std::optional<std::string> DeleteCodes::issue(const std::string_view card, const Clock::time_point now) {
  // 8 letters from 32 random bytes, by rejection (no modulo bias): 31 letters, bytes below 248 (8 x 31) kept.
  std::string code;
  while (code.size() < 9U) {
    auto generated = generate_token();
    auto* bytes = std::get_if<SecretBytes>(&generated);
    if (bytes == nullptr) return std::nullopt;
    for (const char c : bytes->view()) {
      const auto value = static_cast<unsigned char>(c);
      if (code.size() >= 9U) break;
      if (value >= 248U) continue;
      if (code.size() == 4U) code.push_back('-');
      code.push_back(kCodeAlphabet[value % kCodeAlphabet.size()]);
    }
  }
  const std::scoped_lock lock(mutex_);
  pending_[std::string{card}] = Pending{code, now + kLifetime};
  return code;
}

bool DeleteCodes::claim(const std::string_view card, const std::string_view code, const Clock::time_point now) {
  const std::scoped_lock lock(mutex_);
  const auto it = pending_.find(card);
  if (it == pending_.end()) return false;
  const Pending pending = it->second;
  pending_.erase(it);   // one try, right or wrong
  if (now >= pending.expires || code.size() != pending.code.size()) return false;
  unsigned difference = 0U;
  for (std::size_t i = 0; i < code.size(); ++i) difference |= static_cast<unsigned char>(code[i]) ^ static_cast<unsigned char>(pending.code[i]);
  return difference == 0U;
}

std::variant<DeletedFiles, CardError> delete_gguf_model_files(const std::filesystem::path& folder, const std::string_view model_id) {
  std::vector<std::filesystem::path> files{folder / (std::string{model_id} + ".gguf")};
  if (const auto split = split_shard(model_id); split && split->second > 1 && split->second <= 99999) {
    files.clear();
    for (int k = 1; k <= split->second; ++k) {
      // room for any two ints ("-" + 11 + "-of-" + 11 + ".gguf" + NUL = 33), so no output can be cut (-Wformat-truncation)
      char suffix[40];
      (void)std::snprintf(suffix, sizeof(suffix), "-%05d-of-%05d.gguf", k, split->second);
      files.push_back(folder / (split->first + suffix));
    }
  }
  // every file checked before the first is removed
  DeletedFiles done;
  std::vector<std::filesystem::path> present;
  for (std::size_t i = 0; i < files.size(); ++i) {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(files[i], ec);
    if (!std::filesystem::exists(status)) {
      if (i == 0) return CardError{"the model's file is not in its folder"};
      continue;   // a shard already gone
    }
    if (!std::filesystem::is_regular_file(status)) return CardError{"a model file is a link or not a regular file; nothing was deleted"};
    done.bytes += std::filesystem::file_size(files[i], ec);
    present.push_back(files[i]);
  }
  for (const auto& file : present) {
    std::error_code ec;
    if (!std::filesystem::remove(file, ec) || ec) return CardError{"a model file could not be deleted"};
    ++done.files;
  }
  std::error_code ec;
  if (std::filesystem::is_empty(folder, ec) && !ec) done.folder_removed = std::filesystem::remove(folder, ec) && !ec;
  return done;
}

}  // namespace secure_frontend
