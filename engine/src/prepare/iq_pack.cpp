// strata-prepare pack: upstream Strata's tools/iq_pack.py (v0.2.6), function for function (the Python names are kept so
// the two read side by side). The Python tool is the reference: every file this writes is compared with its output
// byte for byte (tools/secure/strata_prepare_parity.py). Its Model, file writes and conversions are pack_common's.
#include "iq_pack.hpp"

#include "gguf_header.hpp"
#include "pack_common.hpp"
#include "py_json.hpp"
#include "sha256.hpp"

#include "ggml.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace strata_prepare {
namespace fs = std::filesystem;

// ---- floats -------------------------------------------------------------------------------------------------------

unsigned short f16_bits(const float value, bool& overflow) noexcept {
  // numpy's npy_floatbits_to_halfbits with NPY_HALF_ROUND_TIES_TO_EVEN (what astype(float16) computes)
  std::uint32_t f = 0;
  std::memcpy(&f, &value, 4);
  const auto h_sgn = static_cast<std::uint16_t>((f & 0x80000000U) >> 16U);
  std::uint32_t f_exp = f & 0x7f800000U;
  if (f_exp >= 0x47800000U) {
    if (f_exp == 0x7f800000U) {
      const std::uint32_t f_sig = f & 0x007fffffU;
      overflow = true;
      return static_cast<unsigned short>(f_sig != 0U ? 0x7e00U + h_sgn : 0x7c00U + h_sgn);
    }
    overflow = true;
    return static_cast<unsigned short>(h_sgn + 0x7c00U);
  }
  if (f_exp <= 0x38000000U) {
    if (f_exp < 0x33000000U) return h_sgn;
    f_exp >>= 23U;
    std::uint32_t f_sig = 0x00800000U + (f & 0x007fffffU);
    f_sig >>= (113U - f_exp);
    if (((f_sig & 0x00003fffU) != 0x00001000U) || (f & 0x000007ffU) != 0U) f_sig += 0x00001000U;
    const auto h_sig = static_cast<std::uint16_t>(f_sig >> 13U);
    return static_cast<unsigned short>(h_sgn + h_sig);
  }
  const auto h_exp = static_cast<std::uint16_t>((f_exp - 0x38000000U) >> 13U);
  std::uint32_t f_sig = f & 0x007fffffU;
  if ((f_sig & 0x00003fffU) != 0x00001000U) f_sig += 0x00001000U;
  auto h_sig = static_cast<std::uint32_t>(f_sig >> 13U);
  h_sig += h_exp;
  if (h_sig == 0x7c00U) overflow = true;
  return static_cast<unsigned short>(h_sgn + h_sig);
}

unsigned short bf16_bits(const float value) noexcept {
  std::uint32_t u = 0;
  std::memcpy(&u, &value, 4);
  return static_cast<unsigned short>((u + (0x7fffU + ((u >> 16U) & 1U))) >> 16U);
}

