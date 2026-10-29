#include "secure_frontend/tokenizer_pack.hpp"

#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/json.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <set>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace secure_frontend {
namespace {

constexpr std::size_t kMaximumFileBytes = 64U * 1024U * 1024U;
constexpr std::size_t kMaximumVocabularyEntries = 1U * 1024U * 1024U;
constexpr std::size_t kMaximumTokenBytes = 16U * 1024U;
constexpr std::size_t kMaximumDecodedBytes = 8U * 1024U * 1024U;

TokenizerPackError error(const TokenizerPackErrorCode code, const std::string_view message) {
  return TokenizerPackError{code, std::string{message}};
}

bool is_continuation(const unsigned char byte) { return (byte & 0xc0U) == 0x80U; }

bool valid_utf8(const std::string_view value) {
  for (std::size_t i = 0U; i < value.size();) {
    const auto byte = static_cast<unsigned char>(value[i]);
    if (byte <= 0x7fU) { ++i; continue; }
    const auto has = [&](const std::size_t count) { return i + count < value.size(); };
    if (byte >= 0xc2U && byte <= 0xdfU && has(1U) && is_continuation(static_cast<unsigned char>(value[i + 1U]))) {
      i += 2U;
    } else if (byte == 0xe0U && has(2U) && static_cast<unsigned char>(value[i + 1U]) >= 0xa0U &&
               static_cast<unsigned char>(value[i + 1U]) <= 0xbfU && is_continuation(static_cast<unsigned char>(value[i + 2U]))) {
      i += 3U;
    } else if (((byte >= 0xe1U && byte <= 0xecU) || (byte >= 0xeeU && byte <= 0xefU)) && has(2U) &&
               is_continuation(static_cast<unsigned char>(value[i + 1U])) && is_continuation(static_cast<unsigned char>(value[i + 2U]))) {
      i += 3U;
    } else if (byte == 0xedU && has(2U) && static_cast<unsigned char>(value[i + 1U]) >= 0x80U &&
               static_cast<unsigned char>(value[i + 1U]) <= 0x9fU && is_continuation(static_cast<unsigned char>(value[i + 2U]))) {
      i += 3U;
    } else if (byte == 0xf0U && has(3U) && static_cast<unsigned char>(value[i + 1U]) >= 0x90U &&
               static_cast<unsigned char>(value[i + 1U]) <= 0xbfU && is_continuation(static_cast<unsigned char>(value[i + 2U])) &&
               is_continuation(static_cast<unsigned char>(value[i + 3U]))) {
      i += 4U;
    } else if (byte >= 0xf1U && byte <= 0xf3U && has(3U) && is_continuation(static_cast<unsigned char>(value[i + 1U])) &&
               is_continuation(static_cast<unsigned char>(value[i + 2U])) && is_continuation(static_cast<unsigned char>(value[i + 3U]))) {
      i += 4U;
    } else if (byte == 0xf4U && has(3U) && static_cast<unsigned char>(value[i + 1U]) >= 0x80U &&
               static_cast<unsigned char>(value[i + 1U]) <= 0x8fU && is_continuation(static_cast<unsigned char>(value[i + 2U])) &&
               is_continuation(static_cast<unsigned char>(value[i + 3U]))) {
      i += 4U;
    } else { return false; }
  }
  return true;
}

void append_utf8(std::string* out, const std::uint32_t point) {
  if (point <= 0x7fU) out->push_back(static_cast<char>(point));
  else if (point <= 0x7ffU) { out->push_back(static_cast<char>(0xc0U | (point >> 6U))); out->push_back(static_cast<char>(0x80U | (point & 0x3fU))); }
  else if (point <= 0xffffU) { out->push_back(static_cast<char>(0xe0U | (point >> 12U))); out->push_back(static_cast<char>(0x80U | ((point >> 6U) & 0x3fU))); out->push_back(static_cast<char>(0x80U | (point & 0x3fU))); }
  else { out->push_back(static_cast<char>(0xf0U | (point >> 18U))); out->push_back(static_cast<char>(0x80U | ((point >> 12U) & 0x3fU))); out->push_back(static_cast<char>(0x80U | ((point >> 6U) & 0x3fU))); out->push_back(static_cast<char>(0x80U | (point & 0x3fU))); }
}

std::variant<std::string, TokenizerPackError> read_required(const CanonicalRoot& root, const std::string_view name) {
  const auto relative = std::get<RelativePath>(parse_relative_path(name));
  auto opened = open_beneath(root, relative, OpenOptions{.kind = OpenKind::regular_file});
  if (const auto* failure = std::get_if<PathError>(&opened)) {
    switch (failure->code) {
      case PathErrorCode::not_found: return error(TokenizerPackErrorCode::required_file_missing, "required tokenizer pack file is missing");
      case PathErrorCode::symlink_not_allowed: return error(TokenizerPackErrorCode::symbolic_link_not_allowed, "tokenizer pack files must not be symbolic links");
      case PathErrorCode::not_regular_file: return error(TokenizerPackErrorCode::unsafe_pack_path, "tokenizer pack paths must name regular files");
      default: return error(TokenizerPackErrorCode::file_read_failed, "unable to open tokenizer pack file");
    }
  }
  const auto& file = std::get<OpenedFile>(opened);
  if (file.size() > kMaximumFileBytes) return error(TokenizerPackErrorCode::file_too_large, "tokenizer pack file exceeds fixed limit");
  auto text = file.read_all(kMaximumFileBytes);
  if (const auto* failure = std::get_if<PathError>(&text)) {
    return error(failure->code == PathErrorCode::too_large ? TokenizerPackErrorCode::file_too_large : TokenizerPackErrorCode::file_read_failed,
                 "unable to read tokenizer pack file");
  }
  return std::get<std::string>(std::move(text));
}

std::array<std::string, 256U> byte_symbols() {
  std::array<std::string, 256U> values{}; std::array<bool, 256U> printable{};
  for (unsigned int byte = 0x21U; byte <= 0x7eU; ++byte) printable[byte] = true;
  for (unsigned int byte = 0xa1U; byte <= 0xacU; ++byte) printable[byte] = true;
  for (unsigned int byte = 0xaeU; byte <= 0xffU; ++byte) printable[byte] = true;
  std::uint32_t shifted = 256U;
  for (unsigned int byte = 0U; byte < 256U; ++byte) { const auto point = printable[byte] ? byte : shifted++; append_utf8(&values[byte], point); }
  return values;
}

std::size_t utf8_width(const unsigned char value) {
  return value <= 0x7fU ? 1U : value <= 0xdfU ? 2U : value <= 0xefU ? 3U : 4U;
}

// ---- Gemma 4 plan 1 Task 5: spm_bpe (spec §2.5, §6.2; llama.cpp 3cf03257 llama-vocab.cpp
// :528-537 the pre-tokenizer, :619-738 the BPE session with the Gemma 4 newline rule at
// :637-644 and the <0xXX> fallback at :718-730, :3186-3200 and :3713-3730 the byte tokens) ----

constexpr std::string_view kSpmSpace = "\xE2\x96\x81";  // U+2581, SentencePiece's escaped space
constexpr int kTokenTypeNormal = 1;
constexpr int kTokenTypeByte = 6;

bool is_special_type(const int type) { return type == 3 || type == 4; }

// <0xXX> with two uppercase hex digits, exactly as the GGUF spells its 256 byte tokens.
std::optional<unsigned char> byte_token_value(const std::string_view token) {
  if (token.size() != 6U || token.substr(0U, 3U) != "<0x" || token[5] != '>') return std::nullopt;
  const auto hex = [](const char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  const int high = hex(token[3]);
  const int low = hex(token[4]);
  if (high < 0 || low < 0) return std::nullopt;
  return static_cast<unsigned char>(high * 16 + low);
}

}  // namespace

// The spm_bpe encoder's lookup tables over one pack's data. Outside the anonymous namespace
// because SpmEncodeIndex (named in the header) holds one.
struct SpmIndex final {
  std::unordered_map<std::string_view, std::size_t> ids;  // views into the indexed pack's own strings
  std::unordered_map<std::uint64_t, std::size_t> ranks;   // (left id << 32) | right id -> rank
  std::array<std::optional<std::size_t>, 256U> byte_ids{};
};

// TokenizerPack::spm_index: built by the loader for an spm_bpe pack, over a private copy of the
// pack's data (so a copy of the pack, or one outliving the loaded pack, never reads freed
// strings), and immutable afterwards.
struct SpmEncodeIndex final {
  TokenizerPack source;  // the pack as loaded (without an index of its own)
  SpmIndex index;        // views into source's strings
};

// TokenizerPack::raon_index: the raon_bpe encoder's lookups, built by the loader (they own their strings) for a pack
// of these sizes, and immutable afterwards.
struct RaonEncodeIndex final {
  std::unordered_map<std::string, std::size_t> ids;
  std::unordered_map<std::string, std::uint32_t> ranks;  // "left right" -> rank (byte-alphabet tokens hold no space)
  std::size_t tokens = 0U, merges = 0U, types = 0U;      // the pack it describes
};

namespace {

std::variant<SpmIndex, TokenizerPackError> build_spm_index(const TokenizerPack& pack) {
  if (pack.tokens_by_id.size() != pack.token_types.size() || pack.tokens_by_id.size() > 0xFFFFFFFFU) {
    return error(TokenizerPackErrorCode::invalid_token_types, "token type entries must match vocabulary entries");
  }
  SpmIndex index;
  index.ids.reserve(pack.tokens_by_id.size());
  for (std::size_t id = 0U; id < pack.tokens_by_id.size(); ++id) {
    if (!index.ids.emplace(pack.tokens_by_id[id], id).second) {
      return error(TokenizerPackErrorCode::invalid_vocabulary, "vocabulary tokens must be unique for encoding");
    }
    if (pack.token_types[id] == kTokenTypeByte) {
      if (const auto value = byte_token_value(pack.tokens_by_id[id])) index.byte_ids[*value] = id;
    }
  }
  index.ranks.reserve(pack.merges_by_rank.size());
  for (std::size_t rank = 0U; rank < pack.merges_by_rank.size(); ++rank) {
    const auto left = index.ids.find(pack.merges_by_rank[rank].first);
    const auto right = index.ids.find(pack.merges_by_rank[rank].second);
    if (left == index.ids.end() || right == index.ids.end()) continue;  // the loader refuses these
    index.ranks.emplace((static_cast<std::uint64_t>(left->second) << 32U) | right->second, rank);  // lowest rank kept
  }
  return index;
}

// The loader's prepared tables when they still describe `pack` (the same vocabulary, merge and
// type counts: a pack is not modified after load, and one that was is not encoded with stale
// tables); null otherwise, and the caller builds tables for that call.
const SpmIndex* prepared_spm_index(const TokenizerPack& pack) {
  const auto& prepared = pack.spm_index;
  if (!prepared || prepared->source.tokens_by_id.size() != pack.tokens_by_id.size() ||
      prepared->source.merges_by_rank.size() != pack.merges_by_rank.size() ||
      prepared->source.token_types.size() != pack.token_types.size()) {
    return nullptr;
  }
  return &prepared->index;
}

// Literal scan: a CONTROL or USER_DEFINED literal in text is never encoded as ordinary text.
std::optional<TokenizerPackError> refuse_special_literals(const TokenizerPack& pack, const std::string_view text) {
  for (std::size_t id = 0U; id < pack.tokens_by_id.size() && id < pack.token_types.size(); ++id) {
    if (is_special_type(pack.token_types[id]) && text.find(pack.tokens_by_id[id]) != std::string_view::npos) {
      return error(TokenizerPackErrorCode::unsupported_special_token, "special token literals are not accepted as ordinary input");
    }
  }
  return std::nullopt;
}

// ---- raon_bpe: raon-1-mini as its reference tokenizer, KRAFTON's llama.cpp (raon-1-mini-preview fb895662),
// src/llama-vocab.cpp's llm_tokenizer_bpe_session::tokenize with its unicode.cpp helpers, ported line for line ----

#include "raon_unicode_data.inc"

// UTF-8 to code points with unicode_cpt_from_utf8's checks: a lead byte whose sequence is cut short or malformed is
// U+FFFD and the next byte is read (unicode_cpts_from_utf8); a 4-byte value past U+10FFFF, which llama.cpp would
// fail to re-encode, is U+FFFD too.
std::vector<std::uint32_t> raon_code_points(const std::string_view text) {
  std::vector<std::uint32_t> out;
  out.reserve(text.size());
  const auto byte = [&text](const std::size_t at) { return static_cast<std::uint32_t>(static_cast<unsigned char>(text[at])); };
  const auto follows = [&](const std::size_t at) { return at < text.size() && (byte(at) & 0xC0U) == 0x80U; };
  for (std::size_t at = 0U; at < text.size();) {
    const std::uint32_t lead = byte(at);
    if ((lead & 0x80U) == 0U) {
      out.push_back(lead);
      at += 1U;
      continue;
    }
    if ((lead & 0x40U) != 0U) {
      if ((lead & 0x20U) == 0U) {
        if (follows(at + 1U)) {
          out.push_back(((lead & 0x1FU) << 6U) | (byte(at + 1U) & 0x3FU));
          at += 2U;
          continue;
        }
      } else if ((lead & 0x10U) == 0U) {
        if (follows(at + 1U) && follows(at + 2U)) {
          out.push_back(((lead & 0x0FU) << 12U) | ((byte(at + 1U) & 0x3FU) << 6U) | (byte(at + 2U) & 0x3FU));
          at += 3U;
          continue;
        }
      } else if ((lead & 0x08U) == 0U) {
        if (follows(at + 1U) && follows(at + 2U) && follows(at + 3U)) {
          const std::uint32_t cpt = ((lead & 0x07U) << 18U) | ((byte(at + 1U) & 0x3FU) << 12U) |
                                    ((byte(at + 2U) & 0x3FU) << 6U) | (byte(at + 3U) & 0x3FU);
          out.push_back(cpt <= 0x10FFFFU ? cpt : 0xFFFDU);
          at += 4U;
          continue;
        }
      }
    }
    out.push_back(0xFFFDU);
    at += 1U;
  }
  return out;
}

void raon_append_utf8(std::string& out, const std::uint32_t cpt) {
  if (cpt <= 0x7FU) {
    out.push_back(static_cast<char>(cpt));
  } else if (cpt <= 0x7FFU) {
    out.push_back(static_cast<char>(0xC0U | (cpt >> 6U)));
    out.push_back(static_cast<char>(0x80U | (cpt & 0x3FU)));
  } else if (cpt <= 0xFFFFU) {
    out.push_back(static_cast<char>(0xE0U | (cpt >> 12U)));
    out.push_back(static_cast<char>(0x80U | ((cpt >> 6U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (cpt & 0x3FU)));
  } else {
    out.push_back(static_cast<char>(0xF0U | (cpt >> 18U)));
    out.push_back(static_cast<char>(0x80U | ((cpt >> 12U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | ((cpt >> 6U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (cpt & 0x3FU)));
  }
}

// unicode_cpt_flags_from_cpt reduced to what the raon splitter reads (0 past U+10FFFF: llama.cpp's UNDEFINED)
std::uint8_t raon_flags(const std::uint32_t cpt) {
  if (cpt >= 0x110000U) return 0U;
  const auto& ranges = raon_unicode::kFlagRanges;
  const auto after = std::upper_bound(ranges.begin(), ranges.end(), cpt,
                                      [](const std::uint32_t value, const raon_unicode::FlagRange& range) { return value < range.start; });
  return std::prev(after)->flags;
}

bool raon_is_whitespace(const std::uint32_t cpt) {
  return std::binary_search(raon_unicode::kWhitespace.begin(), raon_unicode::kWhitespace.end(), cpt);
}

// unicode_canonical_class
std::uint8_t raon_canonical_class(const std::uint32_t cpt) {
  const auto& table = raon_unicode::kCombiningClass;
  const auto at = std::lower_bound(table.begin(), table.end(), cpt,
                                   [](const raon_unicode::CombiningClass& entry, const std::uint32_t value) { return entry.cpt < value; });
  return at != table.end() && at->cpt == cpt ? at->ccc : 0U;
}

// unicode_canonical_decompose: Hangul syllables by rule (UAX #15), the rest by the table, recursively
void raon_canonical_decompose(std::uint32_t cpt, std::vector<std::uint32_t>& result, std::vector<std::uint32_t>& pending) {
  pending.push_back(cpt);
  while (!pending.empty()) {
    cpt = pending.back();
    pending.pop_back();
    if (cpt >= 0xAC00U && cpt < 0xAC00U + 11172U) {
      const std::uint32_t index = cpt - 0xAC00U;
      result.push_back(0x1100U + index / 588U);
      result.push_back(0x1161U + (index % 588U) / 28U);
      if (index % 28U != 0U) result.push_back(0x11A7U + index % 28U);
      continue;
    }
    const auto& table = raon_unicode::kDecomposition;
    const auto at = std::lower_bound(table.begin(), table.end(), cpt,
                                     [](const raon_unicode::Decomposition& entry, const std::uint32_t value) { return entry.cpt < value; });
    if (at == table.end() || at->cpt != cpt) {
      result.push_back(cpt);
      continue;
    }
    if (at->second != 0U) pending.push_back(at->second);
    pending.push_back(at->first);
  }
}

// unicode_canonical_compose: Hangul LV and LVT by rule, the rest by the table; 0 when the pair does not compose
std::uint32_t raon_canonical_compose(const std::uint32_t first, const std::uint32_t second) {
  if (first >= 0x1100U && first < 0x1100U + 19U && second >= 0x1161U && second < 0x1161U + 21U) {
    return 0xAC00U + ((first - 0x1100U) * 21U + second - 0x1161U) * 28U;
  }
  if (first >= 0xAC00U && first < 0xAC00U + 11172U && (first - 0xAC00U) % 28U == 0U && second > 0x11A7U && second < 0x11A7U + 28U) {
    return first + second - 0x11A7U;
  }
  const std::uint64_t pair = (static_cast<std::uint64_t>(first) << 32U) | second;
  const auto& table = raon_unicode::kComposition;
  const auto at = std::lower_bound(table.begin(), table.end(), pair,
                                   [](const raon_unicode::Composition& entry, const std::uint64_t value) { return entry.pair < value; });
  return at != table.end() && at->pair == pair ? at->composite : 0U;
}

// unicode_cpts_normalize_nfc, as written there (its reorder loop included)
std::vector<std::uint32_t> raon_nfc_points(const std::vector<std::uint32_t>& cpts) {
  std::vector<std::uint32_t> decomposed;
  decomposed.reserve(cpts.size());
  std::vector<std::uint32_t> pending;
  for (const std::uint32_t cpt : cpts) raon_canonical_decompose(cpt, decomposed, pending);
  for (std::size_t i = 0U; i < decomposed.size();) {
    if (raon_canonical_class(decomposed[i]) == 0U) ++i;
    const std::size_t begin = i;
    while (i < decomposed.size() && raon_canonical_class(decomposed[i]) != 0U) ++i;
    std::stable_sort(decomposed.begin() + static_cast<std::ptrdiff_t>(begin), decomposed.begin() + static_cast<std::ptrdiff_t>(i),
                     [](const std::uint32_t a, const std::uint32_t b) { return raon_canonical_class(a) < raon_canonical_class(b); });
  }
  std::vector<std::uint32_t> result;
  result.reserve(decomposed.size());
  std::size_t starter = 0U;
  std::uint8_t previous_class = 0U;
  for (const std::uint32_t cpt : decomposed) {
    const std::uint8_t current_class = raon_canonical_class(cpt);
    const std::uint32_t composite = !result.empty() && (previous_class == 0U || previous_class < current_class)
                                        ? raon_canonical_compose(result[starter], cpt)
                                        : 0U;
    if (composite != 0U) {
      result[starter] = composite;
    } else {
      if (current_class == 0U) starter = result.size();
      result.push_back(cpt);
      previous_class = current_class;
    }
  }
  return result;
}

// unicode_regex_split_custom_raon over the whole text: each piece's length in code points
std::vector<std::size_t> raon_split(const std::vector<std::uint32_t>& cpts) {
  const std::size_t end = cpts.size();
  const auto flags = [&](const std::size_t p) -> std::uint8_t { return p < end ? raon_flags(cpts[p]) : 0U; };
  const auto whitespace = [&](const std::size_t p) { return p < end && raon_is_whitespace(cpts[p]); };
  const auto cjk = [&](const std::size_t p) {
    return p < end && ((cpts[p] >= 0x4E00U && cpts[p] <= 0x9FA5U) || (cpts[p] >= 0x3040U && cpts[p] <= 0x30FFU));
  };
  const auto letter = [&](const std::size_t p) { return (flags(p) & raon_unicode::kLetter) != 0U && !cjk(p); };
  const auto mark = [&](const std::size_t p) { return (flags(p) & raon_unicode::kAccentMark) != 0U; };
  const auto number = [&](const std::size_t p) { return (flags(p) & raon_unicode::kNumber) != 0U; };
  const auto punct = [&](const std::size_t q) {
    return q < end && !whitespace(q) && (flags(q) & (raon_unicode::kLetter | raon_unicode::kNumber)) == 0U;
  };
  std::vector<std::size_t> pieces;
  for (std::size_t pos = 0U; pos < end;) {
    const std::size_t start = pos;
    std::size_t p = pos + (cpts[pos] == 0x20U ? 1U : 0U);
    if (cjk(p)) {
      do { ++p; } while (cjk(p));
      pos = p;
    } else if (letter(p)) {
      while (true) {
        ++p;
        while (mark(p)) ++p;
        if (letter(p)) continue;
        if (p < end && cpts[p] == 0x20U && letter(p + 1U)) {
          ++p;
          continue;
        }
        break;
      }
      pos = p;
    } else if (number(pos)) {
      p = pos;
      while (number(p)) ++p;
      const std::size_t first = (p - pos) % 3U;
      pos += first != 0U ? first : 3U;
    } else if (punct(p)) {
      do { ++p; } while (punct(p));
      while (p < end && (cpts[p] == 0x0DU || cpts[p] == 0x0AU || cpts[p] == 0x2FU)) ++p;
      pos = p;
    } else {
      p = pos;
      std::size_t last_newline = pos;
      while (whitespace(p)) {
        if (cpts[p] == 0x0DU || cpts[p] == 0x0AU) last_newline = p + 1U;
        ++p;
      }
      if (last_newline > pos) {
        pos = last_newline;
      } else if (p > pos) {
        pos = p < end && p - pos > 1U ? p - 1U : p;
      } else {
        ++pos;
      }
    }
    pieces.push_back(pos - start);
  }
  return pieces;
}

// ---- flint_bpe: flint as its reference tokenizer, llama.cpp master (e86f5fd1): llm_tokenizer_bpe_session over
// unicode_regex_split with LLAMA_VOCAB_PRE_TYPE_FLINT's three regexes (llama-vocab.cpp:285), each run by
// unicode_regex_split_stl over the pieces the one before it left (unicode.cpp:1484-1700) ----

// A code point as unicode_regex_split's collapsed text reads it: below U+0080 the character itself; above it, llama.cpp's
// whitespace is \v and every other code point its category's byte (k_ucat_cpt; a category with no byte, 0xD0). The
// \p{...} classes are those bytes plus their ASCII ranges (k_ucat_map: \p{L} and every \p{L*} A-Za-z, \p{N} 0-9, \p{P}
// and \p{S} together every ASCII punctuation character), and std::regex's \s is the C locale's \t\n\v\f\r and space.
enum class FlintClass : std::uint8_t { other, letter, number, mark, punctuation, space, newline };

FlintClass flint_class(const std::uint32_t cpt) {
  if (cpt < 0x80U) {
    if (cpt == 0x0DU || cpt == 0x0AU) return FlintClass::newline;
    if (cpt == 0x09U || cpt == 0x0BU || cpt == 0x0CU || cpt == 0x20U) return FlintClass::space;
    if ((cpt >= 0x41U && cpt <= 0x5AU) || (cpt >= 0x61U && cpt <= 0x7AU)) return FlintClass::letter;
    if (cpt >= 0x30U && cpt <= 0x39U) return FlintClass::number;
    if ((cpt >= 0x21U && cpt <= 0x2FU) || (cpt >= 0x3AU && cpt <= 0x40U) || (cpt >= 0x5BU && cpt <= 0x60U) ||
        (cpt >= 0x7BU && cpt <= 0x7EU)) {
      return FlintClass::punctuation;
    }
    return FlintClass::other;
  }
  if (raon_is_whitespace(cpt)) return FlintClass::space;
  switch (raon_flags(cpt)) {  // one category a code point (the generator checks it)
    case raon_unicode::kLetter: return FlintClass::letter;
    case raon_unicode::kNumber: return FlintClass::number;
    case raon_unicode::kAccentMark: return FlintClass::mark;
    case raon_unicode::kPunctuation:
    case raon_unicode::kSymbol: return FlintClass::punctuation;
    default: return FlintClass::other;
  }
}

using FlintPieces = std::vector<std::pair<std::size_t, std::size_t>>;  // [begin, end) in code points

// unicode_regex_split_stl for one regex: in each piece, every leftmost match (match(i, end) is its length at i, 0 for
// none; no regex here matches empty) and every run between matches become pieces in order.
template <typename Match>
FlintPieces flint_pass(const FlintPieces& in, const Match& match) {
  FlintPieces out;
  out.reserve(in.size() * 2U);
  for (const auto& [begin, end] : in) {
    std::size_t gap = begin;
    for (std::size_t i = begin; i < end;) {
      const std::size_t length = match(i, end);
      if (length == 0U) {
        ++i;
        continue;
      }
      if (i > gap) out.emplace_back(gap, i);
      out.emplace_back(i, i + length);
      i += length;
      gap = i;
    }
    if (gap < end) out.emplace_back(gap, end);
  }
  return out;
}

// The three regexes over the whole text: each piece's length in code points.
std::vector<std::size_t> flint_split(const std::vector<std::uint32_t>& cpts) {
  std::vector<FlintClass> cls(cpts.size());
  for (std::size_t i = 0U; i < cpts.size(); ++i) cls[i] = flint_class(cpts[i]);
  const auto is = [&cls](const std::size_t p, const FlintClass c) { return cls[p] == c; };
  const auto letter_or_mark = [&](const std::size_t p) { return is(p, FlintClass::letter) || is(p, FlintClass::mark); };
  const auto blank = [&](const std::size_t p) { return is(p, FlintClass::space) || is(p, FlintClass::newline); };
  FlintPieces pieces;
  if (!cpts.empty()) pieces.emplace_back(0U, cpts.size());

  // 1. \p{N}{1,3}: a digit run in threes from the left.
  pieces = flint_pass(pieces, [&](const std::size_t i, const std::size_t end) {
    std::size_t j = i;
    while (j < end && j - i < 3U && is(j, FlintClass::number)) ++j;
    return j - i;
  });
  // 2. [一-龥぀-ゟ゠-ヿ]+ (std::wregex on the code points themselves).
  pieces = flint_pass(pieces, [&](const std::size_t i, const std::size_t end) {
    const auto cjk = [&](const std::size_t p) {
      return (cpts[p] >= 0x4E00U && cpts[p] <= 0x9FA5U) || (cpts[p] >= 0x3040U && cpts[p] <= 0x30FFU);
    };
    std::size_t j = i;
    while (j < end && cjk(j)) ++j;
    return j - i;
  });
  // 3. [^\r\n\p{L}\p{N}]?[\p{Lu}\p{Lt}\p{Lm}\p{Lo}\p{M}]*[\p{Ll}\p{Lm}\p{Lo}\p{M}]+(?:'s|'t|'re|'ve|'m|'ll|'d, any case)?
  //    (its second alternative matches the same text) | ?[\p{P}\p{S}]+[\r\n]* | \s*[\r\n]+ | \s+(?!\S) | \s+
  pieces = flint_pass(pieces, [&](const std::size_t i, const std::size_t end) -> std::size_t {
    const auto lower = [](const std::uint32_t c) { return c >= 0x41U && c <= 0x5AU ? c + 0x20U : c; };
    const auto contraction = [&](const std::size_t j) -> std::size_t {
      if (j + 1U >= end || cpts[j] != 0x27U) return 0U;
      const std::uint32_t first = lower(cpts[j + 1U]);
      if (first == U's' || first == U't' || first == U'm' || first == U'd') return 2U;
      if (j + 2U >= end) return 0U;
      const std::uint32_t second = lower(cpts[j + 2U]);
      return (first == U'r' && second == U'e') || (first == U'v' && second == U'e') || (first == U'l' && second == U'l') ? 3U
                                                                                                                       : 0U;
    };
    const bool leads = !is(i, FlintClass::newline) && !is(i, FlintClass::letter) && !is(i, FlintClass::number);
    if ((leads && i + 1U < end && letter_or_mark(i + 1U)) || letter_or_mark(i)) {
      std::size_t j = letter_or_mark(i) ? i : i + 1U;
      while (j < end && letter_or_mark(j)) ++j;
      return j - i + contraction(j);
    }
    std::size_t k = cpts[i] == 0x20U && i + 1U < end && is(i + 1U, FlintClass::punctuation) ? i + 1U : i;
    if (is(k, FlintClass::punctuation)) {
      while (k < end && is(k, FlintClass::punctuation)) ++k;
      while (k < end && is(k, FlintClass::newline)) ++k;
      return k - i;
    }
    if (!blank(i)) return 0U;
    std::size_t last_newline = 0U;
    for (k = i; k < end && blank(k); ++k) {
      if (is(k, FlintClass::newline)) last_newline = k + 1U;
    }
    if (last_newline != 0U) return last_newline - i;  // \s*[\r\n]+ ends at the run's last newline
    if (k == end || k - i == 1U) return k - i;         // \s+(?!\S) at the piece's end, or \s+ for one
    return k - 1U - i;                                 // \s+(?!\S): all but the one the next word takes
  });

  std::vector<std::size_t> lengths;
  lengths.reserve(pieces.size());
  for (const auto& [begin, end] : pieces) lengths.push_back(end - begin);
  return lengths;
}

// ---- deepseek_bpe: DeepSeek-V4.1 as Maya's server splits it (Maya v1.1.30 tools/strata_tokenizer.py, DEEPSEEK3_PATTERNS
// through _IsolatedSplits), on the Python regex module's own classes ----

#include "regex_unicode_data.inc"

// The regex module's classes of a code point (regex_unicode::k* bits; none for one no class holds).
std::uint16_t regex_classes(const std::uint32_t cpt) {
  const auto& ranges = regex_unicode::kClassRanges;
  const auto after = std::upper_bound(ranges.begin(), ranges.end(), cpt,
                                      [](const std::uint32_t c, const regex_unicode::ClassRange& r) { return c < r.start; });
  return after == ranges.begin() ? std::uint16_t{0} : std::prev(after)->classes;
}

// The three patterns over the whole text, each an "Isolated" split of the pieces the one before left (the text between
// two matches stays a piece of its own, as flint_pass keeps it): each piece's length in code points.
std::vector<std::size_t> deepseek_split(const std::vector<std::uint32_t>& cpts) {
  std::vector<std::uint16_t> cls(cpts.size());
  for (std::size_t i = 0U; i < cpts.size(); ++i) cls[i] = regex_classes(cpts[i]);
  const auto has = [&cls](const std::size_t p, const std::uint16_t c) { return (cls[p] & c) != 0U; };
  const auto newline = [&cpts](const std::size_t p) { return cpts[p] == 0x0DU || cpts[p] == 0x0AU; };
  FlintPieces pieces;
  if (!cpts.empty()) pieces.emplace_back(0U, cpts.size());

  // 1. \p{N}{1,3}: a digit run in threes from the left.
  pieces = flint_pass(pieces, [&](const std::size_t i, const std::size_t end) {
    std::size_t j = i;
    while (j < end && j - i < 3U && has(j, regex_unicode::kN)) ++j;
    return j - i;
  });
  // 2. [一-龥぀-ゟ゠-ヿ]+
  pieces = flint_pass(pieces, [&](const std::size_t i, const std::size_t end) {
    const auto cjk = [&](const std::size_t p) {
      return (cpts[p] >= 0x4E00U && cpts[p] <= 0x9FA5U) || (cpts[p] >= 0x3040U && cpts[p] <= 0x30FFU);
    };
    std::size_t j = i;
    while (j < end && cjk(j)) ++j;
    return j - i;
  });
  // 3. [!"#$%&'()*+,\-./:;<=>?@\[\\\]^_`{|}~][A-Za-z]+ | [^\r\n\p{L}\p{P}\p{S}]?[\p{L}\p{M}]+ | ?[\p{P}\p{S}]+[\r\n]*
  //    | \s*[\r\n]+ | \s+(?!\S) | \s+
  pieces = flint_pass(pieces, [&](const std::size_t i, const std::size_t end) -> std::size_t {
    const auto ascii_letter = [&](const std::size_t p) {
      return (cpts[p] >= 0x41U && cpts[p] <= 0x5AU) || (cpts[p] >= 0x61U && cpts[p] <= 0x7AU);
    };
    const auto ascii_punctuation = [&](const std::size_t p) {
      const std::uint32_t c = cpts[p];
      return (c >= 0x21U && c <= 0x2FU) || (c >= 0x3AU && c <= 0x40U) || (c >= 0x5BU && c <= 0x60U) || (c >= 0x7BU && c <= 0x7EU);
    };
    if (ascii_punctuation(i) && i + 1U < end && ascii_letter(i + 1U)) {
      std::size_t j = i + 1U;
      while (j < end && ascii_letter(j)) ++j;
      return j - i;
    }
    const auto letter_or_mark = [&](const std::size_t p) { return has(p, regex_unicode::kL | regex_unicode::kM); };
    const bool leads = !newline(i) && !has(i, regex_unicode::kL | regex_unicode::kP | regex_unicode::kS);
    if ((leads && i + 1U < end && letter_or_mark(i + 1U)) || letter_or_mark(i)) {
      std::size_t j = i + 1U;  // (with the leading character, or a letter or mark itself)
      while (j < end && letter_or_mark(j)) ++j;
      return j - i;
    }
    const auto punctuation = [&](const std::size_t p) { return has(p, regex_unicode::kP | regex_unicode::kS); };
    std::size_t k = cpts[i] == 0x20U && i + 1U < end && punctuation(i + 1U) ? i + 1U : i;
    if (punctuation(k)) {
      while (k < end && punctuation(k)) ++k;
      while (k < end && newline(k)) ++k;
      return k - i;
    }
    if (!has(i, regex_unicode::kSpace)) return 0U;
    std::size_t last_newline = 0U;
    for (k = i; k < end && has(k, regex_unicode::kSpace); ++k) {
      if (newline(k)) last_newline = k + 1U;
    }
    if (last_newline != 0U) return last_newline - i;  // \s*[\r\n]+ ends at the run's last newline
    if (k == end || k - i == 1U) return k - i;         // \s+(?!\S) at the piece's end, or \s+ for one
    return k - 1U - i;                                 // \s+(?!\S): all but the one the next word takes
  });

  std::vector<std::size_t> lengths;
  lengths.reserve(pieces.size());
  for (const auto& [begin, end] : pieces) lengths.push_back(end - begin);
  return lengths;
}

// ---- byte_bpe: Qwen as upstream's server splits it (upstream v0.2.6 tools/strata_tokenizer.py QWEN35_PATTERN);
// glm_bpe: GLM-5.3 as Maya's does (Maya v1.1.30 tools/strata_tokenizer.py GLM_PATTERN) - one pattern each through
// regex.findall, on the regex module's own classes ----

// The pattern over the whole text, its alternatives in order at each position; every code point is matched by one of
// them, so findall drops nothing. `glm`: GLM_PATTERN, whose letters are \p{L} alone (a mark leads the next word or
// joins the punctuation after it), whose digits run to three, and whose punctuation is [^\s\p{L}\p{N}]; else
// QWEN35_PATTERN's [\p{L}\p{M}], one digit, and [^\s\p{L}\p{M}\p{N}]. Each piece's length in code points.
std::vector<std::size_t> qwen_shape_split(const std::vector<std::uint32_t>& cpts, const bool glm) {
  const std::size_t end = cpts.size();
  std::vector<std::uint16_t> cls(end);
  for (std::size_t i = 0U; i < end; ++i) cls[i] = regex_classes(cpts[i]);
  const auto has = [&cls](const std::size_t p, const std::uint16_t c) { return (cls[p] & c) != 0U; };
  const auto newline = [&cpts](const std::size_t p) { return cpts[p] == 0x0DU || cpts[p] == 0x0AU; };
  const std::uint16_t word = glm ? regex_unicode::kL : static_cast<std::uint16_t>(regex_unicode::kL | regex_unicode::kM);
  const auto punctuation = [&](const std::size_t p) { return !has(p, static_cast<std::uint16_t>(word | regex_unicode::kN | regex_unicode::kSpace)); };
  const auto lower = [](const std::uint32_t c) { return c >= 0x41U && c <= 0x5AU ? c + 0x20U : c; };
  const auto match = [&](const std::size_t i) -> std::size_t {
    // (?:'[sS]|'[tT]|'[rR][eE]|'[vV][eE]|'[mM]|'[lL][lL]|'[dD])
    if (cpts[i] == 0x27U && i + 1U < end) {
      const std::uint32_t first = lower(cpts[i + 1U]);
      if (first == U's' || first == U't' || first == U'm' || first == U'd') return 2U;
      if (i + 2U < end) {
        const std::uint32_t second = lower(cpts[i + 2U]);
        if ((first == U'r' && second == U'e') || (first == U'v' && second == U'e') || (first == U'l' && second == U'l')) return 3U;
      }
    }
    // [^\r\n\p{L}\p{N}]?[\p{L}\p{M}]+ (glm: \p{L}+), the leading character taken when the run follows it
    const bool leads = !newline(i) && !has(i, regex_unicode::kL | regex_unicode::kN);
    if ((leads && i + 1U < end && has(i + 1U, word)) || has(i, word)) {
      std::size_t j = i + 1U;
      while (j < end && has(j, word)) ++j;
      return j - i;
    }
    // \p{N} (glm: \p{N}{1,3})
    if (has(i, regex_unicode::kN)) {
      std::size_t j = i + 1U;
      while (glm && j < end && j - i < 3U && has(j, regex_unicode::kN)) ++j;
      return j - i;
    }
    // " ?[^\s\p{L}\p{M}\p{N}]+[\r\n]*" (glm: [^\s\p{L}\p{N}])
    std::size_t k = cpts[i] == 0x20U && i + 1U < end && punctuation(i + 1U) ? i + 1U : i;
    if (punctuation(k)) {
      while (k < end && punctuation(k)) ++k;
      while (k < end && newline(k)) ++k;
      return k - i;
    }
    // \s*[\r\n]+ | \s+(?!\S) | \s+ (what is left at i is whitespace)
    std::size_t last_newline = 0U;
    for (k = i; k < end && has(k, regex_unicode::kSpace); ++k) {
      if (newline(k)) last_newline = k + 1U;
    }
    if (last_newline != 0U) return last_newline - i;  // \s*[\r\n]+ ends at the run's last newline
    if (k == end || k - i <= 1U) return std::max<std::size_t>(k - i, 1U);  // \s+(?!\S) at the end, or \s+ for one
    return k - 1U - i;                                                     // \s+(?!\S): all but the one the next word takes
  };
  std::vector<std::size_t> lengths;
  for (std::size_t i = 0U; i < end;) {
    const std::size_t length = match(i);
    lengths.push_back(length);
    i += length;
  }
  return lengths;
}

// ---- minimax_bpe: MiniMax-M3.1 as Maya's server splits it (Maya v1.1.30 tools/strata_tokenizer.py MINIMAX_PATTERN,
// GPT-4o's o200k regex, through regex.findall after NFC), on the regex module's own classes ----

// The pattern's seven alternatives in order at each position, each as the regex engine backtracks it; every code point
// is matched by one of them, so findall drops nothing. Each piece's length in code points (of the NFC text).
std::vector<std::size_t> minimax_split(const std::vector<std::uint32_t>& cpts) {
  using namespace regex_unicode;
  const std::size_t end = cpts.size();
  std::vector<std::uint16_t> cls(end);
  for (std::size_t i = 0U; i < end; ++i) cls[i] = regex_classes(cpts[i]);
  const auto has = [&cls](const std::size_t p, const std::uint16_t c) { return (cls[p] & c) != 0U; };
  const auto newline = [&cpts](const std::size_t p) { return cpts[p] == 0x0DU || cpts[p] == 0x0AU; };
  constexpr std::uint16_t kUpper = kLu | kLt | kLm | kLo | kM;  // [\p{Lu}\p{Lt}\p{Lm}\p{Lo}\p{M}]
  constexpr std::uint16_t kLower = kLl | kLm | kLo | kM;        // [\p{Ll}\p{Lm}\p{Lo}\p{M}]
  // (?i:'s|'t|'re|'ve|'m|'ll|'d): its length at p, 0 for none; no alternative is a prefix of another. Case-insensitive
  // as the regex module folds: the ASCII letters, and s also matches U+017F (long s, which folds to s).
  const auto contraction = [&](const std::size_t p) -> std::size_t {
    if (p + 1U >= end || cpts[p] != 0x27U) return 0U;
    const auto fold = [](const std::uint32_t c) -> std::uint32_t { return c == 0x17FU ? U's' : c >= 0x41U && c <= 0x5AU ? c + 0x20U : c; };
    const std::uint32_t first = fold(cpts[p + 1U]);
    if (first == U's' || first == U't' || first == U'm' || first == U'd') return 2U;
    if (p + 2U >= end) return 0U;
    const std::uint32_t second = fold(cpts[p + 2U]);
    const bool two = (first == U'r' && second == U'e') || (first == U'v' && second == U'e') || (first == U'l' && second == U'l');
    return two ? 3U : 0U;
  };
  // [U]*[L]+ from s: its end, 0 when it cannot match. U* takes its whole run, then gives back until [L]+ can start: at
  // the run's end when that is a lower-kind letter, else at the run's last character of a lower kind (Lm, Lo, a mark).
  const auto lower_word = [&](const std::size_t s) -> std::size_t {
    std::size_t j = s;
    while (j < end && has(j, kUpper)) ++j;
    std::size_t start = end + 1U;
    if (j < end && has(j, kLower)) {
      start = j;
    } else {
      for (std::size_t q = j; q > s; --q) {
        if (has(q - 1U, kLower)) {
          start = q - 1U;
          break;
        }
      }
    }
    if (start > end) return 0U;
    std::size_t k = start + 1U;
    while (k < end && has(k, kLower)) ++k;
    return k;
  };
  // [U]+[L]* from s: its end, 0 when it cannot match.
  const auto upper_word = [&](const std::size_t s) -> std::size_t {
    std::size_t j = s;
    while (j < end && has(j, kUpper)) ++j;
    if (j == s) return 0U;
    while (j < end && has(j, kLower)) ++j;
    return j;
  };
  // [^\r\n\p{L}\p{N}]?: taken first, given back when the word cannot follow it
  const auto word = [&](const std::size_t i, const auto& body) -> std::size_t {
    if (!newline(i) && !has(i, kL | kN) && i + 1U < end) {
      if (const std::size_t k = body(i + 1U); k != 0U) return k + contraction(k) - i;
    }
    if (const std::size_t k = body(i); k != 0U) return k + contraction(k) - i;
    return 0U;
  };
  const auto punctuation = [&](const std::size_t p) { return !has(p, static_cast<std::uint16_t>(kSpace | kL | kN)); };
  const auto match = [&](const std::size_t i) -> std::size_t {
    if (const std::size_t length = word(i, lower_word); length != 0U) return length;
    if (const std::size_t length = word(i, upper_word); length != 0U) return length;
    // \p{N}{1,3}
    if (has(i, kN)) {
      std::size_t j = i + 1U;
      while (j < end && j - i < 3U && has(j, kN)) ++j;
      return j - i;
    }
    // " ?[^\s\p{L}\p{N}]+[\r\n/]*"
    std::size_t k = cpts[i] == 0x20U && i + 1U < end && punctuation(i + 1U) ? i + 1U : i;
    if (punctuation(k)) {
      while (k < end && punctuation(k)) ++k;
      while (k < end && (newline(k) || cpts[k] == 0x2FU)) ++k;
      return k - i;
    }
    // \s*[\r\n]+ | \s+(?!\S) | \s+ (what is left at i is whitespace)
    std::size_t last_newline = 0U;
    for (k = i; k < end && has(k, kSpace); ++k) {
      if (newline(k)) last_newline = k + 1U;
    }
    if (last_newline != 0U) return last_newline - i;                       // \s*[\r\n]+ ends at the run's last newline
    if (k == end || k - i <= 1U) return std::max<std::size_t>(k - i, 1U);  // \s+(?!\S) at the end, or \s+ for one
    return k - 1U - i;                                                     // \s+(?!\S): all but the one the next word takes
  };
  std::vector<std::size_t> lengths;
  for (std::size_t i = 0U; i < end;) {
    const std::size_t length = match(i);
    lengths.push_back(length);
    i += length;
  }
  return lengths;
}

std::variant<std::shared_ptr<const RaonEncodeIndex>, TokenizerPackError> build_raon_index(const TokenizerPack& pack) {
  if (pack.tokens_by_id.size() != pack.token_types.size()) {
    return error(TokenizerPackErrorCode::invalid_token_types, "token type entries must match vocabulary entries");
  }
  auto index = std::make_shared<RaonEncodeIndex>();
  index->ids.reserve(pack.tokens_by_id.size());
  for (std::size_t id = 0U; id < pack.tokens_by_id.size(); ++id) {
    if (!index->ids.emplace(pack.tokens_by_id[id], id).second) {
      return error(TokenizerPackErrorCode::invalid_vocabulary, "vocabulary tokens must be unique for encoding");
    }
  }
  index->ranks.reserve(pack.merges_by_rank.size());
  for (std::size_t rank = 0U; rank < pack.merges_by_rank.size(); ++rank) {
    index->ranks.emplace(pack.merges_by_rank[rank].first + ' ' + pack.merges_by_rank[rank].second,
                         static_cast<std::uint32_t>(rank));  // (the lowest rank kept)
  }
  index->tokens = pack.tokens_by_id.size();
  index->merges = pack.merges_by_rank.size();
  index->types = pack.token_types.size();
  return std::shared_ptr<const RaonEncodeIndex>(std::move(index));
}

// One piece through llm_tokenizer_bpe_session: its bytes in GPT-2's alphabet, one symbol a byte, the pair of the
// lowest rank (then the leftmost) merged until none is a merge; a merged pair that went stale is known by its size.
std::optional<TokenizerPackError> raon_encode_piece(const TokenizerPack& pack, const RaonEncodeIndex& index,
                                                    const std::array<std::string, 256U>& symbols, const std::string_view piece,
                                                    std::vector<std::size_t>& output) {
  struct Symbol final {
    std::size_t offset;
    std::size_t size;  // 0: merged into its left neighbour
    std::ptrdiff_t prev;
    std::ptrdiff_t next;
  };
  struct Bigram final {
    std::uint32_t rank;
    std::ptrdiff_t left;
    std::ptrdiff_t right;
    std::size_t size;
  };
  const auto later = [](const Bigram& a, const Bigram& b) { return a.rank > b.rank || (a.rank == b.rank && a.left > b.left); };
  std::string encoded;
  encoded.reserve(piece.size() * 2U);
  std::vector<Symbol> symbol;
  symbol.reserve(piece.size());
  for (const char c : piece) {
    const std::string& alphabet = symbols[static_cast<unsigned char>(c)];
    const auto at = static_cast<std::ptrdiff_t>(symbol.size());
    symbol.push_back(Symbol{encoded.size(), alphabet.size(), at - 1, at + 1});
    encoded += alphabet;
  }
  if (symbol.empty()) return std::nullopt;
  symbol.back().next = -1;
  std::priority_queue<Bigram, std::vector<Bigram>, decltype(later)> queue(later);
  std::string key;
  const auto add = [&](const std::ptrdiff_t left, const std::ptrdiff_t right) {
    if (left < 0 || right < 0) return;
    const Symbol& l = symbol[static_cast<std::size_t>(left)];
    const Symbol& r = symbol[static_cast<std::size_t>(right)];
    key.assign(encoded, l.offset, l.size);
    key += ' ';
    key.append(encoded, r.offset, r.size);
    const auto found = index.ranks.find(key);
    if (found != index.ranks.end()) queue.push(Bigram{found->second, left, right, l.size + r.size});
  };
  for (std::size_t i = 1U; i < symbol.size(); ++i) add(static_cast<std::ptrdiff_t>(i) - 1, static_cast<std::ptrdiff_t>(i));
  while (!queue.empty()) {
    const Bigram top = queue.top();
    queue.pop();
    Symbol& l = symbol[static_cast<std::size_t>(top.left)];
    Symbol& r = symbol[static_cast<std::size_t>(top.right)];
    if (l.size == 0U || r.size == 0U || l.size + r.size != top.size) continue;
    l.size += r.size;
    r.size = 0U;
    l.next = r.next;
    if (r.next >= 0) symbol[static_cast<std::size_t>(r.next)].prev = top.left;
    add(l.prev, top.left);
    add(top.left, l.next);
  }
  for (std::ptrdiff_t at = 0; at >= 0; at = symbol[static_cast<std::size_t>(at)].next) {
    const Symbol& s = symbol[static_cast<std::size_t>(at)];
    if (s.size == 0U) continue;
    const auto found = index.ids.find(encoded.substr(s.offset, s.size));
    if (found == index.ids.end() || found->second >= pack.token_types.size() || is_special_type(pack.token_types[found->second])) {
      return error(TokenizerPackErrorCode::unencodable_text, "text cannot be encoded by ordinary vocabulary tokens");
    }
    if (output.size() >= kMaximumVocabularyEntries) {
      return error(TokenizerPackErrorCode::input_text_too_large, "encoded token count exceeds fixed limit");
    }
    output.push_back(found->second);
  }
  return std::nullopt;
}

struct SpmSymbol final {
  std::size_t offset;
  std::size_t size;  // 0: merged into its left neighbour
  std::ptrdiff_t prev;
  std::ptrdiff_t next;
};

struct SpmBigram final {
  std::size_t rank;
  std::size_t left;
  std::size_t right;
  std::size_t left_size;   // the symbols' sizes when queued: a changed size means the pair is stale
  std::size_t right_size;
};

// llama.cpp's llm_bigram_bpe::comparator: the lowest rank first, then the leftmost pair.
struct SpmBigramOrder final {
  bool operator()(const SpmBigram& l, const SpmBigram& r) const { return l.rank > r.rank || (l.rank == r.rank && l.left > r.left); }
};

// One pre-tokenized run ([^\n]+ or [\n]+) of the normalized text.
std::optional<TokenizerPackError> spm_encode_run(const TokenizerPack& pack, const SpmIndex& index, const std::string_view run,
                                                 std::vector<std::size_t>& output) {
  std::vector<SpmSymbol> symbols;
  // Gemma 4's rule (llama.cpp :637-644): a newline-only run that is a vocabulary token is that token.
  if (run.find_first_not_of('\n') == std::string_view::npos && index.ids.contains(run)) {
    symbols.push_back(SpmSymbol{0U, run.size(), -1, -1});
  } else {
    for (std::size_t offset = 0U; offset < run.size();) {
      const std::size_t width = std::min(run.size() - offset, utf8_width(static_cast<unsigned char>(run[offset])));
      symbols.push_back(SpmSymbol{offset, width, static_cast<std::ptrdiff_t>(symbols.size()) - 1, -1});
      offset += width;
    }
    for (std::size_t i = 0U; i + 1U < symbols.size(); ++i) symbols[i].next = static_cast<std::ptrdiff_t>(i + 1U);
  }
  std::priority_queue<SpmBigram, std::vector<SpmBigram>, SpmBigramOrder> queue;
  const auto add_bigram = [&](const std::ptrdiff_t left, const std::ptrdiff_t right) {
    if (left < 0 || right < 0) return;
    const auto& l = symbols[static_cast<std::size_t>(left)];
    const auto& r = symbols[static_cast<std::size_t>(right)];
    const auto left_id = index.ids.find(run.substr(l.offset, l.size));
    const auto right_id = index.ids.find(run.substr(r.offset, r.size));
    if (left_id == index.ids.end() || right_id == index.ids.end()) return;
    const auto rank = index.ranks.find((static_cast<std::uint64_t>(left_id->second) << 32U) | right_id->second);
    if (rank == index.ranks.end()) return;
    queue.push(SpmBigram{rank->second, static_cast<std::size_t>(left), static_cast<std::size_t>(right), l.size, r.size});
  };
  for (std::size_t i = 1U; i < symbols.size(); ++i) add_bigram(static_cast<std::ptrdiff_t>(i - 1U), static_cast<std::ptrdiff_t>(i));
  while (!queue.empty()) {
    const SpmBigram bigram = queue.top();
    queue.pop();
    auto& left = symbols[bigram.left];
    auto& right = symbols[bigram.right];
    if (left.size == 0U || right.size == 0U || left.size != bigram.left_size || right.size != bigram.right_size) continue;
    left.size += right.size;
    right.size = 0U;
    left.next = right.next;
    if (right.next >= 0) symbols[static_cast<std::size_t>(right.next)].prev = static_cast<std::ptrdiff_t>(bigram.left);
    add_bigram(left.prev, static_cast<std::ptrdiff_t>(bigram.left));
    add_bigram(static_cast<std::ptrdiff_t>(bigram.left), left.next);
  }
  for (const auto& symbol : symbols) {
    if (symbol.size == 0U) continue;
    const std::string_view piece = run.substr(symbol.offset, symbol.size);
    if (const auto found = index.ids.find(piece); found != index.ids.end()) {
      if (is_special_type(pack.token_types[found->second])) {
        return error(TokenizerPackErrorCode::unencodable_text, "text cannot be encoded by ordinary vocabulary tokens");
      }
      output.push_back(found->second);
      continue;
    }
    // The <0xXX> fallback; llama.cpp drops a byte without a token, this encoder fails closed.
    for (const char byte : piece) {
      const auto& id = index.byte_ids[static_cast<unsigned char>(byte)];
      if (!id.has_value()) return error(TokenizerPackErrorCode::unencodable_text, "text cannot be encoded by ordinary vocabulary tokens");
      output.push_back(*id);
    }
  }
  if (output.size() > kMaximumVocabularyEntries) {
    return error(TokenizerPackErrorCode::input_text_too_large, "encoded token count exceeds fixed limit");
  }
  return std::nullopt;
}

// Every U+0020 becomes U+2581, then runs of [^\n]+ and [\n]+ are encoded in order. No BOS
// (the renderer writes it) and no space prefix (add_space_prefix false in the Gemma 4 GGUF).
std::optional<TokenizerPackError> spm_encode_text(const TokenizerPack& pack, const SpmIndex& index, const std::string_view text,
                                                  std::vector<std::size_t>& output) {
  std::string normalized;
  normalized.reserve(text.size());
  for (const char c : text) {
    if (c == ' ') normalized += kSpmSpace;
    else normalized.push_back(c);
  }
  const std::string_view all = normalized;
  for (std::size_t begin = 0U; begin < all.size();) {
    const bool newline = all[begin] == '\n';
    std::size_t end = begin;
    while (end < all.size() && (all[end] == '\n') == newline) ++end;
    if (const auto failure = spm_encode_run(pack, index, all.substr(begin, end - begin), output)) return failure;
    begin = end;
  }
  return std::nullopt;
}

// One extra entry so an oversized vocabulary still reaches the
// invalid_vocabulary check below with its own error code.
constexpr JsonLimits kPackJsonLimits{.max_bytes = kMaximumFileBytes, .max_depth = 1U,
                                     .max_container_entries = kMaximumVocabularyEntries + 1U,
                                     .max_string_bytes = kMaximumTokenBytes};

std::optional<std::map<std::string, std::size_t>> vocabulary_from(const JsonParseResult& parsed) {
  const auto* root = std::get_if<JsonValue>(&parsed);
  const auto* members = root != nullptr ? root->if_object() : nullptr;
  if (members == nullptr) return std::nullopt;
  std::map<std::string, std::size_t> result;
  for (const auto& member : *members) {
    const auto* id = member.value.if_integer();
    if (id == nullptr || *id < 0) return std::nullopt;
    result.emplace(member.key, static_cast<std::size_t>(*id));
  }
  return result;
}

// Gemma 4 plan 1 Task 4: merges.json is one array of [left, right] pairs (depth 2), each
// string bounded like a vocabulary token, at most the vocabulary cap of entries (+1 so an
// oversized list reaches its own invalid_merge check).
constexpr JsonLimits kMergesJsonLimits{.max_bytes = kMaximumFileBytes, .max_depth = 2U,
                                       .max_container_entries = kMaximumVocabularyEntries + 1U,
                                       .max_string_bytes = kMaximumTokenBytes};

// Present means anything but "not found": a link or a directory named like the other mode's
// merges file also makes the pack ambiguous, so it fails closed too.
bool pack_file_present(const CanonicalRoot& root, const std::string_view name) {
  const auto relative = std::get<RelativePath>(parse_relative_path(name));
  const auto opened = open_beneath(root, relative, OpenOptions{.kind = OpenKind::regular_file});
  const auto* failure = std::get_if<PathError>(&opened);
  return failure == nullptr || failure->code != PathErrorCode::not_found;
}

// merges.txt (byte_bpe): one "left right" pair per line, a trailing \r stripped, blank lines skipped.
std::optional<TokenizerPackError> merges_from_text(const std::string_view remaining, const std::set<std::string>& known,
                                                   TokenizerPack& pack) {
  std::set<std::pair<std::string, std::string>> seen;
  std::size_t start = 0U;
  while (start <= remaining.size()) {
    const auto end = remaining.find('\n', start); std::string_view line = remaining.substr(start, end == std::string_view::npos ? remaining.size() - start : end - start);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1U);
    if (!line.empty()) {
      const auto split = line.find(' ');
      if (split == std::string_view::npos || split == 0U || split + 1U >= line.size() || line.find(' ', split + 1U) != std::string_view::npos) return error(TokenizerPackErrorCode::invalid_merge, "each merge must be exactly two tokens separated by one space");
      std::pair<std::string, std::string> merge{line.substr(0U, split), line.substr(split + 1U)};
      if (!known.contains(merge.first) || !known.contains(merge.second) || !seen.insert(merge).second || pack.merges_by_rank.size() >= kMaximumVocabularyEntries) return error(TokenizerPackErrorCode::invalid_merge, "merges must be unique bounded pairs of vocabulary tokens");
      pack.merges_by_rank.push_back(std::move(merge));
    }
    if (end == std::string_view::npos) break;
    start = end + 1U;
  }
  return std::nullopt;
}

// merges.json (spm_bpe): [["left","right"], ...] in rank order; each side a vocabulary token.
std::optional<TokenizerPackError> merges_from_json(const std::string_view text, const std::set<std::string>& known,
                                                   TokenizerPack& pack) {
  const auto parsed = parse_json(text, kMergesJsonLimits);
  const auto* root = std::get_if<JsonValue>(&parsed);
  const auto* items = root != nullptr ? root->if_array() : nullptr;
  if (items == nullptr) return error(TokenizerPackErrorCode::invalid_json, "merges.json must be a JSON array of two-string arrays");
  if (items->size() > kMaximumVocabularyEntries) return error(TokenizerPackErrorCode::invalid_merge, "merges must be unique bounded pairs of vocabulary tokens");
  std::set<std::pair<std::string, std::string>> seen;
  pack.merges_by_rank.reserve(items->size());
  for (const auto& item : *items) {
    const auto* pair = item.if_array();
    const auto* left = pair != nullptr && pair->size() == 2U ? (*pair)[0].if_string() : nullptr;
    const auto* right = pair != nullptr && pair->size() == 2U ? (*pair)[1].if_string() : nullptr;
    if (left == nullptr || right == nullptr) return error(TokenizerPackErrorCode::invalid_json, "merges.json must be a JSON array of two-string arrays");
    std::pair<std::string, std::string> merge{*left, *right};
    if (!known.contains(merge.first) || !known.contains(merge.second) || !seen.insert(merge).second) return error(TokenizerPackErrorCode::invalid_merge, "merges must be unique bounded pairs of vocabulary tokens");
    pack.merges_by_rank.push_back(std::move(merge));
  }
  return std::nullopt;
}

std::optional<std::vector<int>> token_types_from(const JsonParseResult& parsed) {
  const auto* root = std::get_if<JsonValue>(&parsed);
  const auto* items = root != nullptr ? root->if_array() : nullptr;
  if (items == nullptr || items->size() > kMaximumVocabularyEntries) return std::nullopt;
  std::vector<int> result;
  result.reserve(items->size());
  for (const auto& item : *items) {
    const auto* value = item.if_integer();
    if (value == nullptr || *value < std::numeric_limits<int>::min() || *value > std::numeric_limits<int>::max()) {
      return std::nullopt;
    }
    result.push_back(static_cast<int>(*value));
  }
  return result;
}

}  // namespace

namespace {

// The encoder's tables once, at load, instead of per request: spm_bpe's (about 0.2 s for Gemma 4's vocabulary), and
// every byte-level mode's token and merge lookups.
TokenizerPackLoadResult prepare_encode_index(TokenizerPack pack) {
  if (pack.mode == TokenizerMode::spm_bpe) {
    auto prepared = std::make_shared<SpmEncodeIndex>();
    prepared->source = pack;
    auto built = build_spm_index(prepared->source);
    if (const auto* failure = std::get_if<TokenizerPackError>(&built)) return *failure;
    prepared->index = std::move(std::get<SpmIndex>(built));
    pack.spm_index = std::move(prepared);
  } else {
    auto built = build_raon_index(pack);
    if (const auto* failure = std::get_if<TokenizerPackError>(&built)) return *failure;
    pack.raon_index = std::get<std::shared_ptr<const RaonEncodeIndex>>(std::move(built));
  }
  return pack;
}

}  // namespace

TokenizerPackLoadResult load_local_tokenizer_pack(const std::filesystem::path& pack_root, const TokenizerMode mode) {
  auto root = open_root(pack_root);
  if (const auto* failure = std::get_if<PathError>(&root)) {
    return error(failure->code == PathErrorCode::not_directory ? TokenizerPackErrorCode::pack_root_not_directory
                                                               : TokenizerPackErrorCode::pack_root_unavailable,
                 "tokenizer pack root must be an existing private directory");
  }
  return load_local_tokenizer_pack(std::get<CanonicalRoot>(root), mode);
}

TokenizerPackLoadResult load_local_tokenizer_pack(const CanonicalRoot& root, const TokenizerMode mode) {
  // Gemma 4 plan 1 Task 4 (Review Focus 2): the mode is the profile's; a pack that also holds
  // the other mode's merges file is ambiguous and refused before anything is read.
  const bool spm = mode == TokenizerMode::spm_bpe;
  if (pack_file_present(root, spm ? "merges.txt" : "merges.json")) {
    return error(TokenizerPackErrorCode::invalid_merge, spm ? "an spm_bpe tokenizer pack keeps its merges in merges.json, never merges.txt"
                                                            : "a byte_bpe tokenizer pack keeps its merges in merges.txt, never merges.json");
  }
  const auto vocab_text = read_required(root, "vocab.json"); const auto merges_text = read_required(root, spm ? "merges.json" : "merges.txt"); const auto types_text = read_required(root, "token_type.json");
  if (const auto* e = std::get_if<TokenizerPackError>(&vocab_text)) return *e;
  if (const auto* e = std::get_if<TokenizerPackError>(&merges_text)) return *e;
  if (const auto* e = std::get_if<TokenizerPackError>(&types_text)) return *e;
  const auto vocabulary = vocabulary_from(parse_json(std::get<std::string>(vocab_text), kPackJsonLimits));
  const auto types = token_types_from(parse_json(std::get<std::string>(types_text), kPackJsonLimits));
  if (!vocabulary) return error(TokenizerPackErrorCode::invalid_json, "vocab.json must be a JSON object of token IDs");
  if (!types) return error(TokenizerPackErrorCode::invalid_json, "token_type.json must be a JSON array of integers");
  if (vocabulary->empty() || vocabulary->size() > kMaximumVocabularyEntries) return error(TokenizerPackErrorCode::invalid_vocabulary, "vocabulary size is outside fixed bounds");
  if (types->size() != vocabulary->size()) return error(TokenizerPackErrorCode::invalid_token_types, "token types must have exactly one value per vocabulary ID");
  TokenizerPack pack; pack.tokens_by_id.resize(vocabulary->size()); pack.token_types = *types; pack.mode = mode;
  for (const auto& [token, id] : *vocabulary) {
    if (token.empty() || token.size() > kMaximumTokenBytes || id >= pack.tokens_by_id.size() || !pack.tokens_by_id[id].empty()) return error(TokenizerPackErrorCode::invalid_vocabulary, "vocabulary IDs must be contiguous, unique, and token strings bounded");
    pack.tokens_by_id[id] = token;
  }
  if (std::any_of(pack.tokens_by_id.begin(), pack.tokens_by_id.end(), [](const std::string& token) { return token.empty(); })) return error(TokenizerPackErrorCode::invalid_vocabulary, "vocabulary IDs must start at zero and be contiguous");
  const std::set<std::string> known(pack.tokens_by_id.begin(), pack.tokens_by_id.end());
  const auto merge_error = spm ? merges_from_json(std::get<std::string>(merges_text), known, pack)
                               : merges_from_text(std::get<std::string>(merges_text), known, pack);
  if (merge_error) return *merge_error;
  return prepare_encode_index(std::move(pack));
}

TokenizerPackLoadResult load_tokenizer_from_gguf(const GgufMetadata& metadata, const TokenizerMode mode) {
  // The GGUF's own tokenizer (spec 2026-10-27 §4.2): the vocabulary, types and merges a pack would hold, from the
  // header, under the pack loader's rules. The mode's model: GPT-2's byte alphabet for the byte modes, Gemma 4's
  // SentencePiece-style tokenizer for spm_bpe.
  const bool spm = mode == TokenizerMode::spm_bpe;
  const auto* model = metadata.string("tokenizer.ggml.model");
  if (model == nullptr || *model != (spm ? "gemma4" : "gpt2")) {
    return error(TokenizerPackErrorCode::invalid_vocabulary, spm ? "the GGUF's tokenizer is not Gemma 4's (tokenizer.ggml.model gemma4)"
                                                                 : "the GGUF's tokenizer is not a byte-level BPE (tokenizer.ggml.model gpt2)");
  }
  const auto* tokens = metadata.strings("tokenizer.ggml.tokens");
  const auto* types = metadata.integers("tokenizer.ggml.token_type");
  const auto* merges = metadata.strings("tokenizer.ggml.merges");
  if (tokens == nullptr || tokens->empty() || tokens->size() > kMaximumVocabularyEntries) {
    return error(TokenizerPackErrorCode::invalid_vocabulary, "the GGUF's vocabulary is missing or outside fixed bounds");
  }
  if (types == nullptr || types->size() != tokens->size()) {
    return error(TokenizerPackErrorCode::invalid_token_types, "the GGUF must give exactly one token type per token");
  }
  if (merges == nullptr || merges->size() > kMaximumVocabularyEntries) {
    return error(TokenizerPackErrorCode::invalid_merge, "the GGUF's merges are missing or outside fixed bounds");
  }
  TokenizerPack pack;
  pack.mode = mode;
  pack.tokens_by_id = *tokens;
  pack.token_types.reserve(types->size());
  for (const auto type : *types) {
    if (type < 0 || type > 6) return error(TokenizerPackErrorCode::invalid_token_types, "a GGUF token type is out of range");
    pack.token_types.push_back(static_cast<int>(type));
  }
  std::set<std::string_view> known;
  for (const auto& token : pack.tokens_by_id) {
    if (token.empty() || token.size() > kMaximumTokenBytes || !known.insert(token).second) {
      return error(TokenizerPackErrorCode::invalid_vocabulary, "GGUF vocabulary tokens must be unique and bounded");
    }
  }
  pack.merges_by_rank.reserve(merges->size());
  for (const auto& merge : *merges) {
    const auto space = merge.find(' ');
    if (space == std::string::npos || merge.find(' ', space + 1U) != std::string::npos) {
      return error(TokenizerPackErrorCode::invalid_merge, "a GGUF merge is not two tokens separated by one space");
    }
    std::string left = merge.substr(0, space);
    std::string right = merge.substr(space + 1U);
    if (!known.contains(left) || !known.contains(right)) {
      return error(TokenizerPackErrorCode::invalid_merge, "a GGUF merge names a token that is not in the vocabulary");
    }
    pack.merges_by_rank.emplace_back(std::move(left), std::move(right));
  }
  return prepare_encode_index(std::move(pack));
}

TokenByteDecoder::TokenByteDecoder(const TokenizerPack& pack) : pack_(pack) {
  const auto symbols = byte_symbols();
  for (unsigned int byte = 0U; byte < 256U; ++byte) reverse_.emplace(symbols[byte], static_cast<unsigned char>(byte));
}

TokenizerDecodeResult TokenByteDecoder::bytes_for(const std::size_t id) const {
  if (id >= pack_.tokens_by_id.size()) return error(TokenizerPackErrorCode::token_id_out_of_range, "token ID is outside the loaded vocabulary");
  if (id >= pack_.token_types.size() || pack_.token_types[id] == 3 || pack_.token_types[id] == 4) {
    return error(TokenizerPackErrorCode::undecodable_token, "control and user-defined token types are not ordinary decoded output");
  }
  const std::string_view token = pack_.tokens_by_id[id];
  // Gemma 4 plan 1 Task 5: spm_bpe output is NORMAL tokens (U+2581 is a space) and the
  // <0xXX> BYTE tokens (one raw byte; Utf8StreamDecoder joins a split character); any other
  // type fails closed. A byte_bpe pack never holds a BYTE token: one is refused there too.
  if (pack_.mode == TokenizerMode::spm_bpe) {
    if (pack_.token_types[id] == kTokenTypeByte) {
      const auto value = byte_token_value(token);
      if (!value.has_value()) return error(TokenizerPackErrorCode::undecodable_token, "a byte token must be spelled <0xXX>");
      return std::string(1U, static_cast<char>(*value));
    }
    if (pack_.token_types[id] != kTokenTypeNormal) {
      return error(TokenizerPackErrorCode::undecodable_token, "only normal and byte tokens are ordinary decoded output");
    }
    std::string text;
    text.reserve(token.size());
    for (std::size_t offset = 0U; offset < token.size();) {
      if (token.substr(offset, kSpmSpace.size()) == kSpmSpace) {
        text.push_back(' ');
        offset += kSpmSpace.size();
      } else {
        text.push_back(token[offset]);
        ++offset;
      }
    }
    return text;
  }
  if (pack_.token_types[id] == kTokenTypeByte) {
    return error(TokenizerPackErrorCode::undecodable_token, "byte tokens are not part of a byte-level pack");
  }
  std::string bytes;
  for (std::size_t offset = 0U; offset < token.size();) {
    const auto lead = static_cast<unsigned char>(token[offset]);
    const std::size_t width = lead <= 0x7fU ? 1U : lead <= 0xdfU ? 2U : lead <= 0xefU ? 3U : 4U;
    if (offset + width > token.size()) return error(TokenizerPackErrorCode::undecodable_token, "token is not composed of byte alphabet symbols");
    const auto it = reverse_.find(std::string{token.substr(offset, width)});
    if (it == reverse_.end()) return error(TokenizerPackErrorCode::undecodable_token, "token is not composed of byte alphabet symbols");
    bytes.push_back(static_cast<char>(it->second));
    offset += width;
  }
  return bytes;
}

TokenizerDecodeResult decode_token_ids(const TokenizerPack& pack, const std::vector<std::size_t>& ids) {
  const TokenByteDecoder decoder(pack);
  std::string bytes;
  for (const std::size_t id : ids) {
    auto piece = decoder.bytes_for(id);
    if (auto* failure = std::get_if<TokenizerPackError>(&piece)) return std::move(*failure);
    const auto& text = std::get<std::string>(piece);
    if (bytes.size() + text.size() > kMaximumDecodedBytes) return error(TokenizerPackErrorCode::decoded_text_not_utf8, "decoded token data exceeds fixed limit");
    bytes += text;
  }
  if (!valid_utf8(bytes)) return error(TokenizerPackErrorCode::decoded_text_not_utf8, "decoded token data is not valid UTF-8");
  return bytes;
}

namespace {

// encode_text's work; `refuse`: a CONTROL or USER_DEFINED literal in the text is refused (encode_plain_text: it is text).
TokenizerEncodeResult encode_ordinary(const TokenizerPack& pack, const std::string_view text, const bool refuse) {
  if (text.size() > kMaximumDecodedBytes) return error(TokenizerPackErrorCode::input_text_too_large, "input text exceeds fixed limit");
  if (!valid_utf8(text)) return error(TokenizerPackErrorCode::input_text_not_utf8, "input text must be valid UTF-8");
  if (pack.mode == TokenizerMode::spm_bpe) {  // Gemma 4 plan 1 Task 5
    std::optional<SpmIndex> local;
    const SpmIndex* index = prepared_spm_index(pack);
    if (index == nullptr) {
      auto built = build_spm_index(pack);
      if (const auto* failure = std::get_if<TokenizerPackError>(&built)) return *failure;
      local.emplace(std::move(std::get<SpmIndex>(built)));
      index = &*local;
    }
    if (const auto refused = refuse ? refuse_special_literals(pack, text) : std::nullopt) return *refused;
    std::vector<std::size_t> output;
    if (const auto failure = spm_encode_text(pack, *index, text, output)) return *failure;
    return output;
  }
  // The byte-level modes: raon-1-mini NFC and its pre-tokenizer; flint llama.cpp's flint regexes; DeepSeek-V4.1 Maya's
  // deepseek3 splits; GLM-5.3 Maya's glm4 split; Qwen upstream's qwen35 split (none of the last four normalizes). Then
  // the byte-level BPE by rank (the loader's tables while they describe this pack); glm_bpe first takes a piece that is
  // one ordinary vocabulary token whole (ignore_merges).
  const bool raon = pack.mode == TokenizerMode::raon_bpe;
  const bool glm = pack.mode == TokenizerMode::glm_bpe;
  const bool minimax = pack.mode == TokenizerMode::minimax_bpe;
  std::shared_ptr<const RaonEncodeIndex> index = pack.raon_index;
  if (!index || index->tokens != pack.tokens_by_id.size() || index->merges != pack.merges_by_rank.size() ||
      index->types != pack.token_types.size()) {
    auto built = build_raon_index(pack);
    if (const auto* failure = std::get_if<TokenizerPackError>(&built)) return *failure;
    index = std::get<std::shared_ptr<const RaonEncodeIndex>>(std::move(built));
  }
  if (const auto refused = refuse ? refuse_special_literals(pack, text) : std::nullopt) return *refused;
  // (raon-1-mini and MiniMax-M3.1 normalize NFC first; MiniMax per plain span, as here, between the special tokens)
  const auto cpts = raon || minimax ? raon_nfc_points(raon_code_points(text)) : raon_code_points(text);
  const auto symbols = byte_symbols();
  std::vector<std::size_t> output;
  std::string piece;
  std::string mapped;
  std::size_t at = 0U;
  const auto lengths = raon                                        ? raon_split(cpts)
                       : minimax                                  ? minimax_split(cpts)
                       : pack.mode == TokenizerMode::deepseek_bpe ? deepseek_split(cpts)
                       : pack.mode == TokenizerMode::flint_bpe    ? flint_split(cpts)
                                                                  : qwen_shape_split(cpts, glm);
  for (const std::size_t length : lengths) {
    piece.clear();
    for (std::size_t i = at; i < at + length; ++i) raon_append_utf8(piece, cpts[i]);
    at += length;
    if (glm) {
      // Maya's _encode_plain: the lookup is in the byte alphabet (x=Δy²: "Â²" is a token, "²" is none)
      mapped.clear();
      for (const char c : piece) mapped += symbols[static_cast<unsigned char>(c)];
      const auto whole = index->ids.find(mapped);
      if (whole != index->ids.end() && whole->second < pack.token_types.size() && !is_special_type(pack.token_types[whole->second])) {
        if (output.size() >= kMaximumVocabularyEntries) {
          return error(TokenizerPackErrorCode::input_text_too_large, "encoded token count exceeds fixed limit");
        }
        output.push_back(whole->second);
        continue;
      }
    }
    if (const auto failure = raon_encode_piece(pack, *index, symbols, piece, output)) return *failure;
  }
  return output;
}

}  // namespace

TokenizerEncodeResult encode_text(const TokenizerPack& pack, const std::string_view text) { return encode_ordinary(pack, text, true); }

TokenizerEncodeResult encode_plain_text(const TokenizerPack& pack, const std::string_view text) { return encode_ordinary(pack, text, false); }

namespace {

// A literal that is exactly one CONTROL or USER_DEFINED token of the pack: its id; nullopt otherwise.
std::optional<std::size_t> unique_special_token(const TokenizerPack& pack, const std::string_view literal) {
  std::optional<std::size_t> found;
  for (std::size_t id = 0U; id < pack.tokens_by_id.size(); ++id) {
    if (pack.tokens_by_id[id] != literal) continue;
    if (found.has_value() || id >= pack.token_types.size() || !is_special_type(pack.token_types[id])) return std::nullopt;
    found = id;
  }
  return found;
}

}  // namespace

TokenizerEncodeResult encode_prompt_pieces(const TokenizerPack& pack, const std::span<const PromptPiece> pieces) {
  if (pack.tokens_by_id.size() != pack.token_types.size()) {
    return error(TokenizerPackErrorCode::invalid_token_types, "token type entries must match vocabulary entries");
  }
  std::map<std::string, std::size_t, std::less<>> markers;
  std::vector<std::size_t> output;
  std::string span;
  const auto flush = [&]() -> std::optional<TokenizerPackError> {
    if (span.empty()) return std::nullopt;
    const auto ordinary = encode_plain_text(pack, span);
    if (const auto* failure = std::get_if<TokenizerPackError>(&ordinary)) return *failure;
    const auto& ids = std::get<std::vector<std::size_t>>(ordinary);
    if (output.size() > kMaximumVocabularyEntries - ids.size()) {
      return error(TokenizerPackErrorCode::input_text_too_large, "encoded token count exceeds fixed limit");
    }
    output.insert(output.end(), ids.begin(), ids.end());
    span.clear();
    return std::nullopt;
  };
  for (const PromptPiece& piece : pieces) {
    if (piece.kind == PromptPiece::Kind::marker) {
      if (const auto failure = flush()) return *failure;
      auto known = markers.find(piece.text);
      if (known == markers.end()) {
        const auto id = unique_special_token(pack, piece.text);
        if (!id.has_value()) {
          return error(TokenizerPackErrorCode::unsupported_special_token,
                       "a prompt marker must be exactly one CONTROL or USER_DEFINED token");
        }
        known = markers.emplace(piece.text, *id).first;
      }
      if (output.size() >= kMaximumVocabularyEntries) {
        return error(TokenizerPackErrorCode::input_text_too_large, "encoded token count exceeds fixed limit");
      }
      output.push_back(known->second);
      continue;
    }
    // A text piece keeps encode_text's rule; a plain piece's literals are text. Both join the span around them.
    if (piece.kind == PromptPiece::Kind::text) {
      if (const auto refused = refuse_special_literals(pack, piece.text)) return *refused;
    }
    if (piece.text.size() > kMaximumDecodedBytes - span.size()) {
      return error(TokenizerPackErrorCode::input_text_too_large, "input text exceeds fixed limit");
    }
    span += piece.text;
  }
  if (const auto failure = flush()) return *failure;
  return output;
}

std::optional<std::size_t> find_special_token(const TokenizerPack& pack, const std::string_view literal) {
  return unique_special_token(pack, literal);
}

std::optional<QwenToolTokens> find_qwen_tool_tokens(const TokenizerPack& pack) {
  const auto call_open = unique_special_token(pack, "<tool_call>");
  const auto call_close = unique_special_token(pack, "</tool_call>");
  const auto response_open = unique_special_token(pack, "<tool_response>");
  const auto response_close = unique_special_token(pack, "</tool_response>");
  if (!call_open || !call_close || !response_open || !response_close) return std::nullopt;
  return QwenToolTokens{*call_open, *call_close, *response_open, *response_close};
}

std::optional<std::vector<std::pair<std::size_t, std::string>>> find_glm53_tool_tokens(const TokenizerPack& pack) {
  std::vector<std::pair<std::size_t, std::string>> tags;
  for (const std::string_view literal : {"<tool_call>", "</tool_call>", "<arg_key>", "</arg_key>", "<arg_value>", "</arg_value>",
                                         "<tool_response>", "</tool_response>"}) {
    const auto id = unique_special_token(pack, literal);
    if (!id.has_value()) return std::nullopt;
    tags.emplace_back(*id, std::string{literal});
  }
  return tags;
}

std::string raon_nfc(const std::string_view utf8) {
  std::string out;
  out.reserve(utf8.size());
  for (const std::uint32_t cpt : raon_nfc_points(raon_code_points(utf8))) raon_append_utf8(out, cpt);
  return out;
}

std::vector<std::string> raon_pretokenize(const std::string_view utf8) {
  const auto cpts = raon_code_points(utf8);
  std::vector<std::string> pieces;
  std::size_t at = 0U;
  for (const std::size_t length : raon_split(cpts)) {
    std::string piece;
    for (std::size_t i = at; i < at + length; ++i) raon_append_utf8(piece, cpts[i]);
    pieces.push_back(std::move(piece));
    at += length;
  }
  return pieces;
}

std::vector<std::string> flint_pretokenize(const std::string_view utf8) {
  const auto cpts = raon_code_points(utf8);
  std::vector<std::string> pieces;
  std::size_t at = 0U;
  for (const std::size_t length : flint_split(cpts)) {
    std::string piece;
    for (std::size_t i = at; i < at + length; ++i) raon_append_utf8(piece, cpts[i]);
    pieces.push_back(std::move(piece));
    at += length;
  }
  return pieces;
}

std::vector<std::string> deepseek_pretokenize(const std::string_view utf8) {
  const auto cpts = raon_code_points(utf8);
  std::vector<std::string> pieces;
  std::size_t at = 0U;
  for (const std::size_t length : deepseek_split(cpts)) {
    std::string piece;
    for (std::size_t i = at; i < at + length; ++i) raon_append_utf8(piece, cpts[i]);
    pieces.push_back(std::move(piece));
    at += length;
  }
  return pieces;
}

std::vector<std::string> minimax_pretokenize(const std::string_view utf8) {
  const auto cpts = raon_nfc_points(raon_code_points(utf8));
  std::vector<std::string> pieces;
  std::size_t at = 0U;
  for (const std::size_t length : minimax_split(cpts)) {
    std::string piece;
    for (std::size_t i = at; i < at + length; ++i) raon_append_utf8(piece, cpts[i]);
    pieces.push_back(std::move(piece));
    at += length;
  }
  return pieces;
}

namespace {

std::vector<std::string> pieces_of(const std::vector<std::uint32_t>& cpts, const std::vector<std::size_t>& lengths) {
  std::vector<std::string> pieces;
  std::size_t at = 0U;
  for (const std::size_t length : lengths) {
    std::string piece;
    for (std::size_t i = at; i < at + length; ++i) raon_append_utf8(piece, cpts[i]);
    pieces.push_back(std::move(piece));
    at += length;
  }
  return pieces;
}

}  // namespace

std::vector<std::string> qwen35_pretokenize(const std::string_view utf8) {
  const auto cpts = raon_code_points(utf8);
  return pieces_of(cpts, qwen_shape_split(cpts, false));
}

std::vector<std::string> glm_pretokenize(const std::string_view utf8) {
  const auto cpts = raon_code_points(utf8);
  return pieces_of(cpts, qwen_shape_split(cpts, true));
}

std::optional<ThinkTokens> find_qwen_think_tokens(const TokenizerPack& pack) {
  // Exactly one entry, typed CONTROL or USER_DEFINED; anything else is no reasoning split.
  const auto find = [&pack](const std::string_view literal) -> std::optional<std::size_t> {
    std::optional<std::size_t> found;
    for (std::size_t id = 0U; id < pack.tokens_by_id.size(); ++id) {
      if (pack.tokens_by_id[id] != literal) continue;
      if (found.has_value() || id >= pack.token_types.size() || (pack.token_types[id] != 3 && pack.token_types[id] != 4)) {
        return std::nullopt;
      }
      found = id;
    }
    return found;
  };
  const auto open = find("<think>");
  const auto close = find("</think>");
  if (!open.has_value() || !close.has_value()) return std::nullopt;
  return ThinkTokens{*open, *close};
}

namespace {

// The literals in order, each kept only as exactly one CONTROL (3) token (or USER_DEFINED (4) too, with user_defined).
std::vector<std::size_t> end_of_turn_tokens(const TokenizerPack& pack, const std::initializer_list<std::string_view> literals,
                                            const bool user_defined = false) {
  std::vector<std::size_t> found;
  for (const std::string_view literal : literals) {
    std::optional<std::size_t> id_of;
    bool unique = true;
    for (std::size_t id = 0U; id < pack.tokens_by_id.size(); ++id) {
      if (pack.tokens_by_id[id] != literal) continue;
      if (id_of.has_value()) unique = false;
      id_of = id;
    }
    if (id_of.has_value() && unique && *id_of < pack.token_types.size() &&
        (pack.token_types[*id_of] == 3 || (user_defined && pack.token_types[*id_of] == 4))) {
      found.push_back(*id_of);
    }
  }
  return found;
}

}  // namespace

std::vector<std::size_t> find_qwen_end_of_turn_tokens(const TokenizerPack& pack) {
  return end_of_turn_tokens(pack, {"<|im_end|>", "<|endoftext|>"});
}

std::vector<std::size_t> find_glm53_end_of_turn_tokens(const TokenizerPack& pack) {
  return end_of_turn_tokens(pack, {"<|endoftext|>", "<|user|>", "<|observation|>"});
}

std::vector<std::size_t> find_deepseek_end_of_turn_tokens(const TokenizerPack& pack) {
  return end_of_turn_tokens(pack, {kDeepseekMarkers[6], kDeepseekMarkers[2]}, true);
}

TokenizerEncodeResult encode_qwen_chatml_prompt(const TokenizerPack& pack,
                                                const std::string_view rendered_prompt) {
  constexpr std::string_view kStartMarker = "<|im_start|>";
  constexpr std::string_view kEndMarker = "<|im_end|>";
  if (pack.tokens_by_id.size() != pack.token_types.size()) {
    return error(TokenizerPackErrorCode::invalid_token_types,
                 "token type entries must match vocabulary entries");
  }

  const auto find_marker = [&pack](const std::string_view marker)
      -> std::variant<std::size_t, TokenizerPackError> {
    std::optional<std::size_t> marker_id;
    for (std::size_t id = 0U; id < pack.tokens_by_id.size(); ++id) {
      if (pack.tokens_by_id[id] != marker) continue;
      if (pack.token_types[id] != 3 || marker_id.has_value()) {
        return error(TokenizerPackErrorCode::unsupported_special_token,
                     "Qwen framing marker must be exactly one CONTROL token");
      }
      marker_id = id;
    }
    if (!marker_id.has_value()) {
      return error(TokenizerPackErrorCode::unsupported_special_token,
                   "Qwen framing marker is unavailable as a CONTROL token");
    }
    return *marker_id;
  };

  const auto start_id = find_marker(kStartMarker);
  if (const auto* failure = std::get_if<TokenizerPackError>(&start_id)) return *failure;
  const auto end_id = find_marker(kEndMarker);
  if (const auto* failure = std::get_if<TokenizerPackError>(&end_id)) return *failure;

  // Issue 19: the Qwen3 reasoning tail (render_qwen_text_prompt with QwenThinking::on or
  // off) is accepted only as the exact text after the final generation header. Its
  // markers are never scanned for anywhere else, so every other span keeps
  // encode_text()'s rejection of special literals.
  constexpr std::string_view kGenerationHeader = "<|im_start|>assistant\n";
  constexpr std::string_view kThinkOpenTail = "<think>\n";
  constexpr std::string_view kThinkClosedTail = "<think>\n\n</think>\n\n";
  std::string_view body = rendered_prompt;
  std::string_view think_tail;
  if (const auto header_at = rendered_prompt.rfind(kGenerationHeader); header_at != std::string_view::npos) {
    const std::size_t tail_at = header_at + kGenerationHeader.size();
    const std::string_view tail = rendered_prompt.substr(tail_at);
    if (tail == kThinkOpenTail || tail == kThinkClosedTail) {
      think_tail = tail;
      body = rendered_prompt.substr(0U, tail_at);
    }
  }

  // SP5 M1 Task 12 (Ruling T12-f): the three Qwen vision markers join the scan ONLY
  // when the rendered prompt actually carries them (a text prompt, and every pack
  // predating images, behave exactly as before). Like the framing markers, each must
  // resolve to exactly one CONTROL token.
  std::vector<std::pair<std::string_view, std::size_t>> markers;
  markers.emplace_back(kStartMarker, std::get<std::size_t>(start_id));
  markers.emplace_back(kEndMarker, std::get<std::size_t>(end_id));
  for (const std::string_view vision_marker : {"<|vision_start|>", "<|image_pad|>", "<|vision_end|>"}) {
    if (body.find(vision_marker) == std::string_view::npos) continue;
    const auto vision_id = find_marker(vision_marker);
    if (const auto* failure = std::get_if<TokenizerPackError>(&vision_id)) return *failure;
    markers.emplace_back(vision_marker, std::get<std::size_t>(vision_id));
  }

  std::vector<std::size_t> output;
  std::size_t offset = 0U;
  while (offset < body.size()) {
    std::size_t marker_at = std::string_view::npos;
    std::size_t marker_index = 0U;
    for (std::size_t candidate = 0U; candidate < markers.size(); ++candidate) {
      const auto at = body.find(markers[candidate].first, offset);
      if (at < marker_at) {
        marker_at = at;
        marker_index = candidate;
      }
    }
    if (marker_at == std::string_view::npos) {
      const auto ordinary = encode_text(pack, body.substr(offset));
      if (const auto* failure = std::get_if<TokenizerPackError>(&ordinary)) return *failure;
      const auto& ids = std::get<std::vector<std::size_t>>(ordinary);
      if (output.size() > kMaximumVocabularyEntries - ids.size()) {
        return error(TokenizerPackErrorCode::input_text_too_large,
                     "encoded token count exceeds fixed limit");
      }
      output.insert(output.end(), ids.begin(), ids.end());
      break;
    }

    const auto ordinary = encode_text(pack, body.substr(offset, marker_at - offset));
    if (const auto* failure = std::get_if<TokenizerPackError>(&ordinary)) return *failure;
    const auto& ids = std::get<std::vector<std::size_t>>(ordinary);
    if (ids.size() >= kMaximumVocabularyEntries ||
        output.size() > kMaximumVocabularyEntries - ids.size() - 1U) {
      return error(TokenizerPackErrorCode::input_text_too_large,
                   "encoded token count exceeds fixed limit");
    }
    output.insert(output.end(), ids.begin(), ids.end());
    output.push_back(markers[marker_index].second);
    offset = marker_at + markers[marker_index].first.size();
  }

  if (think_tail.empty()) return output;
  const auto think = find_qwen_think_tokens(pack);
  if (!think.has_value()) {
    return error(TokenizerPackErrorCode::unsupported_special_token,
                 "Qwen reasoning markers must be exactly one CONTROL or USER_DEFINED token each");
  }
  // The template's spelling: <think> "\n" for the open block, <think> "\n\n" </think> "\n\n" closed.
  const auto append_text = [&pack, &output](const std::string_view text) -> std::optional<TokenizerPackError> {
    const auto ordinary = encode_text(pack, text);
    if (const auto* failure = std::get_if<TokenizerPackError>(&ordinary)) return *failure;
    const auto& ids = std::get<std::vector<std::size_t>>(ordinary);
    if (output.size() > kMaximumVocabularyEntries - ids.size() - 1U) {
      return error(TokenizerPackErrorCode::input_text_too_large, "encoded token count exceeds fixed limit");
    }
    output.insert(output.end(), ids.begin(), ids.end());
    return std::nullopt;
  };
  if (output.size() >= kMaximumVocabularyEntries - 2U) {
    return error(TokenizerPackErrorCode::input_text_too_large, "encoded token count exceeds fixed limit");
  }
  output.push_back(think->open);
  if (think_tail == kThinkOpenTail) {
    if (const auto failure = append_text("\n")) return *failure;
    return output;
  }
  if (const auto failure = append_text("\n\n")) return *failure;
  output.push_back(think->close);
  if (const auto failure = append_text("\n\n")) return *failure;
  return output;
}

namespace {

// A prompt rendered by one of the fixed Maya-family renderers (render_glm53_text_prompt, render_deepseek_text_prompt):
// each marker literal must be exactly one CONTROL (3) or USER_DEFINED (4) token of the pack (Maya's server matches both
// kinds as special tokens, strata_tokenizer.py; GLM-5.3-Flash's own tokenizer has <think> USER_DEFINED), and every span
// between them is ordinary text through encode_text, which still refuses any special literal.
TokenizerEncodeResult encode_with_markers(const TokenizerPack& pack, const std::string_view rendered_prompt,
                                          const std::span<const std::string_view> markers, const std::string_view family) {
  if (pack.tokens_by_id.size() != pack.token_types.size()) {
    return error(TokenizerPackErrorCode::invalid_token_types,
                 "token type entries must match vocabulary entries");
  }

  std::vector<std::size_t> marker_ids(markers.size());
  for (std::size_t marker_index = 0U; marker_index < markers.size(); ++marker_index) {
    std::optional<std::size_t> marker_id;
    for (std::size_t id = 0U; id < pack.tokens_by_id.size(); ++id) {
      if (pack.tokens_by_id[id] != markers[marker_index]) continue;
      if ((pack.token_types[id] != 3 && pack.token_types[id] != 4) || marker_id.has_value()) {
        return error(TokenizerPackErrorCode::unsupported_special_token,
                     std::string{family} + " framing markers must be exactly one CONTROL or USER_DEFINED token");
      }
      marker_id = id;
    }
    if (!marker_id.has_value()) {
      return error(TokenizerPackErrorCode::unsupported_special_token,
                   std::string{family} + " framing marker is unavailable as a CONTROL or USER_DEFINED token");
    }
    marker_ids[marker_index] = *marker_id;
  }

  std::vector<std::size_t> output;
  std::size_t offset = 0U;
  while (offset < rendered_prompt.size()) {
    std::size_t marker_at = std::string_view::npos;
    std::size_t marker_index = 0U;
    for (std::size_t index = 0U; index < markers.size(); ++index) {
      const std::size_t found = rendered_prompt.find(markers[index], offset);
      if (found != std::string_view::npos &&
          (marker_at == std::string_view::npos || found < marker_at)) {
        marker_at = found;
        marker_index = index;
      }
    }
    if (marker_at == std::string_view::npos) {
      const auto ordinary = encode_text(pack, rendered_prompt.substr(offset));
      if (const auto* failure = std::get_if<TokenizerPackError>(&ordinary)) return *failure;
      const auto& ids = std::get<std::vector<std::size_t>>(ordinary);
      if (output.size() > kMaximumVocabularyEntries - ids.size()) {
        return error(TokenizerPackErrorCode::input_text_too_large,
                     "encoded token count exceeds fixed limit");
      }
      output.insert(output.end(), ids.begin(), ids.end());
      break;
    }
    const auto ordinary = encode_text(pack, rendered_prompt.substr(offset, marker_at - offset));
    if (const auto* failure = std::get_if<TokenizerPackError>(&ordinary)) return *failure;
    const auto& ids = std::get<std::vector<std::size_t>>(ordinary);
    if (ids.size() >= kMaximumVocabularyEntries ||
        output.size() > kMaximumVocabularyEntries - ids.size() - 1U) {
      return error(TokenizerPackErrorCode::input_text_too_large,
                   "encoded token count exceeds fixed limit");
    }
    output.insert(output.end(), ids.begin(), ids.end());
    output.push_back(marker_ids[marker_index]);
    offset = marker_at + markers[marker_index].size();
  }
  return output;
}

}  // namespace

TokenizerEncodeResult encode_glm53_prompt(const TokenizerPack& pack, const std::string_view rendered_prompt) {
  constexpr std::array<std::string_view, 6U> kMarkers = {
      "[gMASK]", "<sop>", "<|system|>", "<|user|>", "<|assistant|>", "<think>"};
  return encode_with_markers(pack, rendered_prompt, kMarkers, "GLM");
}

TokenizerEncodeResult encode_deepseek_prompt(const TokenizerPack& pack, const std::string_view rendered_prompt) {
  return encode_with_markers(pack, rendered_prompt, kDeepseekMarkers, "DeepSeek");
}

TokenizerEncodeResult encode_raon1_chatml_prompt(const TokenizerPack& pack,
                                                 const std::string_view rendered_prompt) {
  // raon-1-mini's ChatML framing markers (render_raon1_text_prompt). The empty think block
  // in the generation tail is two adjacent markers, so a zero-length ordinary span is normal.
  constexpr std::array<std::string_view, 4U> kMarkers = {
      "<|im_start|>", "<|im_end|>", "<think>", "</think>"};
  if (pack.tokens_by_id.size() != pack.token_types.size()) {
    return error(TokenizerPackErrorCode::invalid_token_types,
                 "token type entries must match vocabulary entries");
  }

  std::array<std::size_t, kMarkers.size()> marker_ids{};
  for (std::size_t marker_index = 0U; marker_index < kMarkers.size(); ++marker_index) {
    std::optional<std::size_t> marker_id;
    for (std::size_t id = 0U; id < pack.tokens_by_id.size(); ++id) {
      if (pack.tokens_by_id[id] != kMarkers[marker_index]) continue;
      if (pack.token_types[id] != 3 || marker_id.has_value()) {
        return error(TokenizerPackErrorCode::unsupported_special_token,
                     "raon1 framing markers must be exactly one CONTROL token");
      }
      marker_id = id;
    }
    if (!marker_id.has_value()) {
      return error(TokenizerPackErrorCode::unsupported_special_token,
                   "raon1 framing marker is unavailable as a CONTROL token");
    }
    marker_ids[marker_index] = *marker_id;
  }

  std::vector<std::size_t> output;
  std::size_t offset = 0U;
  while (offset < rendered_prompt.size()) {
    std::size_t marker_at = std::string_view::npos;
    std::size_t marker_index = 0U;
    for (std::size_t index = 0U; index < kMarkers.size(); ++index) {
      const std::size_t found = rendered_prompt.find(kMarkers[index], offset);
      if (found != std::string_view::npos &&
          (marker_at == std::string_view::npos || found < marker_at)) {
        marker_at = found;
        marker_index = index;
      }
    }
    if (marker_at == std::string_view::npos) {
      const auto ordinary = encode_text(pack, rendered_prompt.substr(offset));
      if (const auto* failure = std::get_if<TokenizerPackError>(&ordinary)) return *failure;
      const auto& ids = std::get<std::vector<std::size_t>>(ordinary);
      if (output.size() > kMaximumVocabularyEntries - ids.size()) {
        return error(TokenizerPackErrorCode::input_text_too_large,
                     "encoded token count exceeds fixed limit");
      }
      output.insert(output.end(), ids.begin(), ids.end());
      break;
    }
    const auto ordinary = encode_text(pack, rendered_prompt.substr(offset, marker_at - offset));
    if (const auto* failure = std::get_if<TokenizerPackError>(&ordinary)) return *failure;
    const auto& ids = std::get<std::vector<std::size_t>>(ordinary);
    if (ids.size() >= kMaximumVocabularyEntries ||
        output.size() > kMaximumVocabularyEntries - ids.size() - 1U) {
      return error(TokenizerPackErrorCode::input_text_too_large,
                   "encoded token count exceeds fixed limit");
    }
    output.insert(output.end(), ids.begin(), ids.end());
    output.push_back(marker_ids[marker_index]);
    offset = marker_at + kMarkers[marker_index].size();
  }
  return output;
}


namespace {

constexpr int kTokenTypeControl = 3;
constexpr int kTokenTypeUserDefined = 4;

// Gemma 4 plan 1 Task 8: the id of `literal` when it occurs exactly once, typed `type` (or `also`, when given).
std::optional<std::size_t> unique_token_of_type(const TokenizerPack& pack, const std::string_view literal, const int type,
                                                const std::optional<int> also = std::nullopt) {
  std::optional<std::size_t> found;
  for (std::size_t id = 0U; id < pack.tokens_by_id.size(); ++id) {
    if (pack.tokens_by_id[id] != literal) continue;
    if (found.has_value() || id >= pack.token_types.size() || (pack.token_types[id] != type && pack.token_types[id] != also)) {
      return std::nullopt;
    }
    found = id;
  }
  return found;
}

}  // namespace

std::optional<Gemma4Markers> find_gemma4_markers(const TokenizerPack& pack) {
  if (pack.mode != TokenizerMode::spm_bpe || pack.tokens_by_id.size() != pack.token_types.size()) return std::nullopt;
  const auto bos = unique_token_of_type(pack, "<bos>", kTokenTypeControl);
  // unsloth's later Gemma 4 GGUFs (gemma-4-26B-A4B-it-Q8_0 at c099eb48; owner 2026-10-28) type <eos> NORMAL and name
  // <turn|> the end of a turn (eos_token_id 106): <eos>, still exactly once, is the marker either way
  const auto eos = unique_token_of_type(pack, "<eos>", kTokenTypeControl, kTokenTypeNormal);
  const auto turn_open = unique_token_of_type(pack, "<|turn>", kTokenTypeControl);
  const auto turn_close = unique_token_of_type(pack, "<turn|>", kTokenTypeControl);
  const auto think = unique_token_of_type(pack, "<|think|>", kTokenTypeControl);
  const auto channel_open = unique_token_of_type(pack, "<|channel>", kTokenTypeUserDefined);
  const auto channel_close = unique_token_of_type(pack, "<channel|>", kTokenTypeUserDefined);
  if (!bos || !eos || !turn_open || !turn_close || !think || !channel_open || !channel_close) return std::nullopt;
  return Gemma4Markers{*bos, *eos, *turn_open, *turn_close, *think, *channel_open, *channel_close};
}

std::vector<std::size_t> find_gemma4_end_of_turn_tokens(const TokenizerPack& pack) {
  std::vector<std::size_t> found;
  if (pack.mode != TokenizerMode::spm_bpe) return found;
  if (const auto id = unique_token_of_type(pack, "<turn|>", kTokenTypeControl)) found.push_back(*id);
  if (const auto id = unique_token_of_type(pack, "<eos>", kTokenTypeControl, kTokenTypeNormal)) found.push_back(*id);   // as the markers
  return found;
}

TokenizerEncodeResult encode_gemma4_prompt(const TokenizerPack& pack, const std::string_view prompt) {
  constexpr std::string_view kBos = "<bos>";
  constexpr std::string_view kTurnOpen = "<|turn>";
  constexpr std::string_view kTurnClose = "<turn|>";
  constexpr std::string_view kThinkingSystemHead = "<|turn>system\n<|think|>";
  constexpr std::string_view kGenerationHeader = "<|turn>model\n";
  constexpr std::string_view kClosedChannelTail = "<|turn>model\n<|channel>thought\n<channel|>";
  if (pack.mode != TokenizerMode::spm_bpe) {
    return error(TokenizerPackErrorCode::unsupported_special_token, "a Gemma 4 prompt needs an spm_bpe tokenizer pack");
  }
  const auto markers = find_gemma4_markers(pack);
  if (!markers.has_value()) {
    return error(TokenizerPackErrorCode::unsupported_special_token,
                 "Gemma 4 markers must be exactly one token each, of the expected type");
  }
  std::optional<SpmIndex> local;
  const SpmIndex* prepared = prepared_spm_index(pack);
  if (prepared == nullptr) {
    auto built = build_spm_index(pack);
    if (const auto* failure = std::get_if<TokenizerPackError>(&built)) return *failure;
    local.emplace(std::move(std::get<SpmIndex>(built)));
    prepared = &*local;
  }
  const SpmIndex& index = *prepared;
  // The vocabulary's CONTROL and USER_DEFINED literals, collected once for every span.
  std::vector<std::string_view> specials;
  for (std::size_t id = 0U; id < pack.tokens_by_id.size(); ++id) {
    if (is_special_type(pack.token_types[id])) specials.emplace_back(pack.tokens_by_id[id]);
  }

  std::vector<std::size_t> output;
  const auto marker = [&output](const std::size_t id) -> std::optional<TokenizerPackError> {
    if (output.size() >= kMaximumVocabularyEntries) {
      return error(TokenizerPackErrorCode::input_text_too_large, "encoded token count exceeds fixed limit");
    }
    output.push_back(id);
    return std::nullopt;
  };
  // Ordinary text: encode_text's rules (bounded, UTF-8, no special literal), one shared index.
  const auto text = [&](const std::string_view span) -> std::optional<TokenizerPackError> {
    if (span.size() > kMaximumDecodedBytes) return error(TokenizerPackErrorCode::input_text_too_large, "input text exceeds fixed limit");
    if (!valid_utf8(span)) return error(TokenizerPackErrorCode::input_text_not_utf8, "input text must be valid UTF-8");
    for (const std::string_view literal : specials) {
      if (span.find(literal) != std::string_view::npos) {
        return error(TokenizerPackErrorCode::unsupported_special_token, "special token literals are not accepted as ordinary input");
      }
    }
    return spm_encode_text(pack, index, span, output);
  };

  std::string_view rest = prompt;
  if (!rest.starts_with(kBos)) {
    return error(TokenizerPackErrorCode::unsupported_special_token, "a Gemma 4 prompt begins with exactly one <bos>");
  }
  if (const auto failure = marker(markers->bos)) return *failure;
  rest.remove_prefix(kBos.size());
  if (rest.starts_with(kThinkingSystemHead)) {  // <|think|> only first in the leading system turn
    if (const auto failure = marker(markers->turn_open)) return *failure;
    if (const auto failure = text("system\n")) return *failure;
    if (const auto failure = marker(markers->think)) return *failure;
    rest.remove_prefix(kThinkingSystemHead.size());
  }
  // The closed thought channel only as the whole tail after the final generation header.
  const bool closed_channel = rest.ends_with(kClosedChannelTail);
  if (closed_channel) rest.remove_suffix(kClosedChannelTail.size() - kGenerationHeader.size());

  while (!rest.empty()) {
    const std::size_t open_at = rest.find(kTurnOpen);
    const std::size_t close_at = rest.find(kTurnClose);
    const std::size_t at = std::min(open_at, close_at);
    if (const auto failure = text(rest.substr(0U, at))) return *failure;
    if (at == std::string_view::npos) break;
    const bool open = at == open_at;
    if (const auto failure = marker(open ? markers->turn_open : markers->turn_close)) return *failure;
    rest.remove_prefix(at + (open ? kTurnOpen.size() : kTurnClose.size()));
  }
  if (closed_channel) {
    if (const auto failure = marker(markers->channel_open)) return *failure;
    if (const auto failure = text("thought\n")) return *failure;
    if (const auto failure = marker(markers->channel_close)) return *failure;
  }
  return output;
}

std::optional<FlintMarkers> find_flint_markers(const TokenizerPack& pack) {
  if (pack.mode != TokenizerMode::flint_bpe || pack.tokens_by_id.size() != pack.token_types.size()) return std::nullopt;
  const auto system = unique_token_of_type(pack, "<|system|>", kTokenTypeControl);
  const auto user = unique_token_of_type(pack, "<|user|>", kTokenTypeControl);
  const auto assistant = unique_token_of_type(pack, "<|assistant|>", kTokenTypeControl);
  const auto think_open = unique_token_of_type(pack, "<think>", kTokenTypeUserDefined);
  const auto think_close = unique_token_of_type(pack, "</think>", kTokenTypeUserDefined);
  if (!system || !user || !assistant || !think_open || !think_close) return std::nullopt;
  return FlintMarkers{*system, *user, *assistant, *think_open, *think_close};
}

std::vector<std::size_t> find_flint_end_of_turn_tokens(const TokenizerPack& pack) {
  std::vector<std::size_t> found;
  if (pack.mode != TokenizerMode::flint_bpe) return found;
  for (const std::string_view literal : {"<|end_of_text|>", "<|user|>", "<|observation|>", "<|developer|>"}) {
    if (const auto id = unique_token_of_type(pack, literal, kTokenTypeControl)) found.push_back(*id);
  }
  return found;
}

TokenizerEncodeResult encode_flint_prompt(const TokenizerPack& pack, const std::string_view prompt) {
  const auto markers = find_flint_markers(pack);
  if (!markers.has_value()) {
    return error(TokenizerPackErrorCode::unsupported_special_token,
                 "a flint prompt needs a flint_bpe pack whose markers are exactly one token each, of the expected type");
  }
  // The thinking-off tail is two adjacent markers, so a zero-length ordinary span is normal.
  const std::array<std::pair<std::string_view, std::size_t>, 5U> framing{{{"<|system|>", markers->system},
                                                                          {"<|user|>", markers->user},
                                                                          {"<|assistant|>", markers->assistant},
                                                                          {"<think>", markers->think_open},
                                                                          {"</think>", markers->think_close}}};
  std::vector<std::size_t> output;
  const auto text = [&](const std::string_view span) -> std::optional<TokenizerPackError> {
    if (span.empty()) return std::nullopt;
    const auto ordinary = encode_text(pack, span);
    if (const auto* failure = std::get_if<TokenizerPackError>(&ordinary)) return *failure;
    const auto& ids = std::get<std::vector<std::size_t>>(ordinary);
    if (output.size() > kMaximumVocabularyEntries - ids.size()) {
      return error(TokenizerPackErrorCode::input_text_too_large, "encoded token count exceeds fixed limit");
    }
    output.insert(output.end(), ids.begin(), ids.end());
    return std::nullopt;
  };
  std::string_view rest = prompt;
  while (!rest.empty()) {
    std::size_t at = std::string_view::npos;
    std::size_t which = 0U;
    for (std::size_t m = 0U; m < framing.size(); ++m) {
      const std::size_t found = rest.find(framing[m].first);
      if (found < at) {
        at = found;
        which = m;
      }
    }
    if (const auto failure = text(rest.substr(0U, at))) return *failure;
    if (at == std::string_view::npos) break;
    if (output.size() >= kMaximumVocabularyEntries) {
      return error(TokenizerPackErrorCode::input_text_too_large, "encoded token count exceeds fixed limit");
    }
    output.push_back(framing[which].second);
    rest.remove_prefix(at + framing[which].first.size());
  }
  return output;
}

}  // namespace secure_frontend
