#pragma once

#include "secure_frontend/tokenizer_pack.hpp"

#include "support/test_support.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace test_support {

// GPT-2 byte-alphabet symbol for one byte (the encoding tokenizer packs use):
// bytes 0x21-0x7E, 0xA1-0xAC and 0xAE-0xFF are their own code point; every
// other byte, in ascending order, is code point 256 + n. Returned as UTF-8.
inline std::string byte_symbol(const unsigned char byte) {
  const auto printable = [](const unsigned int value) {
    return (value >= 0x21U && value <= 0x7eU) || (value >= 0xa1U && value <= 0xacU) || (value >= 0xaeU && value <= 0xffU);
  };
  unsigned int code_point = byte;
  if (!printable(byte)) {
    unsigned int shifted_before = 0U;
    for (unsigned int value = 0U; value < byte; ++value) {
      if (!printable(value)) ++shifted_before;
    }
    code_point = 256U + shifted_before;
  }
  if (code_point < 0x80U) return std::string(1U, static_cast<char>(code_point));
  return {static_cast<char>(0xc0U | (code_point >> 6U)), static_cast<char>(0x80U | (code_point & 0x3fU))};
}

// Token id k is the single byte (k + 65) mod 256, so the fake engine's `T 7` decodes to "H"
// (and a '0' byte is id 239: three decimal digits on the GEN line, used by K2 in Task 22).
inline unsigned char byte_for_id(const std::size_t id) { return static_cast<unsigned char>((id + 65U) & 0xffU); }

// Inverse of byte_for_id: the byte pack's id for one byte.
inline std::size_t id_for_byte(const unsigned char byte) { return static_cast<std::size_t>((byte + 256U - 65U) & 0xffU); }

// An ordinary token of kLongTokenBytes 'a' bytes: volume tests reach 1 MiB within
// limits.chat.max_completion_tokens (spec §9.3 dos02_slow_* cases; plan Controller note 4).
inline constexpr std::size_t kLongTokenId = 258U;
inline constexpr std::size_t kLongTokenBytes = 1024U;

// The --token-ids value that makes the fake emit `bytes`, one byte per token.
inline std::string token_ids_for(const std::string_view bytes) {
  std::string csv;
  for (const char character : bytes) {
    if (!csv.empty()) csv += ',';
    csv += std::to_string(id_for_byte(static_cast<unsigned char>(character)));
  }
  return csv;
}

// Every byte as an ordinary token (type 1) plus the Qwen framing controls (type 3).
inline secure_frontend::TokenizerPack byte_tokenizer() {
  secure_frontend::TokenizerPack pack;
  for (std::size_t id = 0; id < 256U; ++id) {
    pack.tokens_by_id.push_back(byte_symbol(byte_for_id(id)));
    pack.token_types.push_back(1);
  }
  pack.tokens_by_id.emplace_back("<|im_start|>");
  pack.token_types.push_back(3);
  pack.tokens_by_id.emplace_back("<|im_end|>");
  pack.token_types.push_back(3);
  pack.tokens_by_id.emplace_back(kLongTokenBytes, 'a');  // id 258 ('a' is its own byte symbol)
  pack.token_types.push_back(1);
  return pack;
}

// SP5 M1 Task 12 (Ruling T12-f): the byte pack with the three Qwen vision markers
// APPENDED (ids 259-261; every existing id stays put). The managed vision e2e's model
// tokenizer is this pack, so the plan's marker runs resolve as single CONTROL ids.
inline secure_frontend::TokenizerPack vision_byte_tokenizer() {
  auto pack = byte_tokenizer();
  for (const char* marker : {"<|vision_start|>", "<|image_pad|>", "<|vision_end|>"}) {
    pack.tokens_by_id.emplace_back(marker);
    pack.token_types.push_back(3);
  }
  return pack;
}

// ---- Gemma 4 plan 1 Task 5: a small spm_bpe pack shaped like Gemma 4's (spec §2.5) ----
// ids 0-23: the 24 Gemma special tokens with the GGUF's types (17 CONTROL, 7 USER_DEFINED);
// 24-279: the 256 byte tokens <0x00>..<0xFF> (BYTE, type 6); then "\n", "\n\n", U+2581, "\t",
// every printable ASCII character, two CJK characters, and the merge results below
// (NORMAL, type 1). Characters outside it (é, emoji, combining marks, C0 controls) fall back to
// byte tokens. Merges are listed in rank order.
inline constexpr std::string_view kSpmSpace = "\xE2\x96\x81";  // U+2581
inline constexpr std::size_t kSpmByteBase = 24U;               // <0x00> is id 24