namespace {

// ---- iq_pack.py's constants ---------------------------------------------------------------------------------------

const std::set<std::string> kFloat{"BF16", "F32", "F16"};
const std::set<std::string> kNotInPack{"per_layer_token_embd.weight"};
const std::map<std::string, std::string> kForm{
    {"ffn_gate_inp.weight", "BF16"},    {"ffn_gate_inp_shexp.weight", "BF16"}, {"hc_attn_down.weight", "BF16"},
    {"hc_attn_up.weight", "BF16"},      {"hc_attn_inject.weight", "BF16"},    {"hc_ffn_down.weight", "BF16"},
    {"hc_ffn_up.weight", "BF16"},       {"hc_ffn_inject.weight", "BF16"},     {"output_hc_down.weight", "BF16"},
    {"output_hc_up.weight", "BF16"},    {"indexer.k_proj.weight", "BF16"},    {"indexer.q_proj.weight", "BF16"},
    {"ssm_alpha.weight", "BF16"},       {"ssm_beta.weight", "BF16"},          {"ple_key.weight", "BF16"},
    {"ple_value.weight", "BF16"},       {"ple_conv1d.weight", "F16"},         {"attn_q_norm.weight", "F32"},
    {"attn_k_norm.weight", "F32"},      {"hc_attn_norm.weight", "F32"},       {"hc_ffn_norm.weight", "F32"},
    {"output_hc_norm.weight", "F32"},   {"indexer.q_norm.weight", "F32"},     {"indexer.k_norm.weight", "F32"},
    {"ple_norm_conv.weight", "F32"},    {"ple_norm_key.weight", "F32"},       {"ple_norm_query.weight", "F32"},
    {"ssm_a", "F32"},                   {"ssm_conv1d.weight", "F32"},         {"ssm_dt.bias", "F32"},
    {"ssm_norm.weight", "F32"},
};
const std::set<std::string> kNativePleKey{"Q2_0", "Q8_0", "IQ3_XXS", "IQ4_XS"};
const std::map<std::string, std::string> kKind{{"BF16", "4"}, {"F16", "5"}, {"F32", "2"}};
constexpr std::array<const char*, 3> kRoles{"gate", "up", "down"};
constexpr std::uint64_t kAlign = 64;
constexpr std::uint64_t kNvfp4Tail = 16;

std::optional<std::string> form_of(const std::string& name) {
  static const std::regex layer(R"(^blk\.\d+\.)");
  const auto key = std::regex_replace(name, layer, "", std::regex_constants::format_first_only);
  const auto it = kForm.find(key);
  return it == kForm.end() ? std::nullopt : std::optional<std::string>{it->second};
}

bool is_expert(const std::string& name) {
  return name.rfind("blk.", 0) == 0U && name.size() >= 12U && name.compare(name.size() - 12U, 12U, "_exps.weight") == 0;
}

// ---- the expert table ---------------------------------------------------------------------------------------------

struct Layer {
  int l;
  std::uint32_t gu_type;
  std::uint32_t d_type;
  std::uint64_t offset;
  std::uint64_t blob;
  std::array<const Where*, 3> ws;
};

struct Experts {
  std::vector<Layer> layout;
  std::string text;
  std::uint64_t n_expert = 0;
  std::uint64_t total = 0;
};

std::variant<Experts, std::string> expert_layout(const Model& model, std::ostream& log) {
  int max_layer = -1;
  for (const auto& [name, w] : model.order) {
    if (name.rfind("blk.", 0) == 0U && is_expert(name)) {
      const auto dot = name.find('.', 4);
      max_layer = std::max(max_layer, std::stoi(name.substr(4, dot - 4)));
    }
  }
  if (max_layer < 0) return std::string{"the model has no expert tensors (blk.N.ffn_{gate,up,down}_exps.weight)"};
  const int n_layers = max_layer + 1;
  const auto router = [&](const int l) -> const Where* { return model.find("blk." + std::to_string(l) + ".ffn_gate_inp.weight"); };
  const Where* r0 = router(0);
  if (r0 == nullptr || r0->t->shape.size() < 2U) return std::string{"blk.0.ffn_gate_inp.weight: the router is missing"};
  const std::uint64_t n_expert = r0->t->shape[1];
  for (int l = 0; l < n_layers; ++l) {
    const Where* r = router(l);
    if (r == nullptr || r->t->shape.size() < 2U || r->t->shape[1] != n_expert) {
      return std::string{"the routers disagree on the expert count; a per-layer pruned model cannot be packed"};
    }
  }
  Experts out;
  out.n_expert = n_expert;
  std::vector<std::string> lines;
  std::uint64_t offset = 0;
  int n_split = 0;
  for (int l = 0; l < n_layers; ++l) {
    std::array<std::string, 3> names;
    std::array<const Where*, 3> ws{};
    std::string missing;
    for (std::size_t r = 0; r < 3; ++r) {
      names[r] = "blk." + std::to_string(l) + ".ffn_" + kRoles[r] + "_exps.weight";
      ws[r] = model.find(names[r]);
      if (ws[r] == nullptr) missing += (missing.empty() ? "" : ", ") + names[r];
    }
    if (!missing.empty()) return "layer " + std::to_string(l) + ": missing " + missing;
    for (const auto* w : ws) {
      if (!w->t->expected_bytes() || w->t->shape.size() != 3U || w->t->shape[2] != n_expert) {
        return "layer " + std::to_string(l) + ": an expert tensor is not [*, *, " + std::to_string(n_expert) + "] of whole blocks";
      }
    }
    std::array<std::uint64_t, 3> per{};
    for (std::size_t r = 0; r < 3; ++r) per[r] = *ws[r]->t->expected_bytes() / n_expert;
    if (per[0] != per[1] || ws[0]->t->type_name != ws[1]->t->type_name) return "layer " + std::to_string(l) + ": gate and up differ in type";
    std::uint64_t blob = per[0] + per[1] + per[2];
    const bool nvfp4 = ws[0]->t->type_name == "NVFP4" || ws[2]->t->type_name == "NVFP4";
    if (nvfp4) {
      for (std::size_t r = 0; r < 3; ++r) {
        if (ws[r]->t->type_name == "NVFP4" &&
            model.find("blk." + std::to_string(l) + ".ffn_" + kRoles[r] + "_exps.scale") == nullptr) {
          return "layer " + std::to_string(l) + ": NVFP4 experts without their .scale tensors";
        }
      }
      blob += kNvfp4Tail;
    }
    out.layout.push_back(Layer{l, ws[0]->t->type_id, ws[2]->t->type_id, offset, blob, ws});
    std::array<std::string, 3> files;
    for (std::size_t r = 0; r < 3; ++r) files[r] = ws[r]->shard == 0U ? std::string{} : utf8(model.paths[ws[r]->shard].filename());
    const bool same = files[0] == files[1] && files[1] == files[2];
    const std::string column = same ? files[0] : files[0] + "," + files[1] + "," + files[2];
    n_split += same ? 0 : 1;
    char head[160];
    std::snprintf(head, sizeof(head), "%d %u %u %llu %llu", l, ws[0]->t->type_id, ws[2]->t->type_id,
                  static_cast<unsigned long long>(offset), static_cast<unsigned long long>(blob));
    std::string line = head;
    if (nvfp4) {
      lines.push_back(line);
      offset += blob * n_expert;
      continue;
    }
    for (const auto* w : ws) line += " " + std::to_string(w->g->data_start + w->t->offset);
    lines.push_back(line + (column.empty() ? "" : " " + column));
    offset += blob * n_expert;
  }
  const std::string src = utf8(model.paths[0].filename());
  std::string head;
  if (n_split != 0) {
    head = "# strata native experts v4: layer gu_type d_type offset blob_bytes gate_off up_off down_off [shard | gate,up,down] "
           "(n_expert " + std::to_string(n_expert) + ", total " + std::to_string(offset) + "; absolute offsets in " + src +
           ", or in the named shard beside it - per role where the column is gate,up,down)\n";
    std::string which;
    for (const auto& layer : out.layout) {
      if (lines[static_cast<std::size_t>(layer.l)].find(',') != std::string::npos) which += (which.empty() ? "" : ", ") + std::to_string(layer.l);
    }
    log << n_split << " layer(s) have their gate/up/down in different shards: native_experts.txt v4, per-role shard column for "
        << which << "\n";
  } else {
    head = "# strata native experts v3: layer gu_type d_type offset blob_bytes gate_off up_off down_off [shard] (n_expert " +
           std::to_string(n_expert) + ", total " + std::to_string(offset) + "; absolute offsets in " + src +
           ", or in the named shard beside it)\n";
  }
  out.text = head;
  for (const auto& line : lines) out.text += line + "\n";
  out.total = offset;
  return out;
}

std::string native_experts_v5(const std::string& text) {
  const auto nl = text.find('\n');
  const std::string head = nl == std::string::npos ? text : text.substr(0, nl);
  const std::string rest = nl == std::string::npos ? std::string{} : text.substr(nl + 1);
  static const std::regex native(R"(^# strata native experts v(\d+):)");
  std::smatch m;
  if (!std::regex_search(head, m, native) || (m[1].str() != "3" && m[1].str() != "4")) {
    const std::string found = m.empty() ? "without a version header" : "v" + m[1].str();
    return "cannot mark native_experts.txt " + found + " as v5 for Hadamard-folded experts: only v3 and v4 tables are supported";
  }
  return "# strata native experts v5:" + head.substr(static_cast<std::size_t>(m.length(0))) +
         " - v5: Hadamard-folded experts, see hadamard.txt\n" + rest;
}

// hadamard.txt's text, nullopt for a model without prism.hadamard.*, or an error (not starting "# strata hadamard")
std::optional<std::string> hadamard_spec(const GgufHeader& g, const std::vector<Layer>& layout, const std::uint64_t n_embd,
                                         const std::uint64_t n_ff) {
  const auto* version = g.get("prism.hadamard.version");
  if (version == nullptr) return std::nullopt;
  const auto integer = [](const MetaValue* v, const std::int64_t fallback) {
    if (v == nullptr) return fallback;
    if (v->kind == MetaValue::Kind::integer) return v->integer;
    if (v->kind == MetaValue::Kind::real) return static_cast<std::int64_t>(v->real);
    if (v->kind == MetaValue::Kind::text) return static_cast<std::int64_t>(std::stoll(v->text));
    return fallback;
  };
  const auto text = [](const MetaValue* v) -> std::string { return v != nullptr && v->kind == MetaValue::Kind::text ? v->text : std::string{}; };
  if (integer(version, 0) != 1) return "unsupported prism.hadamard.version " + std::to_string(integer(version, 0));
  const std::int64_t block = integer(g.get("prism.hadamard.block_size"), 0);
  if (block <= 1 || (block & (block - 1)) != 0) return "invalid prism.hadamard.block_size " + std::to_string(block);
  if (text(g.get("prism.hadamard.transform")) != "normalized-sylvester-walsh-hadamard") return std::string{"unsupported prism.hadamard.transform"};
  if (text(g.get("prism.hadamard.axis")) != "input-last-dimension") return std::string{"unsupported prism.hadamard.axis"};
  const std::string mode = text(g.get("prism.hadamard.sign_mode"));
  if (mode != "identity" && mode != "explicit") return std::string{"unsupported prism.hadamard.sign_mode"};
  if (const auto* inverse = g.get("prism.hadamard.inverse_weight_names"); inverse != nullptr && !inverse->items.empty()) {
    return std::string{"prism.hadamard.inverse_weight_names (a rotated token embedding) is not supported by this engine"};
  }
  const auto* names = g.get("prism.hadamard.weight_names");
  if (names == nullptr || names->items.empty()) return std::string{"prism.hadamard.weight_names is empty"};
  const std::size_t n_layers = layout.size();
  std::vector<int> roles(n_layers, 0);
  static const std::regex role(R"(^blk\.(\d+)\.ffn_(gate|up|down)_exps\.weight$)");
  for (const auto& item : names->items) {
    std::smatch m;
    if (!std::regex_match(item.text, m, role) || std::stoull(m[1].str()) >= n_layers) {
      return "prism.hadamard folds " + item.text +
             "; this engine applies the activation rotation to the routed experts (blk.N.ffn_{gate,up,down}_exps) only";
    }
    const int bit = m[2].str() == "gate" ? 1 : m[2].str() == "up" ? 2 : 4;
    auto& r = roles[std::stoull(m[1].str())];
    if ((r & bit) != 0) return "duplicate prism.hadamard weight " + item.text;
    r |= bit;
  }
  for (std::size_t l = 0; l < n_layers; ++l) {
    const int r = roles[l];
    if (((r & 1) != 0) != ((r & 2) != 0)) return "layer " + std::to_string(l) + ": prism.hadamard folds only one of gate/up (they share one input)";
    if (r != 0) {
      const std::int64_t a = (r & 3) != 0 ? layout[l].gu_type : -1;
      const std::int64_t b = (r & 4) != 0 ? layout[l].d_type : -1;
      if (a == 42 || b == 42) return "layer " + std::to_string(l) + ": a Hadamard-folded Q2_0 expert (the engine's Q2_0 rows cannot rotate)";
    }
  }
  std::vector<std::string> lines{"# strata hadamard v1: prism.hadamard (normalized-sylvester-walsh-hadamard, input-last-dimension, sign_mode " +
                                     mode + "), written by tools/iq_pack.py",
                                 "block " + std::to_string(block)};
  std::set<std::uint64_t> widths;
  for (const int r : roles) {
    if ((r & 3) != 0) widths.insert(n_embd);
    if ((r & 4) != 0) widths.insert(n_ff);
  }
  for (const auto w : widths) {
    if (w % static_cast<std::uint64_t>(block) != 0U) return std::string{"prism.hadamard block does not divide the expert widths"};
  }
  if (mode == "explicit") {
    std::vector<std::int64_t> sw;
    std::vector<std::int64_t> sv;
    if (const auto* v = g.get("prism.hadamard.sign_widths")) for (const auto& i : v->items) sw.push_back(integer(&i, 0));
    if (const auto* v = g.get("prism.hadamard.sign_values")) for (const auto& i : v->items) sv.push_back(integer(&i, 0));
    std::int64_t sum = 0;
    std::set<std::int64_t> unique(sw.begin(), sw.end());
    bool bad = sw.empty() || unique.size() != sw.size();
    for (const auto w : sw) {
      sum += w;
      bad = bad || w <= 0 || w % block != 0;
    }
    if (bad || sum != static_cast<std::int64_t>(sv.size())) return std::string{"prism.hadamard sign_widths / sign_values are inconsistent"};
    for (const auto v : sv) {
      if (v != 1 && v != -1) return std::string{"prism.hadamard sign values must be +1/-1"};
    }
    std::map<std::int64_t, std::vector<std::int64_t>> table;
    std::size_t at = 0;
    for (const auto w : sw) {
      table[w] = std::vector<std::int64_t>(sv.begin() + static_cast<std::ptrdiff_t>(at), sv.begin() + static_cast<std::ptrdiff_t>(at + static_cast<std::size_t>(w)));
      at += static_cast<std::size_t>(w);
    }
    for (const auto w : widths) {
      if (table.count(static_cast<std::int64_t>(w)) == 0U) return std::string{"prism.hadamard has no sign vector for a width"};
    }
    for (const auto& [w, values] : table) {
      std::string line = "signs " + std::to_string(w);
      for (const auto v : values) line += " " + std::to_string(v);
      lines.push_back(line);
    }
  }
  for (std::size_t l = 0; l < n_layers; ++l) {
    const int r = roles[l];
    if (r != 0) {
      lines.push_back("layer " + std::to_string(l) + " " + std::to_string(r & 1) + " " + std::to_string((r >> 1) & 1) + " " +
                      std::to_string((r >> 2) & 1));
    }
  }
  std::string out;
  for (std::size_t i = 0; i < lines.size(); ++i) out += (i ? "\n" : "") + lines[i];
  return out + "\n";
}

Json experts_source(const Model& model, const std::string& text, const std::uint64_t total) {
  Json shards = Json::list();
  for (std::size_t i = 0; i < model.paths.size(); ++i) {
    shards.push(Json::object()
                    .set("name", Json::text(utf8(model.paths[i].filename())))
                    .set("size", Json::integer(static_cast<std::int64_t>(model.sizes[i]))));
  }
  Sha256 sha;
  sha.update(text.data(), text.size());
  return Json::object()
      .set("schema", Json::integer(1))
      .set("shards", std::move(shards))
      .set("native_experts_sha256", Json::text(sha.hex()))
      .set("bytes", Json::integer(static_cast<std::int64_t>(total)));
}

// ---- conversions --------------------------------------------------------------------------------------------------

struct Converted {
  std::string kind;
  std::vector<unsigned char> data;
  std::uint64_t dst_bytes = 0;
  std::optional<Json> record;   // src_type, dst_type, then method / exact / max_abs_err in iq_pack.py's order
};

double max_abs_err(const std::vector<float>& got, const std::vector<float>& values) {
  double worst = 0.0;
  for (std::size_t i = 0; i < values.size(); ++i) {
    worst = std::max(worst, std::fabs(static_cast<double>(got[i]) - static_cast<double>(values[i])));
  }
  return worst;
}

bool array_equal(const std::vector<float>& a, const std::vector<float>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (!(a[i] == b[i])) return false;
  }
  return true;
}

