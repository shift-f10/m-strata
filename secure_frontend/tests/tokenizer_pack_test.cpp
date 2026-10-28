#include "secure_frontend/tokenizer_pack.hpp"
#include "secure_frontend/crt_environment.hpp"
#include "secure_frontend/json.hpp"
#include "support/test_support.hpp"
#include "support/test_tokenizer.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <system_error>
#include <tuple>
#include <variant>
#include <vector>

namespace {
int failures = 0;
// Gemma 4 plan 1 Task 4: the mode is the caller's (the profile's), never the files'.
constexpr auto kByte = secure_frontend::TokenizerMode::byte_bpe;
constexpr auto kSpm = secure_frontend::TokenizerMode::spm_bpe;
void expect(const bool condition, const std::string& message) {
  if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}

std::filesystem::path temporary_pack() {
  return secure_frontend_test::make_scratch_directory();
}
void write(const std::filesystem::path& path, const std::string_view data) {
  std::ofstream output(path, std::ios::binary); output << data;
}
bool has_error(const secure_frontend::TokenizerPackLoadResult& result,
               const secure_frontend::TokenizerPackErrorCode expected) {
  const auto* value = std::get_if<secure_frontend::TokenizerPackError>(&result);
  return value != nullptr && value->code == expected;
}

void loads_validates_and_decodes_offline_pack() {
  const auto root = temporary_pack();
  // "a" and "b" map directly; U+0120 is the GPT-2 byte alphabet encoding of a space.
  write(root / "vocab.json", R"({"a":0,"b":1,"ab":2,"Ġ":3,"Ã":4,"©":5,"Ã©":6})");
  write(root / "merges.txt", "a b\nÃ ©\n");
  write(root / "token_type.json", "[0,0,0,0,0,0,0]");
  const auto loaded = secure_frontend::load_local_tokenizer_pack(root, kByte);
  const auto* pack = std::get_if<secure_frontend::TokenizerPack>(&loaded);
  expect(pack != nullptr, "valid extracted local pack must load");
  if (pack != nullptr) {
    expect(pack->tokens_by_id.size() == 7U && pack->merges_by_rank.size() == 2U,
           "loader must retain vocabulary and merge rank order");
    const auto decoded = secure_frontend::decode_token_ids(*pack, {2U, 3U, 0U});
    const auto* text = std::get_if<std::string>(&decoded);
    expect(text != nullptr && *text == "ab a", "decoder must reverse byte-level BPE symbols by token ID");
    expect(std::holds_alternative<secure_frontend::TokenizerPackError>(
               secure_frontend::decode_token_ids(*pack, {99U})),
           "decoder must reject out-of-range IDs");
    const auto encoded = secure_frontend::encode_text(*pack, "ab ab");
    const auto* ids = std::get_if<std::vector<std::size_t>>(&encoded);
    expect(ids != nullptr && *ids == std::vector<std::size_t>({2U, 3U, 2U}),
           "encoder must apply BPE ranks after byte-level conversion");
    const auto utf8_encoded = secure_frontend::encode_text(*pack, "é");
    const auto* utf8_ids = std::get_if<std::vector<std::size_t>>(&utf8_encoded);
    expect(utf8_ids != nullptr && *utf8_ids == std::vector<std::size_t>({6U}),
           "encoder must preserve UTF-8 bytes through the GPT-2 byte alphabet");
  }
  std::filesystem::remove_all(root);
}

void encoding_fails_closed_for_special_invalid_and_missing_tokens() {
  const auto root = temporary_pack();
  write(root / "vocab.json", R"({"a":0,"<|stop|>":1})");
  write(root / "merges.txt", "");
  write(root / "token_type.json", "[0,3]");
  const auto loaded = secure_frontend::load_local_tokenizer_pack(root, kByte);
  const auto* pack = std::get_if<secure_frontend::TokenizerPack>(&loaded);
  expect(pack != nullptr, "special-token pack must load for fail-closed encoder tests");
  if (pack != nullptr) {
    const auto special = secure_frontend::encode_text(*pack, "<|stop|>");
    expect(std::get_if<secure_frontend::TokenizerPackError>(&special) != nullptr &&
               std::get<secure_frontend::TokenizerPackError>(special).code == secure_frontend::TokenizerPackErrorCode::unsupported_special_token,
           "encoder must reject special-token literals");
    const auto invalid = secure_frontend::encode_text(*pack, std::string{"\xff", 1U});
    expect(std::get_if<secure_frontend::TokenizerPackError>(&invalid) != nullptr &&
               std::get<secure_frontend::TokenizerPackError>(invalid).code == secure_frontend::TokenizerPackErrorCode::input_text_not_utf8,
           "encoder must reject invalid UTF-8");
    expect(std::holds_alternative<secure_frontend::TokenizerPackError>(secure_frontend::encode_text(*pack, "b")),
           "encoder must reject input without ordinary vocabulary coverage");
  }
  std::filesystem::remove_all(root);
}

void rejects_ambiguous_or_incompatible_pack_assets() {
  const auto root = temporary_pack();
  write(root / "vocab.json", R"({"a":0,"b":2})");
  write(root / "merges.txt", "a b\n");
  write(root / "token_type.json", "[0,0]");
  expect(has_error(secure_frontend::load_local_tokenizer_pack(root, kByte),
                   secure_frontend::TokenizerPackErrorCode::invalid_vocabulary),
         "vocabulary IDs must be contiguous and zero based");
  write(root / "vocab.json", R"({"a":0,"b":1})");
  write(root / "merges.txt", "a c\n");
  expect(has_error(secure_frontend::load_local_tokenizer_pack(root, kByte),
                   secure_frontend::TokenizerPackErrorCode::invalid_merge),
         "merge tokens outside vocabulary must fail closed");
  write(root / "merges.txt", "a b\na b\n");
  expect(has_error(secure_frontend::load_local_tokenizer_pack(root, kByte),
                   secure_frontend::TokenizerPackErrorCode::invalid_merge),
         "duplicate merge ranks must fail closed");
  write(root / "merges.txt", "");
  write(root / "token_type.json", "[0]");
  expect(has_error(secure_frontend::load_local_tokenizer_pack(root, kByte),
                   secure_frontend::TokenizerPackErrorCode::invalid_token_types),
         "token type count must equal vocabulary size");
  std::filesystem::remove_all(root);
}

void rejects_malformed_json_and_special_literals_for_decode() {
  const auto root = temporary_pack();
  write(root / "vocab.json", R"({"a":0,"a":1})");
  write(root / "merges.txt", "");
  write(root / "token_type.json", "[0]");
  expect(has_error(secure_frontend::load_local_tokenizer_pack(root, kByte),
                   secure_frontend::TokenizerPackErrorCode::invalid_json),
         "duplicate JSON keys must fail closed");
  write(root / "vocab.json", R"({"<|control|>":0})");
  write(root / "token_type.json", "[3]");
  const auto loaded = secure_frontend::load_local_tokenizer_pack(root, kByte);
  const auto* pack = std::get_if<secure_frontend::TokenizerPack>(&loaded);
  expect(pack != nullptr, "control literals are valid pack vocabulary entries");
  if (pack != nullptr) {
    const auto decoded = secure_frontend::decode_token_ids(*pack, {0U});
    const auto* error = std::get_if<secure_frontend::TokenizerPackError>(&decoded);
    expect(error != nullptr && error->code == secure_frontend::TokenizerPackErrorCode::undecodable_token,
           "decoder must not silently treat control literals as ordinary output bytes");
  }
  std::filesystem::remove_all(root);
}

#ifndef _WIN32
void rejects_symlinked_assets() {
  const auto root = temporary_pack();
  write(root / "actual.json", R"({"a":0})");
  std::filesystem::create_symlink(root / "actual.json", root / "vocab.json");
  write(root / "merges.txt", "");
  write(root / "token_type.json", "[0]");
  expect(has_error(secure_frontend::load_local_tokenizer_pack(root, kByte),
                   secure_frontend::TokenizerPackErrorCode::symbolic_link_not_allowed),
         "loader must reject symlinked tokenizer files");
  std::filesystem::remove_all(root);
}

void cfg03_tokenizer_file_swapped_to_symlink_rejected() {
  const auto root = temporary_pack();
  const auto outside = temporary_pack();
  write(outside / "vocab.json", R"({"a":0})");
  write(root / "merges.txt", "");
  write(root / "token_type.json", "[0]");
  write(root / "vocab.json", R"({"a":0})");
  expect(std::holds_alternative<secure_frontend::TokenizerPack>(secure_frontend::load_local_tokenizer_pack(root, kByte)),
         "the regular pack loads");
  std::filesystem::remove(root / "vocab.json");
  std::filesystem::create_symlink(outside / "vocab.json", root / "vocab.json");
  expect(has_error(secure_frontend::load_local_tokenizer_pack(root, kByte), secure_frontend::TokenizerPackErrorCode::symbolic_link_not_allowed),
         "after the swap the file is opened by handle with O_NOFOLLOW and rejected");
  std::filesystem::remove_all(root);  // removes the link, not its target
  std::filesystem::remove_all(outside);
}
#endif

#if defined(_WIN32)
void cfg03_tokenizer_file_swapped_to_junction_rejected() {
  const auto root = temporary_pack();
  const auto outside = temporary_pack();
  write(outside / "vocab.json", R"({"a":0})");
  write(root / "merges.txt", "");
  write(root / "token_type.json", "[0]");
  write(root / "vocab.json", R"({"a":0})");
  expect(std::holds_alternative<secure_frontend::TokenizerPack>(secure_frontend::load_local_tokenizer_pack(root, kByte)),
         "the regular pack loads");
  std::filesystem::remove(root / "vocab.json");
  expect(secure_frontend_test::create_junction(root / "vocab.json", outside), "junction created without privilege");
  expect(has_error(secure_frontend::load_local_tokenizer_pack(root, kByte), secure_frontend::TokenizerPackErrorCode::symbolic_link_not_allowed),
         "a junction swapped in for vocab.json is opened with FILE_FLAG_OPEN_REPARSE_POINT and rejected");
  std::error_code ignored;
  std::filesystem::remove(root / "vocab.json", ignored);  // the junction itself first, never its target's contents
  std::filesystem::remove_all(root, ignored);
  std::filesystem::remove_all(outside, ignored);
}
#endif

void token_byte_decoder_returns_raw_token_bytes() {
  const auto pack = test_support::byte_tokenizer();
  const secure_frontend::TokenByteDecoder decoder(pack);
  const auto lead = decoder.bytes_for(test_support::id_for_byte(0xE2U));
  expect(std::holds_alternative<std::string>(lead) && std::get<std::string>(lead) == "\xE2", "a lead byte alone is returned, not rejected");
  const auto long_token = decoder.bytes_for(test_support::kLongTokenId);
  expect(std::holds_alternative<std::string>(long_token) && std::get<std::string>(long_token) == std::string(1024U, 'a'), "multi-symbol token");
  const auto control = decoder.bytes_for(256U);
  expect(std::holds_alternative<secure_frontend::TokenizerPackError>(control) &&
             std::get<secure_frontend::TokenizerPackError>(control).code == secure_frontend::TokenizerPackErrorCode::undecodable_token,
         "control tokens are not ordinary output");
  const auto outside = decoder.bytes_for(100000U);
  expect(std::holds_alternative<secure_frontend::TokenizerPackError>(outside) &&
             std::get<secure_frontend::TokenizerPackError>(outside).code == secure_frontend::TokenizerPackErrorCode::token_id_out_of_range,
         "ids outside the vocabulary");
}

// The byte pack plus the Qwen3 reasoning markers as Qwen3.8 packs carry them (USER_DEFINED, 4).
secure_frontend::TokenizerPack think_pack() {
  auto pack = test_support::byte_tokenizer();
  pack.tokens_by_id.emplace_back("<think>");
  pack.token_types.push_back(4);
  pack.tokens_by_id.emplace_back("</think>");
  pack.token_types.push_back(4);
  return pack;
}

// The markers resolve only as exactly one CONTROL or USER_DEFINED token each.
void finds_qwen_think_tokens_only_when_unambiguous() {
  expect(!secure_frontend::find_qwen_think_tokens(test_support::byte_tokenizer()).has_value(),
         "a ChatML pack without the markers has no reasoning split");
  const auto pack = think_pack();
  const std::size_t open = pack.tokens_by_id.size() - 2U;
  const std::size_t close = pack.tokens_by_id.size() - 1U;
  const auto found = secure_frontend::find_qwen_think_tokens(pack);
  expect(found.has_value() && found->open == open && found->close == close, "USER_DEFINED markers resolve");
  auto control = pack;
  control.token_types[open] = 3;
  control.token_types[close] = 3;
  const auto control_found = secure_frontend::find_qwen_think_tokens(control);
  expect(control_found.has_value() && control_found->open == open && control_found->close == close, "CONTROL markers resolve too");
  auto only_open = test_support::byte_tokenizer();
  only_open.tokens_by_id.emplace_back("<think>");
  only_open.token_types.push_back(4);
  expect(!secure_frontend::find_qwen_think_tokens(only_open).has_value(), "one marker alone is no split");
  auto ordinary = pack;
  ordinary.token_types[close] = 1;
  expect(!secure_frontend::find_qwen_think_tokens(ordinary).has_value(), "an ordinary-typed marker is not a reasoning tag");
  auto duplicated = pack;
  duplicated.tokens_by_id.emplace_back("<think>");
  duplicated.token_types.push_back(4);
  expect(!secure_frontend::find_qwen_think_tokens(duplicated).has_value(), "a duplicated marker is ambiguous and fails closed");
}

std::vector<std::size_t> ids_of(const secure_frontend::TokenizerEncodeResult& result) {
  const auto* ids = std::get_if<std::vector<std::size_t>>(&result);
  return ids != nullptr ? *ids : std::vector<std::size_t>{};
}

// The markers are encoded only as the renderer's generation tail (QwenThinking::on/off);
// anywhere else they keep encode_text()'s special-literal rejection.
void qwen_encoder_takes_think_markers_only_as_generation_tail() {
  const auto pack = think_pack();
  const std::size_t open = pack.tokens_by_id.size() - 2U;
  const std::size_t close = pack.tokens_by_id.size() - 1U;
  const std::string head = "<|im_start|>user\nHi<|im_end|>\n<|im_start|>assistant\n";
  const auto head_ids = ids_of(secure_frontend::encode_qwen_chatml_prompt(pack, head));
  const auto newline = ids_of(secure_frontend::encode_text(pack, "\n"));
  const auto two_newlines = ids_of(secure_frontend::encode_text(pack, "\n\n"));
  expect(!head_ids.empty() && !newline.empty() && !two_newlines.empty(), "the plain prompt and newlines encode");

  std::vector<std::size_t> open_tail = head_ids;
  open_tail.push_back(open);
  open_tail.insert(open_tail.end(), newline.begin(), newline.end());
  const auto on = secure_frontend::encode_qwen_chatml_prompt(pack, head + "<think>\n");
  expect(ids_of(on) == open_tail, "the open think tail encodes as <think> plus the newline");

  std::vector<std::size_t> closed_tail = head_ids;
  closed_tail.push_back(open);
  closed_tail.insert(closed_tail.end(), two_newlines.begin(), two_newlines.end());
  closed_tail.push_back(close);
  closed_tail.insert(closed_tail.end(), two_newlines.begin(), two_newlines.end());
  const auto off = secure_frontend::encode_qwen_chatml_prompt(pack, head + "<think>\n\n</think>\n\n");
  expect(ids_of(off) == closed_tail, "the closed think tail encodes as the template writes it");

  const std::vector<std::string> misplaced{"<|im_start|>user\n<think>\n<|im_end|>\n<|im_start|>assistant\n<think>\n",
                                          head + "<think>\nmore", head + "</think>\n\n", head + "<think>\n<think>\n"};
  for (const auto& prompt : misplaced) {
    const auto encoded = secure_frontend::encode_qwen_chatml_prompt(pack, prompt);
    const auto* failure = std::get_if<secure_frontend::TokenizerPackError>(&encoded);
    expect(failure != nullptr && failure->code == secure_frontend::TokenizerPackErrorCode::unsupported_special_token,
           "a marker outside the exact generation tail fails closed");
  }
  const auto without_markers = secure_frontend::encode_qwen_chatml_prompt(test_support::byte_tokenizer(), head + "<think>\n");
  expect(std::holds_alternative<secure_frontend::TokenizerPackError>(without_markers), "a think tail needs the pack's markers");
}

std::size_t id_of(const secure_frontend::TokenizerPack& pack, const std::string_view literal) {
  for (std::size_t id = 0U; id < pack.tokens_by_id.size(); ++id) {
    if (pack.tokens_by_id[id] == literal) return id;
  }
  return pack.tokens_by_id.size();
}

// Issue 26: upstream's stop ids (serve/server.py:3770-3771), each only as one CONTROL token.
void finds_qwen_end_of_turn_tokens() {
  auto pack = test_support::byte_tokenizer();
  const std::size_t im_end = id_of(pack, "<|im_end|>");
  expect(secure_frontend::find_qwen_end_of_turn_tokens(pack) == std::vector<std::size_t>{im_end}, "a ChatML pack ends turns on <|im_end|>");
  pack.tokens_by_id.emplace_back("<|endoftext|>");
  pack.token_types.push_back(3);
  const std::size_t endoftext = pack.tokens_by_id.size() - 1U;
  expect(secure_frontend::find_qwen_end_of_turn_tokens(pack) == std::vector<std::size_t>({im_end, endoftext}),
         "<|endoftext|> joins it when the pack has it");
  auto ordinary = pack;
  ordinary.token_types[endoftext] = 1;
  expect(secure_frontend::find_qwen_end_of_turn_tokens(ordinary) == std::vector<std::size_t>{im_end},
         "an ordinary-typed literal is not an end-of-turn token");
  auto duplicated = pack;
  duplicated.tokens_by_id.emplace_back("<|im_end|>");
  duplicated.token_types.push_back(3);
  expect(secure_frontend::find_qwen_end_of_turn_tokens(duplicated) == std::vector<std::size_t>{endoftext},
         "a duplicated literal is ambiguous and is left out");
}

// GLM-5.3's stop ids (Maya's glm_eos: <|endoftext|>, <|user|>, <|observation|>), each only as one CONTROL token.
void finds_glm53_end_of_turn_tokens() {
  auto pack = test_support::byte_tokenizer();
  expect(secure_frontend::find_glm53_end_of_turn_tokens(pack).empty(), "a pack without GLM's turn markers has no GLM stops");
  std::vector<std::size_t> ids;
  for (const std::string_view literal : {"<|observation|>", "<|user|>", "<|endoftext|>"}) {
    pack.tokens_by_id.emplace_back(literal);
    pack.token_types.push_back(3);
    ids.push_back(pack.tokens_by_id.size() - 1U);
  }
  expect(secure_frontend::find_glm53_end_of_turn_tokens(pack) == std::vector<std::size_t>({ids[2], ids[1], ids[0]}),
         "<|endoftext|>, <|user|>, <|observation|>, in that order");
  auto ordinary = pack;
  ordinary.token_types[ids[0]] = 4;
  expect(secure_frontend::find_glm53_end_of_turn_tokens(ordinary) == std::vector<std::size_t>({ids[2], ids[1]}),
         "a USER_DEFINED <|observation|> is not an end-of-turn token");
  auto duplicated = pack;
  duplicated.tokens_by_id.emplace_back("<|user|>");
  duplicated.token_types.push_back(3);
  expect(secure_frontend::find_glm53_end_of_turn_tokens(duplicated) == std::vector<std::size_t>({ids[2], ids[0]}),
         "a duplicated literal is ambiguous and is left out");
}

// ---- Gemma 4 plan 1 Task 4 (spec §6.2): the spm_bpe mode reads merges.json ----

secure_frontend::TokenizerPackErrorCode error_code(const secure_frontend::TokenizerPackLoadResult& result) {
  const auto* failure = std::get_if<secure_frontend::TokenizerPackError>(&result);
  return failure != nullptr ? failure->code : secure_frontend::TokenizerPackErrorCode::pack_root_unavailable;
}

// Gemma's merges hold "\n", "\t" and runs of U+2581 (spec §2.6); a line-based merges.txt cannot.
void write_spm_pack(const std::filesystem::path& root, const std::string_view merges_json) {
  write(root / "vocab.json",
        "{\"\\n\":0,\"\\n\\n\":1,\"\\n\\n\\n\":2,\"\\t\":3,\"\\t\\t\":4,\"\xE2\x96\x81\":5,\"\xE2\x96\x81\xE2\x96\x81\":6,"
        "\"\xE2\x96\x81\xE2\x96\x81\xE2\x96\x81\":7,\"a b\":8,\"c\":9,\"a bc\":10}");
  write(root / "merges.json", merges_json);
  write(root / "token_type.json", "[1,1,1,1,1,1,1,1,1,1,1]");
}

constexpr std::string_view kSpmMerges =
    "[[\"\\n\\n\",\"\\n\"],[\"\\t\",\"\\t\"],[\"\xE2\x96\x81\xE2\x96\x81\",\"\xE2\x96\x81\"],[\"\\n\",\"\\n\"],"
    "[\"\xE2\x96\x81\",\"\xE2\x96\x81\"],[\"a b\",\"c\"]]";

void spm_pack_loads_merges_json_with_newline_and_tab_tokens() {
  const auto root = temporary_pack();
  write_spm_pack(root, kSpmMerges);
  const auto loaded = secure_frontend::load_local_tokenizer_pack(root, kSpm);
  const auto* pack = std::get_if<secure_frontend::TokenizerPack>(&loaded);
  expect(pack != nullptr, "an spm_bpe pack with merges.json loads");
  if (pack != nullptr) {
    const std::vector<std::pair<std::string, std::string>> expected{
        {"\n\n", "\n"}, {"\t", "\t"}, {"\xE2\x96\x81\xE2\x96\x81", "\xE2\x96\x81"}, {"\n", "\n"},
        {"\xE2\x96\x81", "\xE2\x96\x81"}, {"a b", "c"}};
    expect(pack->merges_by_rank == expected, "merges keep \\n, \\t, U+2581 runs and inner spaces, in rank order");
    expect(pack->mode == secure_frontend::TokenizerMode::spm_bpe, "the pack records the mode it was loaded in");
    expect(pack->tokens_by_id.size() == 11U && pack->tokens_by_id[2] == "\n\n\n", "the vocabulary decodes JSON escapes");
  }
  write(root / "merges.json", "[]");
  const auto empty = secure_frontend::load_local_tokenizer_pack(root, kSpm);
  expect(std::holds_alternative<secure_frontend::TokenizerPack>(empty) &&
             std::get<secure_frontend::TokenizerPack>(empty).merges_by_rank.empty(),
         "an empty merges array is a pack without merges");
  std::filesystem::remove_all(root);
}

// The spm_bpe encoder's tables are built once at load (the Gemma 4 route encodes every request)
// and give the same ids as tables built per call; copies share them; a byte_bpe pack has none.
void spm_pack_load_prepares_the_encode_index_once() {
  const auto root = temporary_pack();
  test_support::write_spm_tokenizer_pack(root);
  const auto loaded = secure_frontend::load_local_tokenizer_pack(root, kSpm);
  const auto* pack = std::get_if<secure_frontend::TokenizerPack>(&loaded);
  expect(pack != nullptr, "the synthetic spm pack loads");
  if (pack == nullptr) return;
  expect(pack->spm_index != nullptr, "the loader prepares the spm encode index");
  auto plain = *pack;
  plain.spm_index.reset();
  const std::vector<std::string> texts{"Hi world", "  x", "x   ", "abc", "thought", "a\n\nb", "\t\t", "\r\n", "Hi\n world", ""};
  for (const auto& text : texts) {
    expect(ids_of(secure_frontend::encode_text(*pack, text)) == ids_of(secure_frontend::encode_text(plain, text)),
           "the prepared index encodes as the per-call one");
  }
  std::optional<secure_frontend::TokenizerPack> copy;
  {
    auto scoped = std::get<secure_frontend::TokenizerPack>(secure_frontend::load_local_tokenizer_pack(root, kSpm));
    copy = scoped;  // the original goes out of scope: the shared tables own their data
  }
  expect(copy->spm_index != nullptr && ids_of(secure_frontend::encode_text(*copy, "Hi world")) ==
                                           ids_of(secure_frontend::encode_text(plain, "Hi world")),
         "a copy outliving the loaded pack encodes the same");
  auto grown = *pack;
  grown.merges_by_rank.pop_back();
  auto grown_plain = grown;
  grown_plain.spm_index.reset();
  expect(ids_of(secure_frontend::encode_text(grown, "abc thought")) == ids_of(secure_frontend::encode_text(grown_plain, "abc thought")),
         "a pack whose merges changed after load is not encoded with the stale tables");
  test_support::write_tokenizer_pack(root);
  std::filesystem::remove(root / "merges.json");
  const auto byte_pack = secure_frontend::load_local_tokenizer_pack(root, kByte);
  expect(std::holds_alternative<secure_frontend::TokenizerPack>(byte_pack) &&
             std::get<secure_frontend::TokenizerPack>(byte_pack).spm_index == nullptr,
         "a byte_bpe pack carries no spm index");
  std::filesystem::remove_all(root);
}

// Review Focus 2: the profile picks the mode; a pack holding the other mode's merges file fails at load.
void mode_comes_from_the_profile_never_the_files() {
  const auto spm = temporary_pack();
  write_spm_pack(spm, kSpmMerges);
  write(spm / "merges.txt", "a b\n");
  expect(error_code(secure_frontend::load_local_tokenizer_pack(spm, kSpm)) == secure_frontend::TokenizerPackErrorCode::invalid_merge,
         "spm_bpe refuses a pack that also has merges.txt");
  std::filesystem::remove(spm / "merges.json");
  expect(error_code(secure_frontend::load_local_tokenizer_pack(spm, kSpm)) == secure_frontend::TokenizerPackErrorCode::invalid_merge,
         "spm_bpe never falls back to merges.txt");
  std::filesystem::remove_all(spm);

  const auto byte = temporary_pack();
  write(byte / "vocab.json", R"({"a":0,"b":1,"ab":2})");
  write(byte / "merges.txt", "a b\n");
  write(byte / "token_type.json", "[1,1,1]");
  const auto plain = secure_frontend::load_local_tokenizer_pack(byte, kByte);
  expect(std::holds_alternative<secure_frontend::TokenizerPack>(plain) &&
             std::get<secure_frontend::TokenizerPack>(plain).mode == secure_frontend::TokenizerMode::byte_bpe,
         "byte_bpe loads merges.txt and records its mode");
  expect(error_code(secure_frontend::load_local_tokenizer_pack(byte, kSpm)) == secure_frontend::TokenizerPackErrorCode::invalid_merge,
         "a byte-level pack is refused in spm_bpe mode (merges.txt present)");
  write(byte / "merges.json", R"([["a","b"]])");
  expect(error_code(secure_frontend::load_local_tokenizer_pack(byte, kByte)) == secure_frontend::TokenizerPackErrorCode::invalid_merge,
         "byte_bpe refuses a pack that also has merges.json");
  std::filesystem::remove(byte / "merges.txt");
  expect(error_code(secure_frontend::load_local_tokenizer_pack(byte, kByte)) == secure_frontend::TokenizerPackErrorCode::invalid_merge,
         "byte_bpe never falls back to merges.json");
  std::filesystem::remove_all(byte);
}

void merges_json_is_bounded_and_validated() {
  const auto root = temporary_pack();
  using Code = secure_frontend::TokenizerPackErrorCode;
  const std::vector<std::tuple<std::string, Code, std::string>> cases{
      {R"([[["\n","\n"]]])", Code::invalid_json, "depth 3"},
      {R"([["\n",1]])", Code::invalid_json, "a non-string side"},
      {R"([["\n"]])", Code::invalid_json, "one string"},
      {R"([["\n","\n","\n"]])", Code::invalid_json, "three strings"},
      {R"({"\n":"\n"})", Code::invalid_json, "an object, not an array"},
      {R"(["\n \n"])", Code::invalid_json, "a merges.txt line as a string"},
      {R"([["\n","\n"],)", Code::invalid_json, "truncated JSON"},
      {R"([["\n","x"]])", Code::invalid_merge, "an unknown token"},
      {R"([["\n","\n"],["\n","\n"]])", Code::invalid_merge, "a duplicate merge"},
      {R"([["","\n"]])", Code::invalid_merge, "an empty side"},
  };
  for (const auto& [text, code, why] : cases) {
    write_spm_pack(root, text);
    expect(error_code(secure_frontend::load_local_tokenizer_pack(root, kSpm)) == code, "merges.json refused: " + why);
  }
  write_spm_pack(root, std::string(64U * 1024U * 1024U + 1U, ' '));
  expect(error_code(secure_frontend::load_local_tokenizer_pack(root, kSpm)) == Code::file_too_large,
         "merges.json above the 64 MiB file cap is refused before parsing");
  std::filesystem::remove_all(root);
}
// ---- Gemma 4 plan 1 Task 5 (spec §2.5, §6.2; LC llama-vocab.cpp:528-537, 637-644, 3186-3200) ----

std::vector<std::size_t> spm_ids(const secure_frontend::TokenizerPack& pack, const std::vector<std::string>& tokens) {
  std::vector<std::size_t> ids;
  for (const auto& token : tokens) ids.push_back(test_support::token_id(pack, token));
  return ids;
}

std::string spm_byte(const unsigned char byte) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  return std::string{"<0x"} + kHex[byte >> 4U] + kHex[byte & 15U] + ">";
}

