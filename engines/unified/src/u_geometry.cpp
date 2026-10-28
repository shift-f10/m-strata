#include "u_geometry.hpp"

#include <array>
#include <cmath>
#include <string>
#include <string_view>

namespace u {
namespace {

GeometryError refuse(const std::string& arch, const std::string& what) {
  return GeometryError{arch + " geometry: " + what};
}

bool integer_key(const GgufFile& f, const std::string& key, std::int64_t& out) {
  const auto* v = f.find(key);
  if (v == nullptr) return false;
  switch (v->type) {
    case GgufType::u8: case GgufType::i8: case GgufType::u16: case GgufType::i16:
    case GgufType::u32: case GgufType::i32: case GgufType::u64: case GgufType::i64:
      out = v->integer;
      return true;
    default:
      return false;
  }
}

bool real_key(const GgufFile& f, const std::string& key, double& out) {
  const auto* v = f.find(key);
  if (v == nullptr || (v->type != GgufType::f32 && v->type != GgufType::f64)) return false;
  out = v->real;
  return std::isfinite(out);
}

struct Range final {
  std::int64_t lo{0};
  std::int64_t hi{0};
  [[nodiscard]] bool contains(std::int64_t v) const { return v >= lo && v <= hi; }
};

// Pins a dimension to one exact value (used for Raon-1-mini's published shape).
constexpr Range exact(std::int64_t v) { return Range{v, v}; }

struct GeometrySpec final {
  FamilyId family;
  Range block_count;
  Range embedding_length;
  Range head_count;
  Range head_dim;
  Range feed_forward_length;
  Range expert_feed_forward_length;
  Range expert_count;
  Range expert_used_count;
  Range expert_shared_count;
  Range context_length;
  Range vocab_size;
  RouterKind router;
  RopeKind rope;
};

// Generic bounds for the families whose exact shape is bound on the GPU box, not here:
// broad enough for the real GGUFs, tight enough that a hostile header cannot pass a wild
// value. Raon-1-mini is pinned to its published shape below.
constexpr Range kBlock{1, 1024};
constexpr Range kEmbd{1, 1048576};
constexpr Range kHeads{1, 8192};
constexpr Range kHeadDim{1, 8192};
constexpr Range kFf{1, 33554432};
constexpr Range kFfOpt{0, 33554432};  // dense feed_forward_length that a MoE-every-layer family (qwen4exp) omits
constexpr Range kFfExp{1, 33554432};
constexpr Range kExperts{1, 4096};
constexpr Range kExpertsUsed{1, 1024};
constexpr Range kExpertsShared{0, 256};
constexpr Range kContext{1, 16777216};
constexpr Range kVocab{1, 8388608};

constexpr std::array<GeometrySpec, 7> kSpecs{{
    {FamilyId::glm5next, kBlock, kEmbd, kHeads, kHeadDim, kFf, kFfExp, kExperts, kExpertsUsed, kExpertsShared,
     kContext, kVocab, RouterKind::sigmoid, RopeKind::nope},  // GLM-5.3 is NoPE-MLA: no rope.freq_base key
    {FamilyId::ds4, kBlock, kEmbd, kHeads, kHeadDim, kFf, kFfExp, kExperts, kExpertsUsed, kExpertsShared,
     kContext, kVocab, RouterKind::sigmoid, RopeKind::rope},
    // Raon-1-mini: pinned to its published shape; sigmoid router; NoPE (no rope.freq_base).
    {FamilyId::raon1, exact(25), exact(2560), exact(40), exact(128), exact(12032), exact(1088), exact(128),
     exact(6), exact(1), exact(262144), exact(259840), RouterKind::sigmoid, RopeKind::nope},
    {FamilyId::qwen35moe, kBlock, kEmbd, kHeads, kHeadDim, kFf, kFfExp, kExperts, kExpertsUsed, kExpertsShared,
     kContext, kVocab, RouterKind::softmax, RopeKind::rope},  // softmax top-8 (the sigmoid is only the shared-expert gate)
    {FamilyId::qwen4exp, kBlock, kEmbd, kHeads, kHeadDim, kFfOpt, kFfExp, kExperts, kExpertsUsed, kExpertsShared,
     kContext, kVocab, RouterKind::softmax, RopeKind::rope},  // MoE every layer: no dense feed_forward_length key
    {FamilyId::gemma4, kBlock, kEmbd, kHeads, kHeadDim, kFf, kFfExp, kExperts, kExpertsUsed, kExpertsShared,
     kContext, kVocab, RouterKind::softmax, RopeKind::rope},
    // flint (RecursiveSI; spec 2026-10-25-flint-engine-design): published shape 49L, hidden 2048,
    // 32/4 GQA head_dim 128, sliding window 513 on 35 of 49 layers, rope theta 1e4, MoE 256 experts
    // top-6 RENORMALIZED (sigmoid gating: GGUF expert_gating_func = 2.0) + 1 shared (moe_ffn 512),
    // dense intermediate 6144, vocab 150144, ctx 262144. BROAD ranges, not exact pins: unlike raon1
    // this family ships variants (the Apache-2.0 release line is young), and the per-layer
    // full/sliding pattern + the NVFP4 sidecars are graph-lane (H2) concerns, not guard concerns.
    {FamilyId::flint, kBlock, kEmbd, kHeads, kHeadDim, kFf, kFfExp, kExperts, kExpertsUsed, kExpertsShared,
     kContext, kVocab, RouterKind::sigmoid, RopeKind::rope},
}};

const GeometrySpec& spec_for(FamilyId family) {
  for (const auto& s : kSpecs) {
    if (s.family == family) return s;
  }
  return kSpecs.front();  // unreachable: every FamilyId has a spec
}

// Reads one integer key, checks its range, and stores it; records the first failure.
bool read_dim(const GgufFile& f, const std::string& arch, const char* suffix, const Range& range, std::int64_t& out,
              std::string& why) {
  const std::string key = arch + suffix;
  std::int64_t v = 0;
  if (!integer_key(f, key, v)) {
    why = "missing or non-integer key " + key;
    return false;
  }
  if (!range.contains(v)) {
    why = key + " is out of range";
    return false;
  }
  out = v;
  return true;
}

// Like read_dim, but a MISSING key is allowed: it defaults to 0 before the range check. An
// architecture whose range excludes 0 (e.g. a pinned exact(1)) still effectively requires the key,
// while one with no shared experts — gemma4 omits <arch>.expert_shared_count entirely — passes with
// 0. A key that IS present but non-integer is still refused.
bool read_dim_opt(const GgufFile& f, const std::string& arch, const char* suffix, const Range& range,
                  std::int64_t& out, std::string& why) {
  const std::string key = arch + suffix;
  std::int64_t v = 0;
  if (f.find(key) != nullptr && !integer_key(f, key, v)) {
    why = "non-integer key " + key;
    return false;
  }
  if (!range.contains(v)) {  // absent => v == 0
    why = key + " is out of range";
    return false;
  }
  out = v;
  return true;
}

}  // namespace

std::variant<Geometry, GeometryError> read_geometry(const GgufFile& f) {
  const auto* arch_val = f.find("general.architecture");
  if (arch_val == nullptr || arch_val->type != GgufType::string) {
    return GeometryError{"geometry: general.architecture is missing or not a string"};
  }
  const std::string arch = arch_val->text;
  const auto family = family_for_arch(arch);
  if (!family) return GeometryError{"geometry: unknown architecture '" + arch + "'"};

  const GeometrySpec& s = spec_for(*family);
  Geometry g;
  g.family = *family;
  g.arch = arch;
  g.router = s.router;
  g.rope = s.rope;

  std::string why;
  const bool ok =
      read_dim(f, arch, ".block_count", s.block_count, g.block_count, why) &&
      read_dim(f, arch, ".embedding_length", s.embedding_length, g.embedding_length, why) &&
      read_dim(f, arch, ".attention.head_count", s.head_count, g.head_count, why) &&
      read_dim(f, arch, ".attention.key_length", s.head_dim, g.head_dim, why) &&
      read_dim_opt(f, arch, ".feed_forward_length", s.feed_forward_length, g.feed_forward_length, why) &&
      read_dim(f, arch, ".expert_feed_forward_length", s.expert_feed_forward_length, g.expert_feed_forward_length, why) &&
      read_dim(f, arch, ".expert_count", s.expert_count, g.expert_count, why) &&
      read_dim(f, arch, ".expert_used_count", s.expert_used_count, g.expert_used_count, why) &&
      read_dim_opt(f, arch, ".expert_shared_count", s.expert_shared_count, g.expert_shared_count, why) &&
      read_dim(f, arch, ".context_length", s.context_length, g.context_length, why);
  if (!ok) return refuse(arch, why);

  // vocab_size: the <arch>.vocab_size key when present, else the token_embd.weight row count
  // (dims.back() = the logits width). gemma4 omits the key and carries the vocab only in the
  // embedding tensor's shape + the token list, so derive it from token_embd.weight when absent.
  {
    std::int64_t vocab = 0;
    if (!integer_key(f, arch + ".vocab_size", vocab)) {
      const GgufTensor* te = f.tensor("token_embd.weight");
      if (te == nullptr || te->dims.empty())
        return refuse(arch, "missing " + arch + ".vocab_size (and no token_embd.weight to derive it from)");
      vocab = static_cast<std::int64_t>(te->dims.back());
    }
    if (!s.vocab_size.contains(vocab)) return refuse(arch, arch + ".vocab_size is out of range");
    g.vocab_size = vocab;
  }

  if (g.expert_used_count > g.expert_count) return refuse(arch, "expert_used_count exceeds expert_count");

  if (g.rope == RopeKind::rope) {
    double base = 0.0;
    if (!real_key(f, arch + ".rope.freq_base", base)) return refuse(arch, "missing or non-float key " + arch + ".rope.freq_base");
    if (base <= 1.0) return refuse(arch, arch + ".rope.freq_base is out of range");
    g.rope_freq_base = base;
  } else {
    g.rope_freq_base = 0.0;  // NoPE: no rotary base is read or required.
  }
  return g;
}

}  // namespace u
