#pragma once

#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/gguf_metadata.hpp"
#include "secure_frontend/prompt_piece.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace secure_frontend {

// Gemma 4 plan 1 Task 4 (spec §6.2): how a pack's merges are stored and applied. The mode
// is chosen by the engine profile (gemma4 -> spm_bpe, raon1 -> raon_bpe, flint -> flint_bpe, deepseek ->
// deepseek_bpe, glm53 -> glm_bpe, every other -> byte_bpe), never inferred from the pack's files: byte_bpe is GPT-2's
// byte alphabet with merges.txt and upstream's Qwen pre-tokenizer (qwen35_pretokenize); spm_bpe is SentencePiece-style
// BPE over raw UTF-8 characters with merges.json; raon_bpe is byte_bpe's files and alphabet with raon-1-mini's own text
// handling (raon_nfc, raon_pretokenize); flint_bpe is byte_bpe's files and alphabet with flint's pre-tokenizer
// (flint_pretokenize); deepseek_bpe is byte_bpe's files and alphabet with DeepSeek-V4.1's pre-tokenizer as Maya splits
// it (deepseek_pretokenize); glm_bpe is byte_bpe's files and alphabet with GLM-5.3's pre-tokenizer as Maya splits it
// (glm_pretokenize) and its ignore_merges rule; minimax_bpe is byte_bpe's files and alphabet with MiniMax-M3.1's NFC and
// pre-tokenizer as Maya runs them (minimax_pretokenize; minimax -> minimax_bpe).
enum class TokenizerMode { byte_bpe, spm_bpe, raon_bpe, flint_bpe, deepseek_bpe, glm_bpe, minimax_bpe };

// The offline tokenizer assets extracted by upstream/tools/strata_tokenizer.py
// (through our wrapper tools/secure/secure_tokenizer.py for GLM and Gemma 4 packs).  This
// type intentionally owns all input: loading a pack never keeps a file handle,
// invokes a subprocess, or contacts a network service.
struct SpmEncodeIndex;   // tokenizer_pack.cpp: the spm_bpe encoder's lookup tables
struct RaonEncodeIndex;  // tokenizer_pack.cpp: the raon_bpe encoder's lookup tables

struct TokenizerPack final {
  std::vector<std::string> tokens_by_id;
  std::vector<std::pair<std::string, std::string>> merges_by_rank;
  std::vector<int> token_types;
  TokenizerMode mode{TokenizerMode::byte_bpe};  // the mode it was loaded in (the profile's)
  // spm_bpe packs from load_local_tokenizer_pack: the encoder's lookup tables, built once at
  // load (about 0.2 s for Gemma 4's 262,144 tokens and 514,906 merges) and shared by copies.
  // They own their data. A pack is not modified after load; one whose vocabulary, merge or type
  // counts no longer match is encoded with tables built for that call, as a pack built in
  // memory (no tables) always is.
  std::shared_ptr<const SpmEncodeIndex> spm_index;
  // raon_bpe and flint_bpe packs from load_local_tokenizer_pack: the token and merge-rank lookups (the two share
  // llama.cpp's byte-level BPE session), built once at load as spm_index
  std::shared_ptr<const RaonEncodeIndex> raon_index;
};

enum class TokenizerPackErrorCode {
  pack_root_unavailable,
  pack_root_not_directory,
  unsafe_pack_path,
  required_file_missing,
  symbolic_link_not_allowed,
  file_read_failed,
  file_too_large,
  invalid_json,
  invalid_vocabulary,
  invalid_merge,
  invalid_token_types,
  token_id_out_of_range,
  undecodable_token,
  decoded_text_not_utf8,
  input_text_not_utf8,
  input_text_too_large,
  unsupported_special_token,
  unencodable_text,
};

struct TokenizerPackError final {
  TokenizerPackErrorCode code;
  std::string message;
};

using TokenizerPackLoadResult = std::variant<TokenizerPack, TokenizerPackError>;
using TokenizerDecodeResult = std::variant<std::string, TokenizerPackError>;
using TokenizerEncodeResult = std::variant<std::vector<std::size_t>, TokenizerPackError>;