const std::string kU2581{test_support::kSpmSpace};

// Spaces become U+2581 (no space prefix); merges apply by lowest rank, then leftmost.
void spm_encodes_spaces_as_u2581_and_merges_by_rank() {
  const auto pack = test_support::spm_tokenizer();
  const auto encode = [&pack](const std::string_view text) { return ids_of(secure_frontend::encode_text(pack, text)); };
  expect(encode("Hi world") == spm_ids(pack, {"Hi", kU2581 + "world"}), "Hi world -> Hi, U+2581world");
  expect(encode("x") == spm_ids(pack, {"x"}), "no space prefix is added");
  expect(encode("  x") == spm_ids(pack, {kU2581 + kU2581, "x"}), "two leading spaces merge into one U+2581U+2581 token");
  expect(encode("x   ") == spm_ids(pack, {"x", kU2581 + kU2581, kU2581}), "trailing spaces: leftmost pair first");
  expect(encode("abc") == spm_ids(pack, {"a", "bc"}), "b+c (rank 7) wins over a+b (rank 8)");
  expect(encode("aaa") == spm_ids(pack, {"aa", "a"}), "equal ranks merge leftmost first");
  expect(encode("thought") == spm_ids(pack, {"thought"}), "a chain of merges reaches the long token");
  expect(encode("\t\t") == spm_ids(pack, {"\t\t"}), "tabs merge like any character");
  expect(encode("") == std::vector<std::size_t>{}, "empty text is no tokens (no BOS: the renderer adds it)");
  const auto merges_of = [&pack](const std::string& literal) { return test_support::token_id(pack, literal); };
  expect(merges_of(kU2581) != pack.tokens_by_id.size(), "fixture: U+2581 is a token");
}