std::variant<Converted, std::string> convert(const std::string& name, const TensorInfo& t, const std::vector<unsigned char>& raw,
                                             const bool compat_bf16) {
  const auto form = form_of(name);
  const std::string& type_name = t.type_name;
  if (!form || *form == type_name) return Converted{kKind.at(type_name), raw, raw.size(), std::nullopt};
  const auto dequantized = dequantize(raw, t);
  if (!dequantized) return name + " is " + type_name + ", which this tool cannot dequantize";
  const auto& values = *dequantized;
  Json rec = Json::object().set("src_type", Json::text(type_name)).set("dst_type", Json::text(*form));
  if (*form == "BF16") {
    if (type_name == "F32") {
      bool low_zero = true;
      std::vector<unsigned char> data(values.size() * 2U);
      for (std::size_t i = 0; i < values.size(); ++i) {
        std::uint32_t u = 0;
        std::memcpy(&u, &values[i], 4);
        low_zero = low_zero && (u & 0xFFFFU) == 0U;
        const auto hi = static_cast<std::uint16_t>(u >> 16U);
        std::memcpy(data.data() + 2U * i, &hi, 2);
      }
      if (low_zero) {
        if (!all_finite(values)) return name + " is F32 with non-finite values; refused rather than converted";
        rec.set("method", Json::text("f32->bf16: the values are BF16's (low 16 bits zero)"))
            .set("exact", Json::boolean(true))
            .set("max_abs_err", Json::real(0.0));
        const auto size = data.size();
        return Converted{"4", std::move(data), size, std::move(rec)};
      }
    }
    if (!compat_bf16) {
      const std::string what = type_name == "F32" ? "F32 with values that are not BF16" : type_name;
      return name + " is " + what + ", but the engine requires BF16; use --compat-bf16 (rounds to nearest-even)";
    }
    if (!all_finite(values)) return std::string{"cannot convert non-finite weights to BF16"};
    std::vector<unsigned char> data(values.size() * 2U);
    std::vector<float> got(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
      const auto b = bf16_bits(values[i]);
      std::memcpy(data.data() + 2U * i, &b, 2);
      got[i] = f32_from_bf16(b);
    }
    rec.set("method", Json::text(kFloat.count(type_name) == 0U ? std::string{"dequantize->bf16 round-to-nearest-even (--compat-bf16)"}
                                                               : lower(type_name) + "->bf16 round-to-nearest-even (--compat-bf16)"));
    rec.set("exact", Json::boolean(array_equal(got, values)));
    rec.set("max_abs_err", Json::real(values.empty() ? 0.0 : max_abs_err(got, values)));
    const auto size = data.size();
    return Converted{"4", std::move(data), size, std::move(rec)};
  }
  if (*form == "F16") {
    if (type_name != "F32" && !compat_bf16) {
      return name + " is " + type_name + ", but the engine reads F16; use --compat-bf16 (rounds to nearest-even)";
    }
    if (!all_finite(values)) return name + ": cannot convert non-finite weights to F16";
    std::vector<std::uint16_t> h(values.size());
    std::vector<float> back(values.size());
    bool overflow = false;
    for (std::size_t i = 0; i < values.size(); ++i) {
      h[i] = f16_bits(values[i], overflow);
      back[i] = f32_from_f16(h[i]);
    }
    if (overflow || !all_finite(back)) return name + ": a value is beyond F16's range, which the engine reads it as";
    rec.set("exact", Json::boolean(array_equal(back, values)));
    rec.set("max_abs_err", Json::real(values.empty() ? 0.0 : max_abs_err(back, values)));
    if (type_name == "F32") {
      rec.set("method", Json::text("f32->f16 by the loader (index kind 3, round-to-nearest-even)"));
      return Converted{"3", raw, raw.size() / 2U, std::move(rec)};
    }
    rec.set("method", Json::text(lower(type_name) + "->f16 round-to-nearest-even (--compat-bf16)"));
    std::vector<unsigned char> data(h.size() * 2U);
    std::memcpy(data.data(), h.data(), data.size());
    const auto size = data.size();
    return Converted{"5", std::move(data), size, std::move(rec)};
  }
  // form == "F32"
  if (type_name != "F16" && type_name != "BF16" && !compat_bf16) {
    return name + " is " + type_name + ", but the engine reads F32; use --compat-bf16";
  }
  std::vector<unsigned char> data(values.size() * 4U);
  std::memcpy(data.data(), values.data(), data.size());
  rec.set("method", Json::text(lower(type_name) + "->f32 widening")).set("exact", Json::boolean(true)).set("max_abs_err", Json::real(0.0));
  const auto size = data.size();
  return Converted{"2", std::move(data), size, std::move(rec)};
}

