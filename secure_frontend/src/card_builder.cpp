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
#include <system_error>
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
      "\"prefill\":{\"default\":\"auto\"},\"thinking\":{\"default\":\"high\"}}}\n";
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

std::optional<std::filesystem::path> find_mtp_runtime(const std::filesystem::path& folder) {
  std::filesystem::path at = folder;
  for (int up = 0; up < 3 && !at.empty(); ++up) {
    const auto rt = at / "mtp" / "rt";
    std::error_code ec;
    const bool whole = std::all_of(kMtpRuntimeFiles.begin(), kMtpRuntimeFiles.end(), [&rt, &ec](const std::string_view file) {
      return std::filesystem::is_regular_file(rt / file, ec) && !std::filesystem::is_symlink(rt / file, ec);
    });
    if (whole) return rt;
    if (at == at.parent_path()) break;
    at = at.parent_path();
  }
  return std::nullopt;
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
  return std::string{card_json.substr(0, start)} + value + std::string{card_json.substr(end + 1U)};
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
      char suffix[24];
      std::snprintf(suffix, sizeof(suffix), "-%05d-of-%05d.gguf", k, split->second);
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
