#include "pack_common.hpp"

#include "ggml.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace strata_prepare {
namespace fs = std::filesystem;

// ---- Model --------------------------------------------------------------------------------------------------------

Model::Model(const fs::path& first, const ModelFlavor flavor) {
  static const std::regex split(R"(-(\d{5})-of-(\d{5})\.gguf$)");
  const std::string name = utf8(first.filename());
  std::smatch m;
  paths.push_back(first);
  if (std::regex_search(name, m, split)) {
    const int total = std::stoi(m[2].str());
    paths.clear();
    const std::string prefix = name.substr(0, static_cast<std::size_t>(m.position(0)));
    for (int i = 1; i <= total; ++i) {
      char tail[32];
      std::snprintf(tail, sizeof(tail), "-%05d-of-%05d.gguf", i, total);
      paths.push_back(first.parent_path() / path_from_utf8(prefix + tail));
    }
  }
  std::vector<std::string> missing;
  for (const auto& p : paths) {
    if (!fs::is_regular_file(p)) missing.push_back(utf8(p));
  }
  if (!missing.empty()) {
    std::string list;
    for (std::size_t i = 0; i < missing.size(); ++i) list += (i ? ", " : "") + missing[i];
    throw std::runtime_error("missing model shards (wait for the download): " + list);
  }
  for (const auto& p : paths) {
    files.push_back(std::make_unique<GgufHeader>(p));
    sizes.push_back(files.back()->file_size);
  }
  if (flavor == ModelFlavor::upstream) check_split();
  for (std::size_t s = 0; s < files.size(); ++s) {
    const auto& g = *files[s];
    for (const auto& t : g.tensors) {
      // Maya's gguf_reader.py names no APR type: "type143" ... has no block geometry there
      const bool unknown = flavor == ModelFlavor::maya && t.type_id >= 143U && t.type_id <= 146U;
      const auto size = t.expected_bytes();
      if (unknown || !size || g.data_start + t.offset + *size > g.file_size) {
        throw std::runtime_error(utf8(paths[s].filename()) + ": unsupported or truncated tensor " + t.name);
      }
      if (index.count(t.name) != 0U) throw std::runtime_error(utf8(paths[s].filename()) + ": duplicate tensor " + t.name);
      index[t.name] = order.size();
      order.push_back({t.name, Where{&g, &t, s}});
    }
  }
}

const Where* Model::find(const std::string& name) const {
  const auto it = index.find(name);
  return it == index.end() ? nullptr : &order[it->second].second;
}

std::vector<unsigned char> Model::bytes(const Where& w) const {
  return read_range(w.shard, w.g->data_start + w.t->offset, *w.t->expected_bytes());
}

std::vector<unsigned char> Model::read_range(const std::size_t shard, const std::uint64_t at, const std::uint64_t size) const {
  std::ifstream in(paths[shard], std::ios::binary);
  in.seekg(static_cast<std::streamoff>(at));
  std::vector<unsigned char> data(static_cast<std::size_t>(size));
  if (size != 0U) in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
  if (!in) throw std::runtime_error(utf8(paths[shard].filename()) + ": a tensor could not be read");
  return data;
}

// The split keys, as the engine checks them (upstream's check_split): shard 1 carries the metadata, and every shard
// declares split.count / split.no (and split.tensors.count) consistently.
void Model::check_split() const {
  const auto n = static_cast<std::int64_t>(files.size());
  const auto& meta0 = *files[0];
  const auto as_int = [](const MetaValue* v) -> std::optional<std::int64_t> {
    if (v == nullptr || v->kind != MetaValue::Kind::integer) return std::nullopt;
    return v->integer;
  };
  if (n == 1) {
    const auto count = as_int(meta0.get("split.count")).value_or(1);
    if (count > 1) {
      throw std::runtime_error(utf8(paths[0].filename()) + " is shard 1 of " + std::to_string(count) +
                               ", but its name has no -00001-of-0000N.gguf to find the others by");
    }
    return;
  }
  if (meta0.get("general.architecture") == nullptr) {
    throw std::runtime_error(utf8(paths[0].filename()) +
                             " has no general.architecture; the first shard of a split model carries the metadata");
  }
  const auto total = as_int(meta0.get("split.tensors.count"));
  for (std::int64_t i = 0; i < n; ++i) {
    const auto& md = *files[static_cast<std::size_t>(i)];
    if (as_int(md.get("split.count")) != n || as_int(md.get("split.no")) != i ||
        (total && as_int(md.get("split.tensors.count")) != total)) {
      throw std::runtime_error(utf8(paths[static_cast<std::size_t>(i)].filename()) + " does not declare itself shard " +
                               std::to_string(i + 1) + " of " + std::to_string(n) +
                               " of this model (split.count / split.no / split.tensors.count)");
    }
  }
  if (total) {
    std::int64_t held = 0;
    for (const auto& g : files) held += static_cast<std::int64_t>(g->tensors.size());
    if (held != *total) {
      throw std::runtime_error("the " + std::to_string(n) + " shards hold " + std::to_string(held) +
                               " tensors, but split.tensors.count is " + std::to_string(*total));
    }
  }
}

// ---- Python's file writes -----------------------------------------------------------------------------------------

const char* line_end() {
#if defined(_WIN32)
  return "\r\n";
#else
  return "\n";
#endif
}

std::string text_bytes(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    if (c == '\n') out += line_end();
    else out.push_back(c);
  }
  return out;
}

void write_text(const fs::path& path, const std::string& text) { write_lf(path, text_bytes(text)); }

