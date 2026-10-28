#include "g4_cli.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

namespace g4 {
namespace {

bool parse_int(std::string_view text, long long lo, long long hi, long long& out) {
  if (text.empty() || text.size() > 19) return false;
  if (text.front() == '+' || (text.size() > 1 && text.front() == '0')) return false;
  long long v = 0;
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
  if (ec != std::errc{} || end != text.data() + text.size() || v < lo || v > hi) return false;
  out = v;
  return true;
}

bool parse_real(std::string_view text, double lo, double hi, double& out) {
  if (text.empty() || text.size() > 32 || text.front() == '+') return false;
  double v = 0.0;
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
  if (ec != std::errc{} || end != text.data() + text.size() || !std::isfinite(v) || v < lo || v > hi) return false;
  out = v;
  return true;
}

}  // namespace

std::variant<EngineOptions, std::string> parse_cli(const std::vector<std::string>& args) {
  EngineOptions o;
  std::set<std::string> seen;
  bool test_only = false;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (a.rfind("--", 0) != 0) return std::string{"unexpected positional argument"};
    if (!seen.insert(a).second) return std::string{"a flag is given twice"};
    auto value = [&](std::string& out) -> bool {
      if (i + 1 >= args.size()) return false;
      out = args[++i];
      return true;
    };
    auto integer = [&](long long lo, long long hi, int& out) -> bool {
      std::string v;
      long long n = 0;
      if (!value(v) || !parse_int(v, lo, hi, n)) return false;
      out = static_cast<int>(n);
      return true;
    };
    if (a == "--serve") { o.serve = true; continue; }
    if (a == "--no-graph") { o.graph = false; continue; }
    if (a == "--no-prompt-borrow") { o.prompt_borrow = false; continue; }
    if (a == "--check") { o.check = true; continue; }
    if (a == "--native") {
      if (!value(o.model) || o.model.empty()) return std::string{"--native needs the model file"};
      continue;
    }
    if (a == "--gpu") {
      std::string v;
      if (!value(v) || v.empty()) return std::string{"--gpu needs a device list"};
      std::size_t at = 0;
      while (at <= v.size()) {
        const std::size_t comma = std::min(v.find(',', at), v.size());
        long long d = 0;
        if (!parse_int(std::string_view(v).substr(at, comma - at), 0, 255, d)) return std::string{"--gpu takes device numbers 0-255"};
        if (std::find(o.gpus.begin(), o.gpus.end(), static_cast<int>(d)) != o.gpus.end()) return std::string{"--gpu lists a device twice"};
        o.gpus.push_back(static_cast<int>(d));
        if (comma == v.size()) break;
        at = comma + 1;
      }
      if (o.gpus.empty() || o.gpus.size() > 16) return std::string{"--gpu takes 1-16 devices"};
      continue;
    }
    if (a == "--max-context") { if (!integer(1, 262144, o.max_context)) return std::string{"--max-context must be 1-262144"}; continue; }
    if (a == "--chunk") { if (!integer(1, 8192, o.chunk)) return std::string{"--chunk must be 1-8192"}; continue; }
    if (a == "--expert-slots") { if (!integer(0, 128, o.expert_slots)) return std::string{"--expert-slots must be 0-128"}; continue; }
    if (a == "--hybrid-rows") { if (!integer(1, 4096, o.hybrid_rows)) return std::string{"--hybrid-rows must be 1-4096"}; continue; }
    if (a == "--cpu-threads") { if (!integer(1, 256, o.cpu_threads)) return std::string{"--cpu-threads must be 1-256"}; continue; }
    if (a == "--kv") {
      std::string v;
      if (!value(v) || (v != "f16" && v != "f32")) return std::string{"--kv must be f16 or f32"};
      o.kv_f32 = v == "f32";
      continue;
    }
    if (a == "--expert-vram") {
      std::string v;
      if (!value(v) || !parse_real(v, 0.0, 1024.0, o.expert_vram_gib)) return std::string{"--expert-vram must be 0-1024 GiB"};
      continue;
    }
    if (a == "--pcie-frac") {
      std::string v;
      if (!value(v) || !parse_real(v, 0.0, 1.0, o.pcie_frac)) return std::string{"--pcie-frac must be 0-1"};
      continue;
    }
    if (a == "--expert-profile") {
      if (!value(o.expert_profile) || o.expert_profile.empty()) return std::string{"--expert-profile needs a profile file"};
      continue;
    }
    if (a == "--expert-profile-save") {
      if (!value(o.expert_profile_save) || o.expert_profile_save.empty()) return std::string{"--expert-profile-save needs a file"};
      continue;
    }
    if (a == "--cpu-lane") {
      std::string v;
      if (!value(v) || (v != "on" && v != "off")) return std::string{"--cpu-lane must be on or off"};
      o.cpu_lane = v == "on";
      continue;
    }
    // Test-only flags.
    if (a == "--tokens") { test_only = true; if (!value(o.tokens) || o.tokens.empty()) return std::string{"--tokens needs ids"}; continue; }
    if (a == "--force") { test_only = true; if (!value(o.force) || o.force.empty()) return std::string{"--force needs ids"}; continue; }
    if (a == "--dump-full-logits") {
      test_only = true;
      if (!value(o.dump_full_logits) || o.dump_full_logits.empty()) return std::string{"--dump-full-logits needs a file"};
      continue;
    }
    if (a == "--dump-layers") {
      test_only = true;
      if (!value(o.dump_layers) || o.dump_layers.empty()) return std::string{"--dump-layers needs a file"};
      continue;
    }
    if (a == "--n") { test_only = true; if (!integer(0, 1 << 20, o.n_gen)) return std::string{"--n must be 0-1048576"}; continue; }
    if (a == "--bench-pp") { test_only = true; if (!integer(1, 262144, o.bench_pp)) return std::string{"--bench-pp must be 1-262144"}; continue; }
    if (a == "--bench-tg") { test_only = true; if (!integer(1, 1 << 20, o.bench_tg)) return std::string{"--bench-tg must be 1-1048576"}; continue; }
    return std::string{"unknown flag"};
  }
  if (o.model.empty()) return std::string{"--native is required"};
  if (o.serve && (test_only || o.check)) return std::string{"test-only flags are refused with --serve"};
  return o;
}