inline const std::vector<std::pair<std::string, int>>& gemma4_special_tokens() {
  static const std::vector<std::pair<std::string, int>> tokens{
      {"<pad>", 3},          {"<eos>", 3},          {"<bos>", 3},            {"<unk>", 3},
      {"<mask>", 3},         {"<|tool>", 3},        {"<tool|>", 3},          {"<|tool_call>", 4},
      {"<tool_call|>", 4},   {"<|tool_response>", 4}, {"<tool_response|>", 4}, {"<|\"|>", 4},
      {"<|think|>", 3},      {"<|channel>", 4},     {"<channel|>", 4},       {"<|turn>", 3},
      {"<turn|>", 3},        {"<|image>", 3},       {"<|audio>", 3},         {"<|image|>", 3},
      {"<|audio|>", 3},      {"<image|>", 3},       {"<audio|>", 3},         {"<|video|>", 3}};
  return tokens;
}

inline std::vector<std::pair<std::string, std::string>> spm_merges() {
  const std::string s{kSpmSpace};
  return {{s, s},           {"H", "i"},      {"o", "r"},       {s, "w"},          {s + "w", "or"},     {"l", "d"},
          {s + "wor", "ld"}, {"b", "c"},     {"a", "b"},       {"a", "a"},        {"\n", "\n"},        {"\t", "\t"},
          {"t", "h"},       {"o", "u"},      {"g", "h"},       {"th", "ou"},      {"thou", "gh"},      {"though", "t"},
          {"\xE4\xBD\xA0", "\xE5\xA5\xBD"}};  // 你 好
}

inline secure_frontend::TokenizerPack spm_tokenizer() {
  secure_frontend::TokenizerPack pack;
  pack.mode = secure_frontend::TokenizerMode::spm_bpe;
  for (const auto& [literal, type] : gemma4_special_tokens()) {
    pack.tokens_by_id.push_back(literal);
    pack.token_types.push_back(type);
  }
  static constexpr char kHex[] = "0123456789ABCDEF";
  for (unsigned int byte = 0U; byte < 256U; ++byte) {
    pack.tokens_by_id.push_back(std::string{"<0x"} + kHex[byte >> 4U] + kHex[byte & 15U] + ">");
    pack.token_types.push_back(6);
  }
  const auto ordinary = [&pack](const std::string& token) {
    for (const auto& existing : pack.tokens_by_id) {
      if (existing == token) return;
    }
    pack.tokens_by_id.push_back(token);
    pack.token_types.push_back(1);
  };
  for (const char* token : {"\n", "\n\n", "\t"}) ordinary(token);
  ordinary(std::string{kSpmSpace});
  for (char c = 0x21; c <= 0x7e; ++c) ordinary(std::string(1U, c));
  ordinary("\xE4\xBD\xA0");  // 你
  ordinary("\xE5\xA5\xBD");  // 好
  for (const auto& [left, right] : spm_merges()) {
    ordinary(left + right);
    pack.merges_by_rank.emplace_back(left, right);
  }
  return pack;
}

// The id of `literal` in `pack` (tokens_by_id.size() when absent).
inline std::size_t token_id(const secure_frontend::TokenizerPack& pack, const std::string_view literal) {
  for (std::size_t id = 0; id < pack.tokens_by_id.size(); ++id) {
    if (pack.tokens_by_id[id] == literal) return id;
  }
  return pack.tokens_by_id.size();
}

// The same pack as the three files load_local_tokenizer_pack() reads, each
// written 0600 (SP1's fixture rule) into an existing 0700 directory.
inline std::vector<std::pair<std::string, std::string>> tokenizer_pack_files(const secure_frontend::TokenizerPack& pack) {
  std::string vocab = "{";
  for (std::size_t id = 0; id < pack.tokens_by_id.size(); ++id) {
    if (id != 0U) vocab += ',';
    vocab += '"';
    for (const char character : pack.tokens_by_id[id]) {
      if (character == '"' || character == '\\') vocab += '\\';
      vocab += character;
    }
    vocab += "\":" + std::to_string(id);
  }
  vocab += '}';
  std::string types = "[";
  for (std::size_t index = 0; index < pack.token_types.size(); ++index) {
    if (index != 0U) types += ',';
    types += std::to_string(pack.token_types[index]);
  }
  types += ']';
  return {{"vocab.json", vocab}, {"merges.txt", std::string{}}, {"token_type.json", types}};
}

