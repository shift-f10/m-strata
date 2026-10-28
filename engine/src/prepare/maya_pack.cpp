// strata-prepare pack --maya: Project Maya's fork of tools/iq_pack.py (v1.1.30), function for function (the Python names
// are kept so the two read side by side), with our raon1 additions to it (engines/maya-ref/tools/iq_pack.py: the
// RAON1_NATIVE_OK set, no BF16 upgrade, the 3-D tensors, a quantized tensor outside the set dequantized to F32). The
// Python tool is the reference: every file this writes is compared with its output byte for byte
// (tools/secure/strata_prepare_parity.py --maya-tools, --raon-tools). Its Model and file writes are pack_common's, with
// Maya's flavour of the Model (no split check, no APR types).
#include "maya_pack.hpp"

#include "gguf_header.hpp"
#include "pack_common.hpp"
#include "py_json.hpp"
#include "tokenizer_export.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace strata_prepare {
namespace fs = std::filesystem;
namespace {

// ---- iq_pack.py's constants (Maya's fork) -------------------------------------------------------------------------

const std::set<std::string> kFloat{"BF16", "F32", "F16"};
constexpr std::array<std::string_view, 2> kRouters{"ffn_gate_inp.weight", "ffn_gate_inp_shexp.weight"};
constexpr std::string_view kNotInPack = "per_layer_token_embd.weight";
// read as BF16 by qwen4exp's residual, router, GDN, QSA and PLE kernels
constexpr std::array<std::string_view, 13> kBf16Projections{
    "hc_attn_down.weight", "hc_attn_up.weight", "hc_attn_inject.weight", "hc_ffn_down.weight", "hc_ffn_up.weight",
    "hc_ffn_inject.weight", "ssm_alpha.weight", "ssm_beta.weight", "indexer.q_proj.weight", "indexer.k_proj.weight",
    "ple_value.weight", "ffn_gate_inp.weight", "ffn_gate_inp_shexp.weight"};   // ..., *ROUTERS
const std::set<std::string> kBf16Output{"output_hc_down.weight", "output_hc_up.weight"};
// GLM5NEXT_BF16: the mHC mixes, the indexer, the router and its selection bias, the KDA gates, norms and conv weights,
// the MLA norms and the 3-D absorbed projections
constexpr std::array<std::string_view, 30> kGlm5NextBf16{
    "hc_attn_fn.weight", "hc_attn_base.weight", "hc_attn_scale.weight", "hc_ffn_fn.weight", "hc_ffn_base.weight",
    "hc_ffn_scale.weight", "indexer.attn_q_b.weight", "indexer.attn_k.weight", "indexer_compressor_gate.weight",
    "indexer_compressor_ape.weight", "indexer.proj.weight", "indexer.k_norm.weight", "indexer.k_norm.bias",
    "ffn_gate_inp.weight", "exp_probs_b.bias", "ssm_a", "ssm_dt.bias", "ssm_norm.weight", "ssm_beta.weight",
    "ssm_f_a.weight", "ssm_f_b.weight", "ssm_g_a.weight", "ssm_g_b.weight", "ssm_conv1d_q.weight",
    "ssm_conv1d_k.weight", "ssm_conv1d_v.weight", "attn_q_a_norm.weight", "attn_kv_a_norm.weight", "attn_k_b.weight",
    "attn_v_b.weight"};
// GLM5NEXT_NATIVE_OK: the only non-expert tensors a glm5-next pack may serve quantized from the GGUF
constexpr std::array<std::string_view, 15> kGlm5NextNativeOk{
    "attn_q_a.weight", "attn_q_b.weight", "attn_kv_a_mqa.weight", "attn_output.weight", "attn_q.weight", "attn_k.weight",
    "attn_v.weight", "ffn_gate.weight", "ffn_up.weight", "ffn_down.weight", "ffn_gate_shexp.weight", "ffn_up_shexp.weight",
    "ffn_down_shexp.weight", "token_embd.weight", "output.weight"};
// RAON1_NATIVE_OK: the Q8_0 non-expert tensors a raon1 (Raon-1-mini) pack serves from the GGUF; its precision-sensitive
// projections are F16 / F32 there already, and it has a full-rank RDA gate (ssm_g), an MLA output gate, no indexer
constexpr std::array<std::string_view, 16> kRaon1NativeOk{
    "attn_q.weight", "attn_k.weight", "attn_v.weight", "attn_output.weight", "attn_q_a.weight", "attn_q_b.weight",
    "attn_gate.weight", "ssm_g.weight", "ffn_gate.weight", "ffn_up.weight", "ffn_down.weight", "ffn_gate_shexp.weight",
    "ffn_up_shexp.weight", "ffn_down_shexp.weight", "token_embd.weight", "output.weight"};
constexpr std::array<const char*, 3> kRoles{"gate", "up", "down"};
constexpr std::uint64_t kAlign = 64;

bool glm(const std::string& arch) { return arch == "glm5-next" || arch == "glm5next"; }
bool raon1(const std::string& arch) { return arch == "raon1"; }
// the families the Glm5Model runner serves: a key prefix of their own, a trunk, 3-D tensors indexed as 2-D
bool glm_runner(const std::string& arch) { return glm(arch) || raon1(arch); }

template <std::size_t N>
bool ends_with_any(const std::string& name, const std::array<std::string_view, N>& suffixes) {
  for (const auto s : suffixes) {
    if (ends_with(name, s)) return true;
  }
  return false;
}

bool needs_bf16(const std::string& name, const std::string& type_name, const std::string& arch) {
  // raon1's small projections are F16 / F32 in the GGUF and go to dense.bin as they are (the loader reads them as F32)
  if (raon1(arch)) return false;
  if (glm(arch)) return starts_with(name, "blk.") && ends_with_any(name, kGlm5NextBf16);
  if (kBf16Output.count(name) != 0U) return true;
  if (!starts_with(name, "blk.")) return false;
  return ends_with_any(name, kBf16Projections) || (name == "blk.1.ple_key.weight" && type_name != "Q2_0");
}

// index_shape: a 3-D tensor packs as its byte-identical 2-D view (ne0*ne1, ne2)
std::pair<std::uint64_t, std::uint64_t> index_shape(const std::vector<std::uint64_t>& shape) {
  if (shape.empty()) throw std::runtime_error("a tensor of no dimensions has no index shape");
  if (shape.size() >= 3U) return {shape[0] * shape[1], shape[2]};
  return {shape[0], shape.size() > 1U ? shape[1] : 0U};
}

bool is_expert(const std::string& name) { return starts_with(name, "blk.") && ends_with(name, "_exps.weight"); }

// name.split(".")[1] of a blk.N.* name
std::string layer_field(const std::string& name) {
  const auto dot = name.find('.', 4);
  return name.substr(4, dot == std::string::npos ? std::string::npos : dot - 4);
}

bool ascii_digits(const std::string& s) {
  if (s.empty()) return false;
  for (const char c : s) {
    if (std::isdigit(static_cast<unsigned char>(c)) == 0) return false;
  }
  return true;
}

// int(s) of an all-digit field; a number past int's range counts as huge (it is past every layer count)
std::int64_t layer_number(const std::string& digits) { return digits.size() > 9U ? INT64_MAX : std::stoll(digits); }

// f32_bytes: gguf-py's dequantize, as float32 (raon1's quantized tensors outside RAON1_NATIVE_OK)
std::vector<unsigned char> f32_bytes(const std::vector<unsigned char>& raw, const TensorInfo& t) {
  const auto values = dequantize(raw, t);
  if (!values) throw std::runtime_error(t.name + " is " + t.type_name + ", which this tool cannot dequantize");
  std::vector<unsigned char> data(values->size() * 4U);
  std::memcpy(data.data(), values->data(), data.size());
  return data;
}

// bf16_bytes: gguf-py's dequantize, then quantize to BF16 (ggml's round-to-nearest-even); non-finite values refused
std::vector<unsigned char> bf16_bytes(const std::vector<unsigned char>& raw, const TensorInfo& t) {
  const auto values = dequantize(raw, t);
  if (!values) throw std::runtime_error(t.name + " is " + t.type_name + ", which this tool cannot dequantize");
  if (!all_finite(*values)) throw std::runtime_error("cannot convert non-finite weights to BF16");
  std::vector<unsigned char> data(values->size() * 2U);
  for (std::size_t i = 0; i < values->size(); ++i) {
    const auto b = bf16_bits((*values)[i]);
    std::memcpy(data.data() + 2U * i, &b, 2);
  }
  return data;
}

using Row = std::vector<std::string>;

Row zeros_after(Row row) {
  for (int i = 0; i < 7; ++i) row.emplace_back("0");
  return row;
}

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
  write_lf(out / "index.txt", text);
  log << "index.txt: " << rows.size() << " tensors, " << served << " served natively, " << n_extra << " in extra.bin, arena "
      << fmt("%.2f", static_cast<double>(at) / 1073741824.0) << " GiB\n";
}