// ---- the index and dense.bin --------------------------------------------------------------------------------------

using Row = std::vector<std::string>;

void write_index(const fs::path& out, std::vector<Row>& rows, const std::string& src_name, const int served, const int n_extra,
                 std::ostream& log) {
  std::uint64_t at = 0;
  for (auto& r : rows) {
    r[5] = std::to_string(at);
    at += (std::stoull(r[6]) + kAlign - 1U) / kAlign * kAlign;
  }
  std::string text = "# strata pack index v3 -- generated by tools/iq_pack.py (native experts) from " + src_name + "\n";
  text += "# align " + std::to_string(kAlign) + " pool " + std::to_string(at) + " tensors " + std::to_string(rows.size()) + "\n";
  for (const auto& r : rows) {
    for (std::size_t i = 0; i < r.size(); ++i) text += (i ? " " : "") + r[i];
    text += "\n";
  }
  write_lf(out / "index.txt.tmp", text);
  log << "index.txt: " << rows.size() << " tensors, " << served << " served natively, " << n_extra << " in extra.bin, arena "
      << fmt("%.2f", static_cast<double>(at) / 1073741824.0) << " GiB\n";
}

int index_standalone(const fs::path& src, const fs::path& out, const Model& model, const bool compat_bf16, std::ostream& log) {
  struct Todo {
    std::string name;
    const Where* w;
    bool native;
  };
  std::vector<Todo> todo;
  std::vector<std::string> problems;
  for (const auto& [name, w] : model.order) {
    if (is_expert(w.t->name) || kNotInPack.count(w.t->name) != 0U) continue;
    if (w.t->shape.size() > 2U) {
      log << "tensor " << w.t->name << " has " << w.t->shape.size() << " dimensions; the index holds two\n";
      return 1;
    }
    // quantized: served from the GGUF unless the engine reads it from the pack (FORM), which takes --compat-bf16 to
    // dequantize - except the native PLE key encodings
    const auto form = form_of(name);
    const bool quantized = kFloat.count(w.t->type_name) == 0U;
    const bool native = quantized && (!form || (name == "blk.1.ple_key.weight" && kNativePleKey.count(w.t->type_name) != 0U));
    if (quantized && !native && !compat_bf16) {
      problems.push_back(name + " is " + w.t->type_name + ", but the engine requires " + *form + "; use --compat-bf16");
    }
    todo.push_back(Todo{name, &w, native});
  }
  // refused before anything is written: the previous pack stays as it was
  if (!problems.empty()) {
    for (std::size_t i = 0; i < std::min<std::size_t>(8U, problems.size()); ++i) log << problems[i] << "\n";
    if (problems.size() > 8U) log << "... and " << (problems.size() - 8U) << " more\n";
    return 1;
  }
  std::vector<Row> rows;
  std::uint64_t at = 0;
  int served = 0;
  Json records = Json::list();
  struct Summary {
    bool exact;
    double max_abs_err;
    std::string name;
    std::string dst_type;
    std::string method;
  };
  std::vector<Summary> summary;
  Json converted = Json::list();
  std::uint64_t converted_bytes = 0;
  const fs::path dense_tmp = out / "dense.bin.tmp";
  {
    std::ofstream fo(dense_tmp, std::ios::binary | std::ios::trunc);
    if (!fo) {
      log << "dense.bin.tmp: cannot be written\n";
      return 1;
    }
    for (const auto& item : todo) {
      const auto& t = *item.w->t;
      const std::string ne0 = std::to_string(t.shape.empty() ? 0U : t.shape[0]);
      const std::string ne1 = std::to_string(t.shape.size() > 1U ? t.shape[1] : 0U);
      if (item.native) {
        ++served;
        rows.push_back(Row{t.name, "0", "0", "0", "0", "0", "0", ne0, ne1, "8", "0", "32", "0", "0", "0", "0", "0", "0", "0"});
        continue;
      }
      const auto raw = model.bytes(*item.w);
      auto got = convert(item.name, t, raw, compat_bf16);
      if (const auto* message = std::get_if<std::string>(&got)) {
        log << *message << "\n";
        fo.close();
        unlink_missing_ok(dense_tmp);
        return 1;
      }
      auto& c = std::get<Converted>(got);
      if (c.record) {
        Sha256 sha;
        sha.update(raw.data(), raw.size());
        // {"name", "shard", **rec, "source_sha256"}
        Json rec = Json::object();
        rec.set("name", Json::text(item.name)).set("shard", Json::text(utf8(model.paths[item.w->shard].filename())));
        rec.merge(*c.record);
        rec.set("source_sha256", Json::text(sha.hex()));
        const std::string method = c.record->member_text("method");
        summary.push_back(Summary{c.record->member_bool("exact"), c.record->member_real("max_abs_err"), item.name,
                                  c.record->member_text("dst_type"), method});
        records.push(std::move(rec));
        if (method.find("--compat-bf16") != std::string::npos) {   // the rounding the flag allowed
          converted.push(Json::object()
                             .set("name", Json::text(item.name))
                             .set("source_type", Json::text(t.type_name))
                             .set("bytes", Json::integer(static_cast<std::int64_t>(c.data.size()))));
          converted_bytes += c.data.size();
        }
      }
      rows.push_back(Row{t.name, "0", c.kind, std::to_string(at), std::to_string(c.data.size()), "0", std::to_string(c.dst_bytes), ne0, ne1,
                         "0", "0", "1", "0", "0", "0", "0", "0", "0", "0"});
      fo.write(reinterpret_cast<const char*>(c.data.data()), static_cast<std::streamsize>(c.data.size()));
      const std::uint64_t pad = (kAlign - c.data.size() % kAlign) % kAlign;
      const std::string zeros(static_cast<std::size_t>(pad), '\0');
      fo.write(zeros.data(), static_cast<std::streamsize>(pad));
      at += c.data.size() + pad;
    }
    if (!fo) {
      log << "dense.bin.tmp: cannot be written\n";
      return 1;
    }
  }
  write_index(out, rows, utf8(src.filename()), served, 0, log);
  Json shards = Json::list();
  for (std::size_t i = 0; i < model.paths.size(); ++i) {
    shards.push(Json::object()
                    .set("name", Json::text(utf8(model.paths[i].filename())))
                    .set("size", Json::integer(static_cast<std::int64_t>(model.sizes[i]))));
  }
  const Json conversions = Json::object()
                               .set("schema", Json::integer(1))
                               .set("tool", Json::text("tools/iq_pack.py"))
                               .set("compat_bf16", Json::boolean(compat_bf16))
                               .set("source_shards", std::move(shards))
                               .set("tensors", std::move(records));
  write_text(out / "conversions.json.tmp", conversions.dumps(1) + "\n");
  // published together, the completion marker (native_experts.txt, written last) first gone
  unlink_missing_ok(out / "native_experts.txt");
  replace_file(dense_tmp, out / "dense.bin");
  replace_file(out / "index.txt.tmp", out / "index.txt");
  replace_file(out / "conversions.json.tmp", out / "conversions.json");
  if (!summary.empty()) {
    std::size_t exact = 0;
    double worst = 0.0;
    for (const auto& s : summary) {
      exact += s.exact ? 1U : 0U;
      worst = std::max(worst, s.max_abs_err);
    }
    log << "conversions.json: " << summary.size() << " tensors converted to the engine's form (" << exact << " exact, "
        << (summary.size() - exact) << " rounded; max |err| " << fmt("%.3g", worst) << ")\n";
    for (const auto& s : summary) {
      if (s.dst_type == "F16") log << "narrowed to F16 by the loader: " << s.name << " (" << s.method << ", index kind 3)\n";
    }
  }
  if (compat_bf16) {
    const Json compat = Json::object()
                            .set("source", Json::text(utf8(src)))
                            .set("rounding", Json::text("nearest-even"))
                            .set("tensors", std::move(converted));
    write_text(out / "compat-bf16.json", compat.dumps(2) + "\n");
    log << "compat-bf16: " << converted_bytes << " bytes converted; expert and PLE table bytes unchanged\n";
  }
  return 0;
}