// The pre-tokenizer splits [^\n]+ from [\n]+; a newline-only run that is a token is that token.
void spm_newline_runs_are_single_tokens_when_known() {
  const auto pack = test_support::spm_tokenizer();
  const auto encode = [&pack](const std::string_view text) { return ids_of(secure_frontend::encode_text(pack, text)); };
  expect(encode("a\n\nb") == spm_ids(pack, {"a", "\n\n", "b"}), "a known newline run is one token");
  expect(encode("\n\n\n") == spm_ids(pack, {"\n\n", "\n"}), "an unknown run is merged by rank");
  expect(encode("Hi\n world") == spm_ids(pack, {"Hi", "\n", kU2581 + "world"}), "merges never cross a newline");
  expect(encode("\r\n") == spm_ids(pack, {spm_byte(0x0D), "\n"}), "\\r has no token: its byte, then the newline");
  auto no_merge = pack;
  no_merge.merges_by_rank.erase(no_merge.merges_by_rank.begin() + 10);  // ("\n", "\n")
  expect(ids_of(secure_frontend::encode_text(no_merge, "\n\n")) == spm_ids(pack, {"\n\n"}),
         "the newline-run rule needs no merge");
  expect(ids_of(secure_frontend::encode_text(no_merge, "\n\n\n")) == spm_ids(pack, {"\n", "\n", "\n"}),
         "without the token or a merge a run is its characters");
}

// A character the merges cannot reach becomes its UTF-8 bytes' <0xXX> tokens.
void spm_unknown_characters_fall_back_to_byte_tokens() {
  const auto pack = test_support::spm_tokenizer();
  const auto encode = [&pack](const std::string_view text) { return ids_of(secure_frontend::encode_text(pack, text)); };
  expect(encode("\xC3\xA9") == spm_ids(pack, {spm_byte(0xC3), spm_byte(0xA9)}), "e-acute: two byte tokens");
  expect(encode("\xF0\x9F\x98\x80") == spm_ids(pack, {spm_byte(0xF0), spm_byte(0x9F), spm_byte(0x98), spm_byte(0x80)}),
         "an emoji: four byte tokens");
  expect(encode("e\xCC\x81") == spm_ids(pack, {"e", spm_byte(0xCC), spm_byte(0x81)}), "a combining mark after e");
  expect(encode(std::string{"\x01\x7f", 2U}) == spm_ids(pack, {spm_byte(0x01), spm_byte(0x7F)}), "C0 control and DEL");
  expect(encode(std::string{"\0", 1U}) == spm_ids(pack, {spm_byte(0x00)}), "NUL is byte 0");
  expect(encode("\xE4\xBD\xA0\xE5\xA5\xBD") == spm_ids(pack, {"\xE4\xBD\xA0\xE5\xA5\xBD"}), "CJK characters merge as whole characters");
  auto missing = pack;
  missing.tokens_by_id[test_support::kSpmByteBase + 0xA9U] = "<0xA9-gone>";
  const auto refused = secure_frontend::encode_text(missing, "\xC3\xA9");
  const auto* failure = std::get_if<secure_frontend::TokenizerPackError>(&refused);
  expect(failure != nullptr && failure->code == secure_frontend::TokenizerPackErrorCode::unencodable_text,
         "a missing byte token fails closed (llama.cpp would drop the byte)");
  for (const auto& [literal, type] : test_support::gemma4_special_tokens()) {
    const auto special = secure_frontend::encode_text(pack, "a " + literal + " b");
    const auto* error = std::get_if<secure_frontend::TokenizerPackError>(&special);
    expect(error != nullptr && error->code == secure_frontend::TokenizerPackErrorCode::unsupported_special_token,
           "a Gemma marker in text is refused: " + literal);
    static_cast<void>(type);
  }
  const auto invalid = secure_frontend::encode_text(pack, std::string{"\xff", 1U});
  expect(std::holds_alternative<secure_frontend::TokenizerPackError>(invalid), "invalid UTF-8 is refused");
}

