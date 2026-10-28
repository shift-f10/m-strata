// engines/unified/src/u_calibration.cpp - the per-box calibration store (Stage C1).
//
// MSVC flags std::getenv under /WX (C4996); the reads here are plain env lookups, so opt out locally.
#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS 1
#endif
//
// A tiny keyed text store (no JSON dependency): one record per line,
//   <hardware_key>|headroom_mib=<i>;rebalance_every=<i>;rebalance_margin=<f>;warm_seed=<f>;version=<i>
// The key is sanitized to [A-Za-z0-9._-] so it never collides with the '|'/';'/'=' separators.
#include "u_calibration.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace u {
namespace {

CalibrationProfile g_profile;  // defaults until init()

std::string sanitize(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (char c : s) o += (std::isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_') ? c : '_';
  return o;
}

std::string basename_of(const std::string& path) {
  const auto slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

const char* env(const char* name) {
  const char* v = std::getenv(name);
  return (v != nullptr && v[0] != '\0') ? v : nullptr;
}

// Parse "k=v;k=v;..." into `p`, tolerating unknown keys (forward-compat).
void parse_fields(const std::string& body, CalibrationProfile& p) {
  std::stringstream ss(body);
  std::string kv;
  while (std::getline(ss, kv, ';')) {
    const auto eq = kv.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
    try {
      if (k == "headroom_mib") p.residency_headroom_mib = std::stoll(v);
      else if (k == "rebalance_every") p.rebalance_every = std::stoi(v);
      else if (k == "rebalance_margin") p.rebalance_margin = std::stof(v);
      else if (k == "warm_seed") p.warm_seed = std::stod(v);
      else if (k == "stream_prefetch") p.stream_prefetch = std::stoi(v);
      else if (k == "cpu_expert_frac") p.cpu_expert_frac = std::stof(v);
      else if (k == "cpu_workers") p.cpu_workers = std::stoi(v);
      else if (k == "version") p.version = std::stoi(v);
    } catch (...) { /* leave the default for a malformed field */ }
  }
}

std::string format_fields(const CalibrationProfile& p) {
  std::ostringstream os;
  os << "headroom_mib=" << p.residency_headroom_mib << ";rebalance_every=" << p.rebalance_every
     << ";rebalance_margin=" << p.rebalance_margin << ";warm_seed=" << p.warm_seed
     << ";stream_prefetch=" << p.stream_prefetch << ";cpu_expert_frac=" << p.cpu_expert_frac
     << ";cpu_workers=" << p.cpu_workers << ";version=" << p.version;
  return os.str();
}

void apply_env(CalibrationProfile& p) {
  try {
    if (const char* v = env("STRATA_CAL_HEADROOM_MIB")) p.residency_headroom_mib = std::stoll(v);
    if (const char* v = env("STRATA_CAL_REBALANCE_EVERY")) p.rebalance_every = std::stoi(v);
    if (const char* v = env("STRATA_CAL_REBALANCE_MARGIN")) p.rebalance_margin = std::stof(v);
    if (const char* v = env("STRATA_CAL_WARM_SEED")) p.warm_seed = std::stod(v);
    if (const char* v = env("STRATA_CAL_STREAM_PREFETCH")) p.stream_prefetch = std::stoi(v);
    if (const char* v = env("STRATA_CAL_CPU_EXPERT_FRAC")) p.cpu_expert_frac = std::stof(v);
    if (const char* v = env("STRATA_CAL_CPU_WORKERS")) p.cpu_workers = std::stoi(v);
  } catch (...) { /* a malformed override keeps the prior value */ }
}

}  // namespace

std::string Calibration::store_path() {
  if (const char* f = env("STRATA_CAL_FILE")) return f;
  const char* home = env("USERPROFILE");  // Windows
  if (home == nullptr) home = env("HOME");  // POSIX
  const std::string base = home != nullptr ? std::string(home) : std::string(".");
  return (std::filesystem::path(base) / ".strata" / "unified-calibration.cal").string();
}

std::string Calibration::hardware_key(const std::string& gpu_name, std::uint64_t vram_bytes,
                                      const std::string& model_path, std::uint32_t context) {
  const std::uint64_t gib = (vram_bytes + (1ull << 29)) >> 30;  // rounded GiB
  return sanitize(gpu_name) + "__" + std::to_string(gib) + "gib__" + sanitize(basename_of(model_path)) +
         "__ctx" + std::to_string(context);
}

bool Calibration::load_record(const std::string& key, CalibrationProfile& out) {
  std::ifstream in(store_path(), std::ios::binary);
  if (!in) return false;
  std::string line;
  while (std::getline(in, line)) {
    const auto bar = line.find('|');
    if (bar == std::string::npos || line.substr(0, bar) != key) continue;
    CalibrationProfile p;  // start from defaults, then overlay the stored fields
    parse_fields(line.substr(bar + 1), p);
    out = p;
    return true;
  }
  return false;
}

bool Calibration::save(const std::string& key, const CalibrationProfile& p, std::string& err) {
  const std::string path = store_path();
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
  // read existing records (minus this key), append the new one, write a tmp, replace.
  std::vector<std::string> lines;
  {
    std::ifstream in(path, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
      const auto bar = line.find('|');
      if (bar != std::string::npos && line.substr(0, bar) == key) continue;  // drop the old record
      if (!line.empty()) lines.push_back(line);
    }
  }
  lines.push_back(key + "|" + format_fields(p));
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) { err = "calibration: cannot write " + tmp; return false; }
    for (const auto& l : lines) out << l << "\n";
  }
  std::filesystem::remove(path, ec);  // ignore: may not exist
  std::filesystem::rename(tmp, path, ec);
  if (ec) { err = "calibration: cannot replace " + path + ": " + ec.message(); return false; }
  return true;
}

void Calibration::init(const std::string& gpu_name, std::uint64_t vram_bytes, const std::string& model_path,
                       std::uint32_t context) {
  CalibrationProfile p;  // defaults = the pre-C1 literals
  const std::string key = hardware_key(gpu_name, vram_bytes, model_path, context);
  CalibrationProfile rec;
  const bool had = load_record(key, rec);
  if (had && rec.version == CalibrationProfile{}.version) p = rec;
  apply_env(p);
  g_profile = p;
  std::fprintf(stderr,
               "calibration: key=%s headroom %lld MiB, rebalance every %d (margin %.2f), warm_seed %.2f, "
               "stream_prefetch %d, cpu_expert_frac %.2f (workers %d) [%s]\n",
               key.c_str(), (long long)p.residency_headroom_mib, p.rebalance_every, p.rebalance_margin,
               p.warm_seed, p.stream_prefetch, p.cpu_expert_frac, p.cpu_workers,
               had ? "profile" : "defaults");
  std::fflush(stderr);
}

const CalibrationProfile& Calibration::profile() { return g_profile; }

}  // namespace u