// Loads only the fixed local files vocab.json, token_type.json and the mode's merges file
// beneath pack_root, each opened by handle with open_beneath (no link is followed). All
// files must be regular, non-symlink files. Vocabulary IDs must be unique, contiguous, and
// zero-based; merges must name vocabulary tokens and be unique; token types must have
// exactly one value per ID. byte_bpe reads merges.txt (one "left right" per line) and
// refuses a pack that also has merges.json; spm_bpe reads merges.json (a JSON array of
// two-string arrays in rank order, so merges holding newlines, tabs or spaces round-trip) and
// refuses a pack that also has merges.txt (invalid_merge either way).
[[nodiscard]] TokenizerPackLoadResult load_local_tokenizer_pack(const CanonicalRoot& pack_root, TokenizerMode mode);
// Opens the root first (open_root: canonical, private, existing).
[[nodiscard]] TokenizerPackLoadResult load_local_tokenizer_pack(const std::filesystem::path& pack_root, TokenizerMode mode);
// The tokenizer a GGUF model file carries (spec 2026-10-27 §4.2: a card the page writes names no pack): the same
// vocabulary, token types and merges a pack extracted from that file holds, under the same rules. The mode's model must
// match (tokenizer.ggml.model gpt2 for byte_bpe, raon_bpe and flint_bpe; gemma4 for spm_bpe).
[[nodiscard]] TokenizerPackLoadResult load_tokenizer_from_gguf(const GgufMetadata& metadata, TokenizerMode mode);

// Decodes token IDs: byte_bpe through GPT-2's reversible byte alphabet; spm_bpe (Gemma 4
// plan 1 Task 5) as NORMAL tokens with U+2581 for a space and <0xXX> BYTE tokens as their byte.
// CONTROL (3) and USER_DEFINED (4) token types fail closed rather than being silently rendered
// as plain output; so does a BYTE token in a byte_bpe pack, and any type but NORMAL or BYTE in
// an spm_bpe pack.
[[nodiscard]] TokenizerDecodeResult decode_token_ids(const TokenizerPack& pack,
                                                      const std::vector<std::size_t>& ids);

// Raw bytes of ONE ordinary generated token (SP3 streaming). Unlike
// decode_token_ids, the result is not UTF-8 validated: a byte-level token may
// end in the middle of a character, which Utf8StreamDecoder resolves.
// CONTROL (3) and USER_DEFINED (4) types fail closed as in decode_token_ids.
// The pack must outlive the decoder.
class TokenByteDecoder final {
 public:
  explicit TokenByteDecoder(const TokenizerPack& pack);
  [[nodiscard]] TokenizerDecodeResult bytes_for(std::size_t id) const;

 private:
  const TokenizerPack& pack_;
  std::map<std::string, unsigned char> reverse_;
};

// Encodes ordinary UTF-8 text using the GPT-2 byte alphabet and merge ranks in this pack. A byte_bpe pack encodes as
// upstream's server tokenizes a Qwen pack (upstream v0.2.6 tools/strata_tokenizer.py, pre "qwen35"): the text split by
// qwen35_pretokenize (no normalization), each piece through raon_bpe's byte-level BPE (the lowest rank first, then the
// leftmost); no BOS. Input containing a CONTROL or USER_DEFINED vocabulary literal fails closed, as does text which
// cannot be expressed by this pack.
// An spm_bpe pack (Gemma 4 plan 1 Task 5) encodes as llama.cpp's gemma4 tokenizer at
// 3cf03257: spaces become U+2581, runs of non-newlines and runs of newlines are encoded apart, a newline-only
// run that is a token is that token, code points merge by the lowest rank (then leftmost), and
// a symbol that is no token becomes its bytes' <0xXX> tokens; no BOS, no space prefix.
// A raon_bpe pack encodes as raon-1-mini's reference tokenizer, KRAFTON's llama.cpp (raon-1-mini-preview
// fb895662): the text NFC-normalized (raon_nfc), split by raon_pretokenize, each piece in GPT-2's byte alphabet
// merged by the lowest rank (then leftmost); no BOS.
// A flint_bpe pack encodes as flint's reference tokenizer, llama.cpp master (e86f5fd1): the text split by
// flint_pretokenize (no normalization), each piece through raon_bpe's byte-level BPE; no BOS.
// A deepseek_bpe pack encodes as Maya's server tokenizes DeepSeek-V4.1 (Maya v1.1.30 tools/strata_tokenizer.py,
// pre "deepseek3"): the text split by deepseek_pretokenize (no normalization), each piece through raon_bpe's
// byte-level BPE (the lowest rank first, then the leftmost; no ignore_merges); no BOS.
// A glm_bpe pack encodes as Maya's server tokenizes GLM-5.3 (Maya v1.1.30 tools/strata_tokenizer.py, pre "glm4" /
// "glm5"): the text split by glm_pretokenize (no normalization); a piece whose bytes in GPT-2's alphabet are one
// ordinary vocabulary token is that token whole (llama.cpp's ignore_merges), any other through the byte-level BPE; no BOS.
// A minimax_bpe pack encodes as Maya's server tokenizes MiniMax-M3.1 (Maya v1.1.30 tools/strata_tokenizer.py, pre
// "minimax"): each plain span NFC-normalized and split by minimax_pretokenize, each piece through the byte-level BPE
// (the lowest rank first; no ignore_merges); no BOS.
[[nodiscard]] TokenizerEncodeResult encode_text(const TokenizerPack& pack,
                                                 std::string_view text);

