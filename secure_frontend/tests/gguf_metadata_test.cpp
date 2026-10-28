// GGUF metadata (docs/superpowers/specs/2026-10-27-gui-first-install-design.md §4.2): the bounded header reader the page's
// "add a model" uses (no tensor data is read), and the tokenizer read from a GGUF instead of a tokenizer pack.
#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/crt_environment.hpp"
#include "secure_frontend/gguf_metadata.hpp"
#include "secure_frontend/json.hpp"
#include "secure_frontend/tokenizer_pack.hpp"
#include "support/gguf_builder.hpp"
#include "support/test_support.hpp"
#include "support/test_tokenizer.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

using test_support::GgufBuilder;

void reads_typed_metadata_and_stops_at_the_header() {
  const std::string header = GgufBuilder(3U, 7U)
                                 .str("general.architecture", "flint")
                                 .u32("flint.context_length", 262144U)
                                 .i32("tokenizer.ggml.bos_token_id", 150000)
                                 .f32("general.sampling.top_p", 0.95F)
                                 .boolean("tokenizer.ggml.add_bos_token", false)
                                 .strings("tokenizer.ggml.tokens", {"a", "b", "ab"})
                                 .i32s("tokenizer.ggml.token_type", {1, 1, 3})
                                 .bytes();
  const auto parsed = secure_frontend::parse_gguf_metadata(header + std::string(64, '\x7f'));  // tensor infos follow
  const auto* md = std::get_if<secure_frontend::GgufMetadata>(&parsed);
  expect(md != nullptr, "a well-formed header parses");
  if (md == nullptr) return;
  expect(md->version == 3U && md->tensor_count == 7U && md->header_bytes == header.size(), "version, tensor count, header end");
  expect(md->string("general.architecture") != nullptr && *md->string("general.architecture") == "flint", "a string");
  expect(md->integer("flint.context_length") == 262144 && md->integer("tokenizer.ggml.bos_token_id") == 150000, "integers");
  const auto top_p = md->real("general.sampling.top_p");
  expect(top_p.has_value() && *top_p > 0.9499 && *top_p < 0.9501, "a float");
  expect(md->boolean("tokenizer.ggml.add_bos_token") == false, "a boolean");
  expect(md->strings("tokenizer.ggml.tokens") != nullptr && md->strings("tokenizer.ggml.tokens")->size() == 3U, "a string array");
  expect(md->integers("tokenizer.ggml.token_type") != nullptr && md->integers("tokenizer.ggml.token_type")->back() == 3, "an int array");
  expect(md->string("missing") == nullptr && !md->integer("missing").has_value(), "a missing key is absent");
}

void refuses_malformed_headers() {
  using secure_frontend::GgufErrorCode;
  const auto code = [](const std::string& bytes, const secure_frontend::GgufLimits& limits = {}) {
    const auto parsed = secure_frontend::parse_gguf_metadata(bytes, limits);
    const auto* failure = std::get_if<secure_frontend::GgufError>(&parsed);
    return failure == nullptr ? std::optional<GgufErrorCode>{} : std::optional<GgufErrorCode>{failure->code};
  };
  const std::string good = GgufBuilder().str("general.architecture", "x").bytes();
  std::string magic = good;
  magic[0] = 'X';
  expect(code(magic) == GgufErrorCode::not_gguf, "a wrong magic");
  expect(code(GgufBuilder(1U).bytes()) == GgufErrorCode::unsupported_version, "version 1");
  expect(code(good.substr(0, good.size() - 1)) == GgufErrorCode::truncated, "a cut header");
  expect(code(GgufBuilder().raw("k", 8U, GgufBuilder::pod(std::uint64_t{1} << 40)).bytes()) == GgufErrorCode::too_large,
         "a string longer than the limit");
  expect(code(GgufBuilder().raw("k", 9U, GgufBuilder::pod(std::uint32_t{5}) + GgufBuilder::pod(std::uint64_t{1} << 40)).bytes()) ==
             GgufErrorCode::too_large,
         "an array longer than the limit");
  expect(code(GgufBuilder().raw("k", 9U, GgufBuilder::pod(std::uint32_t{9}) + GgufBuilder::pod(std::uint64_t{1})).bytes()) ==
             GgufErrorCode::malformed,
         "an array of arrays");
  expect(code(GgufBuilder().raw("k", 13U, "").bytes()) == GgufErrorCode::malformed, "an unknown type");
  expect(code(GgufBuilder().str("k", "a").str("k", "b").bytes()) == GgufErrorCode::malformed, "a duplicate key");
  expect(code(GgufBuilder().str("k", "a").bytes(), secure_frontend::GgufLimits{.max_keys = 0U}) == GgufErrorCode::too_large,
         "more keys than the limit");
}