inline void write_tokenizer_pack(const std::filesystem::path& directory) {
  for (const auto& [name, bytes] : tokenizer_pack_files(byte_tokenizer())) {
    secure_frontend_test::write_file(directory / name, bytes, 0600);
  }
}

// Web search (spec 2026-10-27-web-search-design.md): the byte pack with Qwen's reasoning and tool tags APPENDED as
// USER_DEFINED tokens (ids 259-264, every byte-pack id stays put).
inline constexpr std::size_t kToolPackThinkOpen = 259U;
inline constexpr std::size_t kToolPackThinkClose = 260U;
inline constexpr std::size_t kToolPackCallOpen = 261U;
inline constexpr std::size_t kToolPackCallClose = 262U;
inline constexpr std::size_t kToolPackResponseOpen = 263U;
inline constexpr std::size_t kToolPackResponseClose = 264U;
inline secure_frontend::TokenizerPack tool_byte_tokenizer() {
  auto pack = byte_tokenizer();
  for (const char* tag : {"<think>", "</think>", "<tool_call>", "</tool_call>", "<tool_response>", "</tool_response>"}) {
    pack.tokens_by_id.emplace_back(tag);
    pack.token_types.push_back(4);
  }
  return pack;
}

inline void write_tool_tokenizer_pack(const std::filesystem::path& directory) {
  for (const auto& [name, bytes] : tokenizer_pack_files(tool_byte_tokenizer())) {
    secure_frontend_test::write_file(directory / name, bytes, 0600);
  }
}

// ---- Gemma 4 plan 1 Task 11: the spm_bpe pack as the files the loader reads (spec §6.2) ----
// A JSON string literal for `text`: '"' and '\' escaped, C0 controls as \n, \t, \r or \u00XX
// (the pack holds "\n", "\n\n" and "\t"), every other byte (UTF-8, U+2581 included) as it is.
inline std::string json_string_literal(const std::string_view text) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out = "\"";
  for (const char character : text) {
    const auto byte = static_cast<unsigned char>(character);
    if (character == '"' || character == '\\') {
      out += '\\';
      out += character;
    } else if (character == '\n') {
      out += "\\n";
    } else if (character == '\t') {
      out += "\\t";
    } else if (character == '\r') {
      out += "\\r";
    } else if (byte < 0x20U) {
      out += "\\u00";
      out += kHex[byte >> 4U];
      out += kHex[byte & 15U];
    } else {
      out += character;
    }
  }
  out += '"';
  return out;
}

// vocab.json, merges.json (a JSON array of [left, right] pairs in rank order) and token_type.json
// for `pack`: the spm_bpe layout, with no merges.txt (spm_bpe refuses a pack that has one).
inline std::vector<std::pair<std::string, std::string>> spm_tokenizer_pack_files(const secure_frontend::TokenizerPack& pack) {
  std::string vocab = "{";
  for (std::size_t id = 0; id < pack.tokens_by_id.size(); ++id) {
    if (id != 0U) vocab += ',';
    vocab += json_string_literal(pack.tokens_by_id[id]) + ":" + std::to_string(id);
  }
  vocab += '}';
  std::string merges = "[";
  for (std::size_t rank = 0; rank < pack.merges_by_rank.size(); ++rank) {
    if (rank != 0U) merges += ',';
    merges += "[" + json_string_literal(pack.merges_by_rank[rank].first) + "," +
              json_string_literal(pack.merges_by_rank[rank].second) + "]";
  }
  merges += ']';
  std::string types = "[";
  for (std::size_t index = 0; index < pack.token_types.size(); ++index) {
    if (index != 0U) types += ',';
    types += std::to_string(pack.token_types[index]);
  }
  types += ']';
  return {{"vocab.json", vocab}, {"merges.json", merges}, {"token_type.json", types}};
}

// spm_tokenizer() (the synthetic Gemma-style pack above) as those three files, each written 0600
// into an existing 0700 directory: the gemma4 profile's roots.tokenizer_pack in the harness.
inline void write_spm_tokenizer_pack(const std::filesystem::path& directory) {
  for (const auto& [name, bytes] : spm_tokenizer_pack_files(spm_tokenizer())) {
    secure_frontend_test::write_file(directory / name, bytes, 0600);
  }
}

}  // namespace test_support