// gate | up | down [| the NVFP4 scale tail] of one expert, read from the shards
std::optional<std::vector<unsigned char>> expert_blob(const Model& model, const Layer& layer, const std::uint64_t n_expert, const std::uint64_t e,
                                                      std::ostream& log) {
  std::vector<unsigned char> blob;
  blob.reserve(static_cast<std::size_t>(layer.blob));
  bool nvfp4 = false;
  for (const auto* w : layer.ws) {
    const std::uint64_t per = *w->t->expected_bytes() / n_expert;
    const auto part = model.read_range(w->shard, w->g->data_start + w->t->offset + e * per, per);
    blob.insert(blob.end(), part.begin(), part.end());
    nvfp4 = nvfp4 || w->t->type_name == "NVFP4";
  }
  if (nvfp4) {
    std::array<float, 4> tail{1.0F, 1.0F, 1.0F, 0.0F};
    for (std::size_t r = 0; r < 3; ++r) {
      if (layer.ws[r]->t->type_name != "NVFP4") continue;
      const Where* scale = model.find("blk." + std::to_string(layer.l) + ".ffn_" + kRoles[r] + "_exps.scale");
      const auto value = model.read_range(scale->shard, scale->g->data_start + scale->t->offset + e * 4U, 4U);
      std::memcpy(&tail[r], value.data(), 4);
    }
    for (std::size_t r = 0; r < 3; ++r) {
      if (!std::isfinite(tail[r]) || tail[r] <= 0.0F) {
        log << "layer " << layer.l << ": a non-finite or non-positive NVFP4 scale\n";
        return std::nullopt;
      }
    }
    const auto* bytes = reinterpret_cast<const unsigned char*>(tail.data());
    blob.insert(blob.end(), bytes, bytes + kNvfp4Tail);
  }
  return blob;
}

}  // namespace