// The GPT-2 byte alphabet as tokens (one a byte) plus "he" and "ll" merged: a tokenizer a GGUF can carry.
GgufBuilder tiny_gpt2(const std::string& model = "gpt2", const std::string& pre = "flint") {
  std::vector<std::string> tokens;
  for (unsigned byte = 0; byte < 256U; ++byte) tokens.push_back(test_support::byte_symbol(static_cast<unsigned char>(byte)));
  tokens.insert(tokens.end(), {"he", "ll", "hell", "<|user|>"});
  std::vector<std::int32_t> types(tokens.size(), 1);
  types.back() = 3;
  return std::move(GgufBuilder()
                       .str("tokenizer.ggml.model", model)
                       .str("tokenizer.ggml.pre", pre)
                       .strings("tokenizer.ggml.tokens", tokens)
                       .i32s("tokenizer.ggml.token_type", types)
                       .strings("tokenizer.ggml.merges", {"h e", "l l", "he ll"}));
}

void tokenizer_comes_from_the_gguf() {
  const auto parsed = secure_frontend::parse_gguf_metadata(tiny_gpt2().bytes());
  const auto& md = std::get<secure_frontend::GgufMetadata>(parsed);
  const auto loaded = secure_frontend::load_tokenizer_from_gguf(md, secure_frontend::TokenizerMode::flint_bpe);
  const auto* pack = std::get_if<secure_frontend::TokenizerPack>(&loaded);
  expect(pack != nullptr, "a gpt2 GGUF tokenizer loads in flint_bpe mode");
  if (pack == nullptr) return;
  expect(pack->tokens_by_id.size() == 260U && pack->merges_by_rank.size() == 3U && pack->token_types.back() == 3,
         "its tokens, merges and types");
  const auto ids = secure_frontend::encode_text(*pack, "hello");
  const auto* encoded = std::get_if<std::vector<std::size_t>>(&ids);
  expect(encoded != nullptr && encoded->size() == 2U && pack->tokens_by_id[(*encoded)[0]] == "hell",
         "it encodes by its merges (hell + o)");
  // the mode's model must match: an spm (gemma4) mode refuses a gpt2 GGUF, a byte mode a gemma4 one
  expect(std::holds_alternative<secure_frontend::TokenizerPackError>(
             secure_frontend::load_tokenizer_from_gguf(md, secure_frontend::TokenizerMode::spm_bpe)),
         "a gpt2 tokenizer is not loaded as spm_bpe");
  const auto other = std::get<secure_frontend::GgufMetadata>(secure_frontend::parse_gguf_metadata(tiny_gpt2("gemma4").bytes()));
  expect(std::holds_alternative<secure_frontend::TokenizerPackError>(
             secure_frontend::load_tokenizer_from_gguf(other, secure_frontend::TokenizerMode::flint_bpe)),
         "a gemma4 tokenizer is not loaded as a byte mode");
  // a merge naming no token, and a type list of the wrong length, are refused as a pack's are
  auto bad_merge = tiny_gpt2();
  bad_merge.strings("tokenizer.ggml.merges", {"h e"});  // a second merges key: a duplicate
  expect(std::holds_alternative<secure_frontend::GgufError>(secure_frontend::parse_gguf_metadata(bad_merge.bytes())),
         "a duplicate key never reaches the tokenizer");
}

const secure_frontend::JsonValue* member(const secure_frontend::JsonValue& object, const std::string_view key) {
  const auto* members = object.if_object();
  if (members == nullptr) return nullptr;
  for (const auto& entry : *members) {
    if (entry.key == key) return &entry.value;
  }
  return nullptr;
}