// encode_text without the special-literal refusal: untrusted text (a tool's result, the model's own words) in which a
// CONTROL or USER_DEFINED literal is ordinary text, as upstream encodes a literal inside a message (#537/#2026).
[[nodiscard]] TokenizerEncodeResult encode_plain_text(const TokenizerPack& pack, std::string_view text);

// Encodes a prompt rendered in pieces (render_qwen_tool_prompt): each marker as the one token its literal names - it
// must occur exactly once in the vocabulary, as a CONTROL (3) or USER_DEFINED (4) token, else unsupported_special_token;
// the pieces between two markers as one span, the way upstream tokenizes a plain span with the text around it, where a
// text piece holding a special literal is refused (encode_text's rule) and a plain piece's literals are ordinary text.
[[nodiscard]] TokenizerEncodeResult encode_prompt_pieces(const TokenizerPack& pack, std::span<const PromptPiece> pieces);

// raon-1-mini's normalizer: Unicode NFC as llama.cpp's unicode_cpts_normalize_nfc computes it from llama.cpp's own
// Unicode tables (src/raon_unicode_data.inc) - canonical decomposition (Hangul by rule), each run of combining marks
// ordered by class, canonical composition. A byte that starts no valid UTF-8 sequence becomes U+FFFD, as there.
[[nodiscard]] std::string raon_nfc(std::string_view utf8);
// raon-1-mini's pre-tokenizer, llama.cpp's unicode_regex_split_custom_raon (its raon regex): CJK / kana runs; runs
// of letters with their marks, single spaces between words inside the run; digits in groups of three from the
// right; punctuation with an optional leading space and its trailing newlines or slashes; whitespace to its last
// newline, else all but the space the next word takes. The pieces concatenate to the text.
[[nodiscard]] std::vector<std::string> raon_pretokenize(std::string_view utf8);
// flint's pre-tokenizer: llama.cpp master's unicode_regex_split over LLAMA_VOCAB_PRE_TYPE_FLINT's three regexes in turn,
// on llama.cpp's collapsed text (each code point at or above U+0080 one class byte from llama.cpp's own tables, where
// every \p{L*} class is the one letter class, so a case change never splits a word): digits in threes from the left;
// CJK / kana runs (U+4E00-U+9FA5, U+3040-U+30FF); then a letter run with its marks after an optional character that is
// no letter, digit or newline, with an English contraction; punctuation and symbols with an optional leading space and
// their trailing newlines; whitespace to its last newline, else all but the space the next word takes. Text no regex
// matches is a piece of its own. Code points past U+FFFF are compared whole (llama.cpp on a 32-bit wchar_t; MSVC's
// 16-bit wchar_t would truncate them in the CJK pass). The pieces concatenate to the text.
[[nodiscard]] std::vector<std::string> flint_pretokenize(std::string_view utf8);
// DeepSeek-V4.1's pre-tokenizer as Maya's server runs it (strata_tokenizer.py DEEPSEEK3_PATTERNS through
// _IsolatedSplits): three "Isolated" splits in turn, each over the pieces the one before left, on the Python regex
// module's own classes (src/regex_unicode_data.inc): \p{N}{1,3}; CJK / kana runs (U+4E00-U+9FA5, U+3040-U+30FF); then
// one ASCII punctuation character leading ASCII letters, a letter-and-mark run after an optional character that is no
// letter, punctuation, symbol or newline, punctuation and symbols after an optional space with their trailing
// newlines, whitespace to its last newline, else all but the space the next word takes. Text no pattern matches is a
// piece of its own. The pieces concatenate to the text.
[[nodiscard]] std::vector<std::string> deepseek_pretokenize(std::string_view utf8);
// MiniMax-M3.1's pre-tokenizer as Maya's server runs it (strata_tokenizer.py: unicodedata NFC, then
// regex.findall(MINIMAX_PATTERN), GPT-4o's o200k regex) on the Python regex module's own classes
// (src/regex_unicode_data.inc): a word is an optional character that is no letter, digit or newline, then letters of
// the upper-case kinds (Lu Lt Lm Lo, marks) and of the lower-case kinds (Ll Lm Lo, marks) - one of the two runs
// required - with an English contraction in any case; digits in groups of three; punctuation and symbols after an
// optional space with their trailing newlines and slashes; whitespace to its last newline, else all but the space the
// next word takes. The pieces concatenate to the NFC text.
[[nodiscard]] std::vector<std::string> minimax_pretokenize(std::string_view utf8);
// Qwen's pre-tokenizer as upstream's server runs it (upstream v0.2.6 tools/strata_tokenizer.py QWEN35_PATTERN through
// regex.findall, llama.cpp's LLAMA_VOCAB_PRE_TYPE_QWEN35), on the Python regex module's own classes
// (src/regex_unicode_data.inc): an English contraction; a run of letters and marks after an optional character that is
// no letter, digit or newline; one digit; punctuation and symbols (no letter, mark, digit or whitespace) after an
// optional space with their trailing newlines; whitespace to its last newline, else all but the space the next word
// takes. The pieces concatenate to the text.
[[nodiscard]] std::vector<std::string> qwen35_pretokenize(std::string_view utf8);
// GLM-5.3's pre-tokenizer as Maya's server runs it (Maya v1.1.30 tools/strata_tokenizer.py GLM_PATTERN, llama.cpp's
// LLAMA_VOCAB_PRE_TYPE_CHATGLM4), on the same classes: qwen35_pretokenize's alternatives with a run of letters only
// (a mark is no letter: it leads the next word or joins punctuation), digits in threes from the left, and punctuation
// that is no letter, digit or whitespace. The pieces concatenate to the text.
[[nodiscard]] std::vector<std::string> glm_pretokenize(std::string_view utf8);