void write_lf(const fs::path& path, const std::string& text) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(text.data(), static_cast<std::streamsize>(text.size()));
  if (!f) throw std::runtime_error(utf8(path.filename()) + ": cannot be written");
}

void replace_file(const fs::path& from, const fs::path& to) {
  std::error_code ec;
  fs::rename(from, to, ec);
  if (ec) throw std::runtime_error(utf8(to.filename()) + ": cannot be replaced: " + ec.message());
}

void unlink_missing_ok(const fs::path& path) {
  std::error_code ec;
  fs::remove(path, ec);
}

std::string read_file_text(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

std::string utf8(const fs::path& path) {
  const auto u8 = path.u8string();
  return {u8.begin(), u8.end()};
}

fs::path path_from_utf8(const std::string& text) { return fs::path(std::u8string(text.begin(), text.end())); }

// ---- small things -------------------------------------------------------------------------------------------------

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string fmt(const char* format, const double value) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), format, value);
  return buffer;
}

bool starts_with(const std::string_view s, const std::string_view prefix) noexcept { return s.substr(0, prefix.size()) == prefix; }

bool ends_with(const std::string_view s, const std::string_view suffix) noexcept {
  return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

std::optional<std::int64_t> python_int(const MetaValue& value) {
  switch (value.kind) {
    case MetaValue::Kind::integer: return value.integer;
    case MetaValue::Kind::real:
      if (!std::isfinite(value.real) || std::fabs(value.real) >= 9.2e18) return std::nullopt;
      return static_cast<std::int64_t>(std::trunc(value.real));
    case MetaValue::Kind::text: {
      // int("  -12 "), int("1_000"): whitespace around, a sign, digits with single underscores between them
      std::string s = value.text;
      const auto space = [](const char c) { return c == ' ' || (c >= '\t' && c <= '\r'); };
      while (!s.empty() && space(s.back())) s.pop_back();
      std::size_t i = 0;
      while (i < s.size() && space(s[i])) ++i;
      bool negative = false;
      if (i < s.size() && (s[i] == '+' || s[i] == '-')) negative = s[i++] == '-';
      std::string digits;
      for (; i < s.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(s[i])) != 0) {
          digits.push_back(s[i]);
        } else if (s[i] == '_' && !digits.empty() && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])) != 0) {
          continue;
        } else {
          return std::nullopt;
        }
      }
      if (digits.empty() || digits.size() > 18U) return std::nullopt;
      const auto v = static_cast<std::int64_t>(std::stoll(digits));
      return negative ? -v : v;
    }
    case MetaValue::Kind::list: return std::nullopt;
  }
  return std::nullopt;
}

bool python_truthy(const MetaValue& value) noexcept {
  switch (value.kind) {
    case MetaValue::Kind::integer: return value.integer != 0;
    case MetaValue::Kind::real: return value.real != 0.0;
    case MetaValue::Kind::text: return !value.text.empty();
    case MetaValue::Kind::list: return !value.items.empty();
  }
  return false;
}

// ---- floats -------------------------------------------------------------------------------------------------------

float f32_from_f16(const std::uint16_t h) noexcept {
  const std::uint32_t sign = static_cast<std::uint32_t>(h & 0x8000U) << 16U;
  std::uint32_t exp = (h >> 10U) & 0x1fU;
  std::uint32_t mant = h & 0x3ffU;
  std::uint32_t bits = 0;
  if (exp == 0U) {
    if (mant == 0U) {
      bits = sign;
    } else {
      exp = 127U - 15U + 1U;
      while ((mant & 0x400U) == 0U) {
        mant <<= 1U;
        --exp;
      }
      mant &= 0x3ffU;
      bits = sign | (exp << 23U) | (mant << 13U);
    }
  } else if (exp == 0x1fU) {
    bits = sign | 0x7f800000U | (mant << 13U);
  } else {
    bits = sign | ((exp + 127U - 15U) << 23U) | (mant << 13U);
  }
  float out = 0.0F;
  std::memcpy(&out, &bits, 4);
  return out;
}

float f32_from_bf16(const std::uint16_t b) noexcept {
  const std::uint32_t bits = static_cast<std::uint32_t>(b) << 16U;
  float out = 0.0F;
  std::memcpy(&out, &bits, 4);
  return out;
}

bool all_finite(const std::vector<float>& values) noexcept {
  return std::all_of(values.begin(), values.end(), [](const float v) { return std::isfinite(v); });
}

std::optional<std::vector<float>> dequantize(const std::vector<unsigned char>& raw, const TensorInfo& t) {
  const auto n = static_cast<std::size_t>(t.elements());
  std::vector<float> values(n);
  if (t.type_name == "F32") {
    std::memcpy(values.data(), raw.data(), n * 4U);
    return values;
  }
  if (t.type_name == "F16" || t.type_name == "BF16") {
    for (std::size_t i = 0; i < n; ++i) {
      std::uint16_t h = 0;
      std::memcpy(&h, raw.data() + 2U * i, 2);
      values[i] = t.type_name == "F16" ? f32_from_f16(h) : f32_from_bf16(h);
    }
    return values;
  }
  if (t.type_id >= static_cast<std::uint32_t>(GGML_TYPE_COUNT)) return std::nullopt;
  const auto type = static_cast<ggml_type>(t.type_id);
  const char* ggml_name = ggml_type_name(type);
  std::string upper = ggml_name != nullptr ? ggml_name : "";
  for (auto& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  const auto* traits = ggml_get_type_traits(type);
  if (upper != t.type_name || traits == nullptr || traits->to_float == nullptr) return std::nullopt;
  traits->to_float(raw.data(), values.data(), static_cast<std::int64_t>(n));
  return values;
}

}  // namespace strata_prepare