// The owner's flint GGUF (SECURE_FRONTEND_FLINT_GGUF: the folder holding flint-Q8_0.gguf; never committed): read
// through the bounded reader, its tokenizer gives llama.cpp's ids for every fixture text, as the pack does.
void real_flint_gguf_tokenizer_matches_the_fixture() {
  const char* configured = secure_frontend::crt_getenv("SECURE_FRONTEND_FLINT_GGUF");
  if (configured == nullptr || *configured == '\0' || !std::filesystem::is_directory(std::filesystem::path{configured})) {
    std::cout << "SKIPPED: SECURE_FRONTEND_FLINT_GGUF names no folder holding flint-Q8_0.gguf\n";
    return;
  }
  // Read in place, whatever the folder's access (the owner's drive grants Authenticated Users write); the serving
  // path's trust checks on a model folder are tested with the launcher.
  auto root = secure_frontend::open_root(std::filesystem::path{configured}, secure_frontend::RootPolicy::external);
  const auto relative = secure_frontend::parse_relative_path("flint-Q8_0.gguf");
  expect(std::holds_alternative<secure_frontend::CanonicalRoot>(root), "the model folder opens");
  if (!std::holds_alternative<secure_frontend::CanonicalRoot>(root)) return;
  auto file = secure_frontend::open_beneath(std::get<secure_frontend::CanonicalRoot>(root), std::get<secure_frontend::RelativePath>(relative));
  expect(std::holds_alternative<secure_frontend::OpenedFile>(file), "the GGUF opens");
  if (!std::holds_alternative<secure_frontend::OpenedFile>(file)) return;
  const auto parsed = secure_frontend::read_gguf_metadata(std::get<secure_frontend::OpenedFile>(file));
  const auto* md = std::get_if<secure_frontend::GgufMetadata>(&parsed);
  expect(md != nullptr && md->string("general.architecture") != nullptr && *md->string("general.architecture") == "flint",
         "the header reads (arch flint)");
  if (md == nullptr) return;
  const auto loaded = secure_frontend::load_tokenizer_from_gguf(*md, secure_frontend::TokenizerMode::flint_bpe);
  const auto* pack = std::get_if<secure_frontend::TokenizerPack>(&loaded);
  expect(pack != nullptr && pack->tokens_by_id.size() == 150144U && pack->merges_by_rank.size() == 149744U,
         "its tokenizer: 150,144 tokens, 149,744 merges");
  if (pack == nullptr) return;
  const std::string text = secure_frontend_test::read_file(secure_frontend_test::extra_arguments().at(0));
  const auto fixture = secure_frontend::parse_json(
      text, secure_frontend::JsonLimits{.max_bytes = 1U << 20U, .max_depth = 5U, .max_container_entries = 65536U,
                                        .max_string_bytes = 1U << 16U});
  const auto* texts = member(std::get<secure_frontend::JsonValue>(fixture), "texts")->if_array();
  std::size_t checked = 0U;
  for (const auto& entry : *texts) {
    std::vector<std::size_t> expected;
    for (const auto& id : *member(entry, "ids")->if_array()) expected.push_back(static_cast<std::size_t>(*id.if_integer()));
    const auto encoded = secure_frontend::encode_text(*pack, *member(entry, "text")->if_string());
    const auto* actual = std::get_if<std::vector<std::size_t>>(&encoded);
    expect(actual != nullptr && *actual == expected, "llama.cpp's ids for " + *member(entry, "name")->if_string());
    ++checked;
  }
  expect(checked >= 38U, "at least 38 texts are compared");
}