std::vector<std::int32_t> parse_id_list(std::string_view text, bool& ok) {
  std::vector<std::int32_t> ids;
  ok = false;
  if (text.empty()) return ids;
  std::size_t at = 0;
  while (true) {
    const std::size_t comma = std::min(text.find(',', at), text.size());
    long long v = 0;
    if (!parse_int(text.substr(at, comma - at), 0, 0x7fffffff, v)) return {};
    ids.push_back(static_cast<std::int32_t>(v));
    if (comma == text.size()) break;
    at = comma + 1;
  }
  ok = true;
  return ids;
}

std::variant<GenRequest, std::string> parse_gen(std::string_view line, int vocab) {
  if (line.rfind("GEN ", 0) != 0) return std::string{"expected: GEN <max_new> [key=value ...] <id,id,...>"};
  std::vector<std::string_view> fields;
  std::size_t at = 4;
  while (at < line.size()) {
    const std::size_t sp = std::min(line.find(' ', at), line.size());
    if (sp == at) return std::string{"bad request: an empty field"};
    fields.push_back(line.substr(at, sp - at));
    at = sp + 1;
    if (sp == line.size()) break;
    if (at == line.size()) return std::string{"bad request: a trailing space"};
  }
  if (fields.size() < 2) return std::string{"bad request: max_new or the ids"};
  GenRequest r;
  if (!parse_int(fields.front(), 1, 1 << 30, r.max_new)) return std::string{"bad request: max_new or the ids"};
  for (std::size_t i = 1; i + 1 < fields.size(); ++i) {
    const std::string_view kv = fields[i];
    const std::size_t eq = kv.find('=');
    if (eq == std::string_view::npos || eq == 0) return std::string{"bad request: a field that is not key=value"};
    const std::string_view key = kv.substr(0, eq), v = kv.substr(eq + 1);
    double real = 0.0;
    long long n = 0;
    Sampling& s = r.sampling;
    if (key == "temperature") { if (!parse_real(v, 0.0, 100.0, real)) return std::string{"bad request: temperature"}; s.temperature = static_cast<float>(real); }
    else if (key == "top_p") { if (!parse_real(v, 0.0, 1.0, real)) return std::string{"bad request: top_p"}; s.top_p = static_cast<float>(real); }
    else if (key == "min_p") { if (!parse_real(v, 0.0, 1.0, real)) return std::string{"bad request: min_p"}; s.min_p = static_cast<float>(real); }
    else if (key == "penalty_repeat") { if (!parse_real(v, 0.0, 100.0, real)) return std::string{"bad request: penalty_repeat"}; s.repeat = static_cast<float>(real); }
    else if (key == "penalty_freq") { if (!parse_real(v, -100.0, 100.0, real)) return std::string{"bad request: penalty_freq"}; s.freq = static_cast<float>(real); }
    else if (key == "penalty_present") { if (!parse_real(v, -100.0, 100.0, real)) return std::string{"bad request: penalty_present"}; s.present = static_cast<float>(real); }
    else if (key == "top_k") { if (!parse_int(v, 0, 1 << 20, n)) return std::string{"bad request: top_k"}; s.top_k = static_cast<int>(n); }
    else if (key == "penalty_last_n") { if (!parse_int(v, 0, 1 << 20, n)) return std::string{"bad request: penalty_last_n"}; s.last_n = static_cast<int>(n); }
    else if (key == "seed") {
      unsigned long long seed = 0;
      const auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), seed);
      if (v.empty() || ec != std::errc{} || end != v.data() + v.size()) return std::string{"bad request: seed"};
      s.seed = seed;
    } else {
      return std::string{"bad request: an unknown sampling key"};
    }
  }
  bool ok = false;
  r.ids = parse_id_list(fields.back(), ok);
  if (!ok || r.ids.empty()) return std::string{"bad request: max_new or the ids"};
  for (const auto id : r.ids) {
    if (id < 0 || id >= vocab) return std::string{"a token id is outside the vocabulary"};
  }
  return r;
}

