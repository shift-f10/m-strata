// Hermetic tests for multiengine_flint's host code (no GPU): the shard names and the model guard on synthetic flint
// headers split across shards (no tensor data). The runner and the GGUF builder are Gemma 4's (tests/gemma4).
#include "f_cli.hpp"
#include "f_model.hpp"
#include "f_profile.hpp"
#include "g4_gguf.hpp"
#include "g4_test.hpp"
#include "gguf_writer.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <string>
#include <variant>
#include <vector>

using g4_test::expect;
using g4_test::GgufBuilder;

namespace {

// ---- shard names (llama.cpp's gguf-split pattern) ----

void split_names_follow_gguf_split() {
  const auto three = fl::split_names("F:/models/flint-BF16-00002-of-00003.gguf");
  expect(three == std::vector<std::string>{"F:/models/flint-BF16-00001-of-00003.gguf", "F:/models/flint-BF16-00002-of-00003.gguf",
                                           "F:/models/flint-BF16-00003-of-00003.gguf"},
         "any shard names all three, in order");
  expect(fl::split_names("C:/m/flint.gguf") == std::vector<std::string>{"C:/m/flint.gguf"}, "a single file is its own list");
  expect(fl::split_names("x-00004-of-00003.gguf").empty(), "a shard number past the count is refused");
  expect(fl::split_names("x-00000-of-00003.gguf").empty(), "shard 0 is refused");
  expect(fl::split_names("x-00001-of-00000.gguf").empty(), "a count of 0 is refused");
  expect(fl::split_names("x-00001-of-00065.gguf").empty(), "more than kMaxShards shards are refused");
  expect(fl::split_names("x-0000a-of-00003.gguf") == std::vector<std::string>{"x-0000a-of-00003.gguf"},
         "a name that does not follow the pattern is one file");
}

// ---- the model guard on synthetic flint headers ----

constexpr std::uint32_t F32 = 0, F16 = 1, Q8 = 8, BF16 = 30;

struct HeaderOptions {
  std::function<void(GgufBuilder&)> extra_keys;  // into shard 0
  std::set<std::string> drop_keys;
  std::set<std::string> drop_tensors;
  std::map<std::string, std::vector<std::uint64_t>> dims;
  std::map<std::string, std::uint32_t> types;
  std::map<std::string, std::uint32_t> u32_overrides;
  std::vector<std::string> extra_tensors;   // more F32 [4] tensors, in the last shard
  std::string duplicate;                    // a tensor written into the first tensor shard as well
  std::uint32_t weight_type{BF16};          // the matrices' and the experts' type
  int shards{3};                            // 1: one file holding everything; else shard 0 metadata only
  int split_count_override{-1};
  bool rope_equals_window{true};
};

std::uint64_t span_bytes(std::uint32_t type, const std::vector<std::uint64_t>& dims) {
  std::uint64_t n = 1;
  for (auto d : dims) n *= d;
  const auto b = g4::tensor_bytes(static_cast<g4::TensorType>(type), dims[0], n);
  return b ? *b : n * 4;
}

bool full_layer(int i) { return i == 0 || i == 1 || i == 48 || (i >= 5 && (i - 1) % 4 == 0); }

std::vector<g4::GgufFile> flint_header(const HeaderOptions& o, std::string& error) {
  const int n_shards = o.shards;
  std::vector<GgufBuilder> b(static_cast<std::size_t>(n_shards));
  auto key_u32 = [&](const std::string& k, std::uint32_t v) {
    if (o.drop_keys.count(k)) return;
    const auto it = o.u32_overrides.find(k);
    b[0].u32(k, it == o.u32_overrides.end() ? v : it->second);
  };
  auto key_f32 = [&](const std::string& k, float v) {
    if (!o.drop_keys.count(k)) b[0].f32(k, v);
  };
  if (!o.drop_keys.count("general.architecture")) b[0].str("general.architecture", "flint");
  key_u32("flint.block_count", 49);
  key_u32("flint.context_length", 262144);
  key_u32("flint.embedding_length", 2048);
  key_u32("flint.feed_forward_length", 6144);
  key_u32("flint.attention.head_count", 32);
  key_u32("flint.attention.head_count_kv", 4);
  key_f32("flint.rope.freq_base", 10000.0f);
  key_f32("flint.attention.layer_norm_rms_epsilon", 1e-6f);
  key_u32("flint.expert_count", 256);
  key_u32("flint.expert_used_count", 6);
  key_u32("flint.expert_gating_func", 2);
  key_u32("flint.attention.key_length", 128);
  key_u32("flint.attention.value_length", 128);
  key_u32("flint.expert_feed_forward_length", 512);
  key_u32("flint.expert_shared_feed_forward_length", 1024);
  key_u32("flint.leading_dense_block_count", 2);
  key_u32("flint.expert_shared_count", 2);
  if (!o.drop_keys.count("flint.expert_weights_norm")) b[0].boolean("flint.expert_weights_norm", true);
  key_f32("flint.expert_weights_scale", 1.0f);
  key_u32("flint.attention.sliding_window", 513);
  std::vector<bool> swa, rope;
  for (int i = 0; i < 49; ++i) {
    swa.push_back(!full_layer(i));
    rope.push_back(o.rope_equals_window ? !full_layer(i) : true);
  }
  if (!o.drop_keys.count("flint.attention.sliding_window_pattern")) b[0].arr_bool("flint.attention.sliding_window_pattern", swa);
  if (!o.drop_keys.count("flint.attention.rope_pattern")) b[0].arr_bool("flint.attention.rope_pattern", rope);
  if (!o.drop_keys.count("tokenizer.ggml.model")) b[0].str("tokenizer.ggml.model", "gpt2");
  if (!o.drop_keys.count("tokenizer.ggml.tokens")) {
    std::vector<std::string> tokens(150144);
    for (std::size_t i = 0; i < tokens.size(); ++i) tokens[i] = "t" + std::to_string(i);
    b[0].arr_str("tokenizer.ggml.tokens", tokens);
  }
  key_u32("tokenizer.ggml.bos_token_id", 150000);
  key_u32("tokenizer.ggml.eos_token_id", 150001);
  if (!o.drop_keys.count("tokenizer.ggml.eog_token_ids")) b[0].arr_i32("tokenizer.ggml.eog_token_ids", {150001, 150003, 150005, 150018});
  if (n_shards > 1) {
    for (int s = 0; s < n_shards; ++s) {
      b[static_cast<std::size_t>(s)].u32("split.no", static_cast<std::uint32_t>(s));
      b[static_cast<std::size_t>(s)].u32("split.count", static_cast<std::uint32_t>(o.split_count_override >= 0 ? o.split_count_override : n_shards));
    }
  }
  if (o.extra_keys) o.extra_keys(b[0]);
  // tensors: shard 1 takes layers 0-24 and the embedding, the last shard the rest (one file: shard 0 holds them all)
  auto shard_of = [&](int layer) { return n_shards == 1 ? 0 : (layer < 25 || n_shards == 2 ? 1 : n_shards - 1); };
  auto add = [&](int shard, const std::string& name, std::vector<std::uint64_t> dims, std::uint32_t type) {
    if (o.drop_tensors.count(name)) return;
    if (const auto it = o.dims.find(name); it != o.dims.end()) dims = it->second;
    if (const auto it = o.types.find(name); it != o.types.end()) type = it->second;
    b[static_cast<std::size_t>(shard)].tensor(name, dims, type, span_bytes(type, dims));
    if (name == o.duplicate) b[static_cast<std::size_t>(shard_of(0))].tensor(name, dims, type, span_bytes(type, dims));
  };
  const std::uint32_t M = o.weight_type;
  add(shard_of(0), "token_embd.weight", {2048, 150144}, M);
  add(shard_of(48), "output_norm.weight", {2048}, F32);
  add(shard_of(48), "output.weight", {2048, 150144}, M);
  for (int i = 0; i < 49; ++i) {
    const int s = shard_of(i);
    const std::string p = "blk." + std::to_string(i) + ".";
    add(s, p + "attn_norm.weight", {2048}, F32);
    add(s, p + "attn_q.weight", {2048, 4096}, M);
    add(s, p + "attn_k.weight", {2048, 512}, M);
    add(s, p + "attn_v.weight", {2048, 512}, M);
    add(s, p + "attn_output.weight", {4096, 2048}, M);
    add(s, p + "attn_q_norm.weight", {128}, F32);
    add(s, p + "attn_k_norm.weight", {128}, F32);
    add(s, p + "attn_sinks.weight", {32}, F32);
    add(s, p + "ffn_norm.weight", {2048}, F32);
    if (i < 2) {
      add(s, p + "ffn_gate.weight", {2048, 6144}, M);
      add(s, p + "ffn_up.weight", {2048, 6144}, M);
      add(s, p + "ffn_down.weight", {6144, 2048}, M);
    } else {
      add(s, p + "ffn_gate_inp.weight", {2048, 256}, BF16);
      add(s, p + "ffn_gate_exps.weight", {2048, 512, 256}, M);
      add(s, p + "ffn_up_exps.weight", {2048, 512, 256}, M);
      add(s, p + "ffn_down_exps.weight", {512, 2048, 256}, M);
      add(s, p + "ffn_gate_shexp.weight", {2048, 1024}, M);
      add(s, p + "ffn_up_shexp.weight", {2048, 1024}, M);
      add(s, p + "ffn_down_shexp.weight", {1024, 2048}, M);
    }
  }
  for (const auto& name : o.extra_tensors) b.back().tensor(name, {4}, F32, 16);
  std::vector<g4::GgufFile> files;
  for (const auto& bb : b) {
    const auto image = bb.bytes(32, true);
    auto parsed = g4::parse_gguf(image.data(), image.size(), image.size() + bb.data_size());
    if (const auto* e = std::get_if<g4::GgufError>(&parsed)) {
      error = e->message;
      return {};
    }
    files.push_back(std::get<g4::GgufFile>(std::move(parsed)));
  }
  return files;
}

std::variant<fl::ModelDesc, fl::ModelError> guard(const HeaderOptions& o = {}) {
  std::string error;
  const auto files = flint_header(o, error);
  if (files.empty()) return fl::ModelError{"(a header did not parse: " + error + ")"};
  return fl::check_model(files);
}

void model_guard_accepts_the_flint_shape() {
  const auto r = guard();
  const auto* m = std::get_if<fl::ModelDesc>(&r);
  expect(m != nullptr, "the flint header passes the guard" + (m ? std::string{} : ": " + std::get<fl::ModelError>(r).message));
  if (m == nullptr) return;
  const auto& L0 = m->layers[0];
  const auto& L2 = m->layers[2];
  const auto& L5 = m->layers[5];
  expect(!L0.moe && !L0.sliding && !L0.rope && L0.ffn_gate.present && !L0.gate_exps.present, "layer 0: dense, full, no RoPE");
  expect(!m->layers[1].moe, "layer 1 is dense");
  expect(L2.moe && L2.sliding && L2.rope && L2.gate_exps.present && !L2.ffn_gate.present, "layer 2: MoE, sliding, RoPE");
  expect(L5.moe && !L5.sliding && !L5.rope, "layer 5: MoE, full, no RoPE");
  expect(!m->layers[48].sliding && m->layers[47].sliding, "layer 48 is full, 47 sliding");
  expect(L2.gate_exps.cols == 2048 && L2.gate_exps.rows == 512 && L2.gate_exps.experts == 256, "the gate stack's shape");
  expect(L2.down_exps.cols == 512 && L2.down_exps.rows == 2048 && L2.down_exps.experts == 256, "the down stack's shape");
  expect(L2.gate_shexp.rows == 1024 && L2.down_shexp.cols == 1024, "the shared expert's width");
  expect(L2.attn_q.rows == 4096 && L2.attn_k.rows == 512 && L2.attn_output.cols == 4096, "GQA 32:4 at head 128");
  expect(L2.attn_sinks.cols == 32 && L2.gate_inp.type == fl::TensorType::bf16, "sinks and a BF16 router");
  expect(m->output.rows == 150144 && m->token_embd.rows == 150144, "an untied head");
  expect(m->layers[3].attn_q.shard == 1 && m->layers[30].attn_q.shard == 2 && m->output.shard == 2, "each span knows its shard");
  expect(m->bos_id == 150000 && m->eos_id == 150001, "bos and eos");
  expect(m->eog_ids == std::vector<std::int32_t>{150001, 150003, 150005, 150018}, "the end-of-generation ids");
  expect(m->rope_base == 10000.0f && m->rms_eps == 1e-6f && m->expert_weights_scale == 1.0f, "RoPE base, eps, weights scale");
}

void model_guard_takes_one_file_and_q8_0_weights() {
  HeaderOptions o;
  o.shards = 1;
  o.weight_type = Q8;
  const auto r = guard(o);
  const auto* m = std::get_if<fl::ModelDesc>(&r);
  expect(m != nullptr, "one Q8_0 file passes" + (m ? std::string{} : ": " + std::get<fl::ModelError>(r).message));
  if (m == nullptr) return;
  expect(m->layers[10].gate_exps.type == fl::TensorType::q8_0 && m->output.type == fl::TensorType::q8_0, "Q8_0 experts and head");
  expect(m->layers[10].gate_exps.shard == 0, "a one-file model's tensors are in shard 0");
}

void model_guard_reads_the_rope_pattern_on_its_own() {
  HeaderOptions o;
  o.rope_equals_window = false;
  const auto r = guard(o);
  const auto* m = std::get_if<fl::ModelDesc>(&r);
  expect(m != nullptr && !m->layers[5].sliding && m->layers[5].rope, "a full layer with RoPE when the pattern says so");
}

void expect_refused(const HeaderOptions& o, const std::string& why) {
  const auto r = guard(o);
  expect(std::holds_alternative<fl::ModelError>(r), "the guard refuses: " + why);
}

void model_guard_refuses_other_shapes() {
  {
    HeaderOptions o;
    o.drop_keys = {"general.architecture"};
    o.extra_keys = [](GgufBuilder& b) { b.str("general.architecture", "llama"); };
    expect_refused(o, "another architecture");
  }
  for (const auto& [key, value] : std::vector<std::pair<std::string, std::uint32_t>>{
           {"flint.block_count", 48}, {"flint.embedding_length", 4096}, {"flint.feed_forward_length", 8192},
           {"flint.attention.head_count", 16}, {"flint.attention.head_count_kv", 8}, {"flint.attention.key_length", 64},
           {"flint.attention.value_length", 64}, {"flint.expert_count", 128}, {"flint.expert_used_count", 8},
           {"flint.expert_gating_func", 1}, {"flint.expert_feed_forward_length", 768},
           {"flint.expert_shared_feed_forward_length", 512}, {"flint.leading_dense_block_count", 3},
           {"flint.attention.sliding_window", 512}, {"tokenizer.ggml.eos_token_id", 150144},
           {"tokenizer.ggml.bos_token_id", 200000}}) {
    HeaderOptions o;
    o.u32_overrides = {{key, value}};
    expect_refused(o, key + " = " + std::to_string(value));
  }
  for (const char* key : {"flint.attention.sliding_window_pattern", "flint.attention.rope_pattern", "flint.expert_weights_norm",
                          "flint.expert_gating_func", "tokenizer.ggml.tokens", "tokenizer.ggml.eos_token_id",
                          "flint.attention.layer_norm_rms_epsilon", "flint.rope.freq_base"}) {
    HeaderOptions o;
    o.drop_keys = {key};
    expect_refused(o, std::string{"missing "} + key);
  }
  {
    HeaderOptions o;
    o.drop_keys = {"flint.attention.sliding_window_pattern"};
    o.extra_keys = [](GgufBuilder& b) { b.arr_bool("flint.attention.sliding_window_pattern", std::vector<bool>(48, true)); };
    expect_refused(o, "a window pattern of 48 layers");
  }
  {
    HeaderOptions o;
    o.drop_keys = {"flint.expert_weights_norm"};
    o.extra_keys = [](GgufBuilder& b) { b.boolean("flint.expert_weights_norm", false); };
    expect_refused(o, "unnormalised expert weights");
  }
  {
    HeaderOptions o;
    o.drop_tensors = {"blk.30.ffn_up_exps.weight"};
    expect_refused(o, "a missing expert stack");
  }
  {
    HeaderOptions o;
    o.drop_tensors = {"blk.7.attn_sinks.weight"};
    expect_refused(o, "missing sinks");
  }
  {
    HeaderOptions o;
    o.dims = {{"blk.3.attn_k.weight", {2048, 1024}}};
    expect_refused(o, "a matrix of the wrong shape");
  }
  {
    HeaderOptions o;
    o.types = {{"blk.2.attn_q.weight", F16}};
    expect_refused(o, "an F16 matrix");
  }
  {
    HeaderOptions o;
    o.types = {{"blk.2.attn_norm.weight", BF16}};
    expect_refused(o, "a BF16 norm vector");
  }
  {
    HeaderOptions o;
    o.types = {{"blk.9.attn_sinks.weight", BF16}};
    expect_refused(o, "BF16 sinks");
  }
  {
    HeaderOptions o;
    o.extra_tensors = {"blk.3.exp_probs_b.bias"};
    expect_refused(o, "an unknown tensor (a router bias this engine does not apply)");
  }
  {
    HeaderOptions o;
    o.extra_tensors = {"blk.0.ffn_gate_exps.weight"};
    expect_refused(o, "a dense layer carrying an expert tensor");
  }
  {
    HeaderOptions o;
    o.duplicate = "blk.40.attn_q.weight";
    expect_refused(o, "a tensor in two shards");
  }
  {
    HeaderOptions o;
    o.split_count_override = 4;
    expect_refused(o, "split.count that does not match the shards read");
  }
}

// ---- the command line, the serve grammar, the sampler, the profile fill (Gemma 4's, at flint's numbers) ----

std::variant<fl::EngineOptions, std::string> cli(const std::vector<std::string>& args) { return fl::parse_cli(args); }

void cli_accepts_the_serve_flags() {
  const auto r = cli({"--serve", "--native", "F:/m/flint-Q8_0-00001-of-00002.gguf", "--gpu", "0", "--max-context", "65536", "--chunk",
                      "1024", "--no-graph", "--expert-vram", "9.5", "--expert-slots", "200", "--cpu-threads", "12", "--pcie-frac",
                      "0.25", "--cpu-lane", "off", "--expert-profile", "C:/p/in.bin", "--expert-profile-save", "C:/p/out.bin",
                      "--no-prompt-borrow"});
  const auto* o = std::get_if<fl::EngineOptions>(&r);
  expect(o != nullptr, "the serve flags parse" + (o ? std::string{} : ": " + std::get<std::string>(r)));
  if (o == nullptr) return;
  expect(o->serve && o->model == "F:/m/flint-Q8_0-00001-of-00002.gguf" && o->gpus == std::vector<int>{0} && o->max_context == 65536 &&
             o->chunk == 1024 && !o->graph && o->expert_vram_gib == 9.5 && o->expert_slots == 200 && o->cpu_threads == 12 &&
             o->pcie_frac == 0.25 && !o->cpu_lane && o->expert_profile == "C:/p/in.bin" && o->expert_profile_save == "C:/p/out.bin" &&
             !o->prompt_borrow,
         "every value lands");
  const auto d = cli({"--native", "m.gguf", "--serve"});
  const auto* od = std::get_if<fl::EngineOptions>(&d);
  expect(od != nullptr && od->max_context == 32768 && od->chunk == 512 && od->graph && od->expert_vram_gib < 0 && od->pcie_frac < 0 &&
             od->cpu_lane && od->prompt_borrow && od->layers == fl::kLayers,
         "defaults: 32768 context, 512 chunk, graphs on, every layer");
  const auto t = cli({"--native", "m.gguf", "--tokens", "1,2", "--layers", "12", "--dump-layers", "l.f32"});
  const auto* ot = std::get_if<fl::EngineOptions>(&t);
  expect(ot != nullptr && ot->layers == 12, "--layers in a test run");
}

void cli_refuses_bad_and_test_only_flags_with_serve() {
  const std::vector<std::vector<std::string>> bad{
      {"--serve"},
      {"--native", "m.gguf", "--serve", "--unknown-zz"},
      {"--native", "m.gguf", "--serve", "--max-context", "262145"},
      {"--native", "m.gguf", "--serve", "--expert-slots", "257"},
      {"--native", "m.gguf", "--serve", "--pcie-frac", "1.5"},
      {"--native", "m.gguf", "--serve", "--kv", "f16"},
      {"--native", "m.gguf", "--serve", "--layers", "12"},
      {"--native", "m.gguf", "--tokens", "1", "--layers", "50"},
      {"--native", "m.gguf", "--tokens", "1", "--layers", "0"},
      {"--native", "m.gguf", "--serve", "--tokens", "1,2"},
      {"--native", "m.gguf", "--serve", "--check"},
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
}

void gen_lines_follow_the_q36_grammar() {
  const auto r = fl::parse_gen("GEN 64 temperature=1 top_p=0.95 top_k=20 seed=42 150000,3,150143", fl::kVocab);
  const auto* g = std::get_if<fl::GenRequest>(&r);
  expect(g != nullptr && g->max_new == 64 && g->ids == std::vector<std::int32_t>{150000, 3, 150143} && g->sampling.top_k == 20 &&
             g->sampling.seed == 42ull,
         "a GEN line with flint's ids and sampling");
  expect(std::holds_alternative<std::string>(fl::parse_gen("GEN 8 2,150144", fl::kVocab)), "an id past the vocabulary is refused");
}

void sampler_and_seeds_match_gemma4s() {
  std::mt19937_64 rng(1);
  std::vector<float> lg{0.0f, 3.0f, 1.0f, 2.0f};
  fl::Sampling greedy;
  auto copy = lg;
  expect(fl::sample(copy, greedy, {}, rng) == 1, "temperature 0 is greedy");
  fl::Sampling pen;
  pen.repeat = 4.0f;
  pen.last_n = 8;
  copy = lg;
  expect(fl::sample(copy, pen, {1}, rng) == 3, "a repeat penalty moves greedy off a seen id");
  fl::Sampling s;
  s.temperature = 0.7f;
  s.top_p = 0.9f;
  s.top_k = 20;
  std::mt19937 gen(99);
  std::uniform_real_distribution<float> d(-8.0f, 8.0f);
  std::vector<float> logits(5000);
  for (auto& v : logits) v = d(gen);
  const int kk = fl::candidate_count(s, 5000);
  std::vector<int> order(logits.size());
  for (std::size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
  std::sort(order.begin(), order.end(), [&](int a, int b) {
    return logits[static_cast<std::size_t>(a)] > logits[static_cast<std::size_t>(b)] ||
           (logits[static_cast<std::size_t>(a)] == logits[static_cast<std::size_t>(b)] && a < b);
  });
  std::vector<float> vals;
  for (int j = 0; j < kk; ++j) vals.push_back(logits[static_cast<std::size_t>(order[static_cast<std::size_t>(j)])]);
  std::mt19937_64 a(1234), b(1234);
  bool same = true;
  for (int i = 0; i < 200; ++i) {
    auto c = logits;
    same = same && fl::sample(c, s, {}, a) == fl::sample_sorted(vals.data(), order.data(), kk, s, b);
  }
  expect(same, "the GPU path's sorted candidates draw like the full sampler");
  expect(fl::request_seed(42, 1000) == 42 && fl::request_seed(0, 5) != 0 && fl::request_seed(0, 5) != fl::request_seed(0, 6),
         "explicit seeds kept, unseeded requests draw fresh seeds");
  const std::vector<std::int32_t> held{1, 2, 3, 4, 5, 6};
  expect(fl::reuse_length(held, {1, 2, 3, 9, 9}, 1024) == 3 && fl::reuse_length(held, {1, 2, 3, 9, 9}, 2) == 0,
         "reuse keeps a common prefix within the ring's spare rows");
}

void profile_fills_each_moe_layer_with_its_top_experts() {
  const std::vector<std::pair<int, int>> ranked = {{2, 7}, {0, 5}, {2, 9}, {0, 1}, {2, 7}, {46, 255}};
  const auto f = fl::fill_from_profile(ranked, 3);
  expect(f.size() == std::size_t(fl::kMoeLayers), "a list per MoE layer (47)");
  if (f.size() != std::size_t(fl::kMoeLayers)) return;
  expect(f[0] == std::vector<int>{5, 1, 0}, "tier layer 0: its ranked experts, then the lowest unranked");
  expect(f[2] == std::vector<int>{7, 9, 0}, "a pair ranked twice counts once");
  expect(f[46] == std::vector<int>{255, 0, 1}, "the last tier layer and the last expert");
  const auto bad = fl::fill_from_profile({{47, 1}, {0, 256}, {-1, 0}}, 2);
  expect(bad[0] == std::vector<int>{0, 1}, "pairs out of range are skipped");
}

void info_line_reports_the_expert_split() {
  fl::LoadReport r;
  r.slots_per_layer = 64;
  r.resident_pct = 25.0;
  r.vram_used_mib = 15500;
  r.vram_total_mib = 16376;
  r.pinned_gib = 37.42;
  r.load_seconds = 40.5;
  r.ring_rows = 1025;
  const std::string line = fl::engine_info_line(r, 262144);
  expect(line.rfind("INFO ", 0) == 0, "an INFO record");
  for (const char* want : {" expert_slots=12032", " expert_slots_primary=3008", " slots_per_layer=64", " context=262144"}) {
    expect(line.find(want) != std::string::npos, std::string{"carries"} + want);
  }
}

}  // namespace

int main(int argc, char** argv) {
  return g4_test::run_cases(argc, argv,
                            {
                                {"split_names_follow_gguf_split", split_names_follow_gguf_split},
                                {"model_guard_accepts_the_flint_shape", model_guard_accepts_the_flint_shape},
                                {"model_guard_takes_one_file_and_q8_0_weights", model_guard_takes_one_file_and_q8_0_weights},
                                {"model_guard_reads_the_rope_pattern_on_its_own", model_guard_reads_the_rope_pattern_on_its_own},
                                {"model_guard_refuses_other_shapes", model_guard_refuses_other_shapes},
                                {"cli_accepts_the_serve_flags", cli_accepts_the_serve_flags},
                                {"cli_refuses_bad_and_test_only_flags_with_serve", cli_refuses_bad_and_test_only_flags_with_serve},
                                {"gen_lines_follow_the_q36_grammar", gen_lines_follow_the_q36_grammar},
                                {"sampler_and_seeds_match_gemma4s", sampler_and_seeds_match_gemma4s},
                                {"profile_fills_each_moe_layer_with_its_top_experts", profile_fills_each_moe_layer_with_its_top_experts},
                                {"info_line_reports_the_expert_split", info_line_reports_the_expert_split},
                            });
}