// upstream tools/strata_tokenizer.py --check's corpus: decode(encode(s)) == s.
void spm_round_trips_the_upstream_corpus() {
  const auto pack = test_support::spm_tokenizer();
  const std::vector<std::string> corpus{"",
                                        "Hello, world!",
                                        "  leading and trailing  ",
                                        "a\n\n\nb",
                                        "def f(x):\n\treturn x  # comment\n",
                                        "\xE4\xBD\xA0\xE5\xA5\xBD\xEF\xBC\x8C\xE4\xB8\x96\xE7\x95\x8C",
                                        "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7",
                                        "\xF0\x9F\x98\x80\xF0\x9F\x9A\x80\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8",
                                        "e\xCC\x81\xCC\x81 combining",
                                        std::string{"\x00\x01\x7f control", 11U},
                                        "\xC2\xA0non-breaking\xC2\xA0space",
                                        "1234567890",
                                        "MixedCASE_and-dashes",
                                        "\r\n\r\n",
                                        "\xE2\x80\xA8\xE2\x80\xA9",
                                        std::string(5000U, 'x')};
  for (const auto& text : corpus) {
    const auto encoded = secure_frontend::encode_text(pack, text);
    const auto* ids = std::get_if<std::vector<std::size_t>>(&encoded);
    expect(ids != nullptr, "the corpus string encodes: " + text.substr(0U, 20U));
    if (ids == nullptr) continue;
    const auto decoded = secure_frontend::decode_token_ids(pack, *ids);
    expect(std::holds_alternative<std::string>(decoded) && std::get<std::string>(decoded) == text,
           "round trip: " + text.substr(0U, 20U));
  }
}

// Decode: U+2581 -> space, <0xXX> (BYTE) -> that byte; CONTROL/USER_DEFINED fail closed; a byte
// token in a byte_bpe pack stays refused.
void spm_decoder_maps_u2581_and_byte_tokens() {
  const auto pack = test_support::spm_tokenizer();
  const auto decoded = secure_frontend::decode_token_ids(pack, spm_ids(pack, {"Hi", kU2581 + "world", kU2581 + kU2581}));
  expect(std::holds_alternative<std::string>(decoded) && std::get<std::string>(decoded) == "Hi world  ", "U+2581 decodes to a space");
  const auto bytes = secure_frontend::decode_token_ids(pack, spm_ids(pack, {spm_byte(0xC3), spm_byte(0xA9)}));
  expect(std::holds_alternative<std::string>(bytes) && std::get<std::string>(bytes) == "\xC3\xA9", "byte tokens join into UTF-8");
  const secure_frontend::TokenByteDecoder decoder(pack);
  const auto lead = decoder.bytes_for(test_support::kSpmByteBase + 0xE2U);
  expect(std::holds_alternative<std::string>(lead) && std::get<std::string>(lead) == "\xE2",
         "one byte token is its raw byte (Utf8StreamDecoder joins a split character)");
  const auto split = secure_frontend::decode_token_ids(pack, spm_ids(pack, {spm_byte(0xC3)}));
  expect(std::holds_alternative<secure_frontend::TokenizerPackError>(split), "a lone lead byte is not valid UTF-8 text");
  for (const auto& [literal, type] : test_support::gemma4_special_tokens()) {
    const auto special = decoder.bytes_for(test_support::token_id(pack, literal));
    const auto* failure = std::get_if<secure_frontend::TokenizerPackError>(&special);
    expect(failure != nullptr && failure->code == secure_frontend::TokenizerPackErrorCode::undecodable_token,
           "a special token is never ordinary output: " + literal);
    static_cast<void>(type);
  }
  auto malformed = pack;
  malformed.tokens_by_id[test_support::kSpmByteBase] = "<0xzz>";
  const auto bad_byte = secure_frontend::TokenByteDecoder(malformed).bytes_for(test_support::kSpmByteBase);
  expect(std::holds_alternative<secure_frontend::TokenizerPackError>(bad_byte), "a BYTE token not spelled <0xXX> fails closed");
  auto unused = pack;
  unused.token_types[test_support::token_id(pack, "x")] = 5;
  expect(std::holds_alternative<secure_frontend::TokenizerPackError>(secure_frontend::TokenByteDecoder(unused).bytes_for(
             test_support::token_id(pack, "x"))),
         "an UNUSED (type 5) token fails closed in spm_bpe");
  auto byte_level = test_support::byte_tokenizer();
  byte_level.tokens_by_id.emplace_back("<0x41>");
  byte_level.token_types.push_back(6);
  const auto refused = secure_frontend::TokenByteDecoder(byte_level).bytes_for(byte_level.tokens_by_id.size() - 1U);
  expect(std::holds_alternative<secure_frontend::TokenizerPackError>(refused), "a BYTE token in a byte_bpe pack stays refused");
}
// ---- Gemma 4 plan 1 Task 7: the real vocabulary (the owner's GGUF, never committed) ----

const secure_frontend::JsonValue* member(const secure_frontend::JsonValue& object, const std::string_view key) {
  const auto* members = object.if_object();
  if (members == nullptr) return nullptr;
  for (const auto& entry : *members) {
    if (entry.key == key) return &entry.value;
  }
  return nullptr;
}

// The pack built by `secure_tokenizer.py --profile gemma4` from the UD-Q8_K_XL GGUF, named by
// SECURE_FRONTEND_GEMMA4_PACK (its tokenizer/ directory); skipped, with the reason printed, when
// unset. The fixture's ids come from llama.cpp's llama-tokenize at 3cf03257 and equal
// tools/secure/spm_bpe.py's (session log, Gemma step).
void gemma4_real_pack_matches_the_fixture() {
  const char* configured = secure_frontend::crt_getenv("SECURE_FRONTEND_GEMMA4_PACK");
  if (configured == nullptr || *configured == '\0' || !std::filesystem::is_directory(std::filesystem::path{configured})) {
    std::cout << "SKIPPED: SECURE_FRONTEND_GEMMA4_PACK names no Gemma 4 tokenizer pack directory (build one with "
                 "tools/secure/secure_tokenizer.py --profile gemma4; it is never committed)\n";
    return;
  }
  const auto loaded = secure_frontend::load_local_tokenizer_pack(std::filesystem::path{configured}, kSpm);
  const auto* pack = std::get_if<secure_frontend::TokenizerPack>(&loaded);
  const auto* load_error = std::get_if<secure_frontend::TokenizerPackError>(&loaded);
  expect(pack != nullptr, "the real Gemma 4 pack loads in spm_bpe mode: " + (load_error ? load_error->message : std::string{}));
  if (pack == nullptr) return;
  expect(pack->tokens_by_id.size() == 262144U && pack->merges_by_rank.size() == 514906U,
         "the UD-Q8_K_XL vocabulary: 262,144 tokens, 514,906 merges");
  const std::string text = secure_frontend_test::read_file(secure_frontend_test::extra_arguments().at(0));
  const auto parsed = secure_frontend::parse_json(
      text, secure_frontend::JsonLimits{.max_bytes = 1U << 20U, .max_depth = 4U, .max_container_entries = 65536U,
                                        .max_string_bytes = 1U << 16U});
  const auto* root = std::get_if<secure_frontend::JsonValue>(&parsed);
  const auto* entries = root != nullptr && member(*root, "texts") != nullptr ? member(*root, "texts")->if_array() : nullptr;
  expect(entries != nullptr && entries->size() >= 60U, "the fixture parses");
  if (entries == nullptr) return;
  std::size_t checked = 0U;
  for (const auto& entry : *entries) {
    const auto* input = member(entry, "text") != nullptr ? member(entry, "text")->if_string() : nullptr;
    expect(input != nullptr, "every fixture entry has a text");
    if (input == nullptr) continue;
    const auto encoded = secure_frontend::encode_text(*pack, *input);
    if (const auto* refused = member(entry, "refused"); refused != nullptr) {
      const auto* failure = std::get_if<secure_frontend::TokenizerPackError>(&encoded);
      expect(failure != nullptr && failure->code == secure_frontend::TokenizerPackErrorCode::unsupported_special_token,
             "a Gemma control literal as text is refused: " + input->substr(0U, 40U));
      continue;
    }
    std::vector<std::size_t> expected;
    if (const auto* ids = member(entry, "ids") != nullptr ? member(entry, "ids")->if_array() : nullptr) {
      for (const auto& id : *ids) expected.push_back(static_cast<std::size_t>(*id.if_integer()));
    }
    const auto* actual = std::get_if<std::vector<std::size_t>>(&encoded);
    expect(actual != nullptr && *actual == expected, "llama.cpp's ids for: " + input->substr(0U, 40U));
    if (actual != nullptr) {
      const auto decoded = secure_frontend::decode_token_ids(*pack, *actual);
      expect(std::holds_alternative<std::string>(decoded) && std::get<std::string>(decoded) == *input,
             "decode(encode(text)) == text: " + input->substr(0U, 40U));
    }
    ++checked;
  }
  expect(checked >= 60U, "at least 60 texts are compared id for id");
}

// ---- raon-1-mini (raon_bpe): its tokenizer is KRAFTON's llama.cpp (raon-1-mini-preview fb895662) ----

// NFC as llama.cpp's unicode_cpts_normalize_nfc: canonical decomposition (Hangul by rule), the marks of a run
// reordered by combining class, then canonical composition.
void raon_nfc_matches_llama_cpp() {
  using secure_frontend::raon_nfc;
  expect(raon_nfc("plain ASCII, 123.") == "plain ASCII, 123.", "ASCII is unchanged");
  expect(raon_nfc("e\xCC\x81") == "\xC3\xA9", "e + combining acute composes to U+00E9");
  expect(raon_nfc("\xE1\x84\x92\xE1\x85\xA1\xE1\x86\xAB") == "\xED\x95\x9C", "Hangul jamo compose to U+D55C");
  expect(raon_nfc("a\xCC\xA8\xCC\x81") == "\xC4\x85\xCC\x81", "a + ogonek + acute: U+0105, then the acute");
  expect(raon_nfc("a\xCC\x81\xCC\xA8") == "\xC4\x85\xCC\x81", "the marks reorder by class first (ogonek 202, acute 230)");
  expect(raon_nfc("\xC3\xA9\xED\x95\x9C") == "\xC3\xA9\xED\x95\x9C", "NFC text is unchanged");
  expect(raon_nfc("") == "", "empty stays empty");
}