// Owner 2026-10-27 ("Qwen 3.6 from the GGUF first"): the tokenizer read from the real Qwen3.6 GGUF is the one upstream's
// strata_tokenizer.py wrote from it (the pack the Python prepare step made): the same vocabulary, types and merges, and
// so the same ids for every text. SECURE_FRONTEND_QWEN36_GGUF names the GGUF file, SECURE_FRONTEND_QWEN36_PACK that
// tokenizer folder; unset, skipped.
void real_qwen36_gguf_tokenizer_matches_its_pack() {
  const char* gguf = secure_frontend::crt_getenv("SECURE_FRONTEND_QWEN36_GGUF");
  const char* pack_dir = secure_frontend::crt_getenv("SECURE_FRONTEND_QWEN36_PACK");
  if (gguf == nullptr || *gguf == '\0' || pack_dir == nullptr || *pack_dir == '\0' || !std::filesystem::is_regular_file(gguf) ||
      !std::filesystem::is_directory(pack_dir)) {
    std::cout << "SKIPPED: SECURE_FRONTEND_QWEN36_GGUF / SECURE_FRONTEND_QWEN36_PACK name no Qwen3.6 GGUF and its tokenizer folder\n";
    return;
  }
  // read in place (the owner's folders), as the flint case does
  const std::filesystem::path file{gguf};
  auto root = secure_frontend::open_root(file.parent_path(), secure_frontend::RootPolicy::external);
  expect(std::holds_alternative<secure_frontend::CanonicalRoot>(root), "the model folder opens");
  if (!std::holds_alternative<secure_frontend::CanonicalRoot>(root)) return;
  auto opened = secure_frontend::open_beneath(std::get<secure_frontend::CanonicalRoot>(root),
                                              std::get<secure_frontend::RelativePath>(secure_frontend::parse_relative_path(
                                                  secure_frontend::path_to_utf8(file.filename()))));
  expect(std::holds_alternative<secure_frontend::OpenedFile>(opened), "the GGUF opens");
  if (!std::holds_alternative<secure_frontend::OpenedFile>(opened)) return;
  const auto parsed = secure_frontend::read_gguf_metadata(std::get<secure_frontend::OpenedFile>(opened));
  const auto* md = std::get_if<secure_frontend::GgufMetadata>(&parsed);
  expect(md != nullptr && md->string("general.architecture") != nullptr && *md->string("general.architecture") == "qwen35moe",
         "the header reads (arch qwen35moe)");
  if (md == nullptr) return;
  const auto from_gguf = secure_frontend::load_tokenizer_from_gguf(*md, secure_frontend::TokenizerMode::byte_bpe);
  auto pack_root = secure_frontend::open_root(std::filesystem::path{pack_dir}, secure_frontend::RootPolicy::external);
  expect(std::holds_alternative<secure_frontend::CanonicalRoot>(pack_root), "the tokenizer folder opens");
  if (!std::holds_alternative<secure_frontend::CanonicalRoot>(pack_root)) return;
  const auto from_pack = secure_frontend::load_local_tokenizer_pack(std::get<secure_frontend::CanonicalRoot>(pack_root),
                                                                    secure_frontend::TokenizerMode::byte_bpe);
  const auto* a = std::get_if<secure_frontend::TokenizerPack>(&from_gguf);
  const auto* b = std::get_if<secure_frontend::TokenizerPack>(&from_pack);
  expect(a != nullptr && b != nullptr, "both load in Qwen's mode");
  if (a == nullptr || b == nullptr) return;
  expect(a->tokens_by_id.size() == 248320U, "248,320 tokens: " + std::to_string(a->tokens_by_id.size()));
  expect(a->tokens_by_id == b->tokens_by_id && a->token_types == b->token_types && a->merges_by_rank == b->merges_by_rank,
         "the GGUF's vocabulary, types and merges are the pack's");
  const std::string text = secure_frontend_test::read_file(secure_frontend_test::extra_arguments().at(0));
  const auto fixture = secure_frontend::parse_json(
      text, secure_frontend::JsonLimits{.max_bytes = 1U << 20U, .max_depth = 5U, .max_container_entries = 65536U,
                                        .max_string_bytes = 1U << 16U});
  std::size_t checked = 0U;
  for (const auto& entry : *member(std::get<secure_frontend::JsonValue>(fixture), "texts")->if_array()) {
    const std::string& sample = *member(entry, "text")->if_string();
    const auto x = secure_frontend::encode_text(*a, sample);
    const auto y = secure_frontend::encode_text(*b, sample);
    const auto* xi = std::get_if<std::vector<std::size_t>>(&x);
    const auto* yi = std::get_if<std::vector<std::size_t>>(&y);
    expect((xi != nullptr) == (yi != nullptr) && (xi == nullptr || *xi == *yi), "the same ids for " + *member(entry, "name")->if_string());
    ++checked;
  }
  expect(checked >= 38U, "at least 38 texts are compared");
}

