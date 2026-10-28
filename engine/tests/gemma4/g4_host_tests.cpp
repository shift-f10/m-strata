// Hermetic tests for strata-gemma4-engine's host code (no GPU): the GGUF reader, the model
// guard, the CLI, GEN parsing, the sampler and the ring arithmetic.
#include "g4_cli.hpp"
#include "g4_gguf.hpp"
#include "g4_model.hpp"
#include "g4_profile.hpp"
#include "g4_test.hpp"
#include "gguf_writer.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

using g4_test::expect;
using g4_test::GgufBuilder;

namespace {

std::variant<g4::GgufFile, g4::GgufError> parse(const std::vector<std::uint8_t>& image) {
  return g4::parse_gguf(image.data(), image.size(), image.size());
}

bool refused(const std::vector<std::uint8_t>& image, const std::string& why) {
  const auto r = parse(image);
  const bool ok = std::holds_alternative<g4::GgufError>(r);
  expect(ok, "refused: " + why);
  return ok;
}

GgufBuilder valid_builder() {
  GgufBuilder b;
  b.str("general.architecture", "gemma4");
  b.u32("gemma4.block_count", 30);
  b.f32("gemma4.rope.freq_base", 1e6f);
  b.boolean("tokenizer.ggml.add_bos_token", true);
  b.arr_i32("gemma4.attention.head_count_kv", {8, 8, 2});
  b.arr_bool("gemma4.attention.sliding_window_pattern", {true, false});
  b.arr_str("tokenizer.ggml.tokens", {"<pad>", "<eos>", "a b"});
  b.arr_f32("tokenizer.ggml.scores", {-1.0f, 0.5f, 2.0f});
  b.u64("general.big", 1ull << 40);
  b.tensor("output_norm.weight", {4, 2}, 0, 32);
  b.tensor("token_embd.weight", {64, 2}, 8, 2 * 2 * 34);
  return b;
}

void gguf_parses_a_valid_header() {
  const auto image = valid_builder().bytes();
  const auto r = parse(image);
  const auto* f = std::get_if<g4::GgufFile>(&r);
  expect(f != nullptr, "a valid image parses");
  if (f == nullptr) return;
  expect(f->version == 3, "version 3");
  const auto* arch = f->find("general.architecture");
  expect(arch != nullptr && arch->type == g4::GgufType::string && arch->text == "gemma4", "string value");
  const auto* blocks = f->find("gemma4.block_count");
  expect(blocks != nullptr && blocks->type == g4::GgufType::u32 && blocks->integer == 30, "u32 value");
  const auto* base = f->find("gemma4.rope.freq_base");
  expect(base != nullptr && base->real == 1e6, "f32 value");
  const auto* bos = f->find("tokenizer.ggml.add_bos_token");
  expect(bos != nullptr && bos->type == g4::GgufType::boolean && bos->integer == 1, "bool value");
  const auto* kv = f->find("gemma4.attention.head_count_kv");
  expect(kv != nullptr && kv->type == g4::GgufType::array && kv->element == g4::GgufType::i32 &&
             kv->integers == std::vector<std::int64_t>{8, 8, 2},
         "i32 array");
  const auto* swa = f->find("gemma4.attention.sliding_window_pattern");
  expect(swa != nullptr && swa->integers == std::vector<std::int64_t>{1, 0}, "bool array");
  const auto* tokens = f->find("tokenizer.ggml.tokens");
  expect(tokens != nullptr && tokens->texts == std::vector<std::string>{"<pad>", "<eos>", "a b"}, "string array");
  const auto* scores = f->find("tokenizer.ggml.scores");
  expect(scores != nullptr && scores->reals.size() == 3 && scores->reals[2] == 2.0, "f32 array");
  const auto* big = f->find("general.big");
  expect(big != nullptr && big->integer == (1ll << 40), "u64 value");
  expect(f->tensors.size() == 2, "two tensors");
  const auto* emb = f->tensor("token_embd.weight");
  expect(emb != nullptr && emb->type == g4::TensorType::q8_0 && emb->dims == std::vector<std::uint64_t>{64, 2} &&
             emb->bytes == 136 && emb->offset == 32,
         "the Q8_0 tensor's type, dims, bytes and offset");
  expect(f->data_offset % 32 == 0 && f->data_offset + 32 + 160 <= image.size(), "data section aligned and inside the file");
  expect(f->tensor("missing") == nullptr && f->find("missing") == nullptr, "lookups of absent names");
}

void gguf_refuses_bad_magic_version_and_truncation() {
  auto image = valid_builder().bytes();
  auto bad = image;
  bad[0] = 'X';
  refused(bad, "bad magic");
  bad = image;
  bad[4] = 2;
  refused(bad, "version 2");
  // Every prefix that cuts the header or a tensor's data short.
  const auto full = parse(image);
  const auto* f = std::get_if<g4::GgufFile>(&full);
  if (f == nullptr) return;
  std::uint64_t need = f->data_offset;
  for (const auto& t : f->tensors) need = std::max(need, f->data_offset + t.offset + t.bytes);
  for (std::size_t n = 0; n < need; n += (n < f->data_offset ? 1 : 16)) {
    std::vector<std::uint8_t> cut(image.begin(), image.begin() + static_cast<std::ptrdiff_t>(n));
    const auto r = g4::parse_gguf(cut.data(), cut.size(), cut.size());
    if (!std::holds_alternative<g4::GgufError>(r)) {
      expect(false, "a truncated image (" + std::to_string(n) + " bytes) is refused");
      break;
    }
  }
}

void gguf_refuses_oversized_counts_and_strings() {
  g4::GgufLimits small;
  small.max_metadata = 4;
  small.max_string_bytes = 8;
  small.max_array_elements = 2;
  small.max_key_bytes = 16;
  {
    GgufBuilder b;
    for (int i = 0; i < 5; ++i) b.u32("k" + std::to_string(i), 1);
    const auto image = b.bytes();
    expect(std::holds_alternative<g4::GgufError>(g4::parse_gguf(image.data(), image.size(), image.size(), small)),
           "more keys than max_metadata");
  }
  {
    GgufBuilder b;
    b.str("k", "123456789");
    const auto image = b.bytes();
    expect(std::holds_alternative<g4::GgufError>(g4::parse_gguf(image.data(), image.size(), image.size(), small)),
           "a string longer than max_string_bytes");
  }
  {
    GgufBuilder b;
    b.arr_i32("k", {1, 2, 3});
    const auto image = b.bytes();
    expect(std::holds_alternative<g4::GgufError>(g4::parse_gguf(image.data(), image.size(), image.size(), small)),
           "an array longer than max_array_elements");
  }
  {
    GgufBuilder b;
    b.u32("a-key-longer-than-16", 1);
    const auto image = b.bytes();
    expect(std::holds_alternative<g4::GgufError>(g4::parse_gguf(image.data(), image.size(), image.size(), small)),
           "a key longer than max_key_bytes");
  }
  {
    GgufBuilder b;
    b.u32("k", 1);
    b.bump_kv(1ull << 40);  // the header claims 2^40 + 1 keys
    refused(b.bytes(), "a key count beyond any cap");
  }
  {
    GgufBuilder b;
    b.u32("k", 1);
    b.u32("k", 2);
    refused(b.bytes(), "a duplicate key");
  }
  {
    GgufBuilder b;
    b.u32("k", 1);
    b.raw_meta()[9] = 13;  // the value type of key "k" (8-byte length + 1 byte key, then the type)
    refused(b.bytes(), "an unknown value type");
  }
  {
    GgufBuilder b;
    b.arr_str("k", {"a"});
    b.raw_meta()[8 + 1 + 4] = 9;  // array of arrays
    refused(b.bytes(), "a nested array");
  }
}

void gguf_refuses_bad_tensors() {
  {
    GgufBuilder b;
    b.tensor("t", {32, 2}, 2, 36);  // Q4_0: not a type this engine knows
    refused(b.bytes(), "an unknown tensor type");
  }
  {
    GgufBuilder b;
    b.tensor("t", {48, 2}, 8, 102);  // Q8_0 needs a row multiple of 32
    refused(b.bytes(), "a Q8_0 row that is not a multiple of 32");
  }
  {
    GgufBuilder b;
    b.tensor("t", {}, 0, 4);
    refused(b.bytes(), "zero dimensions");
  }
  {
    GgufBuilder b;
    b.tensor("t", {1, 1, 1, 1, 1}, 0, 4);
    refused(b.bytes(), "five dimensions");
  }
  {
    GgufBuilder b;
    b.tensor("t", {4, 0}, 0, 0);
    refused(b.bytes(), "a zero dimension");
  }
  {
    GgufBuilder b;
    b.tensor("t", {1ull << 32, 1ull << 32}, 0, 4);
    refused(b.bytes(), "an element count that overflows");
  }
  {
    GgufBuilder b;
    b.tensor("a", {8}, 0, 32);
    b.tensor("b", {8}, 0, 32);
    b.last_offset(16);  // not aligned to 32
    refused(b.bytes(), "a misaligned tensor offset");
  }
  {
    GgufBuilder b;
    b.tensor("a", {16}, 0, 64);
    b.tensor("b", {8}, 0, 32);
    b.last_offset(32);  // inside tensor a
    refused(b.bytes(), "overlapping tensors");
  }
  {
    GgufBuilder b;
    b.tensor("a", {8}, 0, 32);
    b.tensor("b", {8}, 0, 32);
    b.last_offset(4096);  // past the end of the file
    refused(b.bytes(), "a tensor past the end of the file");
  }
  {
    GgufBuilder b;
    b.tensor("a", {8}, 0, 32);
    b.tensor("a", {8}, 0, 32);
    refused(b.bytes(), "a duplicate tensor name");
  }
  {
    GgufBuilder b;
    b.u32("general.alignment", 24);
    b.tensor("a", {8}, 0, 32);
    refused(b.bytes(24), "an alignment that is not a power of two");
  }
  {
    GgufBuilder b;
    b.u32("general.alignment", 64);
    b.tensor("a", {8}, 0, 32);
    b.tensor("b", {8}, 0, 32);
    const auto image = b.bytes(64);
    const auto r = parse(image);
    const auto* f = std::get_if<g4::GgufFile>(&r);
    expect(f != nullptr && f->alignment == 64 && f->data_offset % 64 == 0 && f->tensors[1].offset == 64,
           "general.alignment 64 is honoured");
  }
}

void tensor_bytes_follow_the_types() {
  expect(g4::tensor_bytes(g4::TensorType::f32, 4, 8) == 32u, "F32");
  expect(g4::tensor_bytes(g4::TensorType::f16, 4, 8) == 16u, "F16");
  expect(g4::tensor_bytes(g4::TensorType::bf16, 4, 8) == 16u, "BF16");
  expect(g4::tensor_bytes(g4::TensorType::q8_0, 2816, 2816 * 4) == 88u * 34u * 4u, "Q8_0");
  expect(!g4::tensor_bytes(g4::TensorType::q8_0, 48, 96).has_value(), "Q8_0 with a bad row");
}


// ---- the model guard (spec §4.1) on synthetic 26B-A4B headers (no tensor data) ----

struct HeaderOptions {
  std::function<void(GgufBuilder&)> extra_keys;
  std::set<std::string> drop_keys;     // keys left out
  std::set<std::string> drop_tensors;  // tensors left out
  std::map<std::string, std::vector<std::uint64_t>> dims;  // tensor name -> dims override
  std::map<std::string, std::uint32_t> types;              // tensor name -> type override
  std::vector<std::string> tokens;                         // vocabulary override (default: 262144 synthetic)
  std::vector<std::string> extra_tensors;                  // more F32 [4] tensors
  std::map<std::string, std::uint32_t> u32_overrides;
};

std::uint64_t span_bytes(std::uint32_t type, const std::vector<std::uint64_t>& dims) {
  std::uint64_t n = 1;
  for (auto d : dims) n *= d;
  const auto b = g4::tensor_bytes(static_cast<g4::TensorType>(type), dims[0], n);
  return b ? *b : n * 4;
}

std::variant<g4::GgufFile, g4::GgufError> gemma4_header(const HeaderOptions& o = {}) {
  GgufBuilder b;
  auto key_u32 = [&](const std::string& k, std::uint32_t v) {
    if (o.drop_keys.count(k)) return;
    const auto it = o.u32_overrides.find(k);
    b.u32(k, it == o.u32_overrides.end() ? v : it->second);
  };
  auto key_f32 = [&](const std::string& k, float v) {
    if (!o.drop_keys.count(k)) b.f32(k, v);
  };
  if (!o.drop_keys.count("general.architecture")) b.str("general.architecture", "gemma4");
  key_u32("gemma4.block_count", 30);
  key_u32("gemma4.context_length", 262144);
  key_u32("gemma4.embedding_length", 2816);
  key_u32("gemma4.feed_forward_length", 2112);
  key_u32("gemma4.attention.head_count", 16);
  std::vector<std::int32_t> kv;
  std::vector<bool> swa;
  for (int i = 0; i < 30; ++i) {
    const bool full = i % 6 == 5;
    kv.push_back(full ? 2 : 8);
    swa.push_back(!full);
  }
  if (!o.drop_keys.count("gemma4.attention.head_count_kv")) b.arr_i32("gemma4.attention.head_count_kv", kv);
  key_f32("gemma4.rope.freq_base", 1e6f);
  key_f32("gemma4.rope.freq_base_swa", 1e4f);
  key_f32("gemma4.attention.layer_norm_rms_epsilon", 1e-6f);
  key_u32("gemma4.expert_count", 128);
  key_u32("gemma4.expert_used_count", 8);
  key_u32("gemma4.attention.key_length", 512);
  key_u32("gemma4.attention.value_length", 512);
  key_f32("gemma4.final_logit_softcapping", 30.0f);
  key_u32("gemma4.attention.sliding_window", 1024);
  key_u32("gemma4.attention.shared_kv_layers", 0);
  key_u32("gemma4.embedding_length_per_layer_input", 0);
  if (!o.drop_keys.count("gemma4.attention.sliding_window_pattern")) b.arr_bool("gemma4.attention.sliding_window_pattern", swa);
  key_u32("gemma4.attention.key_length_swa", 256);
  key_u32("gemma4.attention.value_length_swa", 256);
  key_u32("gemma4.expert_feed_forward_length", 704);
  key_u32("gemma4.rope.dimension_count", 512);
  key_u32("gemma4.rope.dimension_count_swa", 256);
  if (!o.drop_keys.count("tokenizer.ggml.model")) b.str("tokenizer.ggml.model", "gemma4");
  std::vector<std::string> tokens = o.tokens;
  if (tokens.empty()) {
    tokens.resize(262144);
    for (std::size_t i = 0; i < tokens.size(); ++i) tokens[i] = "t" + std::to_string(i);
    tokens[0] = "<pad>";
    tokens[1] = "<eos>";
    tokens[2] = "<bos>";
    tokens[106] = "<turn|>";
  }
  if (!o.drop_keys.count("tokenizer.ggml.tokens")) b.arr_str("tokenizer.ggml.tokens", tokens);
  key_u32("tokenizer.ggml.bos_token_id", 2);
  key_u32("tokenizer.ggml.eos_token_id", 1);
  if (o.extra_keys) o.extra_keys(b);
  auto add = [&](const std::string& name, std::vector<std::uint64_t> dims, std::uint32_t type) {
    if (o.drop_tensors.count(name)) return;
    if (const auto it = o.dims.find(name); it != o.dims.end()) dims = it->second;
    if (const auto it = o.types.find(name); it != o.types.end()) type = it->second;
    b.tensor(name, dims, type, span_bytes(type, dims));
  };
  constexpr std::uint32_t F32 = 0, Q8 = 8, BF16 = 30;
  add("output_norm.weight", {2816}, F32);
  add("rope_freqs.weight", {256}, F32);
  add("token_embd.weight", {2816, 262144}, Q8);
  for (int i = 0; i < 30; ++i) {
    const bool full = i % 6 == 5;
    const std::uint64_t hd = full ? 512 : 256, nkv = full ? 2 : 8;
    const std::uint32_t M = i == 1 ? BF16 : Q8;
    const std::string p = "blk." + std::to_string(i) + ".";
    add(p + "attn_norm.weight", {2816}, F32);
    add(p + "attn_q.weight", {2816, 16 * hd}, M);
    add(p + "attn_k.weight", {2816, nkv * hd}, M);
    if (!full) add(p + "attn_v.weight", {2816, nkv * hd}, M);
    add(p + "attn_output.weight", {16 * hd, 2816}, M);
    add(p + "attn_q_norm.weight", {hd}, F32);
    add(p + "attn_k_norm.weight", {hd}, F32);
    add(p + "post_attention_norm.weight", {2816}, F32);
    add(p + "ffn_norm.weight", {2816}, F32);
    add(p + "ffn_gate.weight", {2816, 2112}, M);
    add(p + "ffn_up.weight", {2816, 2112}, M);
    add(p + "ffn_down.weight", {2112, 2816}, i == 29 ? BF16 : M);
    add(p + "post_ffw_norm_1.weight", {2816}, F32);
    add(p + "ffn_gate_inp.weight", {2816, 128}, F32);
    add(p + "ffn_gate_inp.scale", {2816}, F32);
    add(p + "pre_ffw_norm_2.weight", {2816}, F32);
    add(p + "ffn_gate_up_exps.weight", {2816, 1408, 128}, M);
    add(p + "ffn_down_exps.weight", {704, 2816, 128}, i == 29 ? BF16 : M);
    add(p + "ffn_down_exps.scale", {128}, F32);
    add(p + "post_ffw_norm_2.weight", {2816}, F32);
    add(p + "post_ffw_norm.weight", {2816}, F32);
    add(p + "layer_output_scale.weight", {1}, F32);
  }
  for (const auto& name : o.extra_tensors) b.tensor(name, {4}, F32, 16);
  const auto image = b.bytes(32, true);
  return g4::parse_gguf(image.data(), image.size(), image.size() + b.data_size());
}

std::variant<g4::ModelDesc, g4::ModelError> guard(const HeaderOptions& o = {}) {
  const auto parsed = gemma4_header(o);
  const auto* f = std::get_if<g4::GgufFile>(&parsed);
  if (f == nullptr) return g4::ModelError{"(header did not parse: " + std::get<g4::GgufError>(parsed).message + ")"};
  return g4::check_model(*f);
}

void model_guard_accepts_the_26b_a4b_shape() {
  const auto r = guard();
  const auto* m = std::get_if<g4::ModelDesc>(&r);
  expect(m != nullptr, "the 26B-A4B header passes the guard" + (m ? std::string{} : ": " + std::get<g4::ModelError>(r).message));
  if (m == nullptr) return;
  expect(m->layers[0].sliding && m->layers[0].head_dim == 256 && m->layers[0].kv_heads == 8 && m->layers[0].attn_v.present,
         "layer 0 is sliding: head 256, 8 KV heads, its own V");
  expect(!m->layers[5].sliding && m->layers[5].head_dim == 512 && m->layers[5].kv_heads == 2 && !m->layers[5].attn_v.present,
         "layer 5 is full: head 512, 2 KV heads, V from K");
  expect(m->layers[1].attn_q.type == g4::TensorType::bf16 && m->layers[29].down_exps.type == g4::TensorType::bf16,
         "BF16 matrices are accepted");
  expect(m->layers[3].gate_up_exps.rows == 1408 && m->layers[3].gate_up_exps.cols == 2816 && m->layers[3].gate_up_exps.experts == 128,
         "the fused expert stack shape");
  expect(m->token_embd.rows == 262144 && m->token_embd.cols == 2816, "the tied head is token_embd");
  expect(m->eos_id == 1 && m->turn_close_id == 106 && m->bos_id == 2, "stop ids: <eos> by key, <turn|> by literal");
  expect(m->softcap == 30.0f && m->rope_base_full == 1e6f && m->rope_base_swa == 1e4f, "softcap and RoPE bases");
  expect(m->layers[7].attn_q.file_offset > m->layers[6].attn_q.file_offset, "spans carry absolute file offsets");
  expect(m->layers[4].layer_output_scale.present, "the layer output scale is bound");
}

void expect_refused(const HeaderOptions& o, const std::string& why) {
  const auto r = guard(o);
  expect(std::holds_alternative<g4::ModelError>(r), "the guard refuses: " + why);
}

void model_guard_refuses_other_shapes() {
  {
    HeaderOptions o;
    o.drop_keys = {"general.architecture"};
    o.extra_keys = [](GgufBuilder& b) { b.str("general.architecture", "llama"); };
    expect_refused(o, "another architecture");
  }
  for (const auto& [key, value] : std::vector<std::pair<std::string, std::uint32_t>>{
           {"gemma4.block_count", 35}, {"gemma4.embedding_length", 2048}, {"gemma4.embedding_length_per_layer_input", 256},
           {"gemma4.attention.shared_kv_layers", 4}, {"gemma4.attention.sliding_window", 512}, {"gemma4.expert_count", 64},
           {"gemma4.expert_used_count", 4}, {"gemma4.attention.key_length", 256}, {"gemma4.rope.dimension_count", 128},
           {"gemma4.expert_feed_forward_length", 512}, {"tokenizer.ggml.eos_token_id", 262144}}) {
    HeaderOptions o;
    o.u32_overrides = {{key, value}};
    expect_refused(o, key + " = " + std::to_string(value));
  }
  for (const char* key : {"gemma4.attention.head_count_kv", "gemma4.attention.sliding_window_pattern",
                          "gemma4.final_logit_softcapping", "tokenizer.ggml.tokens", "tokenizer.ggml.eos_token_id"}) {
    HeaderOptions o;
    o.drop_keys = {key};
    expect_refused(o, std::string{"missing "} + key);
  }
  {
    HeaderOptions o;
    o.drop_tensors = {"blk.3.ffn_up.weight"};
    expect_refused(o, "a missing tensor");
  }
  {
    HeaderOptions o;
    o.dims = {{"blk.0.attn_q.weight", {2816, 4000}}};
    o.types = {{"blk.0.attn_q.weight", 0}};
    expect_refused(o, "a matrix of the wrong shape");
  }
  {
    HeaderOptions o;
    o.types = {{"blk.2.ffn_gate.weight", 1}};
    expect_refused(o, "an F16 matrix (refused, spec 5.5)");
  }
  {
    HeaderOptions o;
    o.types = {{"blk.2.attn_norm.weight", 30}};
    expect_refused(o, "a BF16 norm vector");
  }
  {
    HeaderOptions o;
    o.extra_tensors = {"blk.0.unknown.weight"};
    expect_refused(o, "an unknown tensor");
  }
  {
    HeaderOptions o;
    o.extra_tensors = {"output.weight"};
    expect_refused(o, "an untied output head");
  }
  {
    HeaderOptions o;
    o.dims = {{"rope_freqs.weight", {128}}};
    expect_refused(o, "rope_freqs of the wrong length");
  }
  {
    HeaderOptions o;
    o.tokens.resize(262144);
    for (std::size_t i = 0; i < o.tokens.size(); ++i) o.tokens[i] = "t" + std::to_string(i);
    o.tokens[1] = "<eos>";
    expect_refused(o, "no <turn|> token");
    o.tokens[106] = "<turn|>";
    o.tokens[107] = "<turn|>";
    expect_refused(o, "<turn|> twice");
  }
}

// ---- the command line, GEN parsing, the sampler and prefix reuse ----

std::variant<g4::EngineOptions, std::string> cli(std::vector<std::string> args) { return g4::parse_cli(args); }

void cli_accepts_the_serve_flags() {
  const auto r = cli({"--serve", "--native", "C:/m/g.gguf", "--gpu", "0", "--max-context", "65536", "--chunk", "1024",
                      "--kv", "f16", "--no-graph", "--expert-vram", "9.5", "--expert-slots", "40", "--hybrid-rows", "64",
                      "--cpu-threads", "12", "--pcie-frac", "0.25", "--cpu-lane", "off", "--expert-profile", "C:/p/in.bin",
                      "--expert-profile-save", "C:/p/out.bin", "--no-prompt-borrow"});
  const auto* o = std::get_if<g4::EngineOptions>(&r);
  expect(o != nullptr, "the serve flags parse" + (o ? std::string{} : ": " + std::get<std::string>(r)));
  if (o == nullptr) return;
  expect(o->serve && o->model == "C:/m/g.gguf" && o->gpus == std::vector<int>{0} && o->max_context == 65536 && o->chunk == 1024 &&
             !o->kv_f32 && !o->graph && o->expert_vram_gib == 9.5 && o->expert_slots == 40 && o->hybrid_rows == 64 &&
             o->cpu_threads == 12 && o->pcie_frac == 0.25 && !o->cpu_lane && o->expert_profile == "C:/p/in.bin" &&
             o->expert_profile_save == "C:/p/out.bin" && !o->prompt_borrow,
         "every value lands");
  const auto d = cli({"--native", "m.gguf", "--serve"});
  const auto* od = std::get_if<g4::EngineOptions>(&d);
  expect(od != nullptr && od->max_context == 32768 && od->chunk == 512 && od->graph && od->expert_vram_gib < 0 &&
             od->pcie_frac < 0 && od->cpu_lane && od->prompt_borrow,
         "defaults: 32768 context, 512 chunk, graphs on, VRAM from the free memory, the CPU lane on its default share");
  const auto g = cli({"--native", "m.gguf", "--serve", "--gpu", "1,0"});
  const auto* og = std::get_if<g4::EngineOptions>(&g);
  expect(og != nullptr && og->gpus == std::vector<int>{1, 0}, "a GPU list");
}

void cli_refuses_bad_and_test_only_flags_with_serve() {
  const std::vector<std::vector<std::string>> bad{
      {"--serve"},                                                   // no model
      {"--native", "m.gguf", "--serve", "--unknown-zz"},
      {"--native", "m.gguf", "--serve", "--max-context", "0"},
      {"--native", "m.gguf", "--serve", "--max-context", "262145"},
      {"--native", "m.gguf", "--serve", "--max-context"},
      {"--native", "m.gguf", "--serve", "--max-context", "12x"},
      {"--native", "m.gguf", "--serve", "--chunk", "8193"},
      {"--native", "m.gguf", "--serve", "--kv", "q8"},
      {"--native", "m.gguf", "--serve", "--expert-vram", "-1"},
      {"--native", "m.gguf", "--serve", "--expert-slots", "129"},
      {"--native", "m.gguf", "--serve", "--pcie-frac", "1.5"},
      {"--native", "m.gguf", "--serve", "--cpu-lane", "zz"},
      {"--native", "m.gguf", "--serve", "--expert-profile"},
      {"--native", "m.gguf", "--serve", "--expert-profile-save", ""},
      {"--native", "m.gguf", "--serve", "--gpu", "a"},
      {"--native", "m.gguf", "--serve", "--gpu", ""},
      {"--native", "m.gguf", "--native", "n.gguf"},
      {"--native", "m.gguf", "--serve", "--tokens", "1,2"},
      {"--native", "m.gguf", "--serve", "--dump-full-logits", "f"},
      {"--native", "m.gguf", "--serve", "--dump-layers", "f"},
      {"--native", "m.gguf", "--serve", "--force", "1"},
      {"--native", "m.gguf", "--serve", "--bench-tg", "8"},
      {"--native", "m.gguf", "positional-zz"},
  };
  for (const auto& args : bad) {
    const auto r = cli(args);
    std::string joined;
    for (const auto& a : args) joined += a + " ";
    expect(std::holds_alternative<std::string>(r), "refused: " + joined);
    if (const auto* m = std::get_if<std::string>(&r)) {
      expect(m->find("zz") == std::string::npos && m->find("m.gguf") == std::string::npos, "the message echoes nothing: " + *m);
    }
  }
  const auto test_mode = cli({"--native", "m.gguf", "--tokens", "2,105", "--n", "8", "--dump-full-logits", "out.f32"});
  expect(std::holds_alternative<g4::EngineOptions>(test_mode), "the test-only flags without --serve");
}

void gen_lines_follow_the_q36_grammar() {
  const auto r = g4::parse_gen("GEN 64 temperature=0.7 top_p=0.9 top_k=40 min_p=0.05 penalty_repeat=1.1 penalty_last_n=64 seed=42 2,105,4,7", 262144);
  const auto* g = std::get_if<g4::GenRequest>(&r);
  expect(g != nullptr, "a full GEN line parses" + (g ? std::string{} : ": " + std::get<std::string>(r)));
  if (g != nullptr) {
    expect(g->max_new == 64 && g->ids == std::vector<std::int32_t>{2, 105, 4, 7}, "max_new and the ids");
    expect(g->sampling.temperature == 0.7f && g->sampling.top_p == 0.9f && g->sampling.top_k == 40 && g->sampling.min_p == 0.05f &&
               g->sampling.repeat == 1.1f && g->sampling.last_n == 64 && g->sampling.seed == 42ull,
           "the sampling keys");
  }
  const auto plain = g4::parse_gen("GEN 8 2,3", 262144);
  const auto* p = std::get_if<g4::GenRequest>(&plain);
  expect(p != nullptr && !p->sampling.needs_logits() && p->ids.size() == 2, "a greedy GEN line");
  for (const char* line : {"GEN 0 2,3", "GEN -1 2,3", "GEN 8", "GEN 8 ", "GEN 8 2,,3", "GEN 8 2,3,", "GEN 8 2,262144",
                           "GEN 8 bogus=1 2,3", "GEN 8 temperature=x 2,3", "GEN 8 temperature=-1 2,3", "GEN 8 top_p=2 2,3",
                           "GENX 8 2,3", "GEN 8 2,-3", "GEN 8 2,3 4"}) {
    expect(std::holds_alternative<std::string>(g4::parse_gen(line, 262144)), std::string{"refused: "} + line);
  }
}

void sampler_matches_the_q36_rules() {
  std::mt19937_64 rng(1);
  std::vector<float> lg{0.0f, 3.0f, 1.0f, 2.0f};
  g4::Sampling greedy;
  auto copy = lg;
  expect(g4::sample(copy, greedy, {}, rng) == 1, "temperature 0 is greedy");
  g4::Sampling pen;
  pen.repeat = 4.0f;
  pen.last_n = 8;
  copy = lg;
  expect(g4::sample(copy, pen, {1}, rng) == 3, "a repeat penalty moves greedy off a seen id (3 / 4 < 2)");
  g4::Sampling hot;
  hot.temperature = 1.0f;
  hot.top_k = 1;
  for (int i = 0; i < 20; ++i) {
    copy = lg;
    if (g4::sample(copy, hot, {}, rng) != 1) {
      expect(false, "top_k 1 always picks the best");
      break;
    }
  }
  hot.top_k = 4;
  hot.top_p = 0.5f;  // p(1) = e^3 / sum ~ 0.64 >= 0.5: only id 1 is kept
  for (int i = 0; i < 20; ++i) {
    copy = lg;
    if (g4::sample(copy, hot, {}, rng) != 1) {
      expect(false, "top_p 0.5 keeps only the head here");
      break;
    }
  }
  std::mt19937_64 a(7), b(7);
  g4::Sampling s;
  s.temperature = 1.0f;
  s.top_k = 4;
  bool same = true;
  for (int i = 0; i < 50; ++i) {
    auto x = lg, y = lg;
    same = same && g4::sample(x, s, {}, a) == g4::sample(y, s, {}, b);
  }
  expect(same, "the same seed gives the same picks");
}

// The serve loop's GPU path (top-k on the device, the draw on the host) gives the same ids as
// the host sampler for the same seed whenever no penalty applies.
void sorted_candidates_sample_like_the_full_sampler() {
  std::mt19937 gen(99);
  std::uniform_real_distribution<float> d(-8.0f, 8.0f);
  std::vector<float> logits(5000);
  for (auto& v : logits) v = d(gen);
  for (const auto& [t, p, mp, k] : std::vector<std::tuple<float, float, float, int>>{
           {1.0f, 0.95f, 0.0f, 64}, {0.7f, 0.9f, 0.05f, 20}, {1.5f, 1.0f, 0.0f, 0}, {0.3f, 0.5f, 0.1f, 1}}) {
    g4::Sampling s;
    s.temperature = t;
    s.top_p = p;
    s.min_p = mp;
    s.top_k = k;
    expect(!g4::penalties_on(s), "no penalty set");
    const int kk = g4::candidate_count(s, 5000);
    expect(kk == (k > 0 ? k : 64), "candidate count: top_k, or 64 when unset");
    std::vector<int> order(logits.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
    std::sort(order.begin(), order.end(), [&](int a, int b) {
      return logits[static_cast<std::size_t>(a)] > logits[static_cast<std::size_t>(b)] || (logits[static_cast<std::size_t>(a)] == logits[static_cast<std::size_t>(b)] && a < b);
    });
    std::vector<float> vals;
    for (int j = 0; j < kk; ++j) vals.push_back(logits[static_cast<std::size_t>(order[static_cast<std::size_t>(j)])]);
    std::mt19937_64 a(1234), b(1234);
    bool same = true;
    for (int i = 0; i < 200; ++i) {
      auto copy = logits;
      same = same && g4::sample(copy, s, {}, a) == g4::sample_sorted(vals.data(), order.data(), kk, s, b);
    }
    expect(same, "the same draws for temperature " + std::to_string(t) + ", top_k " + std::to_string(k));
  }
  g4::Sampling pen;
  pen.repeat = 1.1f;
  pen.last_n = 64;
  expect(g4::penalties_on(pen), "a repeat penalty with a window is on");
  pen.last_n = 0;
  expect(!g4::penalties_on(pen), "a penalty without a window is off (the q36 sampler skips it)");
  g4::Sampling big;
  big.top_k = 100000;
  expect(g4::candidate_count(big, 5000) == 5000, "the count never exceeds the vocabulary");
}

// A GEN line without a seed must not draw the same sample every time (a retry of a bad
// answer would repeat it); an explicit seed still reproduces exactly.
void unseeded_requests_draw_fresh_seeds() {
  expect(g4::request_seed(42, 1000) == 42 && g4::request_seed(42, 2000) == 42, "an explicit seed is kept");
  const auto a = g4::request_seed(0, 123456789ull), b = g4::request_seed(0, 123456790ull);
  expect(a != 0 && b != 0, "an unseeded request never gets seed 0");
  expect(a != b, "two unseeded requests at different clock ticks get different seeds");
  std::mt19937_64 ra(a), rb(b);
  g4::Sampling s;
  s.temperature = 1.0f;
  s.top_k = 64;
  std::vector<float> lg(1000);
  for (std::size_t i = 0; i < lg.size(); ++i) lg[i] = static_cast<float>(i % 17) * 0.1f;
  int differ = 0;
  for (int i = 0; i < 20; ++i) {
    auto x = lg, y = lg;
    differ += g4::sample(x, s, {}, ra) != g4::sample(y, s, {}, rb) ? 1 : 0;
  }
  expect(differ > 0, "different seeds give different draws");
}

// The INFO record: the expert split under the main engine's key names (the frontend's
// Monitor whitelist), and every field in the shape the frontend keeps (a [a-z0-9_] key up to
// 32 bytes, a value up to 64 bytes).
// --expert-profile (paper 3.1: the VRAM tier filled at start from a usage profile): Strata's profile ranks (layer,
// expert) pairs; each layer's S slots take that layer's experts in rank order, then the lowest unranked ones.
void profile_fills_each_layer_with_its_top_experts() {
  const std::vector<std::pair<int, int>> ranked = {{2, 7}, {0, 5}, {2, 9}, {0, 1}, {2, 7}, {2, 3}, {0, 4}, {29, 127}};
  const auto f = g4::fill_from_profile(ranked, 3);
  expect(f.size() == std::size_t(g4::kLayers), "a list per layer");
  if (f.size() != std::size_t(g4::kLayers)) return;
  expect(f[0] == std::vector<int>{5, 1, 4}, "layer 0: its ranked experts in rank order");
  expect(f[2] == std::vector<int>{7, 9, 3}, "layer 2: a pair ranked twice counts once");
  expect(f[29] == std::vector<int>{127, 0, 1}, "layer 29: one ranked, then the lowest unranked");
  expect(f[5] == std::vector<int>{0, 1, 2}, "an unranked layer: experts 0..S-1, as without a profile");
  const auto none = g4::fill_from_profile(ranked, 0);
  expect(std::all_of(none.begin(), none.end(), [](const std::vector<int>& v) { return v.empty(); }), "no slots: nothing");
  const auto all = g4::fill_from_profile(ranked, g4::kExperts);
  bool perm = all.size() == std::size_t(g4::kLayers);
  for (const auto& v : all) {
    std::vector<int> s = v;
    std::sort(s.begin(), s.end());
    std::vector<int> want(static_cast<std::size_t>(g4::kExperts));
    for (int e = 0; e < g4::kExperts; ++e) want[static_cast<std::size_t>(e)] = e;
    perm = perm && s == want;
  }
  expect(perm, "every slot: each layer holds every expert once");
  const auto bad = g4::fill_from_profile({{30, 1}, {0, 128}, {-1, 0}, {0, -1}}, 2);
  expect(bad.size() == std::size_t(g4::kLayers) && bad[0] == std::vector<int>{0, 1}, "pairs out of range are skipped");
}

void info_line_reports_the_expert_split() {
  g4::LoadReport r;
  r.slots_per_layer = 31;
  r.resident_pct = 24.2;
  r.vram_used_mib = 15500;
  r.vram_total_mib = 16376;
  r.pinned_gib = 23.49;
  r.load_seconds = 29.5;
  r.ring_rows = 2048;
  const std::string line = g4::engine_info_line(r, 262144);
  expect(line.rfind("INFO ", 0) == 0, "an INFO record");
  for (const char* want : {" expert_slots=3840", " expert_slots_primary=930", " slots_per_layer=31", " context=262144",
                           " experts_resident_pct=24.2"}) {
    expect(line.find(want) != std::string::npos, std::string{"carries"} + want);
  }
  std::size_t at = 5;
  bool shaped = line.find('\n') == std::string::npos;
  while (at < line.size()) {
    const std::size_t end = std::min(line.find(' ', at), line.size());
    const std::string field = line.substr(at, end - at);
    const std::size_t eq = field.find('=');
    shaped = shaped && eq != std::string::npos && eq > 0 && eq <= 32 && field.size() - eq - 1 <= 64 && field.size() - eq - 1 > 0;
    for (std::size_t i = 0; i < eq && eq != std::string::npos; ++i) {
      const char c = field[i];
      shaped = shaped && ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_');
    }
    at = end + 1;
  }
  expect(shaped, "every field is key=value as the frontend keeps them: " + line);
}

void reuse_keeps_exact_prefixes_within_the_ring() {
  const std::vector<std::int32_t> held{1, 2, 3, 4, 5, 6};
  expect(g4::reuse_length(held, {1, 2, 3, 4, 5, 6, 7, 8}, 1024) == 6, "a strict prefix is reused whole");
  expect(g4::reuse_length(held, {1, 2, 3, 9, 9}, 1024) == 3, "a divergence keeps the common prefix");
  expect(g4::reuse_length(held, {1, 2, 3, 9, 9}, 2) == 0, "unless the held tail past it overflows the ring spare rows");
  expect(g4::reuse_length(held, {1, 2, 3, 9, 9}, 3) == 3, "a tail of exactly the spare rows is fine");
  expect(g4::reuse_length(held, {1, 2, 3}, 1024) == 2, "a prompt that is a prefix of the held tokens re-reads its last token");
  expect(g4::reuse_length(held, {7, 8}, 1024) == 0, "nothing in common");
  expect(g4::reuse_length({}, {1, 2}, 1024) == 0, "nothing held");
}
}  // namespace

int main(int argc, char** argv) {
  return g4_test::run_cases(argc, argv, {
      {"gguf_parses_a_valid_header", gguf_parses_a_valid_header},
      {"gguf_refuses_bad_magic_version_and_truncation", gguf_refuses_bad_magic_version_and_truncation},
      {"gguf_refuses_oversized_counts_and_strings", gguf_refuses_oversized_counts_and_strings},
      {"gguf_refuses_bad_tensors", gguf_refuses_bad_tensors},
      {"tensor_bytes_follow_the_types", tensor_bytes_follow_the_types},
      {"model_guard_accepts_the_26b_a4b_shape", model_guard_accepts_the_26b_a4b_shape},
      {"model_guard_refuses_other_shapes", model_guard_refuses_other_shapes},
      {"cli_accepts_the_serve_flags", cli_accepts_the_serve_flags},
      {"cli_refuses_bad_and_test_only_flags_with_serve", cli_refuses_bad_and_test_only_flags_with_serve},
      {"gen_lines_follow_the_q36_grammar", gen_lines_follow_the_q36_grammar},
      {"sampler_matches_the_q36_rules", sampler_matches_the_q36_rules},
      {"reuse_keeps_exact_prefixes_within_the_ring", reuse_keeps_exact_prefixes_within_the_ring},
      {"sorted_candidates_sample_like_the_full_sampler", sorted_candidates_sample_like_the_full_sampler},
      {"unseeded_requests_draw_fresh_seeds", unseeded_requests_draw_fresh_seeds},
      {"info_line_reports_the_expert_split", info_line_reports_the_expert_split},
      {"profile_fills_each_layer_with_its_top_experts", profile_fills_each_layer_with_its_top_experts},
  });
}