// The pre-tokenizer as llama.cpp's unicode_regex_split_custom_raon (llama-vocab.cpp's raon regex): CJK / kana runs;
// letter runs with their marks, single spaces between words kept in the run; digits in threes from the right;
// punctuation with a leading space and trailing newlines or slashes; whitespace up to its last newline, or all but
// the space a word takes.
void raon_pretokenize_splits_as_llama_cpp() {
  using V = std::vector<std::string>;
  const auto split = [](const std::string_view text) { return secure_frontend::raon_pretokenize(text); };
  expect(split("The quick brown fox, jumps!") == V{"The quick brown fox", ",", " jumps", "!"},
         "words separated by single spaces are one piece");
  expect(split("1234567") == V{"1", "234", "567"}, "digits group in threes from the right");
  expect(split("123456") == V{"123", "456"}, "a whole number of groups");
  expect(split("  \n  x") == V{"  \n", " ", " x"}, "whitespace to its last newline, then one space joins the word");
  expect(split("a  b") == V{"a", " ", " b"}, "two spaces: one alone, one with the word");
  expect(split("end   ") == V{"end", "   "}, "trailing spaces stay together");
  expect(split("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E" "abc") == V{"\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", "abc"},
         "a CJK run is apart from letters");
  expect(split(" 123") == V{" ", "123"}, "a space before digits is its own piece");
  expect(split("a/b //\n") == V{"a", "/", "b", " //\n"}, "punctuation takes a leading space and trailing newlines");
  expect(split("x\xCC\x81y") == V{"x\xCC\x81y"}, "a mark rides its letter");
  expect(split("\xCC\x81x") == V{"\xCC\x81", "x"}, "a mark after no letter is punctuation");
  expect(split("").empty(), "empty text has no pieces");
}

// The real vocabulary (the model's tokenizer pack, never committed) against the reference tokenizer's ids for every
// fixture text: SECURE_FRONTEND_RAON1_PACK names the pack's tokenizer/ directory; unset, the case is skipped.
void raon1_real_pack_matches_the_fixture() {
  const char* configured = secure_frontend::crt_getenv("SECURE_FRONTEND_RAON1_PACK");
  if (configured == nullptr || *configured == '\0' || !std::filesystem::is_directory(std::filesystem::path{configured})) {
    std::cout << "SKIPPED: SECURE_FRONTEND_RAON1_PACK names no raon1 tokenizer pack directory (the model pack's "
                 "tokenizer/: vocab.json, merges.txt, token_type.json; it is never committed)\n";
    return;
  }
  const auto loaded = secure_frontend::load_local_tokenizer_pack(std::filesystem::path{configured},
                                                                 secure_frontend::TokenizerMode::raon_bpe);
  const auto* pack = std::get_if<secure_frontend::TokenizerPack>(&loaded);
  const auto* load_error = std::get_if<secure_frontend::TokenizerPackError>(&loaded);
  expect(pack != nullptr, "the raon1 pack loads in raon_bpe mode: " + (load_error ? load_error->message : std::string{}));
  if (pack == nullptr) return;
  expect(pack->tokens_by_id.size() == 259840U, "raon-1-mini's vocabulary: 259,840 tokens");
  const std::string text = secure_frontend_test::read_file(secure_frontend_test::extra_arguments().at(0));
  const auto parsed = secure_frontend::parse_json(
      text, secure_frontend::JsonLimits{.max_bytes = 1U << 20U, .max_depth = 4U, .max_container_entries = 65536U,
                                        .max_string_bytes = 1U << 16U});
  const auto* root = std::get_if<secure_frontend::JsonValue>(&parsed);
  const auto* entries = root != nullptr && member(*root, "texts") != nullptr ? member(*root, "texts")->if_array() : nullptr;
  expect(entries != nullptr && entries->size() >= 30U, "the fixture parses");
  if (entries == nullptr) return;
  std::size_t checked = 0U;
  for (const auto& entry : *entries) {
    const auto* input = member(entry, "text") != nullptr ? member(entry, "text")->if_string() : nullptr;
    const auto* name = member(entry, "name") != nullptr ? member(entry, "name")->if_string() : nullptr;
    expect(input != nullptr && name != nullptr, "every fixture entry has a name and a text");
    if (input == nullptr || name == nullptr) continue;
    std::vector<std::size_t> expected;
    if (const auto* ids = member(entry, "ids") != nullptr ? member(entry, "ids")->if_array() : nullptr) {
      for (const auto& id : *ids) expected.push_back(static_cast<std::size_t>(*id.if_integer()));
    }
    const auto encoded = secure_frontend::encode_text(*pack, *input);
    const auto* actual = std::get_if<std::vector<std::size_t>>(&encoded);
    std::string where;
    if (actual != nullptr && *actual != expected) {
      std::size_t i = 0U;
      while (i < actual->size() && i < expected.size() && (*actual)[i] == expected[i]) ++i;
      where = " (" + std::to_string(actual->size()) + " ids vs " + std::to_string(expected.size()) +
              ", first difference at " + std::to_string(i) + ")";
    }
    expect(actual != nullptr && *actual == expected, "the reference tokenizer's ids for " + *name + where);
    if (actual != nullptr) {
      const auto decoded = secure_frontend::decode_token_ids(*pack, *actual);
      expect(std::holds_alternative<std::string>(decoded) &&
                 std::get<std::string>(decoded) == secure_frontend::raon_nfc(*input),
             "decode(encode(text)) == NFC(text): " + *name);
    }
    ++checked;
  }
  expect(checked >= 30U, "at least 30 texts are compared id for id");
}

// ---- flint (flint_bpe): its tokenizer is llama.cpp master (e86f5fd1)'s LLAMA_VOCAB_PRE_TYPE_FLINT ----

// The pre-tokenizer as llama.cpp's unicode_regex_split runs flint's three regexes in turn over its collapsed text
// (where every \p{L*} class is the one letter class, so case never splits a word): digits in threes from the left;
// CJK / kana runs; then a letter run with its marks, after an optional character that is no letter, digit or newline,
// and with an English contraction; punctuation with an optional leading space and its trailing newlines; whitespace to
// its last newline, else all but the space the next word takes. Text no regex matches is a piece of its own.
void flint_pretokenize_splits_as_llama_cpp() {
  using V = std::vector<std::string>;
  const auto split = [](const std::string_view text) { return secure_frontend::flint_pretokenize(text); };
  expect(split("The quick brown fox, jumps!") == V{"The", " quick", " brown", " fox", ",", " jumps", "!"},
         "words take their leading space");
  expect(split("HelloWorld CamelCase") == V{"HelloWorld", " CamelCase"}, "case changes do not split a word (llama.cpp)");
  expect(split("1234567") == V{"123", "456", "7"}, "digits group in threes from the left");
  expect(split("x123y") == V{"x", "123", "y"}, "digits split from letters");
  expect(split("12 345") == V{"12", " ", "345"}, "a space between numbers is its own piece");
  expect(split("a  b") == V{"a", " ", " b"}, "two spaces: one alone, one with the word");
  expect(split("end   ") == V{"end", "   "}, "trailing spaces stay together");
  expect(split("  \n  x") == V{"  \n", " ", " x"}, "whitespace to its last newline, then one space joins the word");
  expect(split("a\r\n\r\nb") == V{"a", "\r\n\r\n", "b"}, "a newline run is one piece");
  expect(split("\t\tword") == V{"\t", "\tword"}, "a tab is a word's leading character too");
  expect(split("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E" "abc") == V{"\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", "abc"},
         "a CJK run is apart from letters");
  expect(split(" \xE6\x97\xA5\xE6\x9C\xAC") == V{" ", "\xE6\x97\xA5\xE6\x9C\xAC"}, "a CJK run takes no leading space");
  expect(split("don't we'Re it'sx") == V{"don't", " we'Re", " it's", "x"}, "contractions, any case, stay with their word");
  expect(split(" ((a") == V{" ((", "a"}, "punctuation takes one leading space");
  expect(split("(hello)") == V{"(hello", ")"}, "one punctuation character leads a word");
  expect(split("a/b //\n") == V{"a", "/b", " //\n"}, "punctuation takes its trailing newlines");
  expect(split("x\xCC\x81y") == V{"x\xCC\x81y"}, "a mark rides its letter");
  expect(split("\xCC\x81x") == V{"\xCC\x81x"}, "a mark leads the word after it");
  expect(split("\xCC\x81 ") == V{"\xCC\x81", " "}, "a mark alone is a word");
  expect(split("a\xC2\xA0" "b") == V{"a", "\xC2\xA0" "b"}, "a no-break space leads a word like a space");
  expect(split("\x01\x02") == V{"\x01\x02"}, "unmatched control characters are one piece");
  expect(split("").empty(), "empty text has no pieces");
}

// The real vocabulary (the model's tokenizer pack, never committed) against llama.cpp's ids for every fixture text:
// SECURE_FRONTEND_FLINT_PACK names the pack's tokenizer/ directory; unset, the case is skipped.
void flint_real_pack_matches_the_fixture() {
  const char* configured = secure_frontend::crt_getenv("SECURE_FRONTEND_FLINT_PACK");
  if (configured == nullptr || *configured == '\0' || !std::filesystem::is_directory(std::filesystem::path{configured})) {
    std::cout << "SKIPPED: SECURE_FRONTEND_FLINT_PACK names no flint tokenizer pack directory (the model pack's "
                 "tokenizer/: vocab.json, merges.txt, token_type.json; it is never committed)\n";
    return;
  }
  const auto loaded = secure_frontend::load_local_tokenizer_pack(std::filesystem::path{configured},
                                                                 secure_frontend::TokenizerMode::flint_bpe);
  const auto* pack = std::get_if<secure_frontend::TokenizerPack>(&loaded);
  const auto* load_error = std::get_if<secure_frontend::TokenizerPackError>(&loaded);
  expect(pack != nullptr, "the flint pack loads in flint_bpe mode: " + (load_error ? load_error->message : std::string{}));
  if (pack == nullptr) return;
  expect(pack->tokens_by_id.size() == 150144U && pack->merges_by_rank.size() == 149744U,
         "flint's vocabulary: 150,144 tokens, 149,744 merges");
  const std::string text = secure_frontend_test::read_file(secure_frontend_test::extra_arguments().at(0));
  const auto parsed = secure_frontend::parse_json(
      text, secure_frontend::JsonLimits{.max_bytes = 1U << 20U, .max_depth = 5U, .max_container_entries = 65536U,
                                        .max_string_bytes = 1U << 16U});
  const auto* root = std::get_if<secure_frontend::JsonValue>(&parsed);
  const auto* entries = root != nullptr && member(*root, "texts") != nullptr ? member(*root, "texts")->if_array() : nullptr;
  expect(entries != nullptr && entries->size() >= 38U, "the fixture parses");
  if (entries == nullptr) return;
  std::size_t checked = 0U;
  for (const auto& entry : *entries) {
    const auto* input = member(entry, "text") != nullptr ? member(entry, "text")->if_string() : nullptr;
    const auto* name = member(entry, "name") != nullptr ? member(entry, "name")->if_string() : nullptr;
    expect(input != nullptr && name != nullptr, "every fixture entry has a name and a text");
    if (input == nullptr || name == nullptr) continue;
    std::vector<std::size_t> expected;
    if (const auto* ids = member(entry, "ids") != nullptr ? member(entry, "ids")->if_array() : nullptr) {
      for (const auto& id : *ids) expected.push_back(static_cast<std::size_t>(*id.if_integer()));
    }
    const auto encoded = secure_frontend::encode_text(*pack, *input);
    const auto* actual = std::get_if<std::vector<std::size_t>>(&encoded);
    std::string where;
    if (actual != nullptr && *actual != expected) {
      std::size_t i = 0U;
      while (i < actual->size() && i < expected.size() && (*actual)[i] == expected[i]) ++i;
      where = " (" + std::to_string(actual->size()) + " ids vs " + std::to_string(expected.size()) +
              ", first difference at " + std::to_string(i) + ")";
    }
    expect(actual != nullptr && *actual == expected, "llama.cpp's ids for " + *name + where);
    if (actual != nullptr) {
      const auto decoded = secure_frontend::decode_token_ids(*pack, *actual);
      expect(std::holds_alternative<std::string>(decoded) && std::get<std::string>(decoded) == *input,
             "decode(encode(text)) == text: " + *name);
    }
    ++checked;
  }
  expect(checked >= 38U, "at least 38 texts are compared id for id");
}