// The Qwen3 reasoning markers <think> and </think>. Found only when each literal occurs
// exactly once in the vocabulary as a CONTROL (3) or USER_DEFINED (4) token (Qwen3.8 packs
// mark them USER_DEFINED); nullopt otherwise, so a ChatML pack without reasoning keeps the
// plain generation prompt and its output rules.
struct ThinkTokens final {
  std::size_t open;
  std::size_t close;
};
// The id of `literal` when it is exactly one CONTROL (3) or USER_DEFINED (4) token of the pack, the only entry spelling
// it; nullopt otherwise (encode_prompt_pieces' rule for a marker).
[[nodiscard]] std::optional<std::size_t> find_special_token(const TokenizerPack& pack, std::string_view literal);
[[nodiscard]] std::optional<ThinkTokens> find_qwen_think_tokens(const TokenizerPack& pack);

// The Qwen end-of-turn tokens <|im_end|> and <|endoftext|> (upstream's stop ids,
// serve/server.py:3770-3771; the engine's default --eos-ids). In serve mode the engine
// prints the end-of-turn token as a T record before DONE ... stop; it ends the reply and
// is never output. Each literal counts only as exactly one CONTROL (3) token; the result
// lists those found, in that order (empty: none).
[[nodiscard]] std::vector<std::size_t> find_qwen_end_of_turn_tokens(const TokenizerPack& pack);

// The Qwen tool tags <tool_call>, </tool_call>, <tool_response> and </tool_response> (web search): found only when each
// literal occurs exactly once as a CONTROL (3) or USER_DEFINED (4) token (Qwen3.6 has them USER_DEFINED); nullopt
// otherwise, so a pack without them serves no tool calls.
struct QwenToolTokens final {
  std::size_t call_open;
  std::size_t call_close;
  std::size_t response_open;
  std::size_t response_close;
};
[[nodiscard]] std::optional<QwenToolTokens> find_qwen_tool_tokens(const TokenizerPack& pack);