int iq_pack(const PackOptions& options, std::ostream& log) {
  try {
    // HF snapshot files are symlinks to hash-named blobs: absolute(), not canonical(), keeps the shard's own name
    const fs::path src = fs::absolute(options.gguf).make_preferred();
    const fs::path out = options.out;
    std::error_code ec;
    fs::create_directories(out, ec);
    const Model model(src, ModelFlavor::upstream);
    if (model.paths.size() > 1U) {
      std::string names;
      for (std::size_t i = 0; i < model.paths.size(); ++i) names += (i ? ", " : "") + utf8(model.paths[i].filename());
      log << "model shards: " << names << "\n";
    }
    // ---- the expert table first: a model that cannot be packed is refused before any file of the pack changes
    auto layout_or = expert_layout(model, log);
    if (const auto* message = std::get_if<std::string>(&layout_or)) {
      log << *message << "\n";
      return 1;
    }
    auto& experts = std::get<Experts>(layout_or);
    bool experts_bin = options.experts_bin;
    for (const auto& layer : experts.layout) {
      for (const auto* w : layer.ws) experts_bin = experts_bin || w->t->type_name == "NVFP4";
    }
    // ---- prism.hadamard: hadamard.txt beside native_experts.txt, which becomes v5
    const auto& first = experts.layout.front();
    const auto had = hadamard_spec(*model.files[0], experts.layout, first.ws[0]->t->shape[0], first.ws[2]->t->shape[0]);
    if (had && had->rfind("# strata hadamard", 0) != 0U) {
      log << "prism.hadamard: " << *had << "\n";
      return 1;
    }
    if (had) {
      experts.text = native_experts_v5(experts.text);
      if (experts.text.rfind("# strata native experts v5:", 0) != 0U) {
        log << "prism.hadamard: " << experts.text << "\n";
        return 1;
      }
      std::size_t folded = 0;
      std::istringstream lines(*had);
      for (std::string line; std::getline(lines, line);) folded += line.rfind("layer ", 0) == 0U ? 1U : 0U;
      log << "prism.hadamard: " << folded << " layer(s) of Hadamard-folded experts: hadamard.txt, native_experts.txt v5\n";
    }
    const fs::path path = out / "experts.bin";
    const fs::path sidecar = out / "experts.bin.src.json";
    const Json want = experts_source(model, experts.text, experts.total);
    std::string want_text = want.dumps(2) + "\n";
    const std::string want_file = text_bytes(want_text);
    bool reuse = fs::exists(path) && fs::file_size(path, ec) == experts.total && read_file_text(sidecar) == want_file;
    if (reuse) {
      // the first and last blobs of the first, middle and last layers compared with the GGUF too
      std::ifstream f(path, std::ios::binary);
      const std::array<const Layer*, 3> check{&experts.layout.front(), &experts.layout[experts.layout.size() / 2], &experts.layout.back()};
      for (const auto* layer : check) {
        for (const std::uint64_t e : {std::uint64_t{0}, experts.n_expert - 1U}) {
          f.seekg(static_cast<std::streamoff>(layer->offset + e * layer->blob));
          std::vector<unsigned char> have(static_cast<std::size_t>(layer->blob));
          f.read(reinterpret_cast<char*>(have.data()), static_cast<std::streamsize>(have.size()));
          const auto blob = expert_blob(model, *layer, experts.n_expert, e, log);
          if (!f || !blob || *blob != have) reuse = false;
        }
      }
      if (!reuse) log << utf8(path) << " matches " << sidecar.filename().string() << " but not this GGUF's expert blobs\n";
    }
    if (fs::exists(path) && !reuse && !experts_bin) {
      if (fs::exists(sidecar)) {
        log << utf8(path) << " was cut from another source than this model (" << sidecar.filename().string()
            << "): the engine would read it instead of the GGUF.  Delete it, or rerun with --experts-bin to rewrite it.\n";
        return 1;
      }
      log << "warning: " << utf8(path) << " has no " << sidecar.filename().string()
          << ", so nothing says it belongs to this model; the engine reads it instead of the GGUF (rerun with --experts-bin to rewrite it)\n";
    }
    if (const int rc = index_standalone(src, out, model, options.compat_bf16, log); rc != 0) return rc;
    // ---- the experts. native_experts.txt is the pack's completion marker: written to a temporary name, published last
    if (had) {
      write_lf(out / "hadamard.txt.tmp", *had);
      replace_file(out / "hadamard.txt.tmp", out / "hadamard.txt");
    } else {
      unlink_missing_ok(out / "hadamard.txt");
    }
    write_lf(out / "native_experts.txt.tmp", experts.text);
    replace_file(out / "native_experts.txt.tmp", out / "native_experts.txt");
    if (!experts_bin) {
      if (fs::exists(path)) log << "note: " << utf8(out) << "/experts.bin exists; the engine reads it instead of the GGUF\n";
      return 0;
    }
    if (reuse) {
      log << "experts.bin was cut from this model's shards (" << sidecar.filename().string() << "); not rewritten\n";
      return 0;
    }
    unlink_missing_ok(sidecar);
    const fs::path part = out / "experts.bin.tmp";
    if (fs::exists(part)) log << "the earlier packing was interrupted: starting it again\n";
    {
      std::ofstream fo(part, std::ios::binary | std::ios::trunc);
      for (const auto& layer : experts.layout) {
        for (std::uint64_t e = 0; e < experts.n_expert; ++e) {
          const auto blob = expert_blob(model, layer, experts.n_expert, e, log);
          if (!blob) return 1;
          fo.write(reinterpret_cast<const char*>(blob->data()), static_cast<std::streamsize>(blob->size()));
        }
        if (layer.l % 8 == 0) {
          log << "  layer " << layer.l << "  " << layer.ws[0]->t->type_name << "/" << layer.ws[2]->t->type_name << " blob " << layer.blob
              << "  at " << fmt("%.2f", static_cast<double>(layer.offset) / 1073741824.0) << " GiB\n";
        }
      }
      if (!fo) {
        log << "experts.bin.tmp: cannot be written\n";
        return 1;
      }
    }
    replace_file(part, path);
    unlink_missing_ok(out / "experts.bin.tmp.json");
    write_text(out / "experts.bin.src.json.tmp", want_text);
    replace_file(out / "experts.bin.src.json.tmp", sidecar);
    log << "experts.bin: " << experts.layout.size() << " layers, " << fmt("%.2f", static_cast<double>(experts.total) / 1073741824.0) << " GiB\n";
    return 0;
  } catch (const std::exception& e) {
    log << e.what() << "\n";
    return 1;
  }
}

}  // namespace strata_prepare