// ---- DeepSeek-V4.1 (deepseek_bpe): Maya v1.1.30's deepseek3 pre-tokenizer (tools/strata_tokenizer.py) ----

// Maya's three "Isolated" splits in turn (each expectation is Maya's own output): digits in threes from the left; CJK /
// kana runs; then one ASCII punctuation character leading ASCII letters, a letter-and-mark run after an optional
// character that is no letter, punctuation, symbol or newline, punctuation with an optional space and its trailing
// newlines, whitespace to its last newline, else all but the space the next word takes. Contractions are not words of
// their own here: the apostrophe leads the letters after it.
void deepseek_pretokenize_splits_as_maya() {
  using V = std::vector<std::string>;
  const auto split = [](const std::string_view text) { return secure_frontend::deepseek_pretokenize(text); };
  expect(split("The quick brown fox, jumps!") == V{"The", " quick", " brown", " fox", ",", " jumps", "!"},
         "words take their leading space");
  expect(split("It's we're") == V{"It", "'s", " we", "'re"}, "an apostrophe leads the letters after it");
  expect(split("1234567 x123y") == V{"123", "456", "7", " x", "123", "y"}, "digits group in threes from the left");
  expect(split("12 345") == V{"12", " ", "345"}, "a space between numbers is its own piece");
  expect(split("a  b") == V{"a", " ", " b"}, "two spaces: one alone, one with the word");
  expect(split("end   ") == V{"end", "   "}, "trailing spaces stay together");
  expect(split("  \n  x") == V{"  \n", " ", " x"}, "whitespace to its last newline, then one space joins the word");
  expect(split("a\r\n\r\nb") == V{"a", "\r\n\r\n", "b"}, "a newline run is one piece");
  expect(split("\t\tword") == V{"\t", "\tword"}, "a tab is a word's leading character too");
  expect(split("(hello)") == V{"(hello", ")"}, "one ASCII punctuation character leads ASCII letters");
  expect(split("Mr.Smith") == V{"Mr", ".Smith"}, "a period leads the word after it");
  expect(split(" ((a") == V{" ((", "a"}, "punctuation takes one leading space");
  expect(split("a/b //\n") == V{"a", "/b", " //\n"}, "punctuation takes its trailing newlines");
  expect(split("x\xCC\x81y") == V{"x\xCC\x81y"}, "a mark rides its letter");
  expect(split("\xCC\x81x") == V{"\xCC\x81x"}, "a mark leads the word after it");
  expect(split("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E" "abc") == V{"\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", "abc"},
         "a CJK run is apart from letters");
  expect(split(" \xE6\x97\xA5\xE6\x9C\xAC") == V{" ", "\xE6\x97\xA5\xE6\x9C\xAC"}, "a CJK run takes no leading space");
  expect(split("\xEF\xBC\x8C\xE3\x80\x82") == V{"\xEF\xBC\x8C\xE3\x80\x82"}, "full-width punctuation runs together");
  expect(split("a\xC2\xA0" "b") == V{"a", "\xC2\xA0" "b"}, "a no-break space leads a word like a space");
  expect(split("\xD9\xA1\xD9\xA2\xD9\xA3\xD9\xA4") == V{"\xD9\xA1\xD9\xA2\xD9\xA3", "\xD9\xA4"},
         "every script's digits group in threes");
  expect(split("\xF0\x9F\x98\x80!") == V{"\xF0\x9F\x98\x80!"}, "a symbol run takes the punctuation after it");
  expect(split("\x01\x02") == V{"\x01\x02"}, "unmatched control characters are one piece");
  expect(split("").empty(), "empty text has no pieces");
}

// Every text of tests/fixtures/maya_pretokenize_texts.json (tools/generate_maya_pretokenize_fixture.py: Maya's own
// _IsolatedSplits(DEEPSEEK3_PATTERNS).findall over fixed texts and seeded random strings at the classes' edges).
void deepseek_pretokenize_matches_mayas_fixture() {
  const std::string text = secure_frontend_test::read_file(secure_frontend_test::extra_arguments().at(0));
  const auto parsed = secure_frontend::parse_json(
      text, secure_frontend::JsonLimits{.max_bytes = 1U << 20U, .max_depth = 5U, .max_container_entries = 65536U,
                                        .max_string_bytes = 1U << 16U});
  const auto* root = std::get_if<secure_frontend::JsonValue>(&parsed);
  const auto* entries = root != nullptr && member(*root, "texts") != nullptr ? member(*root, "texts")->if_array() : nullptr;
  expect(entries != nullptr && entries->size() >= 400U, "the fixture parses");
  if (entries == nullptr) return;
  std::size_t checked = 0U;
  for (const auto& entry : *entries) {
    const auto* input = member(entry, "text") != nullptr ? member(entry, "text")->if_string() : nullptr;
    const auto* pieces = member(entry, "deepseek3") != nullptr ? member(entry, "deepseek3")->if_array() : nullptr;
    expect(input != nullptr && pieces != nullptr, "every fixture entry has a text and Maya's deepseek3 pieces");
    if (input == nullptr || pieces == nullptr) continue;
    std::vector<std::string> expected;
    for (const auto& piece : *pieces) expected.push_back(piece.if_string() != nullptr ? *piece.if_string() : std::string{});
    const auto actual = secure_frontend::deepseek_pretokenize(*input);
    std::string where;
    if (actual != expected) {
      std::size_t i = 0U;
      while (i < actual.size() && i < expected.size() && actual[i] == expected[i]) ++i;
      where = " (entry " + std::to_string(checked) + ": " + std::to_string(actual.size()) + " pieces vs " +
              std::to_string(expected.size()) + ", first difference at " + std::to_string(i) + ")";
    }
    expect(actual == expected, "Maya's deepseek3 pieces" + where);
    ++checked;
  }
  expect(checked >= 400U, "at least 400 texts are compared piece for piece");
}

// A deepseek_bpe pack runs Maya's split, then the BPE by rank: "1234" is the pieces "123" and "4" there, so with the
// merges "3 4" (rank 0), "1 2", "12 3" it encodes as [123, 4] - one piece of four digits would be [12, 34].
void deepseek_bpe_encodes_mayas_pieces_by_rank() {
  auto pack = test_support::byte_tokenizer();
  const auto add = [&pack](const std::string_view token) {
    pack.tokens_by_id.emplace_back(token);
    pack.token_types.push_back(1);
    return pack.tokens_by_id.size() - 1U;
  };
  const std::size_t twelve = add("12");
  const std::size_t thirty_four = add("34");
  const std::size_t one_two_three = add("123");
  pack.merges_by_rank = {{"3", "4"}, {"1", "2"}, {"12", "3"}};
  pack.mode = secure_frontend::TokenizerMode::deepseek_bpe;
  const auto encoded = secure_frontend::encode_text(pack, "1234");
  const auto* ids = std::get_if<std::vector<std::size_t>>(&encoded);
  expect(ids != nullptr && *ids == std::vector<std::size_t>({one_two_three, test_support::id_for_byte('4')}),
         "deepseek_bpe: [123, 4]");
  pack.mode = secure_frontend::TokenizerMode::byte_bpe;
  const auto whole = secure_frontend::encode_text(pack, "1234");
  const auto* whole_ids = std::get_if<std::vector<std::size_t>>(&whole);
  // (changed with upstream's qwen35 split for byte_bpe, which takes each digit alone; it was one piece, [12, 34])
  expect(whole_ids != nullptr && *whole_ids == std::vector<std::size_t>({test_support::id_for_byte('1'), test_support::id_for_byte('2'),
                                                                          test_support::id_for_byte('3'), test_support::id_for_byte('4')}),
         "the control: byte_bpe splits every digit apart, [1, 2, 3, 4]");
  (void)twelve;
  (void)thirty_four;
  pack.mode = secure_frontend::TokenizerMode::deepseek_bpe;
  expect(std::holds_alternative<secure_frontend::TokenizerPackError>(secure_frontend::encode_text(pack, "a<|im_end|>")),
         "a special literal in the text is still refused");
}

// ---- MiniMax-M3.1 (minimax_bpe): Maya v1.1.30's minimax pre-tokenizer (tools/strata_tokenizer.py MINIMAX_PATTERN, GPT-4o's
// o200k regex, after NFC) ----

// Maya's split (each expectation is Maya's own output, regex.findall over NFC): a word is an optional leading character
// that is no letter, digit or newline, then upper-case-like letters and lower-case-like letters (so CamelCase splits
// before each capital), with its contraction; digits in threes; punctuation with one leading space and its trailing
// newlines and slashes; whitespace as the other byte-level splits. NFC first: e + U+0301 is the one letter U+00E9.
void minimax_pretokenize_splits_as_maya() {
  using V = std::vector<std::string>;
  const auto split = [](const std::string_view text) { return secure_frontend::minimax_pretokenize(text); };
  expect(split("The quick brown fox, jumps!") == V{"The", " quick", " brown", " fox", ",", " jumps", "!"},
         "words take their leading space");
  expect(split("It's WE'RE they've") == V{"It's", " WE'RE", " they've"}, "a contraction stays with its word, any case");
  expect(split("1234567 x123y") == V{"123", "456", "7", " x", "123", "y"}, "digits group in threes from the left");
  expect(split("HelloWorld CamelCase XMLHttpRequest") == V{"Hello", "World", " Camel", "Case", " XMLHttp", "Request"},
         "a capitalized word ends before the next capital");
  expect(split("a  b") == V{"a", " ", " b"}, "two spaces: one alone, one with the word");
  expect(split("end   ") == V{"end", "   "}, "trailing spaces stay together");
  expect(split("  \n  x") == V{"  \n", " ", " x"}, "whitespace to its last newline, then one space joins the word");
  expect(split("a\r\n\r\nb") == V{"a", "\r\n\r\n", "b"}, "a newline run is one piece");
  expect(split("\t\tword") == V{"\t", "\tword"}, "a tab is a word's leading character too");
  expect(split("(hello)") == V{"(hello", ")"}, "a punctuation character leads the word after it");
  expect(split("a/b //\n") == V{"a", "/b", " //\n"}, "punctuation takes its trailing newlines");
  expect(split("path/to/x\n/\n") == V{"path", "/to", "/x", "\n", "/\n"}, "and its trailing slashes and newlines");
  expect(split("x\xCC\x81y") == V{"x\xCC\x81y"}, "a mark rides its letter");
  expect(split("\xCC\x81x") == V{"\xCC\x81x"}, "a mark leads the word after it");
  expect(split("e\xCC\x81t\xC3\xA9") == V{"\xC3\xA9t\xC3\xA9"}, "NFC composes e and its accent before the split");
  expect(split("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E" "abc") == V{"\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E" "abc"},
         "CJK letters run on into Latin ones (no CJK split, unlike DeepSeek's)");
  expect(split("\xD9\xA1\xD9\xA2\xD9\xA3\xD9\xA4") == V{"\xD9\xA1\xD9\xA2\xD9\xA3", "\xD9\xA4"},
         "every script's digits group in threes");
  expect(split("\xF0\x9F\x98\x80!") == V{"\xF0\x9F\x98\x80!"}, "a symbol run takes the punctuation after it");
  expect(split("a\xC2\xA0" "b") == V{"a", "\xC2\xA0" "b"}, "a no-break space leads a word like a space");
  expect(split("\xC7\x85ungla") == V{"\xC7\x85ungla"}, "a title-case letter starts a word");
  expect(split("").empty(), "empty text has no pieces");
}