// GLM-5.3's end-of-turn tokens <|endoftext|>, <|user|> and <|observation|> (Maya's glm_eos in generate.cpp, the
// ids llama.cpp marks EOG; Maya's server stops on the same three). The engine prints the one that ends the turn as
// a T record before DONE ... stop. Each counts only as exactly one CONTROL (3) token; the result lists those
// found, in that order (empty: none).
[[nodiscard]] std::vector<std::size_t> find_glm53_end_of_turn_tokens(const TokenizerPack& pack);

// GLM-5.3's tool tags (web search) with their ids, in this order: <tool_call>, </tool_call>, <arg_key>, </arg_key>,
// <arg_value>, </arg_value>, <tool_response>, </tool_response>. Found only when every literal occurs exactly once as a
// CONTROL (3) or USER_DEFINED (4) token (GLM-5.3-Flash has them USER_DEFINED); nullopt otherwise.
[[nodiscard]] std::optional<std::vector<std::pair<std::size_t, std::string>>> find_glm53_tool_tokens(const TokenizerPack& pack);

// Encodes a prompt produced by render_qwen_text_prompt.  This deliberately
// recognizes only the two fixed Qwen ChatML framing literals: <|im_start|>
// and <|im_end|>.  Each must be present in the pack as exactly one CONTROL
// (type 3) token.  Every non-marker span still goes through encode_text(), so
// arbitrary CONTROL or USER_DEFINED literals remain rejected.  This is not a
// general special-token parser and must not be used with untrusted templates.
// One exception (issue 19): when the text after the final "<|im_start|>assistant\n"
// is exactly the renderer's reasoning tail ("<think>\n" or
// "<think>\n\n</think>\n\n"), it encodes through find_qwen_think_tokens(); the
// markers anywhere else are rejected as before.
[[nodiscard]] TokenizerEncodeResult encode_qwen_chatml_prompt(
    const TokenizerPack& pack, std::string_view rendered_prompt);

// DeepSeek-V4.1's framing literals as DeepSeek's own encoder writes them (Maya v1.1.30 serve/ds41_encoding.py):
// <｜begin▁of▁sentence｜>, <｜System｜>, <｜User｜>, <｜Assistant｜>, <think>, </think>, <｜end▁of▁sentence｜>
// (｜ is U+FF5C, ▁ U+2581).
inline constexpr std::array<std::string_view, 7U> kDeepseekMarkers{
    "<\xEF\xBD\x9C" "begin\xE2\x96\x81of\xE2\x96\x81sentence\xEF\xBD\x9C>", "<\xEF\xBD\x9CSystem\xEF\xBD\x9C>",
    "<\xEF\xBD\x9CUser\xEF\xBD\x9C>", "<\xEF\xBD\x9C" "Assistant\xEF\xBD\x9C>", "<think>", "</think>",
    "<\xEF\xBD\x9C" "end\xE2\x96\x81of\xE2\x96\x81sentence\xEF\xBD\x9C>"};

// Encodes only the fixed literals render_deepseek_text_prompt emits (kDeepseekMarkers), each exactly once in the
// vocabulary as a CONTROL (3) or USER_DEFINED (4) token; every other span is ordinary text (deepseek_bpe) and keeps
// encode_text()'s special-token refusal. Not a general DeepSeek special-token parser.
[[nodiscard]] TokenizerEncodeResult encode_deepseek_prompt(const TokenizerPack& pack, std::string_view rendered_prompt);

// DeepSeek-V4.1's turn ends, <｜end▁of▁sentence｜> and <｜User｜>: Maya's server stops on both itself (serve/server.py
// :1749-1764), its engine on the EOS only. Each counts only as exactly one CONTROL or USER_DEFINED token (the real
// GGUF's types are not pinned); the result lists those found, in that order (empty: none).
[[nodiscard]] std::vector<std::size_t> find_deepseek_end_of_turn_tokens(const TokenizerPack& pack);

// Encodes only the fixed literals emitted by render_glm53_text_prompt:
// [gMASK], <sop>, <|system|>, <|user|>, <|assistant|>, and <think>.  Each
// literal must occur exactly once in the vocabulary as a CONTROL (type 3) or
// USER_DEFINED (type 4) token (GLM-5.3-Flash has <think> USER_DEFINED).
// Every other span is ordinary text and retains encode_text()'s
// special-token rejection. This is not a general GLM special-token parser.
[[nodiscard]] TokenizerEncodeResult encode_glm53_prompt(
    const TokenizerPack& pack, std::string_view rendered_prompt);

