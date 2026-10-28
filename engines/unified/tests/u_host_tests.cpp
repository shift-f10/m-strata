// Hermetic tests for strata-unified-engine's host code (no GPU): the GGUF reader, the
// per-family geometry guard, the converged protocol dialect, and the family dispatch.
#include "u_cpu_expert.hpp"
#include "u_dispatch.hpp"
#include "u_geometry.hpp"
#include "kernels/nvfp4_ref.hpp"  // u::nvfp4 — the flint H2 host reference
#include "u_gguf.hpp"
#include "u_protocol.hpp"
#include "u_ple.hpp"
#include "u_test.hpp"
#include "u_gguf_writer.hpp"
#include "u_cpu_iq.hpp"
#include "u_cpu_lane.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

using u_test::expect;
using u_test::GgufBuilder;

namespace {

std::variant<u::GgufFile, u::GgufError> parse(const std::vector<std::uint8_t>& image) {
  return u::parse_gguf(image.data(), image.size(), image.size());
}

bool close(double a, double b) { return std::fabs(a - b) < 1e-6; }

// ---- fixtures ----------------------------------------------------------------------------

void put_raon1_meta(GgufBuilder& b) {
  b.str("general.architecture", "raon1");
  b.u32("raon1.block_count", 25);
  b.u32("raon1.embedding_length", 2560);
  b.u32("raon1.attention.head_count", 40);
  b.u32("raon1.attention.key_length", 128);
  b.u32("raon1.feed_forward_length", 12032);
  b.u32("raon1.expert_feed_forward_length", 1088);
  b.u32("raon1.expert_count", 128);
  b.u32("raon1.expert_used_count", 6);
  b.u32("raon1.expert_shared_count", 1);
  b.u32("raon1.context_length", 262144);
  b.u32("raon1.vocab_size", 259840);
}

void put_glm5next_meta(GgufBuilder& b) {
  b.str("general.architecture", "glm5next");
  b.u32("glm5next.block_count", 48);
  b.u32("glm5next.embedding_length", 4096);
  b.u32("glm5next.attention.head_count", 32);
  b.u32("glm5next.attention.key_length", 128);
  b.u32("glm5next.feed_forward_length", 13696);
  b.u32("glm5next.expert_feed_forward_length", 1536);
  b.u32("glm5next.expert_count", 160);
  b.u32("glm5next.expert_used_count", 8);
  b.u32("glm5next.expert_shared_count", 1);
  b.u32("glm5next.context_length", 131072);
  b.u32("glm5next.vocab_size", 151552);
  // No rope.freq_base: the real GLM-5.3 files carry none (NoPE-MLA; c28d6baa loaded one end to end).
}

// Real gemma4 files omit BOTH gemma4.expert_shared_count (no shared expert) and gemma4.vocab_size
// (the vocab lives only in token_embd.weight's shape + the token list). The accept test adds a
// token_embd.weight tensor so the guard can derive the vocab from its row count.
void put_gemma4_meta(GgufBuilder& b) {
  b.str("general.architecture", "gemma4");
  b.u32("gemma4.block_count", 30);
  b.u32("gemma4.embedding_length", 2816);
  b.u32("gemma4.attention.head_count", 16);
  b.u32("gemma4.attention.key_length", 512);
  b.u32("gemma4.feed_forward_length", 2112);
  b.u32("gemma4.expert_feed_forward_length", 704);
  b.u32("gemma4.expert_count", 128);
  b.u32("gemma4.expert_used_count", 8);
  b.u32("gemma4.context_length", 262144);
  b.f32("gemma4.rope.freq_base", 1000000.0f);
  // deliberately NO gemma4.expert_shared_count and NO gemma4.vocab_size
}

// A committed GGUF-header fixture: the raon1 metadata plus a small, well-formed tensor
// directory (two tensors; no weight bytes are read by the host).
GgufBuilder raon1_fixture() {
  GgufBuilder b;
  put_raon1_meta(b);
  b.tensor("output_norm.weight", {2560}, /*f32=*/0, 2560 * 4);
  b.tensor("token_embd.weight", {64, 2}, /*q8_0=*/8, 2 * 2 * 34);
  return b;
}

// The flint shape (Recursive flint, spec 2026-10-25-flint-engine-design): 49 layers of 32/4 GQA
// (head_dim 128, sliding window 513 over 35 of them, rope theta 1e4), MoE 256 experts top-6 + 1
// shared (moe_intermediate 512, dense intermediate 6144), vocab 150144, ctx 262144. Key surface as
// the real flint GGUF spells it (verified against the real 22.9 GB file's 17 MB header image).
void put_flint_meta(GgufBuilder& b) {
  b.str("general.architecture", "flint");
  b.u32("flint.block_count", 49);
  b.u32("flint.embedding_length", 2048);
  b.u32("flint.attention.head_count", 32);
  b.u32("flint.attention.key_length", 128);
  b.u32("flint.feed_forward_length", 6144);
  b.u32("flint.expert_feed_forward_length", 512);
  b.u32("flint.expert_count", 256);
  b.u32("flint.expert_used_count", 6);
  b.u32("flint.expert_shared_count", 2);  // the real file says 2 (the HF config led us to believe 1)
  b.u32("flint.context_length", 262144);
  b.u32("flint.vocab_size", 150144);
  b.f32("flint.rope.freq_base", 10000.0f);
  b.u32("flint.attention.sliding_window", 513);
}

// ---- GGUF reader -------------------------------------------------------------------------

void gguf_parses_a_committed_header() {
  const auto image = raon1_fixture().bytes();
  const auto r = parse(image);
  const auto* f = std::get_if<u::GgufFile>(&r);
  expect(f != nullptr, "the committed raon1 header parses");
  if (f == nullptr) return;
  expect(f->version == 3, "version 3");
  const auto* arch = f->find("general.architecture");
  expect(arch != nullptr && arch->type == u::GgufType::string && arch->text == "raon1", "general.architecture is raon1");
  const auto* blocks = f->find("raon1.block_count");
  expect(blocks != nullptr && blocks->integer == 25, "raon1.block_count is 25");
  const auto* vocab = f->find("raon1.vocab_size");
  expect(vocab != nullptr && vocab->integer == 259840, "raon1.vocab_size is 259840");
  expect(f->tensors.size() == 2, "two tensors in the directory");
  const auto* emb = f->tensor("token_embd.weight");
  expect(emb != nullptr && emb->type == u::TensorType::q8_0 && emb->bytes == 136, "the Q8_0 tensor's type and bytes");
  expect(f->data_offset % 32 == 0, "the data section is aligned");
  // A bad magic and a wrong version are both refused.
  auto bad = image;
  bad[0] = 'X';
  expect(std::holds_alternative<u::GgufError>(parse(bad)), "a bad magic is refused");
  bad = image;
  bad[4] = 2;
  expect(std::holds_alternative<u::GgufError>(parse(bad)), "version 2 is refused");
}

// NVFP4 (ggml tensor type 40): flint's native quant (spec 2026-10-25-flint-engine-design; the
// ggml format llama.cpp master ships for MODEL_ARCH.FLINT). A 64-element block is 32 bytes of
// packed FP4 nibbles + 4 bytes of E4M3 block sub-scales = 36 bytes, exactly 4.5 bits per element.
// The per-tensor (per-expert) F32 weight scale is a SEPARATE `<name>.scale` sidecar tensor — an
// ordinary F32 tensor the reader already handles. Byte math and the 64-element rule are pinned
// here with a synthetic pair; the same math against flint's REAL header (17 MB image of the
// 22.9 GB file) is proven by the flint_probe, recorded in the spec doc.
void gguf_reads_nvfp4_tensors() {
  GgufBuilder b;
  put_flint_meta(b);
  b.tensor("blk.0.ffn_gate_exps.weight", {128}, /*nvfp4=*/40, 72);  // 2 blocks x 36 B
  b.tensor("blk.0.ffn_gate_exps.scale", {1}, /*f32=*/0, 4);         // the sidecar scale
  const auto r = parse(b.bytes());
  const auto* f = std::get_if<u::GgufFile>(&r);
  expect(f != nullptr, "the NVFP4 tensor type parses");
  if (f == nullptr) return;
  const auto* w = f->tensor("blk.0.ffn_gate_exps.weight");
  expect(w != nullptr && w->type == u::TensorType::nvfp4, "type 40 maps to TensorType::nvfp4");
  expect(w != nullptr && w->bytes == 72, "128 NVFP4 elements are 2 blocks x 36 bytes = 72");
  const auto* sc = f->tensor("blk.0.ffn_gate_exps.scale");
  expect(sc != nullptr && sc->type == u::TensorType::f32, "the .scale sidecar parses as ordinary F32");
  // The 64-element block rule: a row that is not a multiple of 64 is refused at parse time.
  GgufBuilder c;
  put_flint_meta(c);
  c.tensor("blk.0.ffn_gate_exps.weight", {66}, 40, 40);
  expect(std::holds_alternative<u::GgufError>(parse(c.bytes())), "an NVFP4 row not a multiple of 64 is refused");
}

void gguf_reads_a_header_from_disk() {
  const auto image = raon1_fixture().bytes();
  const char* path = "u_host_tests_fixture.gguf";
  {
    std::ofstream os(path, std::ios::binary);
    expect(static_cast<bool>(os), "the fixture file opens for writing");
    os.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
  }
  const auto r = u::read_gguf_header(path);
  const auto* f = std::get_if<u::GgufFile>(&r);
  expect(f != nullptr, "read_gguf_header reads the file back");
  if (f != nullptr) {
    const auto* arch = f->find("general.architecture");
    expect(arch != nullptr && arch->text == "raon1", "the disk header is raon1");
  }
  std::remove(path);
  expect(std::holds_alternative<u::GgufError>(u::read_gguf_header("does-not-exist.gguf")), "a missing file is refused");
}

// ---- geometry guard ----------------------------------------------------------------------

void geometry_guard_accepts_valid_key_sets() {
  {
    GgufBuilder b;
    put_raon1_meta(b);
    const auto file = parse(b.bytes());
    const auto* f = std::get_if<u::GgufFile>(&file);
    expect(f != nullptr, "raon1 metadata parses");
    if (f != nullptr) {
      const auto g = u::read_geometry(*f);
      const auto* geom = std::get_if<u::Geometry>(&g);
      expect(geom != nullptr, "raon1 geometry is accepted");
      if (geom != nullptr) {
        expect(geom->family == u::FamilyId::raon1, "family is raon1");
        expect(geom->block_count == 25 && geom->embedding_length == 2560 && geom->head_count == 40, "raon1 core dims");
        expect(geom->head_dim == 128 && geom->feed_forward_length == 12032 && geom->expert_feed_forward_length == 1088,
               "raon1 feed-forward dims");
        expect(geom->expert_count == 128 && geom->expert_used_count == 6 && geom->expert_shared_count == 1,
               "raon1 expert counts");
        expect(geom->vocab_size == 259840 && geom->context_length == 262144, "raon1 vocab and context");
        expect(geom->router == u::RouterKind::sigmoid, "raon1 uses the sigmoid router");
        expect(geom->rope == u::RopeKind::nope && close(geom->rope_freq_base, 0.0), "raon1 is NoPE");
      }
    }
  }
  {
    GgufBuilder b;
    put_glm5next_meta(b);
    const auto file = parse(b.bytes());
    const auto* f = std::get_if<u::GgufFile>(&file);
    expect(f != nullptr, "glm5next metadata parses");
    if (f != nullptr) {
      const auto g = u::read_geometry(*f);
      const auto* geom = std::get_if<u::Geometry>(&g);
      expect(geom != nullptr, "glm5next geometry is accepted");
      if (geom != nullptr) {
        expect(geom->family == u::FamilyId::glm5next, "family is glm5next");
        expect(geom->router == u::RouterKind::sigmoid, "glm5next uses the sigmoid router");
        expect(geom->rope == u::RopeKind::nope && close(geom->rope_freq_base, 0.0), "glm5next is NoPE-MLA with no rope base");
      }
    }
  }
  {
    // flint (RecursiveSI, spec 2026-10-25-flint-engine-design): GQA full+sliding-window attention
    // with rope (theta 1e4), MoE 256 experts top-6 + 1 shared, sigmoid router renormalized
    // (norm_topk_prob). Guard row is BROAD ranges, not exact pins — the shape is the published
    // one, but the family ships variants (H1 notes, spec §).
    GgufBuilder b;
    put_flint_meta(b);
    const auto file = parse(b.bytes());
    const auto* f = std::get_if<u::GgufFile>(&file);
    expect(f != nullptr, "flint metadata parses");
    if (f != nullptr) {
      const auto g = u::read_geometry(*f);
      const auto* geom = std::get_if<u::Geometry>(&g);
      expect(geom != nullptr, "flint geometry is accepted");
      if (geom != nullptr) {
        expect(geom->family == u::FamilyId::flint, "family is flint");
        expect(geom->block_count == 49 && geom->embedding_length == 2048 && geom->head_count == 32, "flint core dims");
        expect(geom->head_dim == 128 && geom->feed_forward_length == 6144 && geom->expert_feed_forward_length == 512,
               "flint feed-forward dims");
        expect(geom->expert_count == 256 && geom->expert_used_count == 6 && geom->expert_shared_count == 2,
               "flint expert counts");
        expect(geom->router == u::RouterKind::sigmoid, "flint is sigmoid-routed (expert_gating_func 2.0)");
        expect(geom->rope == u::RopeKind::rope && close(geom->rope_freq_base, 10000.0), "flint carries rope theta 1e4");
      }
    }
  }
  {
    // gemma4: no expert_shared_count (defaults to 0) and no vocab_size key (derived from
    // token_embd.weight's row count). Matches the real gemma-4-26B-A4B GGUF.
    GgufBuilder b;
    put_gemma4_meta(b);
    b.tensor("token_embd.weight", {64, 256}, /*f32=*/0, 64 * 256 * 4);  // vocab = dims.back() = 256
    const auto file = parse(b.bytes());
    const auto* f = std::get_if<u::GgufFile>(&file);
    expect(f != nullptr, "gemma4 metadata parses");
    if (f != nullptr) {
      const auto g = u::read_geometry(*f);
      const auto* geom = std::get_if<u::Geometry>(&g);
      expect(geom != nullptr, "gemma4 geometry is accepted without expert_shared_count/vocab_size keys");
      if (geom != nullptr) {
        expect(geom->family == u::FamilyId::gemma4, "family is gemma4");
        expect(geom->expert_shared_count == 0, "gemma4 defaults expert_shared_count to 0");
        expect(geom->vocab_size == 256, "gemma4 vocab derived from token_embd.weight rows");
        expect(geom->router == u::RouterKind::softmax, "gemma4 uses the softmax router");
      }
    }
  }
}

bool geometry_refused(const GgufBuilder& b) {
  const auto file = parse(b.bytes());
  const auto* f = std::get_if<u::GgufFile>(&file);
  if (f == nullptr) return true;  // a header that will not even parse is certainly refused
  return std::holds_alternative<u::GeometryError>(u::read_geometry(*f));
}

void geometry_guard_rejects_bad_key_sets() {
  // An out-of-range pinned dimension (raon1.block_count must be exactly 25).
  {
    GgufBuilder b;
    b.str("general.architecture", "raon1");
    b.u32("raon1.block_count", 999);
    b.u32("raon1.embedding_length", 2560);
    b.u32("raon1.attention.head_count", 40);
    b.u32("raon1.attention.key_length", 128);
    b.u32("raon1.feed_forward_length", 12032);
    b.u32("raon1.expert_feed_forward_length", 1088);
    b.u32("raon1.expert_count", 128);
    b.u32("raon1.expert_used_count", 6);
    b.u32("raon1.expert_shared_count", 1);
    b.u32("raon1.context_length", 262144);
    b.u32("raon1.vocab_size", 259840);
    expect(geometry_refused(b), "raon1 with block_count 999 is refused");
  }
  // A missing key (no raon1.vocab_size).
  {
    GgufBuilder b;
    b.str("general.architecture", "raon1");
    b.u32("raon1.block_count", 25);
    b.u32("raon1.embedding_length", 2560);
    b.u32("raon1.attention.head_count", 40);
    b.u32("raon1.attention.key_length", 128);
    b.u32("raon1.feed_forward_length", 12032);
    b.u32("raon1.expert_feed_forward_length", 1088);
    b.u32("raon1.expert_count", 128);
    b.u32("raon1.expert_used_count", 6);
    b.u32("raon1.expert_shared_count", 1);
    b.u32("raon1.context_length", 262144);
    expect(geometry_refused(b), "raon1 missing vocab_size is refused");
  }
  // A RoPE family without a rope.freq_base. (glm5next moved to NoPE-MLA, so the rope-base
  // invariant is proven on qwen35moe instead.)
  {
    GgufBuilder b2;
    b2.str("general.architecture", "qwen35moe");
    b2.u32("qwen35moe.block_count", 48);
    b2.u32("qwen35moe.embedding_length", 4096);
    b2.u32("qwen35moe.attention.head_count", 32);
    b2.u32("qwen35moe.attention.key_length", 128);
    b2.u32("qwen35moe.feed_forward_length", 13696);
    b2.u32("qwen35moe.expert_feed_forward_length", 1536);
    b2.u32("qwen35moe.expert_count", 160);
    b2.u32("qwen35moe.expert_used_count", 8);
    b2.u32("qwen35moe.expert_shared_count", 1);
    b2.u32("qwen35moe.context_length", 131072);
    b2.u32("qwen35moe.vocab_size", 151552);
    expect(geometry_refused(b2), "qwen35moe without rope.freq_base is refused");
  }
  // An unknown architecture.
  {
    GgufBuilder b;
    b.str("general.architecture", "llama");
    b.u32("llama.block_count", 32);
    expect(geometry_refused(b), "an unknown architecture is refused");
  }
  // expert_used_count greater than expert_count.
  {
    GgufBuilder b2;
    b2.str("general.architecture", "glm5next");
    b2.u32("glm5next.block_count", 48);
    b2.u32("glm5next.embedding_length", 4096);
    b2.u32("glm5next.attention.head_count", 32);
    b2.u32("glm5next.attention.key_length", 128);
    b2.u32("glm5next.feed_forward_length", 13696);
    b2.u32("glm5next.expert_feed_forward_length", 1536);
    b2.u32("glm5next.expert_count", 160);
    b2.u32("glm5next.expert_used_count", 200);  // > expert_count
    b2.u32("glm5next.expert_shared_count", 1);
    b2.u32("glm5next.context_length", 131072);
    b2.u32("glm5next.vocab_size", 151552);
    b2.f32("glm5next.rope.freq_base", 1000000.0f);
    expect(geometry_refused(b2), "expert_used_count > expert_count is refused");
  }
}

// ---- protocol ----------------------------------------------------------------------------

const u::Record* ok_record(const std::variant<u::Record, u::ParseError>& r) {
  return std::get_if<u::Record>(&r);
}

template <typename T>
const T* round_trip(const u::Record& in) {
  static thread_local std::variant<u::Record, u::ParseError> held;
  held = u::parse_line(u::emit(in));
  const u::Record* r = ok_record(held);
  return r == nullptr ? nullptr : std::get_if<T>(r);
}

void protocol_round_trips_valid_lines() {
  {
    u::Info info;
    info.fields = {{"engine", "strata-unified"}, {"arch", "raon1"}, {"context", "262144"}};
    const auto* out = round_trip<u::Info>(u::Record{info});
    expect(out != nullptr && out->fields.size() == 3 && out->fields[0].key == "engine" &&
               out->fields[0].value == "strata-unified" && out->fields[2].value == "262144",
           "INFO round-trips");
  }
  {
    const auto* out = round_trip<u::Ready>(u::Record{u::Ready{262144, true}});
    expect(out != nullptr && out->ctx == 262144 && out->stop_honored, "READY with stop round-trips");
    const auto* out2 = round_trip<u::Ready>(u::Record{u::Ready{4096, false}});
    expect(out2 != nullptr && out2->ctx == 4096 && !out2->stop_honored, "READY without stop round-trips");
  }
  {
    u::Gen gen;
    gen.max_new = 128;
    gen.params = {{"temperature", "0.7"}, {"top_k", "40"}, {"seed", "12345"}};
    gen.ids = {2, 105, 2364, 107};
    const auto* out = round_trip<u::Gen>(u::Record{gen});
    expect(out != nullptr && out->max_new == 128 && out->params.size() == 3 && out->params[0].key == "temperature" &&
               out->params[0].value == "0.7" && out->ids == std::vector<std::uint32_t>{2, 105, 2364, 107},
           "GEN round-trips");
    u::Gen gen2;
    gen2.max_new = 16;
    gen2.ids = {1};
    const auto* out2 = round_trip<u::Gen>(u::Record{gen2});
    expect(out2 != nullptr && out2->max_new == 16 && out2->params.empty() &&
               out2->ids == std::vector<std::uint32_t>{1},
           "GEN with no params round-trips");
  }
  expect(round_trip<u::Stop>(u::Record{u::Stop{}}) != nullptr, "STOP round-trips");
  expect(round_trip<u::Quit>(u::Record{u::Quit{}}) != nullptr, "QUIT round-trips");
  {
    const auto* out = round_trip<u::Token>(u::Record{u::Token{45518}});
    expect(out != nullptr && out->id == 45518, "T round-trips");
  }
  {
    const auto* out = round_trip<u::PromptProgress>(u::Record{u::PromptProgress{12, 29, 250.0, 116.0}});
    expect(out != nullptr && out->done == 12 && out->total == 29 && close(out->ms, 250.0) && close(out->tok_s, 116.0),
           "PP round-trips");
  }
  {
    u::Stat stat;
    stat.fields = {{"vram_mib", "15000"}, {"reused", "4"}};
    const auto* out = round_trip<u::Stat>(u::Record{stat});
    expect(out != nullptr && out->fields.size() == 2 && out->fields[1].key == "reused", "STAT round-trips");
  }
  {
    const auto* out = round_trip<u::Done>(u::Record{u::Done{64, 29, 250.0, 1875.5, u::Finish::stop, 7, 12, 4}});
    expect(out != nullptr && out->produced == 64 && out->prompt == 29 && close(out->prompt_ms, 250.0) &&
               close(out->decode_ms, 1875.5) && out->finish == u::Finish::stop && out->spec_hits == 7 &&
               out->spec_steps == 12 && out->reused == 4,
           "DONE round-trips");
    // Every finish value survives.
    for (const auto fin : {u::Finish::stop, u::Finish::length, u::Finish::cancel, u::Finish::nonfinite}) {
      const auto* d = round_trip<u::Done>(u::Record{u::Done{1, 1, 0.0, 0.0, fin, 0, 0, 0}});
      expect(d != nullptr && d->finish == fin, "DONE finish value survives");
    }
  }
  {
    const auto* empty = round_trip<u::Err>(u::Record{u::Err{std::string{}}});
    expect(empty != nullptr && empty->text.empty(), "ERR with no text round-trips");
    const auto* text = round_trip<u::Err>(u::Record{u::Err{"the engine failed and more"}});
    expect(text != nullptr && text->text == "the engine failed and more", "ERR with spaced text round-trips");
  }
}

void protocol_rejects_malformed_and_unknown_lines() {
  std::vector<std::string> bad = {
      "",                                  // empty
      " ",                                 // a lone space
      "FOO 1",                             // unknown prefix
      "NOTE hello",                        // there is no NOTE record
      "RESUME 5",                          // there is no RESUME record
      "REUSED 3",                          // there is no REUSED record
      "READY",                             // missing ctx
      "READY -1",                          // negative ctx
      "READY 01",                          // leading zero
      "READY 1 go",                        // bad optional word
      "READY 1 stop extra",                // trailing junk
      "GEN 1",                             // no id list
      "GEN 0 1,2",                         // max_new 0
      "GEN 5 temperature 1,2",             // a param that is not key=value
      "GEN 5 1,2,",                        // a trailing comma in the ids
      "GEN 5 ,1",                          // a leading comma in the ids
      "T",                                 // missing id
      "T x",                               // id is not a number
      "T 1 2",                             // extra field
      "T 4294967296",                      // overflows u32
      "STOP x",                            // STOP takes no field
      "QUIT x",                            // QUIT takes no field
      "PP 1 2 3",                          // too few fields
      "PP 1 2 x 4",                        // a timing that is not a decimal
      "PP 1 2 1.0 2.0 3",                  // too many fields
      "PP 1 2 -1.0 2.0",                   // a negative timing
      "DONE 1 2 3 4 bogus 0 0 0",          // an unknown finish
      "DONE 1 2 3 4 stop 0 0",             // too few fields
      "INFO a",                            // a field that is not key=value
      "INFO =b",                           // an empty key
      "STAT a",                            // a field that is not key=value
      " READY 1",                          // a leading space
      "READY 1 ",                          // a trailing space
      "GEN  5 1",                          // a doubled space
      "T\t1",                              // a non-printable byte (tab)
  };
  for (const auto& line : bad) {
    const auto r = u::parse_line(line);
    expect(std::holds_alternative<u::ParseError>(r), "fail closed: '" + line + "'");
  }
  // An over-long line (8193 bytes) is refused.
  const std::string over(u::kMaxLineBytes + 1, 'a');
  expect(std::holds_alternative<u::ParseError>(u::parse_line(over)), "fail closed: an over-long line");
  // A line of exactly 8192 bytes passes the length gate (it still fails as an unknown record,
  // but not for length) -- confirm the boundary is inclusive by using a valid record.
  const std::string info_line = "INFO " + std::string(u::kMaxLineBytes - 7, 'a') + "=1";
  expect(info_line.size() == u::kMaxLineBytes, "the boundary line is exactly 8192 bytes");
  expect(std::holds_alternative<u::Record>(u::parse_line(info_line)), "a 8192-byte INFO is accepted");
}

// ---- dispatch ----------------------------------------------------------------------------

void dispatch_maps_known_archs_and_rejects_unknown() {
  const std::pair<const char*, u::FamilyId> known[] = {
      {"glm5next", u::FamilyId::glm5next}, {"ds4", u::FamilyId::ds4},         {"raon1", u::FamilyId::raon1},
      {"qwen35moe", u::FamilyId::qwen35moe}, {"qwen4exp", u::FamilyId::qwen4exp}, {"gemma4", u::FamilyId::gemma4},
      {"flint", u::FamilyId::flint},
  };
  for (const auto& [arch, family] : known) {
    const auto got = u::family_for_arch(arch);
    expect(got.has_value() && *got == family, std::string("dispatch maps ") + arch);
    expect(u::arch_of(family) == arch, std::string("arch_of round-trips ") + arch);
  }
  for (const char* unknown : {"", "llama", "GLM5NEXT", "raon", "gemma", "qwen"}) {
    expect(!u::family_for_arch(unknown).has_value(), std::string("dispatch rejects '") + unknown + "'");
  }
  // The real ds4 GGUFs spell general.architecture "deepseek41" (Maya tools/ds4/pack_gguf.py:387);
  // the canonical name the engine emits stays "ds4" (arch_of), but the file's own name must map.
  const auto alias = u::family_for_arch("deepseek41");
  expect(alias.has_value() && *alias == u::FamilyId::ds4, "dispatch maps the real-file alias deepseek41");
}

// ---- NVFP4 reference (flint H2) ------------------------------------------------------------

// Pins u::nvfp4's host reference to the ggml MASTER NVFP4 layout (ggml/src/ggml-common.h
// block_nvfp4 + ggml-impl.h ggml_ue4m3_to_fp32/ggml_fp32_to_ue4m3, MIT): a 64-element block is
// 4 UE4M3 sub-scales FIRST (one per 16-element sub-block) + 32 packed E2M1 nibble bytes.
// dequant = kvalues_fp4[nib] (the DOUBLED E2M1 table 0,1,2,3,4,6,8,12 with the sign nibble at
// bit 3) x ue4m3_to_f32(scale), and ue4m3_to_f32 returns raw*0.5 to absorb that doubling.
// The C1 device kernel must match these numbers bit for bit.
void nvfp4_block_reference_matches_ggml_master() {
  // Block: scales 0x38 (raw 1.0 -> 0.5), 0x33 (raw 0.6875 -> 0.34375), 0x00 (zero), 0x7F (zero).
  // Sub-block 0 nibble pairs cover the whole kvalues table; the rest are zero nibbles.
  std::uint8_t blk[36] = {};
  blk[0] = 0x38; blk[1] = 0x33; blk[2] = 0x00; blk[3] = 0x7F;
  // qs byte j (sub 0): LOW nibble = element j, HIGH nibble = element 8+j — so code i lands on
  // element i.
  for (int j = 0; j < 8; ++j) blk[4 + j] = static_cast<std::uint8_t>(j | ((8 + j) << 4));
  float out[64];
  u::nvfp4::dequant_block(blk, out);
  // sub-block 0 (scale byte 0x38 = raw 1.0): value = E2M1 * raw, so the true E2M1 set exactly;
  // (the halves inside ue4m3_to_f32 pair with the DOUBLED table, they do not scale the result)
  const float e0[16] = {0.f, .5f, 1.f, 1.5f, 2.f, 3.f, 4.f, 6.f, 0.f, -.5f, -1.f, -1.5f, -2.f, -3.f, -4.f, -6.f};
  for (int i = 0; i < 16; ++i) expect(out[i] == e0[i], "nibble table x raw 1.0 matches, elem order low-then-high");
  for (int i = 16; i < 64; ++i) expect(out[i] == 0.0f, "zero nibbles and the 0x00/0x7F scales give zero");
  // UE4M3 corners (raw*0.5 convention): smallest subnormal, 1.0-scale, saturating max, zero/NaN.
  expect(u::nvfp4::ue4m3_to_f32(0x00) == 0.0f && u::nvfp4::ue4m3_to_f32(0x7F) == 0.0f, "0x00 and 0x7F read as zero");
  expect(u::nvfp4::ue4m3_to_f32(0x01) == 0.0009765625f, "subnormal man*2^-9 then halved = 2^-10");
  expect(u::nvfp4::ue4m3_to_f32(0x38) == 0.5f, "raw 1.0 halves to 0.5 (pairs with the doubled table)");
  expect(u::nvfp4::ue4m3_to_f32(0x7E) == 224.0f, "max normal raw 448 halves to 224");
  // The quantizer side: fp32_to_ue4m3 feeds amax/6.0 into the scale; amax 6.0 -> 0x38 round-
  // trips, and max-representable sub-blocks reconstruct amax exactly (12 * 0.5 = 6.0).
  expect(u::nvfp4::f32_to_ue4m3(1.0f) == 0x38, "raw scale 1.0 encodes as 0x38");
  expect(u::nvfp4::f32_to_ue4m3(0.0f) == 0x00, "zero scale encodes as 0x00");
  // Sub-block nibble indexing: qs[s*8+j] low nibble = element s*16+j, high nibble = element
  // s*16+8+j. Put a single 6.0 (nibble 7) at element 40 (sub 2, j=0 high) and nowhere else.
  std::uint8_t b2[36] = {};
  for (int s = 0; s < 4; ++s) b2[s] = 0x38;         // every sub-scale raw 1.0
  b2[4 + 2 * 8 + 0] = static_cast<std::uint8_t>(0x00 | (7 << 4));
  float o2[64];
  u::nvfp4::dequant_block(b2, o2);
  expect(o2[40] == 6.0f, "high nibble at s*8+j lands on element s*16+8+j (E2M1 6 x raw 1.0)");
  bool rest_zero = true;
  for (int i = 0; i < 64; ++i) if (i != 40 && o2[i] != 0.0f) rest_zero = false;
  expect(rest_zero, "all other elements stay zero");
}

// ---- cpu expert lane (C3) -----------------------------------------------------------------

// Exact fp16 bit patterns for the probe scales (all powers of two, so the fp16 field is exact):
// 1.0, 2.0, 4.0, -1.0.
constexpr std::uint16_t kH1 = 0x3c00, kH2 = 0x4000, kH4 = 0x4400, kHM1 = 0xbc00;

// One 256x256 matrix, every row encoded alike, for one quant, under a value pattern:
//   pat 0: every value +1;  pat 1: every value +2;  pat 2: every value -1;
//   pat 3 ("hi64"): value 4 for columns 0..63, value 1 for columns 64..255 (3 for IQ3_S, whose
//         per-32-col factor 1+2*sc is odd only).
// The encodings are the block layouts the box kernels use (u_iq.cu / maya iq_kernels.cu):
//   IQ4_NL (20): 8 x [f16 d][16 B of nibble-8 = codebook 1], d per 32 columns.
//   IQ3_S  (21): f16 d + qs byte 0 (grid[0] = 0x01010101: four 1s) + signs + per-32-col 4-bit sc,
//         value = d * (1 + 2*sc) * grid byte.
//   IQ4_XS (23): f16 d + per-32-col 6-bit ls (low nibble in scales_l, bits 4..5 in scales_h),
//         value = d * (ls - 32) * codebook[nibble 8 = 1].
//   Q2_0   (42): Strata-custom 2-bit code, value = d * (code - 1), d per 64-column block.
std::vector<std::uint8_t> expert_matrix(int type, int pat) {
  const std::size_t row_bytes = type == 20 ? 144 : type == 21 ? 110 : type == 23 ? 136 : 72;
  std::vector<std::uint8_t> w(256 * row_bytes, 0);
  for (std::size_t r = 0; r < 256; ++r) {
    std::uint8_t* row = w.data() + r * row_bytes;
    const auto put16 = [&](std::size_t at, std::uint16_t v) {
      row[at] = static_cast<std::uint8_t>(v);
      row[at + 1] = static_cast<std::uint8_t>(v >> 8);
    };
    if (type == 20) {
      for (int sb = 0; sb < 8; ++sb) {
        const std::uint16_t d = pat == 0 ? kH1 : pat == 1 ? kH2 : pat == 2 ? kHM1 : (sb < 2 ? kH4 : kH1);
        put16(18 * sb, d);
        std::memset(row + 18 * sb + 2, 0x88, 16);  // 32 nibbles, all index 8 (codebook value 1)
      }
    } else if (type == 21) {
      put16(0, pat == 1 ? kH2 : kH1);  // d; qs/qh stay 0 -> grid[0] (values +1); d=2 makes value 2
      if (pat == 2) std::memset(row + 74, 0xFF, 32);  // signs (bytes 74..105) all set -> value -1
      else if (pat == 3) row[106] = 0x11;  // scales @106: sc=1 for chunks 0,1 (value 3), 0 elsewhere
    } else if (type == 23) {
      put16(0, pat == 2 ? kHM1 : kH1);  // value = d * (ls - 32) * codebook; d stays 1 (or -1)
      put16(2, 0xaaaa);                 // scales_h: ls bits 4..5 are 2 for every group (ls = 33/34/36)
      const std::uint8_t n = pat == 1 ? 0x22 : 0x11;  // ls low nibble: 2 or 1
      row[4] = pat == 3 ? 0x44 : n;                   // scales_l @4..7: groups 0,1 get ls-32 = 4
      row[5] = n;
      row[6] = n;
      row[7] = n;
      std::memset(row + 8, 0x88, 128);  // nibble 8 everywhere (codebook value 1)
    } else {  // Q2_0 (42): 4 x [f16 d][16 B packing four 2-bit codes]
      for (int blk = 0; blk < 4; ++blk) {
        put16(18 * blk, (pat == 3 && blk == 0) ? kH2 : kH1);  // d=2 with code 3 -> value 4 in blk 0
        const std::uint8_t code =
            pat == 0 ? 0xAA : pat == 1 ? 0xFF : pat == 2 ? 0x00 : (blk == 0 ? 0xFF : 0xAA);
        std::memset(row + 18 * blk + 2, code, 16);
      }
    }
  }
  return w;
}

void cpu_expert_supported_covers_glm_and_qwen_sets() {
  for (const int t : {16, 18, 22, 20, 21, 23, 42}) {
    expect(u::cpu_expert_supported(t, t, t), "cpu lane supports quant " + std::to_string(t));
  }
  expect(!u::cpu_expert_supported(99, 16, 16), "cpu lane refuses an unknown quant");
  expect(!u::cpu_expert_supported(16, 16, 99), "cpu lane refuses an unknown down quant");
}

void cpu_expert_ffn_matches_the_qwen_quants() {
  float x[256], out[256], scratch[512];
  for (int i = 0; i < 256; ++i) x[i] = 1.0f;
  // With x = 1 and uniform rows: g = the gate row's value sum, u = the up row's value sum,
  // h = silu(g) * u (swiglu, act 1), out[r] = h * the down row's column sum. A/B check the scale
  // and sign paths; C/D/E put hi64 on the gate/down/up so an element-order bug changes a sum.
  struct Probe {
    int gpat, upat, dpat;
    double want_other, want_iq3s;
  };
  const Probe probes[] = {
      {0, 1, 0, 33554432.0, 33554432.0},  // A: g=256, u=512, down 256 (f32-exact)
      {2, 0, 0, 0.0, 0.0},                // B: g=-256 -> silu(-256) underflows to -0.0 (sign path)
      {3, 0, 0, 29360128.0, 25165824.0},  // C: gate hi64: g=448 (IQ3_S 384), u=256, down 256
      {0, 0, 3, 29360128.0, 25165824.0},  // D: down hi64 column sum 448 (IQ3_S 384)
      {0, 3, 0, 29360128.0, 25165824.0},  // E: up hi64: u=448 (IQ3_S 384)
  };
  for (const int t : {20, 21, 23, 42}) {
    for (const auto& p : probes) {
      const auto gate = expert_matrix(t, p.gpat);
      const auto up = expert_matrix(t, p.upat);
      const auto down = expert_matrix(t, p.dpat);
      u::cpu_expert_ffn(t, gate.data(), t, up.data(), t, down.data(), 256, 256, /*act=*/1, /*limit=*/0.0f,
                        x, out, scratch);
      const double want = t == 21 ? p.want_iq3s : p.want_other;
      const double tol = std::fabs(want) < 1.0 ? 1e-3 : std::fabs(want) * 1e-5;
      bool all_ok = true;
      for (int r = 0; r < 256; ++r) {
        if (std::fabs(static_cast<double>(out[r]) - want) > tol) all_ok = false;
      }
      expect(all_ok, "ffn probe (g" + std::to_string(p.gpat) + ",u" + std::to_string(p.upat) + ",d" +
                         std::to_string(p.dpat) + ") quant " + std::to_string(t));
    }
  }
}

// The PLE n-gram addressing (u_ple.hpp): the pure host part of qwen4exp's per-layer embedding
// lookup. The GPU path is box-proven (token-exact parity, stage 6 of the qwen4exp lane); this
// case anchors the n-gram window semantics (the EOS reset cuts the rest of the window) and the
// hash + row formulas on a synthetic spec with hand-computed values.
static void ple_ngram_window_and_row_addressing() {
  const std::int32_t eos = 151645;
  u::ple::Spec s;
  s.ngram = 3;
  s.per_gram = 2;
  s.eos = eos;
  s.mult = {3, 5, 7};
  s.vocab = {1000, 1000, 700, 700};
  s.off = {0, 1000, 2000, 2700};
  expect(s.n_heads() == 4, "ple n_heads = (ngram-1) * per_gram");

  std::int64_t ctx[3] = {0};
  const std::int32_t prev[2] = {500, 1234};
  u::ple::ngram_ctx(s, 7, prev, 2, ctx);
  expect(ctx[0] == 7 && ctx[1] == 500 && ctx[2] == 1234, "ple window: token + the two preceding tokens");

  const std::int32_t prev_eos[2] = {eos, 99};
  u::ple::ngram_ctx(s, 7, prev_eos, 2, ctx);
  expect(ctx[1] == eos && ctx[2] == eos, "ple window: an EOS at p-1 resets the rest of the window to EOS");

  const std::int32_t prev_mid[2] = {500, eos};
  u::ple::ngram_ctx(s, 7, prev_mid, 2, ctx);
  expect(ctx[1] == 500 && ctx[2] == eos, "ple window: an EOS further back cuts only the tail behind it");

  u::ple::ngram_ctx(s, 7, prev, 1, ctx);   // history shorter than the window: missing slots cut
  expect(ctx[1] == 500 && ctx[2] == eos, "ple window: missing history cuts to EOS");

  const std::uint64_t m2 = u::ple::mixed_n(s, ctx, 2);
  expect(m2 == ((7ULL * 3ULL) ^ (500ULL * 5ULL)), "ple hash: mixed_2 = ctx0*mult0 XOR ctx1*mult1");
  const std::int64_t full[3] = {7, 500, 1234};
  const std::uint64_t m3 = u::ple::mixed_n(s, full, 3);
  expect(m3 == ((7ULL * 3ULL) ^ (500ULL * 5ULL) ^ (1234ULL * 7ULL)), "ple hash: mixed_3 adds the XOR third term");

  // Hand-computed rows: 2513 % {1000,1000} + {0,1000}; 10351 % {700,700} + {2000,2700}.
  expect(u::ple::head_row(s, m2, 0) == 513, "ple row: mixed % vocab[h] + off[h] (head 0)");
  expect(u::ple::head_row(s, m2, 1) == 1513, "ple row: head 1 applies its region offset");
  expect(u::ple::head_row(s, m3, 2) == 2551, "ple row: order-3 head 2 (vocab 700 -> 551 + 2000)");
  expect(u::ple::head_row(s, m3, 3) == 3251, "ple row: order-3 head 3 (551 + 2700)");
}

// ---- the CPU expert lane's row dots (u_cpu_iq) ------------------------------------------------

void cpu_iq_fp16_round_trips() {
  expect(u::cpu::f32_to_fp16(1.0f) == 0x3C00, "fp16(1) = 0x3C00");
  expect(u::cpu::f32_to_fp16(-2.5f) == 0xC100, "fp16(-2.5) = 0xC100");
  expect(u::cpu::f32_to_fp16(65504.0f) == 0x7BFF, "fp16(65504) = 0x7BFF (max)");
  expect(u::cpu::f32_to_fp16(6.103515625e-05f) == 0x0400, "fp16(2^-14) = 0x0400 (min normal)");
  expect(u::cpu::f32_to_fp16(5.9604644775390625e-08f) == 0x0001, "fp16(2^-24) = 0x0001 (min subnormal)");
  expect(u::cpu::f32_to_fp16(1.0f + 1.0f / 2048.0f) == 0x3C00, "a tie rounds to even (down)");
  expect(u::cpu::f32_to_fp16(1.0f + 3.0f / 2048.0f) == 0x3C02, "a tie rounds to even (up)");
  int bad = 0;
  for (std::uint32_t h = 0; h < 0x10000u; ++h) {
    if ((h & 0x7C00u) == 0x7C00u && (h & 0x03FFu) != 0) continue;  // NaNs
    if (u::cpu::f32_to_fp16(u::cpu::fp16_to_f32((std::uint16_t)h)) != (std::uint16_t)h) ++bad;
  }
  expect(bad == 0, "every non-NaN half round-trips through f32 (" + std::to_string(bad) + " did not)");
}

// Random rows of `type` (random code bytes, a sane fp16 scale in every block) for the dot tests.
std::vector<std::uint8_t> cpu_iq_random_row(int type, int n, std::uint32_t& seed) {
  auto next = [&seed]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
  const std::int64_t bytes = u::cpu::row_bytes(type, n);
  std::vector<std::uint8_t> row((std::size_t)bytes);
  for (auto& b : row) b = (std::uint8_t)next();
  const int bb = type == 20 || type == 42 ? 18 : (int)(bytes / (n / 256));
  for (std::int64_t off = 0; off + bb <= bytes; off += bb) {
    const std::uint16_t d = u::cpu::f32_to_fp16(0.002f + 0.02f * (float)(next() % 1000) / 1000.0f);
    std::memcpy(row.data() + off, &d, 2);
  }
  return row;
}

void cpu_iq_dots_match_the_reference() {
  if (!u::cpu::have_avx2()) {
    std::fprintf(stderr, "  (this CPU has no AVX2: the AVX2 forms are not checked here)\n");
    return;
  }
  std::uint32_t seed = 20261025u;
  for (int type : {18, 20, 21, 22, 23, 42}) {
    const int n = u::cpu::iq_type_q8k(type) ? 2560 : 640;  // gate/up rows | down rows
    std::vector<float> x((std::size_t)n);
    for (auto& v : x) {
      seed = seed * 1664525u + 1013904223u;
      v = ((float)(seed >> 9) / 8388608.0f - 1.0f) * 3.0f;
    }
    std::vector<u::cpu::BlockQ8K> xk((std::size_t)(n / 256 + 1));
    std::vector<u::cpu::BlockQ80> x0((std::size_t)(n / 32));
    if (u::cpu::iq_type_q8k(type)) u::cpu::quantize_q8k(x.data(), xk.data(), n);
    else u::cpu::quantize_q80(x.data(), x0.data(), n);
    const void* xq = u::cpu::iq_type_q8k(type) ? (const void*)xk.data() : (const void*)x0.data();
    double worst = 0.0;
    for (int r = 0; r < 64; ++r) {
      const std::vector<std::uint8_t> row = cpu_iq_random_row(type, n, seed);
      const double ref = u::cpu::dot_ref(type, row.data(), xq, n);
      const double avx = u::cpu::detail::dot_avx2(type, row.data(), xq, n);
      worst = std::max(worst, std::fabs(avx - ref) / std::max(1.0, std::fabs(ref)));
    }
    expect(worst < 1e-5, "type " + std::to_string(type) + ": the AVX2 dot matches the scalar reference (worst rel " +
                             std::to_string(worst) + ")");
  }
}

void cpu_iq_q2_0_dot_matches_its_dequant() {
  std::uint32_t seed = 7u;
  const int n = 640;
  std::vector<float> x((std::size_t)n);
  for (auto& v : x) {
    seed = seed * 1664525u + 1013904223u;
    v = (float)(seed >> 9) / 8388608.0f - 1.0f;
  }
  std::vector<u::cpu::BlockQ80> x0((std::size_t)(n / 32));
  u::cpu::quantize_q80(x.data(), x0.data(), n);
  const std::vector<std::uint8_t> row = cpu_iq_random_row(42, n, seed);
  double want = 0.0;  // value n = d * (code - 1), code at qs[n/4] >> 2*(n%4); activation = its Q8_0 dequant
  for (int i = 0; i < n; ++i) {
    const std::uint8_t* blk = row.data() + (i / 64) * 18;
    std::uint16_t d = 0;
    std::memcpy(&d, blk, 2);
    const int k = i % 64;
    const int code = (blk[2 + k / 4] >> (2 * (k % 4))) & 3;
    const double w = (double)u::cpu::fp16_to_f32(d) * (code - 1);
    const double a = (double)u::cpu::fp16_to_f32(x0[(std::size_t)(i / 32)].d) * x0[(std::size_t)(i / 32)].qs[i % 32];
    want += w * a;
  }
  const double got = u::cpu::dot_ref(42, row.data(), x0.data(), n);
  expect(std::fabs(got - want) <= 1e-4 * std::max(1.0, std::fabs(want)), "Q2_0's dot = the dot of its dequantized rows");
}

void cpu_iq_quantizers_bound_their_error() {
  std::uint32_t seed = 3u;
  std::vector<float> x(2560);
  for (auto& v : x) {
    seed = seed * 1664525u + 1013904223u;
    v = ((float)(seed >> 9) / 8388608.0f - 1.0f) * 5.0f;
  }
  std::vector<u::cpu::BlockQ8K> xk(10);
  u::cpu::quantize_q8k(x.data(), xk.data(), 2560);
  std::vector<u::cpu::BlockQ80> x0(80);
  u::cpu::quantize_q80(x.data(), x0.data(), 2560);
  double ek = 0.0, e0 = 0.0;
  int bsum_bad = 0;
  for (int i = 0; i < 2560; ++i) {
    const auto& bk = xk[(std::size_t)(i / 256)];
    ek = std::max(ek, std::fabs((double)bk.d * bk.qs[i % 256] - x[(std::size_t)i]) / std::fabs((double)bk.d));
    const auto& b0 = x0[(std::size_t)(i / 32)];
    const double d0 = u::cpu::fp16_to_f32(b0.d);
    e0 = std::max(e0, std::fabs(d0 * b0.qs[i % 32] - x[(std::size_t)i]) / d0);
  }
  for (const auto& bk : xk)
    for (int j = 0; j < 16; ++j) {
      int s = 0;
      for (int ii = 0; ii < 16; ++ii) s += bk.qs[j * 16 + ii];
      if (s != bk.bsums[j]) ++bsum_bad;
    }
  expect(ek <= 0.5 + 1e-3, "Q8_K: every value within half a step");
  expect(e0 <= 0.5 + 127.0 / 2048.0, "Q8_0: within half a step plus the fp16 scale's rounding (127 x 2^-11)");
  expect(bsum_bad == 0, "Q8_K bsums are the 16-value sums");
}


// ---- the CPU expert lane's pool (u_cpu_lane) ----------------------------------------------------

// One layer of n_exp random experts of these types, contiguous per expert as the GGUF lays them out.
struct LaneFixture {
  std::vector<std::uint8_t> gate, up, down;
  u::cpu::LaneLayer L;
};
LaneFixture lane_fixture(int gt, int ut, int dt, int ne, int nf, int n_exp, std::uint32_t& seed) {
  LaneFixture f;
  auto fill = [&](std::vector<std::uint8_t>& buf, int type, int rows, int n) {
    for (int r = 0; r < n_exp * rows; ++r) {
      const std::vector<std::uint8_t> row = cpu_iq_random_row(type, n, seed);
      buf.insert(buf.end(), row.begin(), row.end());
    }
  };
  fill(f.gate, gt, nf, ne);
  fill(f.up, ut, nf, ne);
  fill(f.down, dt, ne, nf);
  f.L.gate_type = gt;
  f.L.up_type = ut;
  f.L.down_type = dt;
  f.L.gate = f.gate.data();
  f.L.up = f.up.data();
  f.L.down = f.down.data();
  f.L.gate_stride = (std::int64_t)nf * u::cpu::row_bytes(gt, ne);
  f.L.up_stride = (std::int64_t)nf * u::cpu::row_bytes(ut, ne);
  f.L.down_stride = (std::int64_t)ne * u::cpu::row_bytes(dt, nf);
  f.L.n_embd = ne;
  f.L.n_ff = nf;
  return f;
}

void cpu_lane_serves_like_the_reference() {
  const int ne = 512, nf = 128, n_exp = 6, n_tok = 3, k = 4, cpu = -7;
  std::uint32_t seed = 11u;
  u::cpu::LanePool pool(3, false);
  expect(pool.threads() == 4, "three workers beside the caller");
  for (const auto& types : {std::array<int, 3>{21, 21, 20}, std::array<int, 3>{18, 22, 42}}) {
    LaneFixture f = lane_fixture(types[0], types[1], types[2], ne, nf, n_exp, seed);
    expect(f.L.usable(), "the layer is usable");
    std::vector<float> x((std::size_t)n_tok * ne);
    for (auto& v : x) {
      seed = seed * 1664525u + 1013904223u;
      v = ((float)(seed >> 9) / 8388608.0f - 1.0f) * 2.0f;
    }
    // expert 2 serves every token (one job, three tokens); entries with other codes stay the GPU's
    const int ids[n_tok * k] = {2, 0, 5, 1, 4, 2, 3, 0, 2, 5, 1, 3};
    const int eff[n_tok * k] = {cpu, 0, cpu, -1, cpu, cpu, -2, 3, cpu, 1, cpu, cpu};
    for (int round = 0; round < 2; ++round) {
      if (round == 1) std::this_thread::sleep_for(std::chrono::milliseconds(40));  // the workers sleep: wake them
      std::vector<float> ymiss((std::size_t)n_tok * k * ne, 12345.0f);
      expect(pool.serve(f.L, n_tok, k, x.data(), ids, eff, cpu, ymiss.data()), "serve runs");
      double worst = 0.0;
      bool untouched = true;
      std::vector<float> ref((std::size_t)ne);
      for (int j = 0; j < n_tok * k; ++j) {
        const float* got = ymiss.data() + (std::size_t)j * ne;
        if (eff[j] != cpu) {
          for (int r = 0; r < ne; ++r) untouched = untouched && got[r] == 12345.0f;
          continue;
        }
        u::cpu::lane_expert_ref(f.L, ids[j], x.data() + (std::size_t)(j / k) * ne, ref.data());
        double scale = 1.0;
        for (int r = 0; r < ne; ++r) scale = std::max(scale, (double)std::fabs(ref[(std::size_t)r]));
        for (int r = 0; r < ne; ++r) worst = std::max(worst, std::fabs((double)got[r] - ref[(std::size_t)r]) / scale);
      }
      const std::string what = "types " + std::to_string(types[0]) + "/" + std::to_string(types[1]) + "/" +
                               std::to_string(types[2]) + " round " + std::to_string(round);
      expect(worst < 2e-3, what + ": every CPU entry = the reference expert (worst rel " + std::to_string(worst) + ")");
      expect(untouched, what + ": the GPU's entries are not touched");
    }
  }
  expect(pool.experts == 2 * 2 * 5, "five distinct experts per serve, two serves per layer, two layers");
  expect(pool.rows_tokens == 2 * 2 * 7, "seven (expert, token) pairs per serve");
  std::vector<float> none(1);
  const int one = 0;
  u::cpu::LaneLayer L;
  expect(!pool.serve(L, 9, 1, none.data(), &one, &one, 0, none.data()), "nine tokens are refused");
}

}  // namespace

