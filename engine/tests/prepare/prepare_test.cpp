// strata-prepare's unit tests on synthetic GGUFs (spec docs/superpowers/specs/2026-10-27-strata-prepare-design.md).
// The byte-for-byte check against upstream's iq_pack.py itself is tools/secure/strata_prepare_parity.py.
#include "gguf_header.hpp"
#include "iq_pack.hpp"
#include "maya_moe_pack.hpp"
#include "maya_pack.hpp"
#include "mtp_layer.hpp"
#include "pack_common.hpp"
#include "py_json.hpp"
#include "sha256.hpp"
#include "tokenizer_export.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using strata_prepare::Json;

namespace {

int failures = 0;
std::string g_exe;   // strata-prepare, from the command line

void expect(const bool ok, const std::string& what) {
  if (!ok) {
    ++failures;
    std::cerr << "FAILED: " << what << "\n";
  }
}

fs::path scratch(const std::string& name) {
  const fs::path dir = fs::temp_directory_path() / ("strata-prepare-test-" + name + "-" + std::to_string(std::random_device{}()));
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

std::string read_text(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

void write_lf(const fs::path& path, const std::string& text) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(text.data(), static_cast<std::streamsize>(text.size()));
}

bool ends_with_text(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// ---- a GGUF v3 writer -------------------------------------------------------------------------------------------

struct Tensor {
  std::string name;
  std::vector<std::uint64_t> shape;
  std::uint32_t type;
  std::vector<unsigned char> data;
};

struct Kv {
  std::string key;
  std::uint32_t type;   // 4 u32, 2 u16, 5 i32, 8 string, 6 f32, 9 array of strings / i32
  std::uint64_t integer = 0;
  float real = 0.0F;
  std::string text;
  std::vector<std::string> strings;
  std::vector<std::int32_t> ints;
};

class Writer {
 public:
  template <class T> void put(const T value) {
    const auto* p = reinterpret_cast<const unsigned char*>(&value);
    bytes.insert(bytes.end(), p, p + sizeof(T));
  }
  void put_string(const std::string& s) {
    put<std::uint64_t>(s.size());
    bytes.insert(bytes.end(), s.begin(), s.end());
  }
  std::vector<unsigned char> bytes;
};

void write_gguf(const fs::path& path, const std::vector<Kv>& kvs, const std::vector<Tensor>& tensors, const std::uint64_t alignment = 32) {
  Writer w;
  w.put<std::uint32_t>(0x46554747U);
  w.put<std::uint32_t>(3);
  w.put<std::uint64_t>(tensors.size());
  w.put<std::uint64_t>(kvs.size());
  for (const auto& kv : kvs) {
    w.put_string(kv.key);
    w.put<std::uint32_t>(kv.type);
    switch (kv.type) {
      case 2: w.put<std::uint16_t>(static_cast<std::uint16_t>(kv.integer)); break;
      case 4: w.put<std::uint32_t>(static_cast<std::uint32_t>(kv.integer)); break;
      case 5: w.put<std::int32_t>(static_cast<std::int32_t>(kv.integer)); break;
      case 6: w.put<float>(kv.real); break;
      case 8: w.put_string(kv.text); break;
      case 9:
        if (!kv.strings.empty()) {
          w.put<std::uint32_t>(8);
          w.put<std::uint64_t>(kv.strings.size());
          for (const auto& s : kv.strings) w.put_string(s);
        } else {
          w.put<std::uint32_t>(5);
          w.put<std::uint64_t>(kv.ints.size());
          for (const auto v : kv.ints) w.put<std::int32_t>(v);
        }
        break;
      default: break;
    }
  }
  std::uint64_t offset = 0;
  std::vector<std::uint64_t> offsets;
  for (const auto& t : tensors) {
    w.put_string(t.name);
    w.put<std::uint32_t>(static_cast<std::uint32_t>(t.shape.size()));
    for (const auto d : t.shape) w.put<std::uint64_t>(d);
    w.put<std::uint32_t>(t.type);
    w.put<std::uint64_t>(offset);
    offsets.push_back(offset);
    offset += (t.data.size() + alignment - 1U) / alignment * alignment;
  }
  while (w.bytes.size() % alignment != 0U) w.bytes.push_back(0);
  for (const auto& t : tensors) {
    w.bytes.insert(w.bytes.end(), t.data.begin(), t.data.end());
    while (w.bytes.size() % alignment != 0U) w.bytes.push_back(0);
  }
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(w.bytes.data()), static_cast<std::streamsize>(w.bytes.size()));
}

std::vector<unsigned char> f32_bytes(const std::vector<float>& values) {
  std::vector<unsigned char> out(values.size() * 4U);
  std::memcpy(out.data(), values.data(), out.size());
  return out;
}

std::vector<unsigned char> filled(const std::size_t size, const unsigned seed) {
  std::vector<unsigned char> out(size);
  std::mt19937 rng(seed);
  for (auto& b : out) b = static_cast<unsigned char>(rng() & 0x7fU);   // small values: no NaN or Inf in float views
  return out;
}

// A small Qwen-like model: n_embd 64, n_ff 32, 4 experts, 2 layers; experts F16 [64, 32, 4] / [32, 64, 4], routers BF16
// [64, 4], a Q8_0 token embedding (served natively), an F32 norm, an F16 q-norm (FORM F32: widened).
std::vector<Tensor> tiny_model(const int layers = 2) {
  std::vector<Tensor> t;
  t.push_back({"token_embd.weight", {64, 8}, 8, filled(8U * 2U * 34U, 1)});
  t.push_back({"output_norm.weight", {64}, 0, f32_bytes(std::vector<float>(64, 1.0F))});
  for (int l = 0; l < layers; ++l) {
    const std::string b = "blk." + std::to_string(l) + ".";
    t.push_back({b + "ffn_gate_inp.weight", {64, 4}, 30, filled(64U * 4U * 2U, 10U + static_cast<unsigned>(l))});
    t.push_back({b + "attn_q_norm.weight", {64}, 1, filled(64U * 2U, 20U + static_cast<unsigned>(l))});
    t.push_back({b + "ffn_gate_exps.weight", {64, 32, 4}, 1, filled(64U * 32U * 4U * 2U, 30U + static_cast<unsigned>(l))});
    t.push_back({b + "ffn_up_exps.weight", {64, 32, 4}, 1, filled(64U * 32U * 4U * 2U, 40U + static_cast<unsigned>(l))});
    t.push_back({b + "ffn_down_exps.weight", {32, 64, 4}, 1, filled(32U * 64U * 4U * 2U, 50U + static_cast<unsigned>(l))});
  }
  return t;
}

std::vector<Kv> arch_kv() { return {Kv{"general.architecture", 8, 0, 0.0F, "qwen4exp", {}, {}}}; }

int pack(const fs::path& gguf, const fs::path& out, std::string& log, const bool compat = false, const bool experts_bin = false) {
  std::ostringstream messages;
  const int rc = strata_prepare::iq_pack({gguf, out, compat, experts_bin}, messages);
  log = messages.str();
  return rc;
}

// ---- cases --------------------------------------------------------------------------------------------------------

void sha256_known_answers() {
  const auto hash = [](const std::string& s) {
    strata_prepare::Sha256 sha;
    sha.update(s.data(), s.size());
    return sha.hex();
  };
  expect(hash("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256 of nothing");
  expect(hash("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256 of abc");
  expect(hash(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "sha256 of a million a");
}

void python_float_repr_matches_python() {
  const std::vector<std::pair<double, const char*>> cases{
      {0.0, "0.0"}, {-0.0, "-0.0"}, {1.0, "1.0"}, {1.5, "1.5"}, {100.0, "100.0"}, {0.001, "0.001"}, {0.0001, "0.0001"},
      {1e-05, "1e-05"}, {3.0517578125e-05, "3.0517578125e-05"}, {0.000244140625, "0.000244140625"}, {1e16, "1e+16"},
      {123456789012345.6, "123456789012345.6"}, {0.1, "0.1"}, {2.5e-08, "2.5e-08"}, {1.2345e+17, "1.2345e+17"}};
  for (const auto& [value, want] : cases) {
    expect(strata_prepare::python_float_repr(value) == want, std::string{"repr "} + want + ": " + strata_prepare::python_float_repr(value));
  }
}

void json_dumps_matches_python() {
  const Json j = Json::object()
                     .set("schema", Json::integer(1))
                     .set("tool", Json::text("tools/iq_pack.py"))
                     .set("compat_bf16", Json::boolean(false))
                     .set("source_shards", Json::list().push(Json::object().set("name", Json::text("a.gguf")).set("size", Json::integer(5))))
                     .set("tensors", Json::list());
  expect(j.dumps(1) == "{\n \"schema\": 1,\n \"tool\": \"tools/iq_pack.py\",\n \"compat_bf16\": false,\n \"source_shards\": [\n  {\n   \"name\": \"a.gguf\",\n   \"size\": 5\n  }\n ],\n \"tensors\": []\n}",
         "indent=1: " + j.dumps(1));
  const Json k = Json::object().set("pack", Json::text("C:\\x\\é")).set("tokenizer", Json::null_value()).set("err", Json::real(1e-05));
  expect(k.dumps(2) == "{\n  \"pack\": \"C:\\\\x\\\\\\u00e9\",\n  \"tokenizer\": null,\n  \"err\": 1e-05\n}", "indent=2, ensure_ascii: " + k.dumps(2));
  expect(Json::object().set("want", Json::object().set("a", Json::integer(1))).set("layers", Json::integer(3)).dumps(-1) ==
             "{\"want\": {\"a\": 1}, \"layers\": 3}",
         "one line");
}

void half_and_bf16_round_to_even() {
  const auto half = [](const float v) {
    bool overflow = false;
    const auto h = strata_prepare::f16_bits(v, overflow);
    return std::pair<unsigned, bool>{h, overflow};
  };
  expect(half(1.0F).first == 0x3c00U && half(65504.0F).first == 0x7bffU && !half(65504.0F).second, "1.0 and the largest half");
  expect(half(65520.0F).second, "65520 overflows (rounds to inf)");
  expect(half(0.1F).first == 0x2e66U, "0.1 rounds to nearest");
  expect(half(std::ldexp(1.0F, -24)).first == 0x0001U && half(std::ldexp(1.0F, -25)).first == 0x0000U &&
             half(std::ldexp(3.0F, -26)).first == 0x0001U,
         "subnormals: 2^-25 ties to even (0), 1.5 x 2^-25 rounds up");
  const auto bf = [](const std::uint32_t bits) {
    float f = 0.0F;
    std::memcpy(&f, &bits, 4);
    return static_cast<unsigned>(strata_prepare::bf16_bits(f));
  };
  expect(bf(0x3f800000U) == 0x3f80U && bf(0x3f808000U) == 0x3f80U && bf(0x3f818000U) == 0x3f82U && bf(0x3f808001U) == 0x3f81U,
         "bf16: exact, ties to even both ways, above the tie");
}

void gguf_header_reads_like_gguf_reader() {
  const auto dir = scratch("header");
  std::vector<Kv> kvs = arch_kv();
  kvs.push_back(Kv{"general.alignment", 4, 64, 0.0F, "", {}, {}});
  kvs.push_back(Kv{"split.count", 2, 1, 0.0F, "", {}, {}});
  kvs.push_back(Kv{"names", 9, 0, 0.0F, "", {"a", "b"}, {}});
  write_gguf(dir / "m.gguf", kvs, {{"w", {32, 2}, 8, filled(68, 3)}, {"n", {5}, 0, f32_bytes({1, 2, 3, 4, 5})}}, 64);
  const strata_prepare::GgufHeader g(dir / "m.gguf");
  expect(g.alignment == 64U && g.data_start % 64U == 0U && g.tensors.size() == 2U, "alignment, data start, tensors");
  expect(g.tensors[0].type_name == "Q8_0" && g.tensors[0].expected_bytes() == std::optional<std::uint64_t>{68U} &&
             g.tensors[1].offset == 128U,   // 68 bytes padded to the 64-byte alignment
         "type names, block geometry, offsets");
  expect(g.get("split.count") != nullptr && g.get("split.count")->integer == 1 && g.get("names")->items.size() == 2U,
         "metadata: integers and lists");
  expect(strata_prepare::type_name_of(42) == "Q2_0" && strata_prepare::type_name_of(999) == "type999", "Q2_0 and an unknown type");
}

void single_file_pack_tables() {
  const auto dir = scratch("single");
  write_gguf(dir / "tiny.gguf", arch_kv(), tiny_model());
  std::string log;
  const auto out = dir / "pack";
  expect(pack(dir / "tiny.gguf", out, log) == 0, "the pack is made: " + log);
  const std::string index = read_text(out / "index.txt");
  expect(index.rfind("# strata pack index v3 -- generated by tools/iq_pack.py (native experts) from tiny.gguf\n# align 64 pool ", 0) == 0U,
         "the index header: " + index.substr(0, 160));
  expect(index.find("\ntoken_embd.weight 0 0 0 0 0 0 64 8 8 0 32 0 0 0 0 0 0 0\n") != std::string::npos, "a quantized tensor is served natively");
  expect(index.find("\noutput_norm.weight 0 2 0 256 0 256 64 0 0 0 1 0 0 0 0 0 0 0\n") != std::string::npos, "an F32 tensor as stored, at 0");
  expect(index.find("\nblk.0.ffn_gate_inp.weight 0 4 256 512 256 512 64 4 0 0 1 0 0 0 0 0 0 0\n") != std::string::npos,
         "the BF16 router as stored, after the norm: " + index);
  expect(index.find("\nblk.0.attn_q_norm.weight 0 2 768 256 768 256 64 0 0 0 1 0 0 0 0 0 0 0\n") != std::string::npos,
         "the F16 q-norm widened to F32 (FORM), placed after the router: " + index);
  const std::string experts = read_text(out / "native_experts.txt");
  expect(experts.rfind("# strata native experts v3: layer gu_type d_type offset blob_bytes gate_off up_off down_off [shard] (n_expert 4, total ", 0) == 0U,
         "the v3 table: " + experts);
  const strata_prepare::GgufHeader g(dir / "tiny.gguf");
  std::uint64_t gate = 0;
  for (const auto& t : g.tensors) {
    if (t.name == "blk.1.ffn_gate_exps.weight") gate = g.data_start + t.offset;
  }
  expect(experts.find("\n1 1 1 49152 12288 " + std::to_string(gate) + " ") != std::string::npos, "layer 1: offset, blob, absolute offsets: " + experts);
  const std::string conversions = read_text(out / "conversions.json");
  expect(conversions.find("\"method\": \"f16->f32 widening\"") != std::string::npos && conversions.find("\"exact\": true") != std::string::npos,
         "conversions.json records the widening: " + conversions);
  expect(!fs::exists(out / "compat-bf16.json") && !fs::exists(out / "experts.bin"), "nothing more without the flags");
}

void split_layer_pack_is_v4() {
  const auto dir = scratch("split");
  auto all = tiny_model();
  // layer 1's down tensor in shard 2: the per-role column, v4
  std::vector<Tensor> first;
  std::vector<Tensor> second;
  for (auto& t : all) (t.name == "blk.1.ffn_down_exps.weight" ? second : first).push_back(t);
  auto kv1 = arch_kv();
  kv1.push_back(Kv{"split.count", 2, 2, 0.0F, "", {}, {}});
  kv1.push_back(Kv{"split.no", 2, 0, 0.0F, "", {}, {}});
  write_gguf(dir / "m-00001-of-00002.gguf", kv1, first);
  write_gguf(dir / "m-00002-of-00002.gguf", {Kv{"split.count", 2, 2, 0.0F, "", {}, {}}, Kv{"split.no", 2, 1, 0.0F, "", {}, {}}}, second);
  std::string log;
  expect(pack(dir / "m-00001-of-00002.gguf", dir / "pack", log) == 0, "a split model packs: " + log);
  const std::string experts = read_text(dir / "pack" / "native_experts.txt");
  expect(experts.rfind("# strata native experts v4:", 0) == 0U && experts.find(",,m-00002-of-00002.gguf\n") != std::string::npos,
         "v4, the down role in shard 2: " + experts);
  fs::remove(dir / "m-00002-of-00002.gguf");
  expect(pack(dir / "m-00001-of-00002.gguf", dir / "pack2", log) == 1 && log.find("missing model shards") != std::string::npos,
         "a missing shard is refused: " + log);
}

void conversions_and_refusals() {
  const auto dir = scratch("convert");
  auto model = tiny_model(1);
  // an F32 router whose values are BF16: converted exactly
  model[2] = Tensor{"blk.0.ffn_gate_inp.weight", {64, 4}, 0, f32_bytes(std::vector<float>(256, 0.5F))};
  write_gguf(dir / "exact.gguf", arch_kv(), model);
  std::string log;
  expect(pack(dir / "exact.gguf", dir / "p1", log) == 0, "F32 BF16 values convert: " + log);
  expect(read_text(dir / "p1" / "conversions.json").find("\"method\": \"f32->bf16: the values are BF16's (low 16 bits zero)\"") != std::string::npos,
         "recorded as exact");
  // values that are not BF16: refused without --compat-bf16, rounded with it
  model[2] = Tensor{"blk.0.ffn_gate_inp.weight", {64, 4}, 0, f32_bytes(std::vector<float>(256, 0.1F))};
  write_gguf(dir / "round.gguf", arch_kv(), model);
  expect(pack(dir / "round.gguf", dir / "p2", log) == 1 &&
             log.find("is F32 with values that are not BF16, but the engine requires BF16; use --compat-bf16") != std::string::npos,
         "refused without the flag: " + log);
  expect(pack(dir / "round.gguf", dir / "p3", log, true) == 0 &&
             read_text(dir / "p3" / "conversions.json").find("\"method\": \"f32->bf16 round-to-nearest-even (--compat-bf16)\"") != std::string::npos,
         "rounded with it: " + log);
  // a quantized FORM tensor without the flag, and a non-expert tensor of three dimensions
  auto quantized = tiny_model(1);
  quantized.push_back({"blk.0.ssm_alpha.weight", {32, 2}, 8, filled(68, 9)});
  write_gguf(dir / "q.gguf", arch_kv(), quantized);
  expect(pack(dir / "q.gguf", dir / "p4", log) == 1 &&
             log.find("blk.0.ssm_alpha.weight is Q8_0, but the engine requires BF16; use --compat-bf16") != std::string::npos,
         "a quantized BF16 projection needs the flag: " + log);
  auto cube = tiny_model(1);
  cube.push_back({"blk.0.odd.weight", {2, 2, 2}, 0, f32_bytes(std::vector<float>(8, 1.0F))});
  write_gguf(dir / "cube.gguf", arch_kv(), cube);
  expect(pack(dir / "cube.gguf", dir / "p5", log) == 1 && log.find("has 3 dimensions; the index holds two") != std::string::npos,
         "three dimensions refused: " + log);
}

void compat_bf16_dequantizes() {
  const auto dir = scratch("compat");
  auto model = tiny_model(1);
  // a Q8_0 hyper-connection projection: one block, scale 0.5 (F16 0x3800), values 1..32 -> 0.5..16.0
  std::vector<unsigned char> block(34);
  block[0] = 0x00;
  block[1] = 0x38;
  for (int i = 0; i < 32; ++i) block[2U + static_cast<std::size_t>(i)] = static_cast<unsigned char>(i + 1);
  model.push_back({"blk.0.hc_attn_down.weight", {32, 1}, 8, block});
  write_gguf(dir / "c.gguf", arch_kv(), model);
  std::string log;
  expect(pack(dir / "c.gguf", dir / "pack", log, true) == 0, "packs with --compat-bf16: " + log);
  const std::string conversions = read_text(dir / "pack" / "conversions.json");
  expect(conversions.find("\"method\": \"dequantize->bf16 round-to-nearest-even (--compat-bf16)\"") != std::string::npos &&
             conversions.find("\"compat_bf16\": true") != std::string::npos,
         "recorded: " + conversions);
  const std::string compat = read_text(dir / "pack" / "compat-bf16.json");
  expect(compat.find("\"name\": \"blk.0.hc_attn_down.weight\"") != std::string::npos && compat.find("\"bytes\": 64") != std::string::npos &&
             compat.find("\"rounding\": \"nearest-even\"") != std::string::npos,
         "compat-bf16.json: " + compat);
  // its BF16 values in dense.bin: 0.5, 1.0, ... (exact in BF16)
  const std::string index = read_text(dir / "pack" / "index.txt");
  const auto at = index.find("\nblk.0.hc_attn_down.weight 0 4 ");
  expect(at != std::string::npos, "indexed as BF16: " + index);
  if (at == std::string::npos) return;
  std::istringstream row(index.substr(at + 1));
  std::string name, file, kind, offset;
  row >> name >> file >> kind >> offset;
  const std::string dense = read_text(dir / "pack" / "dense.bin");
  const auto o = std::stoull(offset);
  std::uint16_t first = 0;
  std::uint16_t last = 0;
  std::memcpy(&first, dense.data() + o, 2);
  std::memcpy(&last, dense.data() + o + 62, 2);
  expect(first == 0x3f00U && last == 0x4180U, "0.5 and 16.0 in BF16");
}

void experts_bin_blobs() {
  const auto dir = scratch("experts");
  write_gguf(dir / "e.gguf", arch_kv(), tiny_model());
  std::string log;
  expect(pack(dir / "e.gguf", dir / "pack", log, false, true) == 0, "packs with --experts-bin: " + log);
  const std::string bin = read_text(dir / "pack" / "experts.bin");
  const std::uint64_t blob = 64U * 32U * 2U * 2U + 32U * 64U * 2U;   // gate + up + down of one expert (F16)
  expect(bin.size() == 2U * 4U * blob, "two layers of four experts");
  if (bin.size() != 2U * 4U * blob) return;
  // expert 2 of layer 1: its gate slice first
  const auto gate = filled(64U * 32U * 4U * 2U, 31);
  expect(std::memcmp(bin.data() + 4U * blob + 2U * blob, gate.data() + 2U * 64U * 32U * 2U, 64U * 32U * 2U) == 0, "the gate slice of expert 2, layer 1");
  const std::string sidecar = read_text(dir / "pack" / "experts.bin.src.json");
  expect(sidecar.find("\"native_experts_sha256\": ") != std::string::npos && sidecar.find("\"bytes\": " + std::to_string(bin.size())) != std::string::npos,
         "the sidecar: " + sidecar);
  // a second run reuses it
  expect(pack(dir / "e.gguf", dir / "pack", log, false, true) == 0 && log.find("not rewritten") != std::string::npos, "reused: " + log);
}

std::string run(const std::string& command) {
  const std::string full = "\"" + command + "\"";   // cmd /c strips one pair of quotes
  std::string out;
#if defined(_WIN32)
  FILE* p = _popen(full.c_str(), "r");
#else
  FILE* p = popen(command.c_str(), "r");
#endif
  if (p == nullptr) return out;
  char buffer[512];
  while (std::fgets(buffer, sizeof(buffer), p) != nullptr) out += buffer;
#if defined(_WIN32)
  _pclose(p);
#else
  pclose(p);
#endif
  return out;
}

void download_protocol() {
  if (g_exe.empty()) {
    expect(false, "the strata-prepare path is an argument");
    return;
  }
  const auto dir = scratch("download");
  auto model = tiny_model(1);
  model.push_back({"per_layer_token_embd.weight", {64, 2}, 8, filled(4U * 34U, 5)});
  write_gguf(dir / "d.gguf", arch_kv(), model);
  const std::string exe = "\"" + g_exe + "\"";
  const std::string got = run(exe + " download --gguf \"" + (dir / "d.gguf").string() + "\" --out \"" + (dir / "pack").string() + "\" 2>&1");
  expect(got.find("STEP pack\n") != std::string::npos && got.find("STEP ple\n") != std::string::npos && got.find("DONE\n") != std::string::npos,
         "the protocol: " + got);
  const std::string prepared = read_text(dir / "pack" / "prepared.json");
  expect(prepared.find("\"tokenizer\": null") != std::string::npos && prepared.find("\"shards\": 1") != std::string::npos &&
             prepared.find("d.gguf\"") != std::string::npos,
         "prepared.json: the pack, no tokenizer, the PLE shard: " + prepared);
  expect(run(exe + " download --gguf x") == "ERROR bad_arguments\n", "bad arguments");
  expect(run(exe + " download --gguf \"" + (dir / "x.bin").string() + "\" --out \"" + (dir / "o").string() + "\"") == "ERROR not_gguf\n",
         "not a GGUF");
  expect(run(exe + " download --gguf \"" + (dir / "m-00002-of-00003.gguf").string() + "\" --out o") == "ERROR missing_gguf\n",
         "a missing file");
}

// ---- Project Maya's tools (v1.1.30): strata_tokenizer.py and its fork of iq_pack.py --------------------------------

#if defined(_WIN32)
const std::string kLe = "\r\n";   // Path.write_text's "\n"
#else
const std::string kLe = "\n";
#endif

std::string lines(const std::vector<std::string>& parts) {
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) out += (i ? kLe : "") + parts[i];
  return out;
}

void utf8_replace_like_python() {
  // bytes.decode("utf-8", "replace") (CPython 3.12, checked): each maximal invalid subpart is one U+FFFD
  const std::string r = "\xEF\xBF\xBD";
  const std::vector<std::pair<std::string, std::string>> cases{
      {"abc", "abc"},           {"\xc3\xa9", "\xc3\xa9"},  {"bad\xff", "bad" + r},           {"\xe2\x82", r},
      {"\xe2\x82x", r + "x"},   {"\xed\xa0\x80", r + r + r}, {"\xf0\x9f\x98", r},              {"\xc0\x80", r + r},
      {"\xf4\x90\x80\x80", r + r + r + r}, {"\xe0\x80\xaf", r + r + r}, {"a\xf0\x9f\x98\x80" "b", "a\xf0\x9f\x98\x80" "b"}};
  for (const auto& [in, want] : cases) expect(strata_prepare::utf8_replace(in) == want, "utf8_replace of a " + std::to_string(in.size()) + "-byte case");
  // json.dumps(..., ensure_ascii=False) and repr(), as Python writes them
  expect(strata_prepare::python_json_string("q\"\\\x01\x7f\xc3\xa9\xe2\x80\xa8", false) == "\"q\\\"\\\\\\u0001\x7f\xc3\xa9\xe2\x80\xa8\"",
         "a JSON string without ensure_ascii");
  expect(strata_prepare::python_repr("it's") == "\"it's\"" && strata_prepare::python_repr("a\"b") == "'a\"b'" &&
             strata_prepare::python_repr("glm4") == "'glm4'",
         "repr() of a str");
}

// The tokenizer metadata of a small byte-level BPE model: a non-ASCII token, a token needing JSON escapes, a control
// token, a token of invalid UTF-8 (U+FFFD once decoded), a merge listed twice (its later rank counts), the specials in
// file order (eos before bos), and a chat template with a CRLF in it.
std::vector<Kv> tokenizer_kv(const std::string& pre = "glm4") {
  std::vector<Kv> kvs{Kv{"tokenizer.ggml.model", 8, 0, 0.0F, "gpt2", {}, {}}};
  if (!pre.empty()) kvs.push_back(Kv{"tokenizer.ggml.pre", 8, 0, 0.0F, pre, {}, {}});
  kvs.push_back(Kv{"tokenizer.ggml.tokens", 9, 0, 0.0F, "", {"a", "b", "ab", "\xc3\xa9", "q\"\\", "<|end|>", "\x01", "bad\xff"}, {}});
  kvs.push_back(Kv{"tokenizer.ggml.merges", 9, 0, 0.0F, "", {"a b", "ab a", "a b"}, {}});
  kvs.push_back(Kv{"tokenizer.ggml.token_type", 9, 0, 0.0F, "", {}, {1, 1, 1, 1, 1, 3, 1, 1}});
  kvs.push_back(Kv{"tokenizer.ggml.eos_token_id", 4, 5, 0.0F, "", {}, {}});
  kvs.push_back(Kv{"tokenizer.ggml.bos_token_id", 4, 0, 0.0F, "", {}, {}});
  kvs.push_back(Kv{"tokenizer.chat_template", 8, 0, 0.0F, "{{ x }}\r\nline\n", {}, {}});
  return kvs;
}

int tokenizer(const fs::path& gguf, const fs::path& out, std::string& log) {
  std::ostringstream messages;
  const int rc = strata_prepare::export_tokenizer(gguf, out, messages);
  log = messages.str();
  return rc;
}

const char* kGlmPatternJson =
    R"x("(?:'[sS]|'[tT]|'[rR][eE]|'[vV][eE]|'[mM]|'[lL][lL]|'[dD])|[^\\r\\n\\p{L}\\p{N}]?\\p{L}+|\\p{N}{1,3}| ?[^\\s\\p{L}\\p{N}]+[\\r\\n]*|\\s*[\\r\\n]+|\\s+(?!\\S)|\\s+")x";
const char* kQwen35PatternJson =
    R"x("(?:'[sS]|'[tT]|'[rR][eE]|'[vV][eE]|'[mM]|'[lL][lL]|'[dD])|[^\\r\\n\\p{L}\\p{N}]?[\\p{L}\\p{M}]+|\\p{N}| ?[^\\s\\p{L}\\p{M}\\p{N}]+[\\r\\n]*|\\s*[\\r\\n]+|\\s+(?!\\S)|\\s+")x";
// RAON_PATTERN as json.dumps writes it (the existing raon1 pack's tokenizer.json, which the Python wrote, holds it so):
// its CJK class as escape text, the backslash doubled once more by JSON
const char* kRaonPatternJson =
    "\" ?[\\\\u4e00-\\\\u9fa5\\\\u3040-\\\\u309f\\\\u30a0-\\\\u30ff]+| ?(?:(?:(?![\\\\u4e00-\\\\u9fa5\\\\u3040-\\\\u309f\\\\u30a0-\\\\u30ff])"
    R"x(\\p{L})\\p{M}*)+)x"
    "(?: (?:(?:(?![\\\\u4e00-\\\\u9fa5\\\\u3040-\\\\u309f\\\\u30a0-\\\\u30ff])"
    R"x(\\p{L})\\p{M}*)+)*|\\p{N}{1,3}(?=(?:\\p{N}{3})*(?!\\p{N}))| ?[^\\s\\p{L}\\p{N}]+[\\r\\n/]*|\\s*[\\r\\n]+|\\s+(?!\\S)|\\s+")x";

void tokenizer_export_like_strata_tokenizer() {
  const auto dir = scratch("tokenizer");
  auto kvs = arch_kv();
  for (const auto& kv : tokenizer_kv()) kvs.push_back(kv);
  const std::vector<Tensor> one{{"w", {32}, 0, f32_bytes(std::vector<float>(32, 1.0F))}};
  write_gguf(dir / "t.gguf", kvs, one);
  std::string log;
  expect(tokenizer(dir / "t.gguf", dir, log) == 0, "exported: " + log);
  const auto t = dir / "tokenizer";
  expect(read_text(t / "vocab.json") ==
             "{\"a\": 0, \"b\": 1, \"ab\": 2, \"\xc3\xa9\": 3, \"q\\\"\\\\\": 4, \"<|end|>\": 5, \"\\u0001\": 6, \"bad\xEF\xBF\xBD\": 7}",
         "vocab.json: json.dumps(ids, ensure_ascii=False): " + read_text(t / "vocab.json"));
  expect(read_text(t / "merges.txt") == "ab a" + kLe + "a b", "merges.txt: by rank, the repeated merge at its later rank");
  expect(read_text(t / "token_type.json") == "[1, 1, 1, 1, 1, 3, 1, 1]", "token_type.json");
  const std::string want = lines({"{", " \"model\": \"gpt2\",", " \"pre\": \"glm4\",", " \"vocab_size\": 8,", " \"n_merges\": 2,",
                                  " \"special_ids\": {", "  \"tokenizer.ggml.eos_token_id\": 5,", "  \"tokenizer.ggml.bos_token_id\": 0", " },",
                                  " \"add_bos_token\": false,", " \"ignore_merges\": true,", std::string{" \"pre_pattern\": "} + kGlmPatternJson + ",",
                                  " \"pre_pattern_source\": \".ref/llama.cpp src/llama-vocab.cpp L406 (LLAMA_VOCAB_PRE_TYPE_CHATGLM4; glm4/glm5 set ignore_merges)\"",
                                  "}"});
  expect(read_text(t / "tokenizer.json") == want, "tokenizer.json: " + read_text(t / "tokenizer.json"));
  expect(read_text(t / "chat_template.jinja") == "{{ x }}\r\nline\n", "the chat template as it is (newline=\"\\n\")");
  expect(log.find("tokenizer/: vocab 8, merges 2, pre glm4, specials {'tokenizer.ggml.eos_token_id': 5, 'tokenizer.ggml.bos_token_id': 0}") !=
             std::string::npos,
         "the report: " + log);
  // no pre (qwen35, its pattern), no token types (null), no template (no file)
  std::vector<Kv> plain = arch_kv();
  for (const auto& kv : tokenizer_kv("")) {
    if (kv.key != "tokenizer.ggml.token_type" && kv.key != "tokenizer.chat_template") plain.push_back(kv);
  }
  write_gguf(dir / "q.gguf", plain, one);
  expect(tokenizer(dir / "q.gguf", dir / "q", log) == 0, "a qwen35 tokenizer: " + log);
  const std::string q = read_text(dir / "q" / "tokenizer" / "tokenizer.json");
  expect(q.find(" \"pre\": \"qwen35\",") != std::string::npos && q.find(" \"ignore_merges\": false,") != std::string::npos &&
             q.find(std::string{" \"pre_pattern\": "} + kQwen35PatternJson + ",") != std::string::npos &&
             q.find("L396 (LLAMA_VOCAB_PRE_TYPE_QWEN35)\"") != std::string::npos,
         "qwen35's pattern: " + q);
  expect(read_text(dir / "q" / "tokenizer" / "token_type.json") == "null" && !fs::exists(dir / "q" / "tokenizer" / "chat_template.jinja"),
         "token types null, no template file");
}

void tokenizer_export_refusals() {
  const auto dir = scratch("tokenizer-refused");
  const std::vector<Tensor> one{{"w", {32}, 0, f32_bytes(std::vector<float>(32, 1.0F))}};
  const auto refused = [&](const std::string& name, const std::function<void(std::vector<Kv>&)>& change, const std::string& message) {
    auto kvs = arch_kv();
    for (const auto& kv : tokenizer_kv()) kvs.push_back(kv);
    change(kvs);
    write_gguf(dir / (name + ".gguf"), kvs, one);
    std::string log;
    expect(tokenizer(dir / (name + ".gguf"), dir / name, log) == 1 && log.find(message) != std::string::npos, name + ": " + log);
    expect(fs::is_directory(dir / name / "tokenizer") && !fs::exists(dir / name / "tokenizer" / "vocab.json"),
           name + ": tokenizer/ made first, nothing written in it");
  };
  const auto set = [](std::vector<Kv>& kvs, const std::string& key, const std::function<void(Kv&)>& f) {
    for (auto& kv : kvs) {
      if (kv.key == key) f(kv);
    }
  };
  refused("no-merges", [](std::vector<Kv>& kvs) { std::erase_if(kvs, [](const Kv& kv) { return kv.key == "tokenizer.ggml.merges"; }); },
          "GGUF is missing tokenizer.ggml.merges; it does not carry a tokenizer");
  refused("llama", [&](std::vector<Kv>& kvs) { set(kvs, "tokenizer.ggml.model", [](Kv& kv) { kv.text = "llama"; }); },
          "expected a byte-level BPE tokenizer (model 'gpt2'), got 'llama'");
  refused("pre", [&](std::vector<Kv>& kvs) { set(kvs, "tokenizer.ggml.pre", [](Kv& kv) { kv.text = "llama3"; }); },
          "the 'llama3' pre-tokenizer is not transcribed; known: qwen35, glm4, glm5, deepseek3, minimax, raon\n");
  refused("duplicate", [&](std::vector<Kv>& kvs) { set(kvs, "tokenizer.ggml.tokens", [](Kv& kv) { kv.strings[1] = "a"; }); },
          "vocabulary has duplicate tokens: 8 entries, 7 unique");
  refused("pair", [&](std::vector<Kv>& kvs) { set(kvs, "tokenizer.ggml.merges", [](Kv& kv) { kv.strings[0] = "a  b"; }); },
          "merge 0 is not a pair: 'a  b'");
  refused("outside", [&](std::vector<Kv>& kvs) { set(kvs, "tokenizer.ggml.merges", [](Kv& kv) { kv.strings[1] = "a z"; }); },
          "merge 1 names a token outside the vocabulary: 'a z'");
}

std::vector<unsigned char> f16_ones(const std::size_t count) {
  std::vector<unsigned char> out;
  for (std::size_t i = 0; i < count; ++i) {
    out.push_back(0x00);
    out.push_back(0x3c);   // 1.0
  }
  return out;
}

std::vector<Kv> glm_kv(const std::string& arch = "glm5-next", const bool block_count = true) {
  std::vector<Kv> kvs{Kv{"general.architecture", 8, 0, 0.0F, arch, {}, {}}};
  if (block_count) kvs.push_back(Kv{arch + ".block_count", 4, 3, 0.0F, "", {}, {}});
  kvs.push_back(Kv{arch + ".nextn_predict_layers", 4, 1, 0.0F, "", {}, {}});
  kvs.push_back(Kv{arch + ".leading_dense_block_count", 4, 1, 0.0F, "", {}, {}});
  for (const auto& kv : tokenizer_kv()) kvs.push_back(kv);
  return kvs;
}

void experts_of(std::vector<Tensor>& t, const int l) {
  const std::string b = "blk." + std::to_string(l) + ".";
  t.push_back({b + "ffn_gate_exps.weight", {64, 32, 4}, 1, f16_ones(64U * 32U * 4U)});
  t.push_back({b + "ffn_up_exps.weight", {64, 32, 4}, 1, f16_ones(64U * 32U * 4U)});
  t.push_back({b + "ffn_down_exps.weight", {32, 64, 4}, 1, f16_ones(32U * 64U * 4U)});
}

// A small GLM-5.3-like model: block_count 3 counting one NextN block (blk.2) and one leading dense block (blk.0), so the
// servable MoE trunk is layer 1 alone. n_embd 64, 4 F16 experts; a 3-D absorbed projection and a 4-D conv weight with a
// trailing 1, F32 router and bias (all on the BF16 list), quantized tensors of the native set.
std::vector<Tensor> glm_model(const bool odd_norm = false) {
  std::vector<Tensor> t;
  t.push_back({"token_embd.weight", {64, 8}, 8, filled(8U * 2U * 34U, 1)});
  t.push_back({"output_norm.weight", {64}, 0, f32_bytes(std::vector<float>(64, 1.0F))});
  t.push_back({"blk.0.attn_norm.weight", {64}, 0, f32_bytes(std::vector<float>(64, 1.0F))});
  t.push_back({"blk.0.ffn_gate.weight", {64, 32}, 8, filled(32U * 2U * 34U, 2)});
  t.push_back({"blk.0.attn_k_b.weight", {4, 2, 8}, 1, f16_ones(64)});
  t.push_back({"blk.1.ffn_gate_inp.weight", {64, 4}, 0, f32_bytes(std::vector<float>(256, 0.1F))});
  t.push_back({"blk.1.exp_probs_b.bias", {4}, 0, f32_bytes({0.5F, 1.0F, 2.0F, 4.0F})});
  t.push_back({"blk.1.ssm_conv1d_q.weight", {4, 1, 16, 1}, 0, f32_bytes(std::vector<float>(64, 0.25F))});
  t.push_back({"blk.1.attn_q_a.weight", {64, 32}, 8, filled(32U * 2U * 34U, 3)});
  if (odd_norm) t.push_back({"blk.1.attn_norm.weight", {64}, 8, filled(2U * 34U, 4)});
  experts_of(t, 1);
  t.push_back({"blk.2.ffn_gate_inp.weight", {64, 4}, 0, f32_bytes(std::vector<float>(256, 0.1F))});
  t.push_back({"blk.2.eh_proj.weight", {64, 64}, 8, filled(64U * 2U * 34U, 5)});
  experts_of(t, 2);
  return t;
}

int maya_pack(const fs::path& gguf, const fs::path& out, std::string& log, const bool compat = true) {
  std::ostringstream messages;
  const int rc = strata_prepare::maya_iq_pack({gguf, out, compat, false}, messages);
  log = messages.str();
  return rc;
}

std::uint64_t absolute_offset(const fs::path& gguf, const std::string& name) {
  const strata_prepare::GgufHeader g(gguf);
  for (const auto& t : g.tensors) {
    if (t.name == name) return g.data_start + t.offset;
  }
  return 0;
}

void maya_glm_pack_tables() {
  const auto dir = scratch("maya-glm");
  write_gguf(dir / "g.gguf", glm_kv(), glm_model());
  std::string log;
  const auto out = dir / "pack";
  expect(maya_pack(dir / "g.gguf", out, log) == 0, "the GLM pack is made: " + log);
  const std::string index = read_text(out / "index.txt");
  expect(index.rfind("# strata pack index v3 -- generated by tools/iq_pack.py (native experts) from g.gguf\n# align 64 pool 1344 tensors 9\n", 0) == 0U,
         "the index header: the NextN block and the experts left out: " + index);
  for (const char* row : {"\ntoken_embd.weight 0 0 0 0 0 0 64 8 8 0 32 0 0 0 0 0 0 0\n", "\noutput_norm.weight 0 2 0 256 0 256 64 0 0 0 1 0 0 0 0 0 0 0\n",
                          "\nblk.0.ffn_gate.weight 0 0 0 0 512 0 64 32 8 0 32 0 0 0 0 0 0 0\n",
                          "\nblk.0.attn_k_b.weight 0 4 512 128 512 128 8 8 0 0 1 0 0 0 0 0 0 0\n",
                          "\nblk.1.ffn_gate_inp.weight 0 4 640 512 640 512 64 4 0 0 1 0 0 0 0 0 0 0\n",
                          "\nblk.1.exp_probs_b.bias 0 4 1152 8 1152 8 4 0 0 0 1 0 0 0 0 0 0 0\n",
                          "\nblk.1.ssm_conv1d_q.weight 0 4 1216 128 1216 128 4 16 0 0 1 0 0 0 0 0 0 0\n",
                          "\nblk.1.attn_q_a.weight 0 0 0 0 1344 0 64 32 8 0 32 0 0 0 0 0 0 0\n"}) {
    expect(index.find(row) != std::string::npos, std::string{"index row"} + row);
  }
  const std::string dense = read_text(out / "dense.bin");
  expect(dense.size() == 1344U, "dense.bin: the float rows, 64-aligned");
  std::uint16_t k_b = 0;
  std::uint16_t router = 0;
  if (dense.size() == 1344U) {
    std::memcpy(&k_b, dense.data() + 512, 2);
    std::memcpy(&router, dense.data() + 640, 2);
  }
  expect(k_b == 0x3f80U && router == 0x3dcdU, "F16 1.0 and F32 0.1 as BF16, rounded to nearest-even");
  const std::string want_experts =
      "# strata native experts v3: layer gu_type d_type offset blob_bytes gate_off up_off down_off [shard | gate_shard up_shard down_shard] "
      "(n_expert 4, total 49152; absolute offsets in g.gguf, or in the named shard(s) beside it)\n1 1 1 0 12288 " +
      std::to_string(absolute_offset(dir / "g.gguf", "blk.1.ffn_gate_exps.weight")) + " " +
      std::to_string(absolute_offset(dir / "g.gguf", "blk.1.ffn_up_exps.weight")) + " " +
      std::to_string(absolute_offset(dir / "g.gguf", "blk.1.ffn_down_exps.weight")) + "\n";
  expect(read_text(out / "native_experts.txt") == want_experts, "Maya's v3 table, the MoE trunk only: " + read_text(out / "native_experts.txt"));
  const std::string compat = read_text(out / "compat-bf16.json");
  expect(compat.find("\"name\": \"blk.0.attn_k_b.weight\"," + kLe + "      \"source_type\": \"F16\"," + kLe + "      \"bytes\": 128") != std::string::npos &&
             compat.find("\"rounding\": \"nearest-even\"") != std::string::npos,
         "compat-bf16.json: " + compat);
  expect(!fs::exists(out / "conversions.json") && !fs::exists(out / "experts.bin"), "no conversions.json in Maya's pack, no experts.bin");
  expect(read_text(out / "tokenizer" / "vocab.json").rfind("{\"a\": 0, ", 0) == 0U, "the tokenizer exported into the pack");
  expect(log.find("NextN/MTP: 2 tensors of blk.2+ left out of the pack") != std::string::npos &&
             log.find("glm5-next: 3 NextN expert tensors present (blk.2.*); the trunk keeps layers 0..1") != std::string::npos,
         "the NextN block reported: " + log);
  expect(maya_pack(dir / "g.gguf", out, log) == 0 && log.find("tokenizer/: vocab") == std::string::npos, "a second run keeps tokenizer/: " + log);
}

void maya_glm_refusals() {
  const auto dir = scratch("maya-refused");
  std::string log;
  write_gguf(dir / "llama.gguf", glm_kv("llama"), glm_model());
  expect(maya_pack(dir / "llama.gguf", dir / "p1", log) == 1 &&
             log.find("general.architecture is 'llama'; the packer knows qwen4exp and glm5-next and glm5next and raon1\n") != std::string::npos,
         "another architecture: " + log);
  write_gguf(dir / "nobc.gguf", glm_kv("glm5-next", false), glm_model());
  expect(maya_pack(dir / "nobc.gguf", dir / "p2", log) == 1 &&
             log.find("glm5-next: glm5-next.block_count is missing; cannot tell the trunk from the NextN block") != std::string::npos,
         "no block_count: " + log);
  write_gguf(dir / "g.gguf", glm_kv(), glm_model());
  expect(maya_pack(dir / "g.gguf", dir / "p3", log, false) == 1 &&
             log.find("blk.0.attn_k_b.weight is F16, but the engine requires BF16; use --compat-bf16") != std::string::npos &&
             !fs::exists(dir / "p3" / "dense.bin"),
         "without --compat-bf16, before anything is written: " + log);
  write_gguf(dir / "odd.gguf", glm_kv(), glm_model(true));
  expect(maya_pack(dir / "odd.gguf", dir / "p4", log) == 1 &&
             log.find("blk.1.attn_norm.weight is Q8_0: not on the BF16 upgrade list nor the glm5-next native set (GLM5NEXT_NATIVE_OK); "
                      "refusing to pack it silently") != std::string::npos,
         "a quantized tensor outside the native set fails closed: " + log);
  write_gguf(dir / "unsloth.gguf", glm_kv("glm5next"), glm_model());
  expect(maya_pack(dir / "unsloth.gguf", dir / "p5", log) == 0 && log.find("glm5next: block_count 3, nextn 1 -> trunk layers 0..1") != std::string::npos,
         "unsloth's spelling, its own key prefix: " + log);
}

// Q8_0 blocks of known values: scale 1.0 and quants j - 16, so the dequantized floats are exact
std::vector<unsigned char> q8_known(const std::size_t blocks) {
  std::vector<unsigned char> out;
  for (std::size_t b = 0; b < blocks; ++b) {
    out.push_back(0x00);
    out.push_back(0x3c);
    for (int j = 0; j < 32; ++j) out.push_back(static_cast<unsigned char>(static_cast<signed char>(j - 16)));
  }
  return out;
}

// A small raon-1-mini-like model (owner 2026-10-27: a raon1 card from its files): block_count 3 (no NextN), one leading
// dense block, so the MoE layers are 1 and 2. Its small projections F16 / F32 as they come (no BF16 upgrade), a 3-D one
// among them, the RAON1_NATIVE_OK tensors Q8_0 (served from the GGUF), and a quantized tensor outside that set - what the
// IQ4_XS build does to attn_k_b - dequantized to F32 into dense.bin.
std::vector<Tensor> raon1_model(const bool block_count_key = true, std::vector<Kv>* kvs = nullptr) {
  if (kvs != nullptr) {
    kvs->push_back(Kv{"general.architecture", 8, 0, 0.0F, "raon1", {}, {}});
    if (block_count_key) kvs->push_back(Kv{"raon1.block_count", 4, 3, 0.0F, "", {}, {}});
    kvs->push_back(Kv{"raon1.leading_dense_block_count", 4, 1, 0.0F, "", {}, {}});
    for (const auto& kv : tokenizer_kv("raon")) kvs->push_back(kv);
  }
  std::vector<Tensor> t;
  t.push_back({"token_embd.weight", {64, 8}, 8, filled(8U * 2U * 34U, 1)});
  t.push_back({"output_norm.weight", {64}, 0, f32_bytes(std::vector<float>(64, 1.0F))});
  t.push_back({"blk.0.attn_norm.weight", {64}, 0, f32_bytes(std::vector<float>(64, 1.0F))});
  t.push_back({"blk.0.ffn_gate.weight", {64, 32}, 8, filled(32U * 2U * 34U, 2)});
  t.push_back({"blk.0.attn_v_b.weight", {4, 2, 8}, 1, f16_ones(64)});
  t.push_back({"blk.1.ffn_gate_inp.weight", {64, 4}, 0, f32_bytes(std::vector<float>(256, 0.1F))});
  t.push_back({"blk.1.attn_k_b.weight", {32, 2, 4}, 8, q8_known(8)});
  t.push_back({"blk.1.attn_gate.weight", {64, 32}, 8, filled(32U * 2U * 34U, 3)});
  t.push_back({"blk.1.ssm_g.weight", {64, 32}, 8, filled(32U * 2U * 34U, 4)});
  experts_of(t, 1);
  t.push_back({"blk.2.ffn_gate_inp.weight", {64, 4}, 0, f32_bytes(std::vector<float>(256, 0.1F))});
  experts_of(t, 2);
  return t;
}

void maya_raon1_pack_tables() {
  const auto dir = scratch("maya-raon1");
  std::vector<Kv> kvs;
  const auto model = raon1_model(true, &kvs);
  write_gguf(dir / "r.gguf", kvs, model);
  std::string log;
  const auto out = dir / "pack";
  expect(maya_pack(dir / "r.gguf", out, log) == 0, "the raon1 pack is made: " + log);
  const std::string index = read_text(out / "index.txt");
  expect(index.rfind("# strata pack index v3 -- generated by tools/iq_pack.py (native experts) from r.gguf\n# align 64 pool 3712 tensors 10\n", 0) == 0U,
         "the index header: every tensor but the experts: " + index);
  for (const char* row : {"\ntoken_embd.weight 0 0 0 0 0 0 64 8 8 0 32 0 0 0 0 0 0 0\n",
                          "\nblk.0.attn_v_b.weight 0 5 512 128 512 128 8 8 0 0 1 0 0 0 0 0 0 0\n",
                          "\nblk.1.ffn_gate_inp.weight 0 2 640 1024 640 1024 64 4 0 0 1 0 0 0 0 0 0 0\n",
                          "\nblk.1.attn_k_b.weight 0 2 1664 1024 1664 1024 64 4 0 0 1 0 0 0 0 0 0 0\n",
                          "\nblk.1.attn_gate.weight 0 0 0 0 2688 0 64 32 8 0 32 0 0 0 0 0 0 0\n",
                          "\nblk.1.ssm_g.weight 0 0 0 0 2688 0 64 32 8 0 32 0 0 0 0 0 0 0\n"}) {
    expect(index.find(row) != std::string::npos, std::string{"index row"} + row);
  }
  const std::string dense = read_text(out / "dense.bin");
  expect(dense.size() == 3712U, "dense.bin: the float rows, 64-aligned: " + std::to_string(dense.size()));
  std::uint16_t v_b = 0;
  float k_b_first = 0.0F;
  float k_b_last = 0.0F;
  if (dense.size() == 3712U) {
    std::memcpy(&v_b, dense.data() + 512, 2);
    std::memcpy(&k_b_first, dense.data() + 1664, 4);
    std::memcpy(&k_b_last, dense.data() + 1664 + 1020, 4);
  }
  expect(v_b == 0x3c00U, "raon1's F16 projections go in as they are (no BF16 upgrade)");
  expect(k_b_first == -16.0F && k_b_last == 15.0F, "a quantized tensor outside RAON1_NATIVE_OK is dequantized to F32");
  const std::string compat = read_text(out / "compat-bf16.json");
  expect(compat.find("\"name\": \"blk.1.attn_k_b.weight\"," + kLe + "      \"source_type\": \"Q8_0\"," + kLe + "      \"bytes\": 1024") !=
                 std::string::npos &&
             compat.find("attn_v_b") == std::string::npos,
         "compat-bf16.json lists the F32 conversion alone: " + compat);
  const std::string experts = read_text(out / "native_experts.txt");
  expect(experts.rfind("# strata native experts v3: ", 0) == 0U && experts.find("(n_expert 4, total 98304; absolute offsets in r.gguf,") != std::string::npos &&
             experts.find("\n1 1 1 0 12288 ") != std::string::npos && experts.find("\n2 1 1 49152 12288 ") != std::string::npos,
         "the MoE layers 1 and 2: " + experts);
  const std::string tk = read_text(out / "tokenizer" / "tokenizer.json");
  expect(tk.find(" \"pre\": \"raon\",") != std::string::npos && tk.find(" \"ignore_merges\": false,") != std::string::npos &&
             tk.find(std::string{" \"pre_pattern\": "} + kRaonPatternJson + ",") != std::string::npos &&
             tk.find("L396 (LLAMA_VOCAB_PRE_TYPE_QWEN35)\"") != std::string::npos,
         "the raon tokenizer, its pattern shipped: " + tk);
  expect(log.find("raon1: packing for the Glm5Model runner (raon1 variant; quantized dense served natively)") != std::string::npos &&
             log.find("raon1: block_count 3, nextn 0 -> trunk layers 0..2") != std::string::npos &&
             log.find("compat-bf16: 1 tensors, 0.00 GiB; expert and PLE table bytes unchanged") != std::string::npos,
         "the log: " + log);
  // without --compat-bf16 nothing needs it (no BF16 list for raon1)
  expect(maya_pack(dir / "r.gguf", dir / "plain", log, false) == 0 && !fs::exists(dir / "plain" / "compat-bf16.json") &&
             read_text(dir / "plain" / "dense.bin") == dense,
         "the same dense.bin without --compat-bf16: " + log);
  std::vector<Kv> no_count;
  const auto same = raon1_model(false, &no_count);
  write_gguf(dir / "nobc.gguf", no_count, same);
  expect(maya_pack(dir / "nobc.gguf", dir / "p2", log) == 1 &&
             log.find("raon1: raon1.block_count is missing; cannot tell the trunk from the NextN block") != std::string::npos,
         "no block_count: " + log);
}

void maya_split_layer_is_v4() {
  const auto dir = scratch("maya-split");
  std::vector<Tensor> first;
  std::vector<Tensor> second;
  for (auto& t : glm_model()) (t.name == "blk.1.ffn_down_exps.weight" ? second : first).push_back(t);
  write_gguf(dir / "m-00001-of-00002.gguf", glm_kv(), first);
  write_gguf(dir / "m-00002-of-00002.gguf", {}, second);   // no split keys: Maya's fork does not check them
  std::string log;
  expect(maya_pack(dir / "m-00001-of-00002.gguf", dir / "pack", log) == 0, "a split GLM model packs: " + log);
  const std::string experts = read_text(dir / "pack" / "native_experts.txt");
  expect(experts.rfind("# strata native experts v4: ", 0) == 0U &&
             experts.find(" " + std::to_string(absolute_offset(dir / "m-00002-of-00002.gguf", "blk.1.ffn_down_exps.weight")) +
                          " m-00001-of-00002.gguf m-00001-of-00002.gguf m-00002-of-00002.gguf\n") != std::string::npos,
         "v4: every role names its shard, shard 1 too: " + experts);
}

void maya_qwen4exp_routers() {
  const auto dir = scratch("maya-qwen");
  auto kvs = arch_kv();
  for (const auto& kv : tokenizer_kv()) kvs.push_back(kv);
  auto model = tiny_model(1);
  model[2] = Tensor{"blk.0.ffn_gate_inp.weight", {64, 4}, 0, f32_bytes(std::vector<float>(256, 0.5F))};
  write_gguf(dir / "exact.gguf", kvs, model);
  std::string log;
  expect(maya_pack(dir / "exact.gguf", dir / "p1", log, false) == 0, "Maya's qwen4exp: an F32 router of BF16 values: " + log);
  const std::string index = read_text(dir / "p1" / "index.txt");
  expect(index.find("\nblk.0.ffn_gate_inp.weight 0 4 256 512 256 512 64 4 0 0 1 ") != std::string::npos &&
             index.find("\nblk.0.attn_q_norm.weight 0 5 768 128 768 128 64 0 0 0 1 ") != std::string::npos,
         "the router as BF16, the F16 norm as stored (Maya's fork has no FORM table): " + index);
  model[2] = Tensor{"blk.0.ffn_gate_inp.weight", {64, 4}, 0, f32_bytes(std::vector<float>(256, 0.1F))};
  write_gguf(dir / "round.gguf", kvs, model);
  expect(maya_pack(dir / "round.gguf", dir / "p2", log, false) == 1 &&
             log.find("router blk.0.ffn_gate_inp.weight is F32 with values that are not BF16; the engine's router is BF16") != std::string::npos,
         "other F32 values refused: " + log);
}

void download_maya_protocol() {
  if (g_exe.empty()) {
    expect(false, "the strata-prepare path is an argument");
    return;
  }
  const auto dir = scratch("download-maya");
  write_gguf(dir / "g.gguf", glm_kv(), glm_model());
  const std::string exe = "\"" + g_exe + "\"";
  const std::string got = run(exe + " download --maya --gguf \"" + (dir / "g.gguf").string() + "\" --out \"" + (dir / "pack").string() + "\" 2>&1");
  expect(got.find("STEP pack\n") != std::string::npos && got.find("STEP sources\n") != std::string::npos && got.find("DONE\n") != std::string::npos,
         "the protocol: " + got);
  const std::string sources = read_text(dir / "pack" / "sources.json");
  const std::string head = lines({"{", " \"files\": [", "  {", "   \"name\": \"g.gguf\",", "   \"size\": " + std::to_string(fs::file_size(dir / "g.gguf")) + ",",
                                  "   \"mtime_ns\": "});
  const std::string tail = lines({"", "  }", " ]", "}"});
  const bool shaped = sources.rfind(head, 0) == 0U && sources.size() > head.size() + tail.size() &&
                      sources.compare(sources.size() - tail.size(), tail.size(), tail) == 0;
  const std::string ns = shaped ? sources.substr(head.size(), sources.size() - head.size() - tail.size()) : std::string{};
  expect(shaped && ns.size() >= 18U && ns.find_first_not_of("0123456789") == std::string::npos,
         "sources.json: json.dumps({\"files\": [...]}, indent=1), no newline at the end: " + sources);
  const std::string prepared = read_text(dir / "pack" / "prepared.json");
  expect(prepared.find("\"tokenizer\": null") == std::string::npos && prepared.find("tokenizer\",") != std::string::npos &&
             prepared.find("\"ple\": null") != std::string::npos && prepared.find("\"shards\": 1") != std::string::npos,
         "prepared.json: the pack's tokenizer, no PLE: " + prepared);
  expect(run(exe + " download --maya --gguf \"" + (dir / "g.gguf").string() + "\" --out o --pack-arg --compat-bf16") == "ERROR bad_arguments\n",
         "Maya's step takes no pack arguments");
  // maya.py pack_step: a pack of this model whose files are all there and whose sources.json still matches the shards
  // is used as it is (pack_source, pack_stale); a shard changed since is packed again
  const auto dense_time = fs::last_write_time(dir / "pack" / "dense.bin");
  const std::string again = run(exe + " download --maya --gguf \"" + (dir / "g.gguf").string() + "\" --out \"" + (dir / "pack").string() + "\" 2>&1");
  expect(again.find("pack already built") != std::string::npos && again.find("DONE\n") != std::string::npos &&
             fs::last_write_time(dir / "pack" / "dense.bin") == dense_time,
         "an up-to-date pack is reused, not rewritten: " + again);
  fs::last_write_time(dir / "g.gguf", fs::last_write_time(dir / "g.gguf") + std::chrono::seconds(5));
  const std::string stale = run(exe + " download --maya --gguf \"" + (dir / "g.gguf").string() + "\" --out \"" + (dir / "pack").string() + "\" 2>&1");
  expect(stale.find("the pack is built again: g.gguf changed after the pack was built") != std::string::npos && stale.find("DONE\n") != std::string::npos,
         "a shard changed since: packed again: " + stale);
  auto bare = glm_kv();
  std::erase_if(bare, [](const Kv& kv) { return kv.key == "tokenizer.chat_template"; });
  write_gguf(dir / "bare.gguf", bare, glm_model());
  expect(run(exe + " download --maya --gguf \"" + (dir / "bare.gguf").string() + "\" --out \"" + (dir / "bare").string() + "\"").find("ERROR pack_incomplete\n") !=
             std::string::npos,
         "no chat template: the pack is not complete (maya.py's PACK_FILES)");
}

// ---- Maya's DeepSeek-V4.1 and MiniMax-M3.1 packs (tools/ds4/pack_gguf.py, tools/mm3/pack_gguf.py) ------------------

int moe_pack(const bool ds4, const strata_prepare::MoePackOptions& options, std::string& log) {
  std::ostringstream messages;
  const int rc = ds4 ? strata_prepare::maya_ds4_pack(options, messages) : strata_prepare::maya_mm3_pack(options, messages);
  log = messages.str();
  return rc;
}

std::string canonical_text(const fs::path& p) { return fs::weakly_canonical(p).make_preferred().string(); }

// A tiny DeepSeek-V4.1 GGUF (no Engram layers): an F32 embedding (vocab 8), a two-group output projection, two F16
// experts; the geometry keys the pack reads, a few of them left out (the lines left out of ds4.txt).
std::vector<Kv> ds4_kv(const std::string& arch = "deepseek41") {
  std::vector<Kv> kvs{Kv{"general.architecture", 8, 0, 0.0F, arch, {}, {}}};
  for (const auto& [key, value] : std::vector<std::pair<std::string, std::uint64_t>>{
           {".block_count", 1}, {".embedding_length", 64}, {".attention.head_count", 2}, {".attention.output_group_count", 2},
           {".expert_count", 2}, {".expert_used_count", 1}}) {
    kvs.push_back(Kv{arch + key, 4, value, 0.0F, "", {}, {}});
  }
  kvs.push_back(Kv{arch + ".attention.layer_norm_rms_epsilon", 6, 0, 1e-6F, "", {}, {}});
  for (const auto& kv : tokenizer_kv()) kvs.push_back(kv);
  return kvs;
}

std::vector<Tensor> ds4_tensors(const bool odd = false) {
  std::vector<Tensor> t;
  t.push_back({"token_embd.weight", {64, 8}, 0, f32_bytes(std::vector<float>(512, 0.5F))});
  t.push_back({"blk.0.attn_output_a.weight", {64, 32}, 0, f32_bytes(std::vector<float>(2048, 0.25F))});
  t.push_back({"blk.0.ffn_gate_exps.weight", {64, 32, 2}, 1, f16_ones(64U * 32U * 2U)});
  t.push_back({"output_norm.weight", {64}, 0, f32_bytes(std::vector<float>(64, 1.0F))});
  if (odd) t.push_back({"blk.0.odd.weight", {64}, 0, f32_bytes(std::vector<float>(64, 1.0F))});
  return t;
}

void maya_ds4_pack_tables() {
  const auto dir = scratch("maya-ds4");
  const auto model = dir / "DeepSeek-Test";
  fs::create_directories(model);
  write_gguf(model / "d.gguf", ds4_kv(), ds4_tensors());
  write_lf(dir / "ds4-template.jinja", "<|User|>{{ x }}\r\nend\n");
  std::string log;
  expect(moe_pack(true, {model, {}, {}, dir / "ds4-template.jinja", {}, {}}, log) == 0, "the DeepSeek pack is made: " + log);
  const auto out = model / "maya-ds4";
  const std::string geo = read_text(out / "ds4.txt");
  expect(geo.rfind("vocab_size 8" + kLe + "hidden_size 64" + kLe + "num_hidden_layers 1" + kLe + "num_attention_heads 2" + kLe + "o_groups 2" +
                       kLe + "n_routed_experts 2" + kLe + "num_experts_per_tok 1" + kLe + "eos_token_id 5" + kLe +
                       "rms_norm_eps 9.999999974752427e-07" + kLe + "swiglu_limit 0.0" + kLe + "compress_ratios " + kLe,
                   0) == 0U,
         "ds4.txt: config.json's names, None left out, f32 as Python prints it, an empty list as its key and a space: " + geo);
  expect(geo.find("dspark_block_size 0" + kLe) != std::string::npos &&
             ends_with_text(geo, "dspark_target_layer_ids " + kLe + "format gguf" + kLe + "model_dir " + canonical_text(model) + kLe),
         "ds4.txt ends with the DSpark defaults, the format and the model's folder: " + geo);
  const strata_prepare::GgufHeader g(model / "d.gguf");
  const auto at = [&g](const std::string& name) {
    for (const auto& t : g.tensors) {
      if (t.name == name) return g.data_start + t.offset;
    }
    return std::uint64_t{0};
  };
  const std::string tensors = read_text(out / "tensors.txt");
  for (const auto& line : std::vector<std::string>{"file 0 d.gguf", "t embed.weight F32 0 " + std::to_string(at("token_embd.weight")) + " 2048 8 64",
                                 "t layers.0.attn.wo_a.slice.0.weight F32 0 " + std::to_string(at("blk.0.attn_output_a.weight")) + " 4096 16 64",
                                 "t layers.0.attn.wo_a.slice.1.weight F32 0 " + std::to_string(at("blk.0.attn_output_a.weight") + 4096U) + " 4096 16 64",
                                 "t layers.0.ffn.experts.1.w1.weight F16 0 " + std::to_string(at("blk.0.ffn_gate_exps.weight") + 4096U) + " 4096 32 64",
                                 "t norm.weight F32 0 " + std::to_string(at("output_norm.weight")) + " 256 64"}) {
    expect(tensors.find(line + kLe) != std::string::npos, "tensors.txt: " + line + " in " + tensors);
  }
  expect(!fs::exists(out / "engram.bin"), "no Engram layers: no engram.bin");
  expect(read_text(out / "tokenizer" / "tokenizer.json") ==
             lines({"{", " \"pre\": \"deepseek3\",", " \"special_ids\": {", "  \"bos\": 0,", "  \"eos\": 5,", "  \"pad\": 2", " }", "}"}),
         "tokenizer.json: deepseek3, bos / eos / pad: " + read_text(out / "tokenizer" / "tokenizer.json"));
  expect(read_text(out / "tokenizer" / "merges.txt") == "a b" + kLe + "ab a" + kLe + "a b", "merges.txt: the GGUF's merges as listed");
  expect(read_text(out / "tokenizer" / "chat_template.jinja") == "<|User|>{{ x }}" + kLe + "end" + kLe,
         "Maya's template, read with universal newlines, written with the platform's");
  // the GGUF's own template when Maya's is not named; refusals
  expect(moe_pack(true, {model, dir / "own", {}, {}, {}, {}}, log) == 0 &&
             read_text(dir / "own" / "tokenizer" / "chat_template.jinja") == "{{ x }}\r" + kLe + "line" + kLe,
         "the GGUF's template when Maya's is not there: " + log);
  write_gguf(model / "d.gguf", ds4_kv(), ds4_tensors(true));
  expect(moe_pack(true, {model, dir / "odd", {}, {}, {}, {}}, log) == 1 && log.find("tensors the pack does not map (1): blk.0.odd.weight") != std::string::npos,
         "an unmapped tensor: " + log);
  write_gguf(model / "d.gguf", ds4_kv("llama"), ds4_tensors());
  expect(moe_pack(true, {model, dir / "llama", {}, {}, {}, {}}, log) == 1 &&
             log.find("not a DeepSeek-V4.1 GGUF (architecture 'llama')") != std::string::npos,
         "another architecture: " + log);
}

// Maya's DeepSeek setup step (maya.py v1.1.30 ds4_pack_step and ds4_seed_profile) through `download --maya`, which the
// GGUF's architecture sends there: pack_gguf.py's pack, checked against ds4_pack_step's `need` list; sources.json is
// stamp_pack's with the "tools" that built it; a pack of the same tools whose files are all there and whose shards are
// unchanged is reused; the shipped profile (--expert-profile) is copied in as expert-profile.bin when the pack has none
// and it is the pack's size (layers x routed experts x 4 bytes).
void download_maya_ds4_step() {
  if (g_exe.empty()) {
    expect(false, "the strata-prepare path is an argument");
    return;
  }
  const auto dir = scratch("download-maya-ds4");
  const auto model = dir / "DeepSeek";
  fs::create_directories(model);
  auto kvs = ds4_kv();
  // one Engram layer: maya.py's need list holds engram.bin
  kvs.push_back(Kv{"deepseek41.engram.layers", 9, 0, 0.0F, "", {}, {0}});
  for (const auto& [key, value] : std::vector<std::pair<std::string, std::uint64_t>>{
           {"deepseek41.engram.max_ngram_size", 2}, {"deepseek41.engram.head_count", 1}, {"deepseek41.engram.head_dim", 4},
           {"deepseek41.engram.pad_token_id", 0}}) {
    kvs.push_back(Kv{key, 4, value, 0.0F, "", {}, {}});
  }
  kvs.push_back(Kv{"deepseek41.engram.token_map", 9, 0, 0.0F, "", {}, {0, 1, 2, 3, 4, 5, 6, 7}});
  kvs.push_back(Kv{"deepseek41.engram.multipliers", 9, 0, 0.0F, "", {}, {3, 5}});
  kvs.push_back(Kv{"deepseek41.engram.primes", 9, 0, 0.0F, "", {}, {7}});
  kvs.push_back(Kv{"deepseek41.engram.offsets", 9, 0, 0.0F, "", {}, {0}});
  auto tensors = ds4_tensors();  // the layer's tables, kv projection and gate
  tensors.push_back({"blk.0.engram_embd.weight", {4, 8}, 0, f32_bytes(std::vector<float>(32, 0.5F))});
  tensors.push_back({"blk.0.engram_kv.weight", {64, 4}, 0, f32_bytes(std::vector<float>(256, 0.25F))});
  tensors.push_back({"blk.0.engram_gate.weight", {64}, 0, f32_bytes(std::vector<float>(64, 1.0F))});
  write_gguf(model / "d.gguf", kvs, tensors);
  write_lf(dir / "expert-profile-ds4.bin", std::string(8, 'p'));  // 1 layer x 2 experts x 4 bytes
  write_lf(dir / "wrong-size.bin", std::string(12, 'w'));
  const std::string exe = "\"" + g_exe + "\"";
  const auto pack = model / "maya-ds4";
  const auto download = [&](const fs::path& out, const fs::path& profile) {
    return run(exe + " download --maya --gguf \"" + (model / "d.gguf").string() + "\" --out \"" + out.string() +
               "\" --expert-profile \"" + profile.string() + "\" 2>&1");
  };
  const std::string got = download(pack, dir / "expert-profile-ds4.bin");
  expect(got.find("STEP pack\n") != std::string::npos && got.find("STEP sources\n") != std::string::npos && got.find("DONE\n") != std::string::npos,
         "the protocol: " + got);
  for (const char* file : {"ds4.txt", "tensors.txt", "engram.bin", "tokenizer/vocab.json", "tokenizer/chat_template.jinja"}) {
    expect(fs::is_regular_file(pack / file), std::string{"the pack holds "} + file);
  }
  const std::string sources = read_text(pack / "sources.json");
  expect(sources.rfind(lines({"{", " \"files\": [", "  {", "   \"name\": \"d.gguf\","}), 0) == 0U &&
             ends_with_text(sources, lines({"  }", " ],", " \"tools\": \"strata-prepare (Maya v1.1.30 tools/ds4/pack_gguf.py)\"", "}"})),
         "sources.json: stamp_pack's files, then the tools that built the pack: " + sources);
  expect(read_text(pack / "expert-profile.bin") == std::string(8, 'p'), "the shipped profile is the pack's expert-profile.bin");
  const std::string prepared = read_text(pack / "prepared.json");
  expect(prepared.find("tokenizer\",") != std::string::npos && prepared.find("\"ple\": null") != std::string::npos,
         "prepared.json: the pack's tokenizer, no PLE: " + prepared);
  // reused while it matches; a profile already there is the engine's own (it saves what it learns): never replaced
  write_lf(pack / "expert-profile.bin", std::string(8, 'L'));
  const auto geometry_time = fs::last_write_time(pack / "ds4.txt");
  const std::string again = download(pack, dir / "expert-profile-ds4.bin");
  expect(again.find("pack already built") != std::string::npos && again.find("DONE\n") != std::string::npos &&
             fs::last_write_time(pack / "ds4.txt") == geometry_time && read_text(pack / "expert-profile.bin") == std::string(8, 'L'),
         "an up-to-date pack is reused, its learned profile kept: " + again);
  // another tool's pack (Maya's own, its hash of its Python tools) is built again
  write_lf(pack / "sources.json", sources.substr(0, sources.find(" \"tools\"")) + " \"tools\": \"0123abcd\"\n}");
  expect(download(pack, dir / "expert-profile-ds4.bin").find("the pack is built again") != std::string::npos,
         "a pack another tool built is built again");
  // a profile of another geometry is not copied
  const std::string other = download(dir / "other", dir / "wrong-size.bin");
  expect(other.find("DONE\n") != std::string::npos && !fs::exists(dir / "other" / "expert-profile.bin"),
         "a profile of another geometry is not copied: " + other);
  // --expert-profile is the DeepSeek step's alone
  write_gguf(dir / "g.gguf", glm_kv(), glm_model());
  expect(run(exe + " download --maya --gguf \"" + (dir / "g.gguf").string() + "\" --out \"" + (dir / "gp").string() + "\" --expert-profile \"" +
             (dir / "expert-profile-ds4.bin").string() + "\"") == "ERROR bad_arguments\n",
         "a GLM step takes no --expert-profile");
}

// A tiny MiniMax-M3 GGUF set: one MoE layer whose indexers come from the side file, two F16 experts, an F16 shared
// expert (its ff: shared_intermediate_size), the name the quant is listed by.
std::vector<Kv> mm3_kv() {
  std::vector<Kv> kvs{Kv{"general.architecture", 8, 0, 0.0F, "minimax-m3", {}, {}}, Kv{"general.name", 8, 0, 0.0F, "MiniMax Test", {}, {}}};
  for (const auto& [key, value] : std::vector<std::pair<std::string, std::uint64_t>>{
           {"block_count", 1}, {"embedding_length", 64}, {"attention.key_length", 16}, {"rope.dimension_count", 8}, {"expert_count", 2}}) {
    kvs.push_back(Kv{"minimax-m3." + key, 4, value, 0.0F, "", {}, {}});
  }
  for (const auto& kv : tokenizer_kv()) kvs.push_back(kv);
  return kvs;
}

void maya_mm3_pack_tables() {
  const auto dir = scratch("maya-mm3");
  const auto model = dir / "MiniMax-Test";
  fs::create_directories(model);
  std::vector<Tensor> main{{"token_embd.weight", {64, 8}, 0, f32_bytes(std::vector<float>(512, 0.5F))},
                           {"blk.0.ffn_gate_exps.weight", {64, 32, 2}, 1, f16_ones(64U * 32U * 2U)},
                           {"blk.0.ffn_gate_shexp.weight", {64, 48}, 1, f16_ones(64U * 48U)}};
  std::vector<Tensor> side;
  for (const char* n : {"indexer.q_proj.weight", "indexer.k_proj.weight", "indexer.q_norm.weight", "indexer.k_norm.weight"}) {
    side.push_back({std::string{"blk.0."} + n, {64}, 0, f32_bytes(std::vector<float>(64, 1.0F))});
  }
  write_gguf(model / "m.gguf", mm3_kv(), main);
  write_lf(dir / "mm3-template.jinja", "]~!b[{{ x }}\n");
  std::string log;
  expect(moe_pack(false, {model, {}, {}, dir / "mm3-template.jinja", {}, {}}, log) == 1 &&
             log.find("the sparse-attention indexer of layer 0 is not in the set: MiniMax-M3.1 runs it in every layer") != std::string::npos,
         "without the indexers side file: refused: " + log);
  write_gguf(model / "m-maya-indexers.gguf", {Kv{"general.architecture", 8, 0, 0.0F, "minimax-m3", {}, {}}}, side);
  expect(moe_pack(false, {model, {}, {}, dir / "mm3-template.jinja", {}, {}}, log) == 0, "the MiniMax pack is made: " + log);
  const auto out = model / "maya-mm3";
  const std::string geo = read_text(out / "mm3.txt");
  expect(geo.rfind("hidden_size 64\nnum_hidden_layers 1\nhead_dim 16\nrotary_dim 8\npartial_rotary_factor 0.5\nvocab_size 8\nnum_local_experts 2\n"
                   "shared_intermediate_size 48\nrouted_scaling_factor 1.0\nswiglu_alpha 1.702\nswiglu_limit 7.0\nmoe_layer_freq 1\n",
                   0) == 0U,
         "mm3.txt: the geometry (LF), MiniMax's swiglu defaults: " + geo);
  expect(ends_with_text(geo, "norms_plus_one 1\nbos_token_id 0\neos_token_id 5\nspecial.bos 0\nspecial.eos 5\nformat gguf\nquant_name MiniMax-Test\n"
                             "gguf.dense F16\ngguf.experts F16x1\ngguf.indexers m-maya-indexers.gguf\nmodel_dir " + canonical_text(model) + "\n"),
         "mm3.txt's tail: specials, the quant, the types, the indexers file, the folder: " + geo);
  const std::string tensors = read_text(out / "tensors.txt");
  expect(tensors.rfind("file 0 m.gguf\nfile 1 m-maya-indexers.gguf\nt language_model.model.embed_tokens.weight F32 0 ", 0) == 0U &&
             tensors.find("\nt language_model.model.layers.0.self_attn.index_k_norm.weight F32 1 ") != std::string::npos &&
             tensors.find("\nt language_model.model.layers.0.block_sparse_moe.experts.1.w1.weight F16 0 ") != std::string::npos,
         "tensors.txt (LF): the checkpoint's names, the side file's indexers: " + tensors);
  expect(read_text(out / "tokenizer" / "tokenizer.json") ==
                 lines({"{", " \"pre\": \"minimax\",", " \"special_ids\": {", "  \"bos\": 0,", "  \"eos\": 5", " }", "}"}) &&
             read_text(out / "tokenizer" / "chat_template.jinja") == "]~!b[{{ x }}" + kLe,
         "the tokenizer: minimax, bos / eos, Maya's template");
}

// Maya's MiniMax setup step for a GGUF quant (maya.py v1.1.30 mm3_pack_step, owner 2026-10-28) through `download --maya`,
// which the GGUF's architecture (minimax-m3) sends there: pack_gguf.py's text-only pack of the shard set and its
// indexers side file - the folder's *-maya-indexers.gguf whose maya.indexers.for names the set - with Maya's chat
// template (--template, the install's copy), checked against mm3_pack_step's `need` list; sources.json is stamp_pack's
// over every file with the "tools" that built it; a pack of the same tools whose files are unchanged is reused.
void download_maya_mm3_step() {
  if (g_exe.empty()) {
    expect(false, "the strata-prepare path is an argument");
    return;
  }
  const auto dir = scratch("download-maya-mm3");
  const auto model = dir / "MiniMax";
  fs::create_directories(model);
  std::vector<Tensor> main{{"token_embd.weight", {64, 8}, 0, f32_bytes(std::vector<float>(512, 0.5F))},
                           {"blk.0.ffn_gate_exps.weight", {64, 32, 2}, 1, f16_ones(64U * 32U * 2U)},
                           {"blk.0.ffn_gate_shexp.weight", {64, 48}, 1, f16_ones(64U * 48U)}};
  std::vector<Tensor> side;
  for (const char* n : {"indexer.q_proj.weight", "indexer.k_proj.weight", "indexer.q_norm.weight", "indexer.k_norm.weight"}) {
    side.push_back({std::string{"blk.0."} + n, {64}, 0, f32_bytes(std::vector<float>(64, 1.0F))});
  }
  write_gguf(model / "MiniMax-Test-IQ1_S-00001-of-00001.gguf", mm3_kv(), main);
  const auto indexers = [](const std::string& for_) {
    return std::vector<Kv>{Kv{"general.architecture", 8, 0, 0.0F, "minimax-m3", {}, {}}, Kv{"maya.indexers.for", 8, 0, 0.0F, for_, {}, {}}};
  };
  write_gguf(model / "Another-Quant-maya-indexers.gguf", indexers("Another-Quant"), side);  // another quant's, first by name
  write_gguf(model / "MiniMax-Test-maya-indexers.gguf", indexers("MiniMax-Test"), side);
  write_lf(dir / "chat_template.jinja", "]~!b[{{ x }}\n");
  const std::string exe = "\"" + g_exe + "\"";
  const auto pack = model / "maya-mm3";
  const auto download = [&](const std::string& extra) {
    return run(exe + " download --maya --gguf \"" + (model / "MiniMax-Test-IQ1_S-00001-of-00001.gguf").string() + "\" --out \"" +
               pack.string() + "\"" + extra + " 2>&1");
  };
  const std::string with_template = " --template \"" + (dir / "chat_template.jinja").string() + "\"";
  const std::string got = download(with_template);
  expect(got.find("STEP pack\n") != std::string::npos && got.find("STEP sources\n") != std::string::npos && got.find("DONE\n") != std::string::npos,
         "the protocol: " + got);
  for (const char* file : {"mm3.txt", "tensors.txt", "tokenizer/vocab.json", "tokenizer/chat_template.jinja"}) {
    expect(fs::is_regular_file(pack / file), std::string{"the pack holds "} + file);
  }
  expect(read_text(pack / "mm3.txt").find("gguf.indexers MiniMax-Test-maya-indexers.gguf\n") != std::string::npos,
         "the set's own indexers file, by maya.indexers.for: " + read_text(pack / "mm3.txt"));
  expect(read_text(pack / "tokenizer" / "chat_template.jinja") == "]~!b[{{ x }}" + kLe, "Maya's template, the install's copy");
  const std::string sources = read_text(pack / "sources.json");
  expect(sources.find("\"name\": \"MiniMax-Test-IQ1_S-00001-of-00001.gguf\"") != std::string::npos &&
             sources.find("\"name\": \"MiniMax-Test-maya-indexers.gguf\"") != std::string::npos &&
             sources.find("Another-Quant") == std::string::npos &&
             ends_with_text(sources, lines({"  }", " ],", " \"tools\": \"strata-prepare (Maya v1.1.30 tools/mm3/pack_gguf.py)\"", "}"})),
         "sources.json: the shards and the indexers file, then the tools that built the pack: " + sources);
  const std::string prepared = read_text(pack / "prepared.json");
  expect(prepared.find("tokenizer\",") != std::string::npos && prepared.find("\"ple\": null") != std::string::npos,
         "prepared.json: the pack's tokenizer, no PLE: " + prepared);
  const auto geometry_time = fs::last_write_time(pack / "mm3.txt");
  const std::string again = download(with_template);
  expect(again.find("pack already built") != std::string::npos && fs::last_write_time(pack / "mm3.txt") == geometry_time,
         "an up-to-date pack is reused: " + again);
  // the step needs Maya's template; the GLM step takes none
  expect(download("") == "ERROR bad_arguments\n", "a MiniMax download without --template");
  write_gguf(dir / "g.gguf", glm_kv(), glm_model());
  expect(run(exe + " download --maya --gguf \"" + (dir / "g.gguf").string() + "\" --out \"" + (dir / "gp").string() + "\"" + with_template) ==
             "ERROR bad_arguments\n",
         "a GLM step takes no --template");
}


// ---- Qwen3.8's MTP draft layer (owner 2026-10-29: "create native versions ... add it to strata prepare") ----------

std::vector<float> floats_of(const std::uint32_t* bits, const std::size_t n) {
  std::vector<float> out(n);
  std::memcpy(out.data(), bits, n * 4U);
  return out;
}

// mtp_pack.py's q2_0 and mtp_rt.py's q8_0 against the bytes upstream's own numpy code gives for the same blocks (numpy
// 2.5.3): zeros, a ramp, a normal block, one outlier, a uniform one (BF16 values, as the checkpoint holds)
void mtp_quantizers_match_numpy() {
  struct Case {
    const char* name;
    std::array<std::uint32_t, 64> in;
    std::array<unsigned char, 18> out;
  };
  const std::vector<Case> cases{
      {"zeros", {0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U}, {0, 0, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85}},
      {"ramp", {0xbcf50000U, 0xbceb0000U, 0xbce00000U, 0xbcd60000U, 0xbccc0000U, 0xbcc10000U, 0xbcb70000U, 0xbcac0000U, 0xbca20000U, 0xbc980000U, 0xbc8d0000U, 0xbc830000U, 0xbc710000U, 0xbc5d0000U, 0xbc480000U, 0xbc330000U, 0xbc1e0000U, 0xbc090000U, 0xbbea0000U, 0xbbc00000U, 0xbb960000U, 0xbb5a0000U, 0xbb070000U, 0xba500000U, 0x39f90000U, 0x3ae40000U, 0x3b450000U, 0x3b8c0000U, 0x3bb60000U, 0x3bdf0000U, 0x3c040000U, 0x3c190000U, 0x3c2e0000U, 0x3c430000U, 0x3c570000U, 0x3c6c0000U, 0x3c800000U, 0x3c8b0000U, 0x3c950000U, 0x3c9f0000U, 0x3caa0000U, 0x3cb40000U, 0x3cbf0000U, 0x3cc90000U, 0x3cd30000U, 0x3cde0000U, 0x3ce80000U, 0x3cf30000U, 0x3cfd0000U, 0x3d030000U, 0x3d090000U, 0x3d0e0000U, 0x3d130000U, 0x3d180000U, 0x3d1d0000U, 0x3d230000U, 0x3d280000U, 0x3d2d0000U, 0x3d320000U, 0x3d370000U, 0x3d3d0000U, 0x3d420000U, 0x3d470000U, 0x3d4c0000U}, {96, 38, 0, 0, 0, 80, 85, 85, 85, 85, 165, 170, 170, 170, 170, 250, 255, 255}},
      {"normal", {0xbcd00000U, 0x3d0a0000U, 0x3cd30000U, 0x3c890000U, 0xbc420000U, 0xbd3a0000U, 0xbc8b0000U, 0x3bef0000U, 0x3d030000U, 0xbc2d0000U, 0xbc290000U, 0xba420000U, 0xbc240000U, 0xbba80000U, 0xbcdc0000U, 0x3cbd0000U, 0x3c3a0000U, 0x3d010000U, 0xbbc50000U, 0x3c4a0000U, 0x3cd40000U, 0x3bec0000U, 0xbcdd0000U, 0xbd1f0000U, 0xbb8e0000U, 0x3c770000U, 0xb9c90000U, 0x3a7c0000U, 0x39ab0000U, 0x3c720000U, 0xbc190000U, 0xbbd30000U, 0x3c300000U, 0x3cc30000U, 0x3b7c0000U, 0x3abb0000U, 0x3d0a0000U, 0xbb9e0000U, 0xbcda0000U, 0xbcb40000U, 0x3b820000U, 0x3c910000U, 0x3c780000U, 0xbd130000U, 0x3bf90000U, 0xbca10000U, 0xbbce0000U, 0xbce00000U, 0xbcb20000U, 0xbd010000U, 0xbb860000U, 0x3af00000U, 0x3d0a0000U, 0x3c380000U, 0xba7d0000U, 0x3c690000U, 0x3bf50000U, 0xbc340000U, 0x3cc60000U, 0xbc620000U, 0x3ca40000U, 0xbc070000U, 0xbb760000U, 0x3bcf0000U}, {138, 38, 168, 65, 86, 133, 89, 6, 89, 89, 89, 6, 41, 17, 80, 150, 37, 86}},
      {"outlier", {0xbc4f0000U, 0xbbcd0000U, 0x3c890000U, 0x3b3b0000U, 0x3b4d0000U, 0x3c440000U, 0x3c7a0000U, 0x3b2d0000U, 0xbbc50000U, 0x3c100000U, 0x3bfa0000U, 0x3c290000U, 0xbc2b0000U, 0x3c780000U, 0xbc010000U, 0x3bf90000U, 0xbccd0000U, 0x39260000U, 0x3a700000U, 0xbbdd0000U, 0xbb8e0000U, 0xbb9a0000U, 0xbbbe0000U, 0xbb820000U, 0x3aa20000U, 0xbbc50000U, 0x39450000U, 0x3afc0000U, 0xbc260000U, 0x3c6c0000U, 0x3b920000U, 0x3b8d0000U, 0x3b500000U, 0x3bd60000U, 0x3c800000U, 0xbb8c0000U, 0xbc1e0000U, 0x3c380000U, 0x3c650000U, 0xbc020000U, 0x3c890000U, 0xbc1e0000U, 0xba210000U, 0xbac30000U, 0x3bd10000U, 0xbabc0000U, 0xba520000U, 0x3cdc0000U, 0xbc0c0000U, 0xbc230000U, 0x3c2a0000U, 0xbc660000U, 0x39bd0000U, 0x3cbb0000U, 0xbb880000U, 0x3c390000U, 0x3c7e0000U, 0xbc7a0000U, 0x3bd50000U, 0x3c620000U, 0xbc190000U, 0xbc360000U, 0x3c040000U, 0x3ecc0000U}, {96, 50, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 213}},
      {"uniform", {0xbec50000U, 0xbed00000U, 0xbe220000U, 0x3e940000U, 0x3f5f0000U, 0xbd860000U, 0xbf0a0000U, 0xbdcb0000U, 0x3f470000U, 0xbf480000U, 0xbf5e0000U, 0x3f240000U, 0xbe960000U, 0x3dfb0000U, 0xbe450000U, 0x3ee90000U, 0x3e6a0000U, 0xbf770000U, 0x3f1c0000U, 0xbd3d0000U, 0xbf4a0000U, 0x3dcf0000U, 0xbf630000U, 0xbeb30000U, 0xbd8f0000U, 0x3f4c0000U, 0x3ee00000U, 0x3ef10000U, 0x3e680000U, 0xbe6a0000U, 0x3f680000U, 0xbf260000U, 0xbe9f0000U, 0x3e350000U, 0x3e910000U, 0x3c650000U, 0x3f230000U, 0xbef80000U, 0x3d680000U, 0x3f790000U, 0xbf740000U, 0xbe730000U, 0xbf130000U, 0xbe790000U, 0xbd060000U, 0xbd690000U, 0xbe5d0000U, 0xbf1f0000U, 0x3f6a0000U, 0x3ee20000U, 0xbe570000U, 0xbef30000U, 0xbf540000U, 0x39180000U, 0xbeb30000U, 0xbf4e0000U, 0xbf6e0000U, 0x3e930000U, 0x3f7c0000U, 0x3e830000U, 0xbf260000U, 0x3f6d0000U, 0x3f530000U, 0x3f680000U}, {224, 55, 144, 71, 131, 148, 97, 4, 173, 53, 100, 210, 68, 21, 27, 4, 184, 252}},
  };
  for (const auto& c : cases) {
    const auto x = floats_of(c.in.data(), 64U);
    std::array<unsigned char, 18> got{};
    strata_prepare::q2_0_block(x.data(), got.data());
    expect(got == c.out, std::string{"q2_0 "} + c.name);
  }
  const std::array<std::uint32_t, 32> q8_in{0x3d0c0000U, 0xbc3e0000U, 0xbd010000U, 0x3ca80000U, 0xbd3f0000U, 0xbcb50000U, 0xbd0c0000U, 0xbd120000U, 0xbc8e0000U, 0x3c260000U, 0xbc560000U, 0xbcc50000U, 0x3d550000U, 0x3c940000U, 0x3d5a0000U, 0x3ccc0000U, 0x3cbf0000U, 0x3d110000U, 0xbd3a0000U, 0x3c300000U, 0x3cc40000U, 0xbc960000U, 0x3d220000U, 0xbc790000U, 0x3cad0000U, 0xbc9c0000U, 0x3c450000U, 0xbcf00000U, 0xbdbc0000U, 0xbd8d0000U, 0xbcfc0000U, 0x3b9e0000U};
  const std::array<unsigned char, 34> q8_out{236, 17, 47, 240, 212, 28, 191, 225, 209, 207, 232, 14, 238, 223, 72, 25, 74, 34, 32, 49, 193, 15, 33, 231, 55, 235, 29, 230, 17, 215, 129, 161, 213, 7};
  const auto x = floats_of(q8_in.data(), 32U);
  std::array<unsigned char, 34> got{};
  strata_prepare::q8_0_block(x.data(), got.data());
  expect(got == q8_out, "q8_0");
  // numpy's floor(|v| + 0.5) in float32: 0.49999997 + 0.5 rounds to 1 (roundf would give 0)
  std::array<float, 32> edge{};
  edge[0] = 127.0F;
  edge[1] = 0.49999997F;
  strata_prepare::q8_0_block(edge.data(), got.data());
  expect(got[2] == 127U && got[3] == 1U, "q8_0 rounds as numpy's float32 floor(|v| + 0.5)");
}

// mtp_rt.py's blob_of: gate and up rows interleaved, every block's 16 code bytes, then every block's 2 scale bytes
void mtp_expert_blob_layout() {
  constexpr std::int64_t h = 128;
  constexpr std::int64_t ff = 64;
  std::vector<unsigned char> gate_up(static_cast<std::size_t>(2 * ff * (h / 64) * 18));
  std::vector<unsigned char> down(static_cast<std::size_t>(h * (ff / 64) * 18));
  for (std::size_t i = 0; i < gate_up.size(); ++i) gate_up[i] = static_cast<unsigned char>(i % 251U);
  for (std::size_t i = 0; i < down.size(); ++i) down[i] = static_cast<unsigned char>((i * 7U) % 253U);
  const std::string blob = strata_prepare::expert_blob(gate_up.data(), down.data(), h, ff);
  expect(blob.size() == static_cast<std::size_t>(3 * h * ff / 64 * 18), "the blob is 3 x h x ff / 64 blocks");
  const auto at = [&blob](const std::size_t i) { return static_cast<unsigned char>(blob[i]); };
  const std::size_t gu_blocks = static_cast<std::size_t>(2 * ff * (h / 64));
  const std::size_t dn_blocks = static_cast<std::size_t>(h * (ff / 64));
  // codes: inter row 0 = gate row 0 (its 2 blocks), inter row 1 = up row 0 (gate_up row ff)
  expect(at(0) == gate_up[2] && at(15) == gate_up[17] && at(16) == gate_up[18 + 2], "inter row 0: gate row 0's codes");
  const std::size_t up0 = static_cast<std::size_t>(ff * (h / 64) * 18);
  expect(at(32) == gate_up[up0 + 2] && at(48) == gate_up[up0 + 18 + 2], "inter row 1: up row 0's codes");
  const std::size_t down_codes = gu_blocks * 16U;
  expect(at(down_codes) == down[2] && at(down_codes + 16U) == down[18 + 2], "then down's codes");
  const std::size_t scales = (gu_blocks + dn_blocks) * 16U;
  expect(at(scales) == gate_up[0] && at(scales + 1U) == gate_up[1] && at(scales + 4U) == gate_up[up0] &&
             at(scales + gu_blocks * 2U) == down[0],
         "then the scales: inter rows, then down");
}

// A small model's tensors through the whole step: experts.bin from the Q2_0 rows, dense.bin with the 256-byte
// alignment, dense.txt (short names, kinds, offsets), the vocabulary last - and each refusal
void mtp_runtime_from_tensors() {
  const auto dir = scratch("mtp");
  const auto src = dir / "src";
  fs::create_directories(src / "tensors");
  std::mt19937 rng(7U);
  std::normal_distribution<float> normal(0.0F, 0.02F);
  const auto bf16_file = [&](const std::string& name, const std::size_t count) {
    std::vector<std::uint16_t> v(count);
    for (auto& x : v) x = strata_prepare::bf16_bits(normal(rng));
    std::ofstream f(src / "tensors" / (name + ".bin"), std::ios::binary);
    f.write(reinterpret_cast<const char*>(v.data()), static_cast<std::streamsize>(count * 2U));
    return v;
  };
  constexpr std::int64_t h = 64;
  constexpr std::int64_t ff = 64;
  constexpr std::int64_t ne = 2;
  const auto norm = bf16_file("mtp.a.weight", 64);
  const auto gate = bf16_file("mtp.layers.0.mlp.gate.weight", 2 * 64);
  bf16_file("mtp.layers.0.self_attn.q_proj.weight", 64 * 64);
  const auto gate_up = bf16_file("mtp.layers.0.mlp.experts.gate_up_proj", ne * 2 * ff * h);
  const auto down = bf16_file("mtp.layers.0.mlp.experts.down_proj", ne * h * ff);
  strata_prepare::Sha256 sha;
  sha.update(norm.data(), norm.size() * 2U);
  const std::string norm_sha = sha.hex();
  const std::vector<strata_prepare::MtpTensor> table{
      {"mtp.a.weight", "s", 0, 128, {64, 0, 0}, 1, norm_sha},
      {"mtp.layers.0.mlp.experts.down_proj", "s", 0, ne * h * ff * 2, {ne, h, ff}, 3, ""},
      {"mtp.layers.0.mlp.experts.gate_up_proj", "s", 0, ne * 2 * ff * h * 2, {ne, 2 * ff, h}, 3, ""},
      {"mtp.layers.0.mlp.gate.weight", "s", 0, 256, {2, 64, 0}, 2, ""},
      {"mtp.layers.0.self_attn.q_proj.weight", "s", 0, 64 * 64 * 2, {64, 64, 0}, 2, ""}};
  {
    std::ofstream vocab(dir / "vocab.bin", std::ios::binary);
    vocab << "VOCAB";
  }
  strata_prepare::MtpOptions options{.src = src, .out = dir / "rt", .draft_vocab = dir / "vocab.bin", .threads = 2};
  std::ostringstream steps;
  std::ostringstream log;
  const std::string reason = strata_prepare::mtp_runtime(options, table, steps, log);
  expect(reason.empty(), "the runtime folder is made: " + reason + " " + log.str());
  expect(steps.str() == "STEP verify\nSTEP experts\nSTEP dense\nSTEP vocab\n", "the steps: " + steps.str());
  // experts.bin: each expert's blob from its Q2_0 rows
  const auto q2_rows = [](const std::uint16_t* bf, const std::size_t values) {
    std::vector<unsigned char> out(values / 64U * 18U);
    std::vector<float> x(64);
    for (std::size_t b = 0; b < values / 64U; ++b) {
      for (std::size_t i = 0; i < 64U; ++i) x[i] = strata_prepare::f32_from_bf16(bf[b * 64U + i]);
      strata_prepare::q2_0_block(x.data(), out.data() + b * 18U);
    }
    return out;
  };
  std::string want;
  for (std::int64_t e = 0; e < ne; ++e) {
    const auto gu = q2_rows(gate_up.data() + e * 2 * ff * h, static_cast<std::size_t>(2 * ff * h));
    const auto dn = q2_rows(down.data() + e * h * ff, static_cast<std::size_t>(h * ff));
    want += strata_prepare::expert_blob(gu.data(), dn.data(), h, ff);
  }
  expect(read_text(dir / "rt" / "experts.bin") == want, "experts.bin: the experts' blobs in order");
  std::string dense_txt = read_text(dir / "rt" / "dense.txt");
  dense_txt.erase(std::remove(dense_txt.begin(), dense_txt.end(), '\r'), dense_txt.end());
  expect(dense_txt == "a.weight f32 1 64 0 256\nmlp.gate.weight bf16 2 64 256 256\nself_attn.q_proj.weight q8_0 64 64 512 4352\n",
         "dense.txt: " + dense_txt);
  const std::string dense = read_text(dir / "rt" / "dense.bin");
  float first = 0.0F;
  std::memcpy(&first, dense.data(), 4);
  expect(dense.size() == 512U + 4352U && first == strata_prepare::f32_from_bf16(norm[0]) + 1.0F &&
             std::memcmp(dense.data() + 256, gate.data(), 256) == 0,
         "dense.bin: the norm + 1 as F32, the gate's BF16 bytes at 256, then the Q8_0 projection");
  expect(read_text(dir / "rt" / "draft_vocab.bin") == "VOCAB", "the vocabulary copied in");
  // refusals: nothing past the failed step is published
  const auto refused = [&](const std::vector<strata_prepare::MtpTensor>& t, const strata_prepare::MtpOptions& o) {
    std::ostringstream s;
    std::ostringstream l;
    return strata_prepare::mtp_runtime(o, t, s, l);
  };
  auto other = options;
  other.out = dir / "rt2";
  auto wrong = table;
  wrong[0].sha256 = "00";
  expect(refused(wrong, other) == "wrong_sha256" && !fs::exists(dir / "rt2" / "experts.bin"), "a tensor that is not the checkpoint's");
  wrong = table;
  wrong[3].bytes = 254;
  expect(refused(wrong, other) == "wrong_size", "a tensor of another size");
  wrong = table;
  wrong[1].name = "mtp.layers.0.mlp.experts.down_proj.missing";
  expect(refused(wrong, other) == "missing_tensor", "a tensor not fetched");
  wrong = table;
  wrong[2].shape = {ne, 2 * ff, 60};
  wrong[2].bytes = static_cast<std::uint64_t>(ne * 2 * ff * 60 * 2);
  expect(refused(wrong, other) != "", "experts whose rows are no whole blocks");
  auto no_vocab = other;
  no_vocab.draft_vocab = dir / "none.bin";
  expect(refused(table, no_vocab) == "missing_vocab" && !fs::exists(dir / "rt2" / "draft_vocab.bin"), "a missing vocabulary file");
}

// The pinned table: the 31 tensors of the pinned revision, sorted, 5,214,301,696 bytes; the command's protocol
void mtp_command_protocol() {
  const auto tensors = strata_prepare::mtp_tensors();
  std::uint64_t total = 0;
  bool sorted = true;
  for (std::size_t i = 0; i < tensors.size(); ++i) {
    total += tensors[i].bytes;
    if (i > 0 && !(tensors[i - 1].name < tensors[i].name)) sorted = false;
  }
  expect(tensors.size() == 31U && sorted && total == 5214301696ULL, "the pinned table: 31 tensors, sorted, 5.2 GB");
  expect(tensors[0].name == "mtp.fc_embedding.weight" && tensors[0].shard == "model-00120-of-00131.safetensors" &&
             tensors[0].start == 1935412640ULL && tensors[0].sha256 == "241d6e87ded317a61b1aeb3ca4e28c53f1caba09ad5be66092e91fe5e3e14751",
         "its first entry, as mtp_fetch.py pins it");
  if (g_exe.empty()) {
    expect(false, "the strata-prepare path is an argument");
    return;
  }
  const auto dir = scratch("mtpcmd");
  const std::string exe = "\"" + g_exe + "\"";
  expect(run(exe + " mtp --src x") == "ERROR bad_arguments\n", "bad arguments");
  expect(run(exe + " mtp --src \"" + (dir / "none").string() + "\" --out \"" + (dir / "rt").string() + "\"") ==
             "STEP verify\nERROR missing_tensor\n",
         "no tensors fetched");
}

}  // namespace

int main(int argc, char** argv) {
  const std::map<std::string, std::function<void()>> cases{
      {"sha256_known_answers", sha256_known_answers},
      {"python_float_repr_matches_python", python_float_repr_matches_python},
      {"json_dumps_matches_python", json_dumps_matches_python},
      {"half_and_bf16_round_to_even", half_and_bf16_round_to_even},
      {"gguf_header_reads_like_gguf_reader", gguf_header_reads_like_gguf_reader},
      {"single_file_pack_tables", single_file_pack_tables},
      {"split_layer_pack_is_v4", split_layer_pack_is_v4},
      {"conversions_and_refusals", conversions_and_refusals},
      {"compat_bf16_dequantizes", compat_bf16_dequantizes},
      {"experts_bin_blobs", experts_bin_blobs},
      {"download_protocol", download_protocol},
      {"utf8_replace_like_python", utf8_replace_like_python},
      {"tokenizer_export_like_strata_tokenizer", tokenizer_export_like_strata_tokenizer},
      {"tokenizer_export_refusals", tokenizer_export_refusals},
      {"maya_glm_pack_tables", maya_glm_pack_tables},
      {"maya_glm_refusals", maya_glm_refusals},
      {"maya_raon1_pack_tables", maya_raon1_pack_tables},
      {"download_maya_mm3_step", download_maya_mm3_step},
      {"mtp_quantizers_match_numpy", mtp_quantizers_match_numpy},
      {"mtp_expert_blob_layout", mtp_expert_blob_layout},
      {"mtp_runtime_from_tensors", mtp_runtime_from_tensors},
      {"mtp_command_protocol", mtp_command_protocol},
      {"maya_split_layer_is_v4", maya_split_layer_is_v4},
      {"maya_qwen4exp_routers", maya_qwen4exp_routers},
      {"download_maya_protocol", download_maya_protocol},
      {"download_maya_ds4_step", download_maya_ds4_step},
      {"maya_ds4_pack_tables", maya_ds4_pack_tables},
      {"maya_mm3_pack_tables", maya_mm3_pack_tables},
  };
  std::string only;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--case" && i + 1 < argc) only = argv[++i];
    else g_exe = a;
  }
  for (const auto& [name, test] : cases) {
    if (!only.empty() && name != only) continue;
    test();
  }
  if (failures != 0) std::cerr << failures << " failure(s)\n";
  return failures == 0 ? 0 : 1;
}