// Encodes only the fixed literals emitted by render_raon1_text_prompt:
// <|im_start|>, <|im_end|>, <think>, and </think>.  Each literal must occur
// exactly once in the vocabulary as a CONTROL (type 3) token. Every other span
// is ordinary text and retains encode_text()'s special-token rejection. This is
// not a general ChatML special-token parser.
[[nodiscard]] TokenizerEncodeResult encode_raon1_chatml_prompt(
    const TokenizerPack& pack, std::string_view rendered_prompt);

// Gemma 4 plan 1 Task 8 (spec §2.5, §6.3): the seven Gemma 4 markers the renderer and the
// reasoning split use, each found by its literal, never by number. Found only in an spm_bpe pack
// and only when every literal occurs exactly once with the type the GGUF gives it: <bos>, <eos>,
// <|turn>, <turn|> and <|think|> CONTROL (3); <|channel> and <channel|> USER_DEFINED (4). nullopt
// otherwise (a pack that is not Gemma's cannot serve the gemma4 profile).
struct Gemma4Markers final {
  std::size_t bos;
  std::size_t eos;
  std::size_t turn_open;      // <|turn>
  std::size_t turn_close;     // <turn|>
  std::size_t think;          // <|think|>
  std::size_t channel_open;   // <|channel>
  std::size_t channel_close;  // <channel|>
};
[[nodiscard]] std::optional<Gemma4Markers> find_gemma4_markers(const TokenizerPack& pack);

// Encodes a prompt produced by render_gemma4_text_prompt (spm_bpe packs only). It recognizes the
// renderer's own markers by position, never by a scan of client text: <bos> exactly once, first;
// <|think|> only right after a leading "<|turn>system\n"; <|channel> and <channel|> only as the
// closed thought channel "<|channel>thought\n<channel|>" that ends the prompt right after its
// final "<|turn>model\n"; <|turn> and <turn|> as the turn framing. Each maps to its single id
// (find_gemma4_markers); every other span is ordinary text through the spm_bpe encoder, so any
// CONTROL or USER_DEFINED literal anywhere else is refused (unsupported_special_token).
[[nodiscard]] TokenizerEncodeResult encode_gemma4_prompt(const TokenizerPack& pack, std::string_view prompt);

// Gemma 4 plan 1 Task 9 (spec §6.5): the end-of-turn ids <turn|> and <eos>, in that order, each
// listed only when it occurs exactly once as a CONTROL (3) token of an spm_bpe pack (empty: none).
// The engine prints the one it stopped on as a T record before DONE ... stop; it ends the reply
// and is never output.
[[nodiscard]] std::vector<std::size_t> find_gemma4_end_of_turn_tokens(const TokenizerPack& pack);

// flint: the five markers its renderer writes (render_flint_text_prompt), each found by its literal, never by number.
// Found only in a flint_bpe pack and only when every literal occurs exactly once with the type the GGUF gives it:
// <|system|>, <|user|> and <|assistant|> CONTROL (3); <think> and </think> USER_DEFINED (4). nullopt otherwise.
struct FlintMarkers final {
  std::size_t system;
  std::size_t user;
  std::size_t assistant;
  std::size_t think_open;   // <think>
  std::size_t think_close;  // </think>
};
[[nodiscard]] std::optional<FlintMarkers> find_flint_markers(const TokenizerPack& pack);

// Encodes a prompt produced by render_flint_text_prompt (flint_bpe packs only). Each of the five markers maps to its
// single id (find_flint_markers); every other span is ordinary text through the flint_bpe encoder, so any other
// CONTROL or USER_DEFINED literal is refused (unsupported_special_token). The renderer refuses content that would hold
// one of the five after the template's escapes, so every occurrence this scans for is the renderer's own.
[[nodiscard]] TokenizerEncodeResult encode_flint_prompt(const TokenizerPack& pack, std::string_view prompt);

// flint's end-of-generation ids, the GGUF's tokenizer.ggml.eog_token_ids by their literals and in that order:
// <|end_of_text|>, <|user|>, <|observation|> and <|developer|>, each listed only when it occurs exactly once as a
// CONTROL (3) token of a flint_bpe pack (empty: none). The engine prints the one it stopped on as a T record before
// DONE ... stop; it ends the reply and is never output.
[[nodiscard]] std::vector<std::size_t> find_flint_end_of_turn_tokens(const TokenizerPack& pack);

}  // namespace secure_frontend