int main(int argc, char** argv) {
  return u_test::run_cases(argc, argv,
                           {
                               {"gguf_parses_a_committed_header", gguf_parses_a_committed_header},
                               {"gguf_reads_a_header_from_disk", gguf_reads_a_header_from_disk},
                               {"gguf_reads_nvfp4_tensors", gguf_reads_nvfp4_tensors},
                               {"nvfp4_block_reference_matches_ggml_master", nvfp4_block_reference_matches_ggml_master},
                               {"geometry_guard_accepts_valid_key_sets", geometry_guard_accepts_valid_key_sets},
                               {"geometry_guard_rejects_bad_key_sets", geometry_guard_rejects_bad_key_sets},
                               {"protocol_round_trips_valid_lines", protocol_round_trips_valid_lines},
                               {"protocol_rejects_malformed_and_unknown_lines", protocol_rejects_malformed_and_unknown_lines},
                               {"dispatch_maps_known_archs_and_rejects_unknown", dispatch_maps_known_archs_and_rejects_unknown},
                               {"cpu_expert_supported_covers_glm_and_qwen_sets", cpu_expert_supported_covers_glm_and_qwen_sets},
                               {"cpu_expert_ffn_matches_the_qwen_quants", cpu_expert_ffn_matches_the_qwen_quants},
                               {"ple_ngram_window_and_row_addressing", ple_ngram_window_and_row_addressing},
                               {"cpu_iq_fp16_round_trips", cpu_iq_fp16_round_trips},
                               {"cpu_iq_dots_match_the_reference", cpu_iq_dots_match_the_reference},
                               {"cpu_iq_q2_0_dot_matches_its_dequant", cpu_iq_q2_0_dot_matches_its_dequant},
                               {"cpu_iq_quantizers_bound_their_error", cpu_iq_quantizers_bound_their_error},
                               {"cpu_lane_serves_like_the_reference", cpu_lane_serves_like_the_reference},
                           });
}
