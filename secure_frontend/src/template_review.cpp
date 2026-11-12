#include "secure_frontend/template_review.hpp"
#include "secure_frontend/gguf_metadata.hpp"
#include "secure_frontend/json.hpp"
#include "secure_frontend/launch_composition.hpp"
#include "secure_frontend/sha256.hpp"
#include <algorithm>
#include <array>

namespace secure_frontend {
std::vector<TemplateRisk> review_template_risks(const std::string_view source) {
  struct Rule { std::string_view code, message; std::vector<std::string_view> words; };
  const std::array<Rule, 6> rules{{
      {"object_access", "Object introspection or dynamic attribute access; no host objects are available in the renderer.",
       {"__", "|attr", "getattr", "globals", "builtins"}},
      {"external_access", "Attempts to import code or access files, commands or network resources are unsupported.",
       {"import ", "include ", "extends ", "subprocess", "os.", "open(", "eval(", "exec(", "system(", "http://", "https://"}},
      {"resource_use", "Loops, macros or repetition can consume resources; rendering has hard time and memory limits.",
       {"for ", "macro ", "recursive", "range(", "range (", " * ", "**"}},
      {"input_transform", "This section transforms or omits conversation content; review its effect on instructions and tool calls.",
       {"replace(", "replace (", "selectattr", "rejectattr", "messages[", "messages [", "|safe"}},
      {"control_tokens", "Literal control tokens can change roles, reasoning or tool boundaries. Check them against this model.",
       {"<|", "[INST]", "[SYSTEM]", "<tool_call", "<think>"}},
      {"hidden_text", "Bidirectional or invisible Unicode controls can disguise how source appears. Review the raw characters carefully.",
       {"\xE2\x80\xAA", "\xE2\x80\xAB", "\xE2\x80\xAC", "\xE2\x80\xAD", "\xE2\x80\xAE",
        "\xE2\x81\xA6", "\xE2\x81\xA7", "\xE2\x81\xA8", "\xE2\x81\xA9", "\xE2\x80\x8B"}},
  }};
  std::vector<TemplateRisk> out;
  std::size_t begin = 0, line = 1;
  while (begin < source.size() && out.size() < 256U) {
    const auto end = source.find('\n', begin);
    const auto text = source.substr(begin, end == std::string_view::npos ? source.size() - begin : end - begin);
    for (const auto& rule : rules) {
      if (std::any_of(rule.words.begin(), rule.words.end(), [&](const auto word) { return text.find(word) != std::string_view::npos; })) {
        out.push_back({line, line, std::string{rule.code}, std::string{rule.message}});
        if (out.size() == 256U) break;
      }
    }
    if (end == std::string_view::npos) break;
    begin = end + 1U;
    ++line;
  }
  if (out.size() == 256U) out.push_back({line, line, "review_truncated", "Only the first 256 findings are shown; additional risks may follow."});
  return out;
}

std::variant<EmbeddedChatTemplate, std::string> read_embedded_chat_template(
    const CanonicalRoot& models, const ComposedLaunch& launch) {
  std::string source, origin;
  std::optional<std::string> bos, eos;
  std::optional<std::size_t> bos_id, eos_id;
  if (launch.tokenizer_gguf) {
    auto file = open_beneath(models, *launch.tokenizer_gguf);
    if (!std::holds_alternative<OpenedFile>(file)) return std::string{"the model header could not be opened"};
    const auto metadata = read_gguf_metadata(std::get<OpenedFile>(file));
    const auto* md = std::get_if<GgufMetadata>(&metadata);
    if (md == nullptr) return std::string{"the model header could not be read"};
    const auto* text = md->string("tokenizer.chat_template");
    if (text == nullptr) return std::string{"this GGUF has no string chat template"};
    source = *text;
    const auto* tokens = md->strings("tokenizer.ggml.tokens");
    const auto token = [&](std::string_view key) -> std::optional<std::string> {
      const auto id = md->integer(key);
      if (!id || *id < 0 || tokens == nullptr || static_cast<std::uint64_t>(*id) >= tokens->size()) return std::nullopt;
      return (*tokens)[static_cast<std::size_t>(*id)];
    };
    bos = token("tokenizer.ggml.bos_token_id");
    eos = token("tokenizer.ggml.eos_token_id");
    origin = "GGUF tokenizer.chat_template";
  } else if (launch.tokenizer_root) {
    const auto name = std::get<RelativePath>(parse_relative_path("chat_template.jinja"));
    auto file = open_beneath(*launch.tokenizer_root, name);
    if (!std::holds_alternative<OpenedFile>(file)) return std::string{"this tokenizer pack has no readable extracted chat template"};
    auto bytes = std::get<OpenedFile>(file).read_all(kMaxEmbeddedTemplateBytes);
    if (!std::holds_alternative<std::string>(bytes)) return std::string{"the extracted chat template exceeds its limit or could not be read"};
    source = std::get<std::string>(std::move(bytes));
    origin = "Tokenizer pack chat_template.jinja (exported during preparation)";
    // Our pack exporter preserves the GGUF's special IDs. Some Maya packs use
    // short bos/eos names. Resolve the IDs against the loaded vocabulary at start.
    const auto metadata_name = std::get<RelativePath>(parse_relative_path("tokenizer.json"));
    auto metadata_file = open_beneath(*launch.tokenizer_root, metadata_name);
    if (auto* opened = std::get_if<OpenedFile>(&metadata_file)) {
      auto metadata_text = opened->read_all(1U << 20U);
      if (const auto* bytes_text = std::get_if<std::string>(&metadata_text)) {
        const auto parsed = parse_json(*bytes_text, {1U << 20U, 8U, 4096U, 1U << 16U, true});
        if (const auto* root = std::get_if<JsonValue>(&parsed); root != nullptr && root->if_object() != nullptr) {
          for (const auto& field : *root->if_object()) {
            if (field.key != "special_ids" || field.value.if_object() == nullptr) continue;
            for (const auto& special : *field.value.if_object()) {
              const auto* id = special.value.if_integer();
              if (id == nullptr || *id < 0 || *id >= 1048576) continue;
              if (special.key == "tokenizer.ggml.bos_token_id" || special.key == "bos") bos_id = static_cast<std::size_t>(*id);
              if (special.key == "tokenizer.ggml.eos_token_id" || special.key == "eos") eos_id = static_cast<std::size_t>(*id);
            }
          }
        }
      }
    }
  } else return std::string{"this model has no template source"};
  if (source.empty() || source.size() > kMaxEmbeddedTemplateBytes || !is_valid_utf8(source) || source.find('\0') != std::string::npos ||
      std::count(source.begin(), source.end(), '\n') >= 10000) {
    return std::string{"chat templates must be nonempty text, at most 256 KiB and 10000 lines, without NUL bytes"};
  }
  auto risks = review_template_risks(source);
  auto digest = to_hex(sha256(source));
  return EmbeddedChatTemplate{std::move(source), std::move(digest), std::move(origin), std::move(risks), std::move(bos), std::move(eos), bos_id, eos_id};
}

std::string template_review_json(const EmbeddedChatTemplate& review, const bool selected) {
  const auto quote = [](std::string_view value) { std::string out; append_json_string(&out, value); return out; };
  std::string out = "{\"available\":true,\"mode\":\"" + std::string{selected ? "embedded" : "frontend"} +
      "\",\"source\":" + quote(review.source) + ",\"sha256\":" + quote(review.sha256) + ",\"origin\":" + quote(review.origin) +
      ",\"warning\":\"Heuristic review, not a safety or correctness guarantee. Templates can change instructions and output. Host access stays blocked; unsupported operations fail.\",\"risks\":[";
  for (std::size_t i = 0; i < review.risks.size(); ++i) {
    const auto& risk = review.risks[i];
    out += std::string{i == 0U ? "" : ","} + "{\"line\":" + std::to_string(risk.line) + ",\"end_line\":" +
        std::to_string(risk.end_line) + ",\"code\":" + quote(risk.code) + ",\"message\":" + quote(risk.message) + "}";
  }
  return out + "]}";
}
}  // namespace secure_frontend