// The search loop's doubled spaces (2026-10-27): the tokenizer read from the real Qwen3.6 GGUF encodes as upstream's own
// does (tests/fixtures/byte_bpe_reference_ids.json "qwen36": upstream v0.2.6 tools/strata_tokenizer.py from_gguf on the
// same file, from tools/generate_byte_bpe_reference_fixture.py), id for id - the model's reasoning and a search result's
// lines among the texts. SECURE_FRONTEND_QWEN36_GGUF names the GGUF; unset, skipped. Read in place, never written.
void real_qwen36_gguf_encodes_as_upstream() {
  const char* gguf = secure_frontend::crt_getenv("SECURE_FRONTEND_QWEN36_GGUF");
  if (gguf == nullptr || *gguf == '\0' || !std::filesystem::is_regular_file(gguf)) {
    std::cout << "SKIPPED: SECURE_FRONTEND_QWEN36_GGUF names no Qwen3.6 GGUF\n";
    return;
  }
  const std::filesystem::path file{gguf};
  auto root = secure_frontend::open_root(file.parent_path(), secure_frontend::RootPolicy::external);
  expect(std::holds_alternative<secure_frontend::CanonicalRoot>(root), "the model folder opens");
  if (!std::holds_alternative<secure_frontend::CanonicalRoot>(root)) return;
  auto opened = secure_frontend::open_beneath(std::get<secure_frontend::CanonicalRoot>(root),
                                              std::get<secure_frontend::RelativePath>(secure_frontend::parse_relative_path(
                                                  secure_frontend::path_to_utf8(file.filename()))));
  expect(std::holds_alternative<secure_frontend::OpenedFile>(opened), "the GGUF opens");
  if (!std::holds_alternative<secure_frontend::OpenedFile>(opened)) return;
  const auto parsed = secure_frontend::read_gguf_metadata(std::get<secure_frontend::OpenedFile>(opened));
  const auto* md = std::get_if<secure_frontend::GgufMetadata>(&parsed);
  expect(md != nullptr, "the header reads");
  if (md == nullptr) return;
  const auto loaded = secure_frontend::load_tokenizer_from_gguf(*md, secure_frontend::TokenizerMode::byte_bpe);
  const auto* pack = std::get_if<secure_frontend::TokenizerPack>(&loaded);
  expect(pack != nullptr, "it loads in Qwen's mode");
  if (pack == nullptr) return;
  const std::string text = secure_frontend_test::read_file(secure_frontend_test::extra_arguments().at(0));
  const auto fixture = secure_frontend::parse_json(
      text, secure_frontend::JsonLimits{.max_bytes = 1U << 20U, .max_depth = 5U, .max_container_entries = 65536U,
                                        .max_string_bytes = 1U << 16U});
  std::size_t checked = 0U;
  for (const auto& entry : *member(std::get<secure_frontend::JsonValue>(fixture), "texts")->if_array()) {
    std::vector<std::size_t> expected;
    for (const auto& id : *member(entry, "qwen36")->if_array()) expected.push_back(static_cast<std::size_t>(*id.if_integer()));
    const auto encoded = secure_frontend::encode_plain_text(*pack, *member(entry, "text")->if_string());
    const auto* ids = std::get_if<std::vector<std::size_t>>(&encoded);
    expect(ids != nullptr && *ids == expected, "upstream's ids for " + *member(entry, "name")->if_string());
    ++checked;
  }
  expect(checked >= 42U, "at least 42 texts are compared id for id");
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"reads_typed_metadata_and_stops_at_the_header", reads_typed_metadata_and_stops_at_the_header},
      {"refuses_malformed_headers", refuses_malformed_headers},
      {"tokenizer_comes_from_the_gguf", tokenizer_comes_from_the_gguf},
      {"real_flint_gguf_tokenizer_matches_the_fixture", real_flint_gguf_tokenizer_matches_the_fixture},
      {"real_qwen36_gguf_tokenizer_matches_its_pack", real_qwen36_gguf_tokenizer_matches_its_pack},
      {"real_qwen36_gguf_encodes_as_upstream", real_qwen36_gguf_encodes_as_upstream},
  }, failures);
}