// Every text of tests/fixtures/maya_pretokenize_texts.json against Maya's own minimax pieces (regex.findall over
// unicodedata NFC, tools/generate_maya_pretokenize_fixture.py).
void minimax_pretokenize_matches_mayas_fixture() {
  const std::string text = secure_frontend_test::read_file(secure_frontend_test::extra_arguments().at(0));
  const auto parsed = secure_frontend::parse_json(
      text, secure_frontend::JsonLimits{.max_bytes = 1U << 20U, .max_depth = 5U, .max_container_entries = 65536U,
                                        .max_string_bytes = 1U << 16U});
  const auto* root = std::get_if<secure_frontend::JsonValue>(&parsed);
  const auto* entries = root != nullptr && member(*root, "texts") != nullptr ? member(*root, "texts")->if_array() : nullptr;
  expect(entries != nullptr && entries->size() >= 400U, "the fixture parses");
  if (entries == nullptr) return;
  std::size_t checked = 0U;
  for (const auto& entry : *entries) {
    const auto* input = member(entry, "text") != nullptr ? member(entry, "text")->if_string() : nullptr;
    const auto* pieces = member(entry, "minimax") != nullptr ? member(entry, "minimax")->if_array() : nullptr;
    expect(input != nullptr && pieces != nullptr, "every fixture entry has a text and Maya's minimax pieces");
    if (input == nullptr || pieces == nullptr) continue;
    std::vector<std::string> expected;
    for (const auto& piece : *pieces) expected.push_back(piece.if_string() != nullptr ? *piece.if_string() : std::string{});
    const auto actual = secure_frontend::minimax_pretokenize(*input);
    std::string where;
    if (actual != expected) {
      std::size_t i = 0U;
      while (i < actual.size() && i < expected.size() && actual[i] == expected[i]) ++i;
      where = " (entry " + std::to_string(checked) + ": " + std::to_string(actual.size()) + " pieces vs " +
              std::to_string(expected.size()) + ", first difference at " + std::to_string(i) + ")";
    }
    expect(actual == expected, "Maya's minimax pieces" + where);
    ++checked;
  }
  expect(checked >= 400U, "at least 400 texts are compared piece for piece");
}

// A minimax_bpe pack normalizes, splits, then runs the BPE by rank: e + U+0301 is "é" (C3 A9, "Ã©" in the byte
// alphabet), which the merge "Ã ©" makes one token; byte_bpe does not normalize and keeps the three bytes apart.
void minimax_bpe_normalizes_before_the_bpe() {
  auto pack = test_support::byte_tokenizer();
  pack.tokens_by_id.emplace_back("\xC3\x83\xC2\xA9");  // "Ã©"
  pack.token_types.push_back(1);
  const std::size_t e_acute = pack.tokens_by_id.size() - 1U;
  pack.merges_by_rank = {{"\xC3\x83", "\xC2\xA9"}};
  pack.mode = secure_frontend::TokenizerMode::minimax_bpe;
  const auto encoded = secure_frontend::encode_text(pack, "e\xCC\x81");
  const auto* ids = std::get_if<std::vector<std::size_t>>(&encoded);
  expect(ids != nullptr && *ids == std::vector<std::size_t>({e_acute}), "minimax_bpe: NFC, then [é]");
  pack.mode = secure_frontend::TokenizerMode::byte_bpe;
  const auto plain = secure_frontend::encode_text(pack, "e\xCC\x81");
  const auto* plain_ids = std::get_if<std::vector<std::size_t>>(&plain);
  expect(plain_ids != nullptr && plain_ids->size() == 3U, "the control: byte_bpe keeps e and the accent's two bytes");
  pack.mode = secure_frontend::TokenizerMode::minimax_bpe;
  expect(std::holds_alternative<secure_frontend::TokenizerPackError>(secure_frontend::encode_text(pack, "a<|im_end|>")),
         "a special literal in the text is still refused");
}

// ---- Qwen (byte_bpe): upstream v0.2.6's qwen35 pre-tokenizer; GLM-5.3 (glm_bpe): Maya v1.1.30's glm4 pre-tokenizer
// and ignore_merges (both tools/strata_tokenizer.py) ----

// Upstream's QWEN35_PATTERN through regex.findall (each expectation is upstream's own output): a word takes its leading
// space or other character; digits one by one; a mark rides the letters before it; punctuation takes one leading space
// and its trailing newlines; whitespace to its last newline, else all but the space the next word takes.
void qwen35_pretokenize_splits_as_upstream() {
  using V = std::vector<std::string>;
  const auto split = [](const std::string_view text) { return secure_frontend::qwen35_pretokenize(text); };
  expect(split("The quick brown fox, jumps!") == V{"The", " quick", " brown", " fox", ",", " jumps", "!"},
         "words take their leading space");
  expect(split("It's we're") == V{"It", "'s", " we", "'re"}, "contractions are pieces of their own");
  expect(split("don'T") == V{"don", "'T"}, "in either case");
  expect(split("'hello") == V{"'hello"}, "an apostrophe that starts no contraction leads the word");
  expect(split("1234567 x123y") == V{"1", "2", "3", "4", "5", "6", "7", " x", "1", "2", "3", "y"}, "every digit alone");
  expect(split("a  b") == V{"a", " ", " b"}, "two spaces: one alone, one with the word");
  expect(split("end   ") == V{"end", "   "}, "trailing spaces stay together");
  expect(split("  \n  x") == V{"  \n", " ", " x"}, "whitespace to its last newline, then one space joins the word");
  expect(split("a\r\n\r\nb") == V{"a", "\r\n\r\n", "b"}, "a newline run is one piece");
  expect(split("\t\tword") == V{"\t", "\tword"}, "a tab leads a word too");
  expect(split("(hello)") == V{"(hello", ")"}, "punctuation leads the word after it");
  expect(split(" ((a") == V{" ((", "a"}, "punctuation takes one leading space");
  expect(split("a/b //\n") == V{"a", "/b", " //\n"}, "punctuation takes its trailing newlines");
  expect(split("x\xCC\x81y") == V{"x\xCC\x81y"}, "a mark rides its letter");
  expect(split("a\xCC\x81_j") == V{"a\xCC\x81", "_j"}, "a mark stays with the letters, not the punctuation after it");
  expect(split("\xCC\x81x") == V{"\xCC\x81x"}, "a mark starts a word");
  expect(split("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E" "abc") == V{"\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E" "abc"},
         "CJK letters run on with Latin ones");
  expect(split("a\xC2\xA0" "b") == V{"a", "\xC2\xA0" "b"}, "a no-break space leads a word like a space");
  expect(split("\xF0\x9F\x98\x80!") == V{"\xF0\x9F\x98\x80!"}, "a symbol run takes the punctuation after it");
  expect(split("x=\xCE\x94y\xC2\xB2") == V{"x", "=\xCE\x94y", "\xC2\xB2"}, "a superscript digit is a digit");
  expect(split("\x01\x02") == V{"\x01\x02"}, "control characters are punctuation");
  expect(split("").empty(), "empty text has no pieces");
}

// Maya's GLM_PATTERN through regex.findall (each expectation is Maya's own output): qwen35's shape with digits in threes
// and letters without their marks.
void glm_pretokenize_splits_as_maya() {
  using V = std::vector<std::string>;
  const auto split = [](const std::string_view text) { return secure_frontend::glm_pretokenize(text); };
  expect(split("The quick brown fox, jumps!") == V{"The", " quick", " brown", " fox", ",", " jumps", "!"},
         "words take their leading space");
  expect(split("It's we're") == V{"It", "'s", " we", "'re"}, "contractions are pieces of their own");
  expect(split("1234567 x123y") == V{"123", "456", "7", " x", "123", "y"}, "digits in threes from the left");
  expect(split("a  b") == V{"a", " ", " b"}, "two spaces: one alone, one with the word");
  expect(split("  \n  x") == V{"  \n", " ", " x"}, "whitespace to its last newline, then one space joins the word");
  expect(split("x\xCC\x81y") == V{"x", "\xCC\x81y"}, "a mark is no letter: it leads the next word");
  expect(split("a\xCC\x81_j") == V{"a", "\xCC\x81_", "j"}, "or joins the punctuation after it");
  expect(split("\xCC\x81x") == V{"\xCC\x81x"}, "a mark leads a word");
  expect(split("(hello)") == V{"(hello", ")"}, "punctuation leads the word after it");
  expect(split("a/b //\n") == V{"a", "/b", " //\n"}, "punctuation takes its trailing newlines");
  expect(split("x=\xCE\x94y\xC2\xB2") == V{"x", "=\xCE\x94y", "\xC2\xB2"}, "a superscript digit is a digit");
  expect(split("").empty(), "empty text has no pieces");
}

// Every text of tests/fixtures/maya_pretokenize_texts.json against the reference's pieces under `key`.
void compare_split_fixture(const std::string& key, std::vector<std::string> (*split)(std::string_view), const std::string& who) {
  const std::string text = secure_frontend_test::read_file(secure_frontend_test::extra_arguments().at(0));
  const auto parsed = secure_frontend::parse_json(
      text, secure_frontend::JsonLimits{.max_bytes = 1U << 20U, .max_depth = 5U, .max_container_entries = 65536U,
                                        .max_string_bytes = 1U << 16U});
  const auto* root = std::get_if<secure_frontend::JsonValue>(&parsed);
  const auto* entries = root != nullptr && member(*root, "texts") != nullptr ? member(*root, "texts")->if_array() : nullptr;
  expect(entries != nullptr && entries->size() >= 470U, "the fixture parses");
  if (entries == nullptr) return;
  std::size_t checked = 0U;
  for (const auto& entry : *entries) {
    const auto* input = member(entry, "text") != nullptr ? member(entry, "text")->if_string() : nullptr;
    const auto* pieces = member(entry, key) != nullptr ? member(entry, key)->if_array() : nullptr;
    expect(input != nullptr && pieces != nullptr, "every fixture entry has a text and " + who + "'s " + key + " pieces");
    if (input == nullptr || pieces == nullptr) continue;
    std::vector<std::string> expected;
    for (const auto& piece : *pieces) expected.push_back(piece.if_string() != nullptr ? *piece.if_string() : std::string{});
    const auto actual = split(*input);
    std::string where;
    if (actual != expected) {
      std::size_t i = 0U;
      while (i < actual.size() && i < expected.size() && actual[i] == expected[i]) ++i;
      where = " (entry " + std::to_string(checked) + ": " + std::to_string(actual.size()) + " pieces vs " +
              std::to_string(expected.size()) + ", first difference at " + std::to_string(i) + ")";
    }
    expect(actual == expected, who + "'s " + key + " pieces" + where);
    ++checked;
  }
  expect(checked >= 470U, "at least 470 texts are compared piece for piece");
}

void qwen35_pretokenize_matches_upstreams_fixture() {
  compare_split_fixture("qwen35", &secure_frontend::qwen35_pretokenize, "upstream");
}

void glm_pretokenize_matches_mayas_fixture() { compare_split_fixture("glm4", &secure_frontend::glm_pretokenize, "Maya"); }