unsigned long long request_seed(const unsigned long long requested, const unsigned long long clock_ticks) {
  if (requested > 0) return requested;
  // splitmix64 of the clock: neighbouring ticks give unrelated seeds.
  unsigned long long z = clock_ticks + 0x9E3779B97F4A7C15ull;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  z ^= z >> 31;
  return z == 0 ? 1 : z;
}

std::string engine_info_line(const LoadReport& r, const int context) {
  char buffer[512];
  std::snprintf(buffer, sizeof(buffer),
                "INFO engine=strata-gemma4 context=%d expert_slots=%d expert_slots_primary=%d slots_per_layer=%d "
                "experts_resident_pct=%.1f vram_used_mib=%.0f vram_total_mib=%.0f pinned_gib=%.2f ring_rows=%d load_s=%.1f",
                context, kLayers * kExperts, kLayers * r.slots_per_layer, r.slots_per_layer, r.resident_pct, r.vram_used_mib,
                r.vram_total_mib, r.pinned_gib, r.ring_rows, r.load_seconds);
  return buffer;
}

bool penalties_on(const Sampling& s) { return s.last_n != 0 && (s.repeat != 1.0f || s.freq != 0.0f || s.present != 0.0f); }

int candidate_count(const Sampling& s, const int vocab) { return std::max(1, std::min(s.top_k > 0 ? s.top_k : 64, vocab)); }

int sample_sorted(const float* vals, const int* ids, const int kk, const Sampling& s, std::mt19937_64& rng) {
  const std::size_t n = static_cast<std::size_t>(std::max(kk, 1));
  std::vector<double> p(n);
  const double mx = vals[0];
  double sum = 0;
  for (std::size_t i = 0; i < n; ++i) sum += (p[i] = std::exp((vals[i] - mx) / s.temperature));
  std::size_t keep = n;
  double cum = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (p[i] / sum < s.min_p * (p[0] / sum)) { keep = i; break; }
    cum += p[i] / sum;
    if (cum >= s.top_p) { keep = i + 1; break; }
  }
  keep = std::max<std::size_t>(1, keep);
  double tot = 0;
  for (std::size_t i = 0; i < keep; ++i) tot += p[i];
  std::uniform_real_distribution<double> u(0.0, tot);
  double r = u(rng);
  for (std::size_t i = 0; i < keep; ++i) {
    if ((r -= p[i]) <= 0) return ids[i];
  }
  return ids[keep - 1];
}

int sample(std::vector<float>& lg, const Sampling& s, const std::vector<std::int32_t>& hist, std::mt19937_64& rng) {
  if (penalties_on(s)) {
    std::map<int, int> cnt;
    const std::size_t from = s.last_n < 0 || static_cast<std::size_t>(s.last_n) >= hist.size() ? 0 : hist.size() - static_cast<std::size_t>(s.last_n);
    for (std::size_t i = from; i < hist.size(); ++i) ++cnt[hist[i]];
    for (const auto& [id, c] : cnt) {
      if (id < 0 || static_cast<std::size_t>(id) >= lg.size()) continue;
      float& v = lg[static_cast<std::size_t>(id)];
      if (s.repeat != 1.0f) v = v > 0 ? v / s.repeat : v * s.repeat;
      v -= static_cast<float>(c) * s.freq + s.present;
    }
  }
  if (s.temperature <= 0.0f) return static_cast<int>(std::max_element(lg.begin(), lg.end()) - lg.begin());
  const int kk = candidate_count(s, static_cast<int>(lg.size()));
  std::vector<int> idx(lg.size());
  for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = static_cast<int>(i);
  // Highest first, ties by the lower id (the GPU's top-k ranks the same way).
  std::partial_sort(idx.begin(), idx.begin() + kk, idx.end(), [&](int a, int b) {
    const float la = lg[static_cast<std::size_t>(a)], lb = lg[static_cast<std::size_t>(b)];
    return la > lb || (la == lb && a < b);
  });
  std::vector<float> vals(static_cast<std::size_t>(kk));
  for (std::size_t i = 0; i < vals.size(); ++i) vals[i] = lg[static_cast<std::size_t>(idx[i])];
  return sample_sorted(vals.data(), idx.data(), kk, s, rng);
}

std::size_t reuse_length(const std::vector<std::int32_t>& held, const std::vector<std::int32_t>& prompt, std::size_t spare) {
  std::size_t l = 0;
  while (l < held.size() && l < prompt.size() && held[l] == prompt[l]) ++l;
  if (prompt.empty()) return 0;
  const std::size_t r = std::min(l, prompt.size() - 1);  // at least one prompt token is read
  if (r == 0) return 0;
  if (held.size() - r > spare) return 0;
  return r;
}

}  // namespace g4