int index_standalone(const fs::path& src, const fs::path& out, const Model& model, const bool compat_bf16, const std::string& arch,
                     const std::optional<std::int64_t> block_count, std::ostream& log) {
  int dropped = 0;
  if (!compat_bf16) {
    for (const auto& [name, w] : model.order) {
      const auto& type = w.t->type_name;
      if (needs_bf16(name, type, arch) && type != "BF16" && !(type == "F32" && ends_with_any(name, kRouters) && arch == "qwen4exp")) {
        log << name << " is " << type << ", but the engine requires BF16; use --compat-bf16\n";
        return 1;
      }
    }
  }
  std::vector<Row> rows;
  std::uint64_t at = 0;
  int served = 0;
  Json converted = Json::list();
  std::size_t n_converted = 0;
  std::uint64_t converted_bytes = 0;
  {
    std::ofstream fo(out / "dense.bin", std::ios::binary | std::ios::trunc);
    if (!fo) throw std::runtime_error("dense.bin: cannot be written");
    for (const auto& [name, w] : model.order) {
      const auto& t = *w.t;
      if (is_expert(t.name) || t.name == kNotInPack) continue;
      if (block_count && starts_with(name, "blk.")) {
        const std::string il = layer_field(name);
        if (ascii_digits(il) && layer_number(il) >= *block_count) {
          ++dropped;
          continue;
        }
      }
      // a trailing ne3 of 1 adds no bytes ([4, 1, 8192, 1] packs as [4, 1, 8192])
      const std::size_t dims = t.shape.size() - (t.shape.size() == 4U && t.shape[3] == 1U ? 1U : 0U);
      if (dims > 2U && !(glm_runner(arch) && dims == 3U)) {
        log << "tensor " << t.name << " has " << t.shape.size() << " dimensions; the index holds two\n";
        return 1;
      }
      const auto [ne0, ne1] = index_shape(t.shape);
      const bool convert = compat_bf16 && needs_bf16(t.name, t.type_name, arch) && t.type_name != "BF16";
      if (kFloat.count(t.type_name) != 0U || convert) {
        auto raw = model.bytes(w);
        std::string kind;
        if (convert) {
          raw = bf16_bytes(raw, t);
          kind = "4";
          converted.push(Json::object()
                             .set("name", Json::text(t.name))
                             .set("source_type", Json::text(t.type_name))
                             .set("bytes", Json::integer(static_cast<std::int64_t>(raw.size()))));
          ++n_converted;
          converted_bytes += raw.size();
        } else {
          kind = t.type_name == "BF16" ? "4" : t.type_name == "F16" ? "5" : "2";
        }
        if (!convert && arch == "qwen4exp" && t.type_name == "F32" && ends_with_any(t.name, kRouters)) {
          std::vector<unsigned char> exact(raw.size() / 2U);
          for (std::size_t i = 0; i < raw.size() / 4U; ++i) {
            std::uint32_t u = 0;
            std::memcpy(&u, raw.data() + 4U * i, 4);
            if ((u & 0xFFFFU) != 0U) {
              log << "router " << t.name << " is F32 with values that are not BF16; the engine's router is BF16\n";
              return 1;
            }
            const auto hi = static_cast<std::uint16_t>(u >> 16U);
            std::memcpy(exact.data() + 2U * i, &hi, 2);
          }
          raw = std::move(exact);   // the exact BF16 values
          kind = "4";
        }
        const std::string size = std::to_string(raw.size());
        rows.push_back(zeros_after(Row{t.name, "0", kind, std::to_string(at), size, "0", size, std::to_string(ne0), std::to_string(ne1), "0",
                                       "0", "1"}));
        fo.write(reinterpret_cast<const char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
        const std::uint64_t pad = (kAlign - raw.size() % kAlign) % kAlign;
        const std::string padding(static_cast<std::size_t>(pad), '\0');
        fo.write(padding.data(), static_cast<std::streamsize>(pad));
        at += raw.size() + pad;
      } else if (raon1(arch) && !ends_with_any(name, kRaon1NativeOk)) {
        // the IQ4_XS build quantizes projections the Q8_0 build keeps F32 (attn_k_b, attn_v_b): served from dense.bin as F32
        const auto raw = f32_bytes(model.bytes(w), t);
        const std::string size = std::to_string(raw.size());
        rows.push_back(zeros_after(Row{t.name, "0", "2", std::to_string(at), size, "0", size, std::to_string(ne0), std::to_string(ne1), "0",
                                       "0", "1"}));
        fo.write(reinterpret_cast<const char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
        const std::uint64_t pad = (kAlign - raw.size() % kAlign) % kAlign;
        const std::string padding(static_cast<std::size_t>(pad), '\0');
        fo.write(padding.data(), static_cast<std::streamsize>(pad));
        at += raw.size() + pad;
        converted.push(Json::object()
                           .set("name", Json::text(t.name))
                           .set("source_type", Json::text(t.type_name))
                           .set("bytes", Json::integer(static_cast<std::int64_t>(raw.size()))));
        ++n_converted;
        converted_bytes += raw.size();
      } else if (glm(arch) && !ends_with_any(name, kGlm5NextNativeOk)) {
        log << t.name << " is " << t.type_name
            << ": not on the BF16 upgrade list nor the glm5-next native set (GLM5NEXT_NATIVE_OK); refusing to pack it silently\n";
        return 1;
      } else {
        ++served;
        rows.push_back(zeros_after(Row{t.name, "0", "0", "0", "0", "0", "0", std::to_string(ne0), std::to_string(ne1), "8", "0", "32"}));
      }
    }
    if (!fo) throw std::runtime_error("dense.bin: cannot be written");
  }
  if (dropped != 0) {
    log << "NextN/MTP: " << dropped << " tensors of blk." << *block_count
        << "+ left out of the pack - the engine reads the MTP draft block from the model's files itself\n";
  }
  write_index(out, rows, utf8(src.filename()), served, 0, log);
  if (compat_bf16) {
    const Json compat = Json::object()
                            .set("source", Json::text(utf8(src)))
                            .set("rounding", Json::text("nearest-even"))
                            .set("tensors", std::move(converted));
    write_text(out / "compat-bf16.json", compat.dumps(2) + "\n");
    log << "compat-bf16: " << n_converted << " tensors, " << fmt("%.2f", static_cast<double>(converted_bytes) / 1073741824.0)
        << " GiB; expert and PLE table bytes unchanged\n";
  }
  return 0;
}

struct Layer {
  std::int64_t l;
  std::uint32_t gu_type;
  std::uint32_t d_type;
  std::uint64_t offset;
  std::uint64_t blob;
  std::array<const Where*, 3> ws;
};

}  // namespace

int maya_iq_pack(const PackOptions& options, std::ostream& log) {
  try {
    // HF snapshot files are symlinks to hash-named blobs: absolute(), not canonical(), keeps the shard's own name
    const fs::path src = fs::absolute(options.gguf).make_preferred();
    const fs::path out = options.out;
    fs::create_directories(out);
    const GgufHeader g(src);
    const MetaValue* arch_value = g.get("general.architecture");
    // str(metadata.get("general.architecture", ""))
    const std::string arch = arch_value == nullptr ? std::string{}
                             : arch_value->kind == MetaValue::Kind::text ? arch_value->text
                             : arch_value->kind == MetaValue::Kind::integer ? std::to_string(arch_value->integer)
                                                                          : std::string{"?"};
    if (arch != "qwen4exp" && !glm_runner(arch)) {
      log << "general.architecture is " << python_repr(arch) << "; the packer knows qwen4exp and glm5-next and glm5next and raon1\n";
      return 1;
    }
    // two spellings in the wild (llama.cpp's "glm5-next", unsloth's "glm5next"): the key prefix follows the file's
    const std::string prefix = raon1(arch) ? "raon1." : arch == "glm5-next" ? "glm5-next." : "glm5next.";
    if (glm(arch)) {
      log << arch << ": packing for the Glm5Model runner (quantized dense served natively; the driver gate remains closed until "
                     "the pack wiring lands - docs/GLM5-FLASH.md \xC2\xA7" "9)\n";
    } else if (raon1(arch)) {
      log << "raon1: packing for the Glm5Model runner (raon1 variant; quantized dense served natively)\n";
    }
    std::optional<std::int64_t> trunk;
    if (glm_runner(arch)) {
      const MetaValue* bc = g.get(prefix + "block_count");
      if (bc == nullptr) {
        log << arch << ": " << prefix << "block_count is missing; cannot tell the trunk from the NextN block\n";
        return 1;
      }
      const auto block_count = python_int(*bc);
      if (!block_count) throw std::runtime_error(prefix + "block_count is not an integer");
      // the NextN/MTP layer counts INTO block_count in the wild: the servable trunk is what is left
      std::int64_t nextn = 0;
      if (const MetaValue* n = g.get(prefix + "nextn_predict_layers"); n != nullptr && python_truthy(*n)) {
        const auto v = python_int(*n);
        if (!v) throw std::runtime_error(prefix + "nextn_predict_layers is not an integer");
        nextn = *v;
      }
      trunk = *block_count - nextn;
      if (*trunk < 1) {
        log << arch << ": block_count " << *block_count << " - nextn " << nextn << " leaves no trunk\n";
        return 1;
      }
      log << arch << ": block_count " << *block_count << ", nextn " << nextn << " -> trunk layers 0.." << (*trunk - 1) << "\n";
    }
    const Model model(src, ModelFlavor::maya);
    if (model.paths.size() > 1U) {
      std::string names;
      for (std::size_t i = 0; i < model.paths.size(); ++i) names += (i ? ", " : "") + utf8(model.paths[i].filename());
      log << "model shards: " << names << "\n";
    }
    if (const int rc = index_standalone(src, out, model, options.compat_bf16, arch, trunk, log); rc != 0) return rc;
    if (!fs::exists(out / "tokenizer" / "vocab.json") || !fs::exists(out / "tokenizer" / "chat_template.jinja")) {
      // writes <out>/tokenizer/ (a subprocess with check=True there: its failure fails the pack)
      if (export_tokenizer(src, out, log) != 0) return 1;
    }

    // ---- the experts
    const auto layer_of = [](const std::string& name) {
      const std::string il = layer_field(name);
      if (!ascii_digits(il)) throw std::runtime_error("invalid literal for int() with base 10: " + python_repr(il));
      return layer_number(il);
    };
    std::int64_t n_layers = 0;
    if (trunk) {
      // the trunk only: the NextN/MTP layer ships its own experts, which the driver must never see as a layer
      n_layers = *trunk;
      bool any = false;
      int dropped_experts = 0;
      const std::string nextn_prefix = "blk." + std::to_string(*trunk) + ".";
      for (const auto& [name, w] : model.order) {
        if (ends_with(name, "_exps.weight") && starts_with(name, "blk.")) {
          const auto l = layer_of(name);
          any = any || (l >= 0 && l < *trunk);
        }
        if (starts_with(name, nextn_prefix) && ends_with(name, "_exps.weight")) ++dropped_experts;
      }
      if (!any) {
        log << arch << ": no routed experts in the trunk; the pack has nothing to serve\n";
        return 1;
      }
      if (dropped_experts != 0) {
        log << arch << ": " << dropped_experts << " NextN expert tensors present (blk." << *trunk << ".*); the trunk keeps layers 0.."
            << (*trunk - 1) << "\n";
      }
    } else {
      std::int64_t top = -1;
      for (const auto& [name, w] : model.order) {
        if (starts_with(name, "blk.") && ends_with(name, "_exps.weight")) top = std::max(top, layer_of(name));
      }
      if (top < 0) throw std::runtime_error("max() arg is an empty sequence");
      n_layers = 1 + top;
    }
    // the MoE layers only: the leading dense blocks ship no router and no experts
    std::int64_t lead = 0;
    if (const MetaValue* v = g.get(glm_runner(arch) ? prefix + "leading_dense_block_count" : std::string{"qwen4exp.leading_dense_block_count"});
        v != nullptr && python_truthy(*v)) {
      const auto n = python_int(*v);
      if (!n) throw std::runtime_error("leading_dense_block_count is not an integer");
      lead = *n;
    }
    std::vector<std::int64_t> moe_layers;
    for (std::int64_t l = 0; l < n_layers; ++l) {
      if (l >= lead) moe_layers.push_back(l);
    }
    if (moe_layers.empty()) throw std::runtime_error("list index out of range (no MoE layer)");
    const auto tensor = [&model](const std::string& name) -> const Where& {
      const Where* w = model.find(name);
      if (w == nullptr) throw std::runtime_error("KeyError: " + python_repr(name));
      return *w;
    };
    const auto router_rows = [&tensor](const std::int64_t l) {
      const auto& shape = tensor("blk." + std::to_string(l) + ".ffn_gate_inp.weight").t->shape;
      if (shape.size() < 2U) throw std::runtime_error("blk." + std::to_string(l) + ".ffn_gate_inp.weight: tuple index out of range");
      return shape[1];
    };
    const std::uint64_t n_expert = router_rows(moe_layers.front());   // router rows = experts kept
    for (const auto l : moe_layers) {
      if (router_rows(l) != n_expert) {
        log << "the routers disagree on the expert count; a per-layer pruned model cannot be packed\n";
        return 1;
      }
    }
    if (n_expert == 0U) throw std::runtime_error("integer division or modulo by zero");
    std::vector<Layer> layout;
    std::uint64_t offset = 0;
    for (const auto l : moe_layers) {
      std::array<const Where*, 3> ws{};
      for (std::size_t r = 0; r < 3; ++r) ws[r] = &tensor("blk." + std::to_string(l) + ".ffn_" + kRoles[r] + "_exps.weight");
      std::array<std::uint64_t, 3> per{};
      for (std::size_t r = 0; r < 3; ++r) per[r] = *ws[r]->t->expected_bytes() / n_expert;
      if (per[0] != per[1] || ws[0]->t->type_name != ws[1]->t->type_name) {
        log << "layer " << l << ": gate and up differ in type\n";
        return 1;
      }
      const std::uint64_t blob = per[0] + per[1] + per[2];
      layout.push_back(Layer{l, ws[0]->t->type_id, ws[2]->t->type_id, offset, blob, ws});
      offset += blob * n_expert;
    }
    bool straddle = false;
    for (const auto& layer : layout) {
      straddle = straddle || layer.ws[0]->shard != layer.ws[1]->shard || layer.ws[1]->shard != layer.ws[2]->shard;
    }
    std::string text = "# strata native experts " + std::string{straddle ? "v4" : "v3"} +
                       ": layer gu_type d_type offset blob_bytes gate_off up_off down_off [shard | gate_shard up_shard down_shard] (n_expert " +
                       std::to_string(n_expert) + ", total " + std::to_string(offset) + "; absolute offsets in " + utf8(src.filename()) +
                       ", or in the named shard(s) beside it)\n";
    for (const auto& layer : layout) {
      // each role's offset is absolute in its own shard: that file's data_start + the tensor's data-section offset
      std::string line = std::to_string(layer.l) + " " + std::to_string(layer.gu_type) + " " + std::to_string(layer.d_type) + " " +
                         std::to_string(layer.offset) + " " + std::to_string(layer.blob);
      for (const auto* w : layer.ws) line += " " + std::to_string(w->g->data_start + w->t->offset);
      const bool one = layer.ws[0]->shard == layer.ws[1]->shard && layer.ws[1]->shard == layer.ws[2]->shard;
      if (one) {
        // v3 row: one shard for the whole blob (omitted when it is shard 1)
        if (layer.ws[0]->shard != 0U) line += " " + utf8(model.paths[layer.ws[0]->shard].filename());
      } else {
        // v4 row: every role names its own file, shard 1 by name too
        for (const auto* w : layer.ws) line += " " + utf8(model.paths[w->shard].filename());
      }
      text += line + "\n";
    }
    write_lf(out / "native_experts.txt", text);
    const fs::path path = out / "experts.bin";
    if (!options.experts_bin) {
      if (fs::exists(path)) log << "note: " << utf8(out) << "/experts.bin exists; the engine reads it instead of the GGUF\n";
      return 0;
    }
    std::error_code ec;
    if (fs::exists(path) && fs::file_size(path, ec) == offset) {
      log << "experts.bin exists with the right size; not rewritten\n";
      return 0;
    }
    std::ofstream fo(path, std::ios::binary | std::ios::trunc);
    for (const auto& layer : layout) {
      // per expert: gate | up | down
      std::array<std::vector<unsigned char>, 3> parts;
      for (std::size_t r = 0; r < 3; ++r) parts[r] = model.bytes(*layer.ws[r]);
      for (std::uint64_t e = 0; e < n_expert; ++e) {
        for (std::size_t r = 0; r < 3; ++r) {
          const std::uint64_t per = parts[r].size() / n_expert;
          fo.write(reinterpret_cast<const char*>(parts[r].data() + e * per), static_cast<std::streamsize>(per));
        }
      }
      if (layer.l % 8 == 0) {
        char line[160];
        std::snprintf(line, sizeof(line), "  layer %2lld  %-8s/%-7s blob %8llu  at %.2f GiB", static_cast<long long>(layer.l),
                      layer.ws[0]->t->type_name.c_str(), layer.ws[2]->t->type_name.c_str(), static_cast<unsigned long long>(layer.blob),
                      static_cast<double>(layer.offset) / 1073741824.0);
        log << line << "\n";
      }
    }
    if (!fo) throw std::runtime_error("experts.bin: cannot be written");
    log << "experts.bin: " << n_layers << " layers, " << fmt("%.2f", static_cast<double>(offset) / 1073741824.0) << " GiB\n";
    return 0;
  } catch (const std::exception& e) {
    log << e.what() << "\n";
    return 1;
  }
}

}  // namespace strata_prepare