// The search loop's doubled spaces (2026-10-27): the old byte_bpe split cut every word from its leading space, so the
// model read its own reasoning as [Ġ][word] pairs and wrote it back with two spaces. With upstream's split " user" is one
// piece: with the merges "Ġ u", "Ġu s", "Ġus e", "Ġuse r" the text "a user" is [a, Ġuser], never [a, Ġ, u, s, e, r].
void byte_bpe_takes_a_word_with_its_leading_space() {
  auto pack = test_support::byte_tokenizer();
  const auto add = [&pack](const std::string_view token) {
    pack.tokens_by_id.emplace_back(token);
    pack.token_types.push_back(1);
    return pack.tokens_by_id.size() - 1U;
  };
  (void)add("\xC4\xA0u");
  (void)add("\xC4\xA0us");
  (void)add("\xC4\xA0use");
  const std::size_t user = add("\xC4\xA0user");
  pack.merges_by_rank = {{"\xC4\xA0", "u"}, {"\xC4\xA0u", "s"}, {"\xC4\xA0us", "e"}, {"\xC4\xA0use", "r"}};
  pack.mode = secure_frontend::TokenizerMode::byte_bpe;
  const auto encoded = secure_frontend::encode_text(pack, "a user");
  const auto* ids = std::get_if<std::vector<std::size_t>>(&encoded);
  expect(ids != nullptr && *ids == std::vector<std::size_t>({test_support::id_for_byte('a'), user}), "byte_bpe: [a, Ġuser]");
  const auto plain = secure_frontend::encode_plain_text(pack, "a user");
  expect(std::get_if<std::vector<std::size_t>>(&plain) != nullptr && std::get<std::vector<std::size_t>>(plain) == *ids,
         "plain text (the model's own words, a tool's result) encodes the same");
  pack.mode = secure_frontend::TokenizerMode::glm_bpe;
  const auto glm = secure_frontend::encode_text(pack, "a user");
  expect(std::get_if<std::vector<std::size_t>>(&glm) != nullptr && std::get<std::vector<std::size_t>>(glm) == *ids,
         "glm_bpe too");
  pack.mode = secure_frontend::TokenizerMode::byte_bpe;
  expect(std::holds_alternative<secure_frontend::TokenizerPackError>(secure_frontend::encode_text(pack, "a<|im_end|>")),
         "a special literal in the text is still refused");
}

// GLM-5.3's ignore_merges (Maya's _encode_plain, llama-vocab.cpp): a piece whose bytes in GPT-2's alphabet are one
// vocabulary token is that token whole, even where the merges would cut it otherwise; byte_bpe merges by rank. "abc"
// with the token "abc" and the one merge "b c" is [abc] in glm_bpe, [a, bc] in byte_bpe; "²" (C2 B2, "Â²" in the
// alphabet) with the token "Â²" and no merge is [Â²] in glm_bpe, its two bytes in byte_bpe.
void glm_bpe_takes_a_whole_vocabulary_piece() {
  auto pack = test_support::byte_tokenizer();
  const auto add = [&pack](const std::string_view token) {
    pack.tokens_by_id.emplace_back(token);
    pack.token_types.push_back(1);
    return pack.tokens_by_id.size() - 1U;
  };
  const std::size_t bc = add("bc");
  const std::size_t abc = add("abc");
  const std::size_t squared = add("\xC3\x82\xC2\xB2");
  pack.merges_by_rank = {{"b", "c"}};
  const auto ids_of = [&pack](const std::string_view text) {
    const auto encoded = secure_frontend::encode_text(pack, text);
    const auto* ids = std::get_if<std::vector<std::size_t>>(&encoded);
    return ids != nullptr ? *ids : std::vector<std::size_t>{};
  };
  pack.mode = secure_frontend::TokenizerMode::glm_bpe;
  expect(ids_of("abc") == std::vector<std::size_t>({abc}), "glm_bpe: the whole piece, [abc]");
  expect(ids_of("\xC2\xB2") == std::vector<std::size_t>({squared}), "glm_bpe: [Â²], looked up in the byte alphabet");
  pack.mode = secure_frontend::TokenizerMode::byte_bpe;
  expect(ids_of("abc") == std::vector<std::size_t>({test_support::id_for_byte('a'), bc}), "byte_bpe: by rank, [a, bc]");
  expect(ids_of("\xC2\xB2") == std::vector<std::size_t>({test_support::id_for_byte(0xC2U), test_support::id_for_byte(0xB2U)}),
         "byte_bpe: its two bytes");
}

// The real GLM-5.3-Flash pack against Maya's own ids (tests/fixtures/byte_bpe_reference_ids.json "glm53", from
// tools/generate_byte_bpe_reference_fixture.py). SECURE_FRONTEND_GLM53_PACK names the pack's tokenizer/ folder; unset,
// skipped. It is read in place (the owner's folder), never written.
void glm53_real_pack_encodes_as_maya() {
  const char* configured = secure_frontend::crt_getenv("SECURE_FRONTEND_GLM53_PACK");
  if (configured == nullptr || *configured == '\0' || !std::filesystem::is_directory(std::filesystem::path{configured})) {
    std::cout << "SKIPPED: SECURE_FRONTEND_GLM53_PACK names no GLM-5.3 tokenizer pack directory (the model pack's "
                 "tokenizer/: vocab.json, merges.txt, token_type.json; it is never committed)\n";
    return;
  }
  auto root = secure_frontend::open_root(std::filesystem::path{configured}, secure_frontend::RootPolicy::external);
  expect(std::holds_alternative<secure_frontend::CanonicalRoot>(root), "the tokenizer folder opens");
  if (!std::holds_alternative<secure_frontend::CanonicalRoot>(root)) return;
  const auto loaded = secure_frontend::load_local_tokenizer_pack(std::get<secure_frontend::CanonicalRoot>(root),
                                                                 secure_frontend::TokenizerMode::glm_bpe);
  const auto* pack = std::get_if<secure_frontend::TokenizerPack>(&loaded);
  const auto* load_error = std::get_if<secure_frontend::TokenizerPackError>(&loaded);
  expect(pack != nullptr, "the GLM-5.3 pack loads in glm_bpe mode: " + (load_error ? load_error->message : std::string{}));
  if (pack == nullptr) return;
  const std::string text = secure_frontend_test::read_file(secure_frontend_test::extra_arguments().at(0));
  const auto parsed = secure_frontend::parse_json(
      text, secure_frontend::JsonLimits{.max_bytes = 1U << 20U, .max_depth = 5U, .max_container_entries = 65536U,
                                        .max_string_bytes = 1U << 16U});
  const auto* fixture = std::get_if<secure_frontend::JsonValue>(&parsed);
  const auto* entries = fixture != nullptr && member(*fixture, "texts") != nullptr ? member(*fixture, "texts")->if_array() : nullptr;
  expect(entries != nullptr, "the fixture parses");
  if (entries == nullptr) return;
  std::size_t checked = 0U;
  for (const auto& entry : *entries) {
    const std::string& name = *member(entry, "name")->if_string();
    std::vector<std::size_t> expected;
    for (const auto& id : *member(entry, "glm53")->if_array()) expected.push_back(static_cast<std::size_t>(*id.if_integer()));
    const auto encoded = secure_frontend::encode_plain_text(*pack, *member(entry, "text")->if_string());
    const auto* ids = std::get_if<std::vector<std::size_t>>(&encoded);
    expect(ids != nullptr && *ids == expected, "Maya's ids for " + name);
    ++checked;
  }
  expect(checked >= 42U, "at least 42 texts are compared id for id");
}
}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"finds_qwen_think_tokens_only_when_unambiguous", finds_qwen_think_tokens_only_when_unambiguous},
      {"qwen_encoder_takes_think_markers_only_as_generation_tail", qwen_encoder_takes_think_markers_only_as_generation_tail},
      {"finds_qwen_end_of_turn_tokens", finds_qwen_end_of_turn_tokens},
      {"finds_glm53_end_of_turn_tokens", finds_glm53_end_of_turn_tokens},
      {"deepseek_pretokenize_splits_as_maya", deepseek_pretokenize_splits_as_maya},
      {"deepseek_pretokenize_matches_mayas_fixture", deepseek_pretokenize_matches_mayas_fixture},
      {"deepseek_bpe_encodes_mayas_pieces_by_rank", deepseek_bpe_encodes_mayas_pieces_by_rank},
      {"minimax_pretokenize_splits_as_maya", minimax_pretokenize_splits_as_maya},
      {"minimax_pretokenize_matches_mayas_fixture", minimax_pretokenize_matches_mayas_fixture},
      {"minimax_bpe_normalizes_before_the_bpe", minimax_bpe_normalizes_before_the_bpe},
      {"qwen35_pretokenize_splits_as_upstream", qwen35_pretokenize_splits_as_upstream},
      {"glm_pretokenize_splits_as_maya", glm_pretokenize_splits_as_maya},
      {"qwen35_pretokenize_matches_upstreams_fixture", qwen35_pretokenize_matches_upstreams_fixture},
      {"glm_pretokenize_matches_mayas_fixture", glm_pretokenize_matches_mayas_fixture},
      {"byte_bpe_takes_a_word_with_its_leading_space", byte_bpe_takes_a_word_with_its_leading_space},
      {"glm_bpe_takes_a_whole_vocabulary_piece", glm_bpe_takes_a_whole_vocabulary_piece},
      {"glm53_real_pack_encodes_as_maya", glm53_real_pack_encodes_as_maya},
      {"loads_validates_and_decodes_offline_pack", loads_validates_and_decodes_offline_pack},
      {"rejects_ambiguous_or_incompatible_pack_assets", rejects_ambiguous_or_incompatible_pack_assets},
      {"rejects_malformed_json_and_special_literals_for_decode", rejects_malformed_json_and_special_literals_for_decode},
      {"encoding_fails_closed_for_special_invalid_and_missing_tokens", encoding_fails_closed_for_special_invalid_and_missing_tokens},
      {"token_byte_decoder_returns_raw_token_bytes", token_byte_decoder_returns_raw_token_bytes},
      {"spm_pack_loads_merges_json_with_newline_and_tab_tokens", spm_pack_loads_merges_json_with_newline_and_tab_tokens},
      {"mode_comes_from_the_profile_never_the_files", mode_comes_from_the_profile_never_the_files},
      {"raon_nfc_matches_llama_cpp", raon_nfc_matches_llama_cpp},
      {"raon_pretokenize_splits_as_llama_cpp", raon_pretokenize_splits_as_llama_cpp},
      {"raon1_real_pack_matches_the_fixture", raon1_real_pack_matches_the_fixture},
      {"flint_pretokenize_splits_as_llama_cpp", flint_pretokenize_splits_as_llama_cpp},
      {"flint_real_pack_matches_the_fixture", flint_real_pack_matches_the_fixture},
      {"merges_json_is_bounded_and_validated", merges_json_is_bounded_and_validated},
      {"spm_encodes_spaces_as_u2581_and_merges_by_rank", spm_encodes_spaces_as_u2581_and_merges_by_rank},
      {"spm_newline_runs_are_single_tokens_when_known", spm_newline_runs_are_single_tokens_when_known},
      {"spm_unknown_characters_fall_back_to_byte_tokens", spm_unknown_characters_fall_back_to_byte_tokens},
      {"spm_round_trips_the_upstream_corpus", spm_round_trips_the_upstream_corpus},
      {"spm_decoder_maps_u2581_and_byte_tokens", spm_decoder_maps_u2581_and_byte_tokens},
      {"gemma4_real_pack_matches_the_fixture", gemma4_real_pack_matches_the_fixture},
      {"spm_pack_load_prepares_the_encode_index_once", spm_pack_load_prepares_the_encode_index_once},
#ifndef _WIN32
      {"rejects_symlinked_assets", rejects_symlinked_assets},
      {"cfg03_tokenizer_file_swapped_to_symlink_rejected", cfg03_tokenizer_file_swapped_to_symlink_rejected},
#endif
#if defined(_WIN32)
      {"cfg03_tokenizer_file_swapped_to_junction_rejected", cfg03_tokenizer_file_swapped_to_junction_rejected},
#endif
  }, failures);
}
