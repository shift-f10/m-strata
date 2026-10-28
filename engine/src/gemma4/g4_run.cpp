// The Gemma 4 family's entry, gemma4_main - the only export of multiengine_gemma4.dll, which the multiengine launcher
// calls for a gemma4 model: the command line, the serve loop (strata_0143, as upstream's q36 engine prints it) and
// the test-only modes (plan Task 7).  Formerly strata-gemma4-engine's main.
#include "g4_cli.hpp"
#include "g4_engine.hpp"
#include "g4_gguf.hpp"
#include "g4_model.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

void set_env(const char* key, const std::string& value) {
#if defined(_WIN32)
  _putenv_s(key, value.c_str());
#else
  setenv(key, value.c_str(), 1);
#endif
}

int run_check(const g4::ModelDesc& m) {
  int full = 0, bf16 = 0;
  for (const auto& L : m.layers) {
    full += L.sliding ? 0 : 1;
    bf16 += L.attn_q.type == g4::TensorType::bf16 ? 1 : 0;
  }
  std::printf("gemma4 26B-A4B: 30 layers (%d full, %d sliding), %d layers with BF16 attention, eos %d, <turn|> %d, softcap %.1f\n",
              full, 30 - full, bf16, m.eos_id, m.turn_close_id, double(m.softcap));
  return 0;
}

struct Serve {
  g4::Engine& E;
  std::mutex mu;
  std::condition_variable cv;
  std::deque<std::string> lines;
  bool eof{false};
  std::atomic<bool> stop{false};
  std::vector<std::int32_t> held;
  std::vector<float> lg;
  std::int32_t eos_a, eos_b;

  Serve(g4::Engine& e, std::int32_t a, std::int32_t b) : E(e), eos_a(a), eos_b(b) {}

  bool is_eos(std::int32_t t) const { return t == eos_a || t == eos_b; }

  // Returns false when the engine failed (the process then exits non-zero).
  bool request(const std::string& line) {
    const auto parsed = g4::parse_gen(line, g4::kVocab);
    if (const auto* err = std::get_if<std::string>(&parsed)) {
      std::printf("ERR %s\n", err->c_str());
      std::fflush(stdout);
      return true;
    }
    const auto& req = std::get<g4::GenRequest>(parsed);
    const auto& ids = req.ids;
    if (static_cast<long long>(ids.size()) + req.max_new > E.context()) {
      std::printf("ERR prompt (%zu tokens) + max_new (%lld) exceeds the context (%d)\n", ids.size(), req.max_new, E.context());
      std::fflush(stdout);
      return true;
    }
    stop.store(false);
    const std::size_t reuse = g4::reuse_length(held, ids, static_cast<std::size_t>(E.ring_spare()));
    E.truncate(static_cast<int>(reuse));
    held.resize(reuse);
    const auto t0 = Clock::now();
    const int fresh = static_cast<int>(ids.size() - reuse);
    const int got = E.process(ids.data() + reuse, fresh, [&](int done) {
      std::printf("PP %d %d %.0f %.1f\n", done, fresh, ms_since(t0), double(done) / std::max(1e-9, ms_since(t0) / 1000.0));
      std::fflush(stdout);
      return !stop.load();
    });
    if (got < 0) {
      std::printf("ERR the engine failed (CUDA)\n");
      std::fflush(stdout);
      return false;
    }
    held.insert(held.end(), ids.begin() + static_cast<std::ptrdiff_t>(reuse), ids.begin() + static_cast<std::ptrdiff_t>(reuse) + got);
    const double prompt_ms = ms_since(t0);
    if (got < fresh) {
      std::printf("DONE 0 %zu %.1f 0.0 cancel 0 0 %zu 0 0 0 0 0.0 %d 0\n", ids.size(), prompt_ms, reuse, got);
      std::fflush(stdout);
      return true;
    }
    std::mt19937_64 rng(g4::request_seed(
        req.sampling.seed, static_cast<unsigned long long>(Clock::now().time_since_epoch().count())));
    std::vector<std::int32_t> hist = ids;
    const auto d0 = Clock::now();
    long long produced = 0;
    const char* finish = "length";
    const int kk = g4::candidate_count(req.sampling, g4::kVocab);
    const bool gpu_rank = !g4::penalties_on(req.sampling) && kk <= g4::kSampleCandidatesMax;
    std::vector<float> cand_v(static_cast<std::size_t>(g4::kSampleCandidatesMax));
    std::vector<int> cand_i(static_cast<std::size_t>(g4::kSampleCandidatesMax));
    auto pick = [&]() -> int {
      if (!req.sampling.needs_logits()) return E.greedy();
      if (gpu_rank) {
        const int got = E.top_candidates(kk, cand_v.data(), cand_i.data());
        if (got < 0) return got;
        return g4::sample_sorted(cand_v.data(), cand_i.data(), kk, req.sampling, rng);
      }
      if (!E.logits(lg)) return -1;
      for (const float v : lg) {
        if (!std::isfinite(v)) return -2;
      }
      return g4::sample(lg, req.sampling, hist, rng);
    };
    int nx = pick();
    for (;;) {
      if (nx == -2) {
        finish = "nonfinite";
        break;
      }
      if (nx < 0) {
        std::printf("ERR the engine failed (CUDA)\n");
        std::fflush(stdout);
        return false;
      }
      std::printf("T %d\n", nx);
      std::fflush(stdout);
      ++produced;
      hist.push_back(nx);
      if (is_eos(nx)) { finish = "stop"; break; }
      if (produced >= req.max_new) break;
      if (stop.load()) { finish = "cancel"; break; }
      if (E.position() + 1 > E.context()) break;
      const std::int32_t one = nx;
      if (E.process(&one, 1, {}) != 1) {
        std::printf("ERR the engine failed (CUDA)\n");
        std::fflush(stdout);
        return false;
      }
      held.push_back(nx);
      if (produced % g4::kDecodeRebalanceEvery == 0) E.rebalance(g4::kDecodeRebalanceSwaps);
      nx = pick();
    }
    const double decode_ms = ms_since(d0);
    std::printf("DONE %lld %zu %.1f %.1f %s 0 0 %zu 0 0 0 0 0.0 %d 0\n", produced, ids.size(), prompt_ms, decode_ms, finish, reuse, fresh);
    std::fflush(stdout);
    std::fprintf(stderr, "strata-gemma4 serve: prompt %zu tokens = %zu reused + %d read in %.0f ms (%.1f tok/s), %lld generated in %.0f ms (%.1f tok/s) (%s)\n",
                 ids.size(), reuse, fresh, prompt_ms, prompt_ms > 0 ? 1000.0 * fresh / prompt_ms : 0.0, produced, decode_ms,
                 decode_ms > 0 ? 1000.0 * double(produced) / decode_ms : 0.0, finish);
    std::string why;   // between requests: the learned profile, when its interval has passed
    if (!E.save_profile(false, why)) std::fprintf(stderr, "strata-gemma4: the expert profile was not saved: %s\n", why.c_str());
    return true;
  }

  int run(const g4::LoadReport& r) {
    std::printf("%s\n", g4::engine_info_line(r, E.context()).c_str());
    std::printf("READY %d stop\n", E.context());
    std::fflush(stdout);
    std::thread reader([this] {
      std::string l;
      while (std::getline(std::cin, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (l == "STOP") {
          stop.store(true);
          continue;
        }
        std::lock_guard<std::mutex> lk(mu);
        lines.push_back(l);
        cv.notify_one();
      }
      std::lock_guard<std::mutex> lk(mu);
      eof = true;
      cv.notify_one();
    });
    reader.detach();
    for (;;) {
      std::string line;
      {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&] { return eof || !lines.empty(); });
        if (lines.empty()) return 0;
        line = lines.front();
        lines.pop_front();
      }
      if (line.empty()) continue;
      if (line == "QUIT") return 0;
      if (line.rfind("GEN ", 0) == 0) {
        if (!request(line)) return 1;
      } else {
        std::printf("ERR expected: GEN <max_new> [key=value ...] <id,id,...>\n");
        std::fflush(stdout);
      }
    }
  }
};

bool write_floats(const std::string& path, const std::vector<float>& v) {
  std::FILE* f = nullptr;
#if defined(_WIN32)
  if (fopen_s(&f, path.c_str(), "wb") != 0) f = nullptr;
#else
  f = std::fopen(path.c_str(), "wb");
#endif
  if (f == nullptr) return false;
  const bool ok = std::fwrite(v.data(), 4, v.size(), f) == v.size();
  return std::fclose(f) == 0 && ok;
}

int run_tokens(g4::Engine& E, const g4::EngineOptions& o) {
  bool ok = false;
  const auto ids = g4::parse_id_list(o.tokens, ok);
  bool fok = true;
  const auto forced = o.force.empty() ? std::vector<std::int32_t>{} : g4::parse_id_list(o.force, fok);
  if (!ok || !fok) {
    std::fprintf(stderr, "bad --tokens or --force\n");
    return 2;
  }
  for (auto t : ids) {
    if (t < 0 || t >= g4::kVocab) { std::fprintf(stderr, "a token id is outside the vocabulary\n"); return 2; }
  }
  std::vector<float> layers, full, lg;
  if (!o.dump_layers.empty()) E.set_layer_sink(&layers);
  const int n_gen = forced.empty() ? o.n_gen : static_cast<int>(forced.size()) + 1;
  std::printf("prompt:");
  for (auto t : ids) std::printf(" %d", t);
  std::printf("\n");
  const auto t0 = Clock::now();
  if (E.process(ids.data(), static_cast<int>(ids.size()), {}) != static_cast<int>(ids.size())) {
    std::fprintf(stderr, "the prompt failed\n");
    return 1;
  }
  const double pp = ms_since(t0);
  std::vector<std::int32_t> gen;
  const auto d0 = Clock::now();
  for (int i = 0; i < n_gen; ++i) {
    if (!o.dump_full_logits.empty()) {
      if (!E.logits(lg)) return 1;
      full.insert(full.end(), lg.begin(), lg.end());
    }
    const int a = E.greedy();
    if (a < 0) return 1;
    gen.push_back(a);
    if (i + 1 == n_gen) break;
    const std::int32_t t = i < static_cast<int>(forced.size()) ? forced[static_cast<std::size_t>(i)] : a;
    if (E.process(&t, 1, {}) != 1) return 1;
  }
  const double tg = ms_since(d0);
  std::printf("gen:");
  for (auto t : gen) std::printf(" %d", t);
  std::printf("\n");
  std::fflush(stdout);
  std::fprintf(stderr, "prompt %zu tokens in %.0f ms (%.1f tok/s); %d steps in %.0f ms (%.1f tok/s)\n", ids.size(), pp,
               1000.0 * double(ids.size()) / std::max(pp, 1e-9), n_gen, tg, 1000.0 * n_gen / std::max(tg, 1e-9));
  if (!o.dump_full_logits.empty() && !write_floats(o.dump_full_logits, full)) return 1;
  if (!o.dump_layers.empty() && !write_floats(o.dump_layers, layers)) return 1;
  return 0;
}

int run_bench(g4::Engine& E, const g4::EngineOptions& o) {
  std::vector<std::int32_t> ids;
  for (int i = 0; i < std::max(o.bench_pp, 1); ++i) ids.push_back(1000 + (i * 7919) % 200000);
  if (o.bench_pp > 0) {
    const auto t0 = Clock::now();
    if (E.process(ids.data(), o.bench_pp, {}) != o.bench_pp) return 1;
    const double ms = ms_since(t0);
    std::printf("bench-pp %d tokens: %.0f ms, %.1f tok/s\n", o.bench_pp, ms, 1000.0 * o.bench_pp / ms);
  } else if (E.process(ids.data(), 1, {}) != 1) {
    return 1;
  }
  if (o.bench_tg > 0) {
    const auto t0 = Clock::now();
    for (int i = 0; i < o.bench_tg; ++i) {
      const int a = E.greedy();
      if (a < 0) return 1;
      const std::int32_t t = a;
      if (E.process(&t, 1, {}) != 1) return 1;
      if ((i + 1) % g4::kDecodeRebalanceEvery == 0) E.rebalance(g4::kDecodeRebalanceSwaps);
    }
    const double ms = ms_since(t0);
    std::printf("bench-tg %d tokens: %.0f ms, %.1f tok/s\n", o.bench_tg, ms, 1000.0 * o.bench_tg / ms);
  }
  std::fflush(stdout);
  return 0;
}

int run(const std::vector<std::string>& args) {
  const auto parsed = g4::parse_cli(args);
  if (const auto* err = std::get_if<std::string>(&parsed)) {
    std::fprintf(stderr, "strata-gemma4-engine: %s\n", err->c_str());
    return 2;
  }
  const auto& o = std::get<g4::EngineOptions>(parsed);
  if (!o.gpus.empty()) {
    std::string list;
    for (int g : o.gpus) list += (list.empty() ? "" : ",") + std::to_string(g);
    set_env("CUDA_DEVICE_ORDER", "PCI_BUS_ID");
    set_env("CUDA_VISIBLE_DEVICES", list);
  }
  const auto header = g4::read_gguf_header(o.model);
  if (const auto* err = std::get_if<g4::GgufError>(&header)) {
    std::fprintf(stderr, "strata-gemma4-engine: %s\n", err->message.c_str());
    return 1;
  }
  const auto model = g4::check_model(std::get<g4::GgufFile>(header));
  if (const auto* err = std::get_if<g4::ModelError>(&model)) {
    std::fprintf(stderr, "strata-gemma4-engine: %s\n", err->message.c_str());
    return 1;
  }
  const auto& m = std::get<g4::ModelDesc>(model);
  if (o.check) return run_check(m);
  if (o.kv_f32) {
    std::fprintf(stderr, "strata-gemma4-engine: --kv f32 is not supported yet (v1 keeps the caches in f16)\n");
    return 2;
  }
  g4::EngineConfig cfg;
  cfg.model = o.model;
  cfg.max_context = o.max_context;
  cfg.chunk = std::min(o.chunk, 2048);   // the paper's prompt chunk (3.6) at most
  cfg.expert_vram_gib = o.expert_vram_gib;
  cfg.expert_slots = o.expert_slots;
  cfg.cpu_lane = o.cpu_lane;
  cfg.pcie_frac = o.pcie_frac;
  cfg.cpu_threads = o.cpu_threads;
  cfg.graph = o.graph;
  cfg.expert_profile = o.expert_profile;
  cfg.expert_profile_save = o.expert_profile_save;
  cfg.prompt_borrow = o.prompt_borrow;
  std::string error;
  auto engine = g4::Engine::create(cfg, m, error);
  if (!engine) {
    std::fprintf(stderr, "strata-gemma4-engine: %s\n", error.c_str());
    return 1;
  }
  const auto r = engine->report();
  std::fprintf(stderr, "strata-gemma4: loaded in %.1f s; %d expert slots a layer (%.1f%% resident), VRAM %.0f of %.0f MiB, pinned RAM %.2f GiB, ring %d rows, context %d\n",
               r.load_seconds, r.slots_per_layer, r.resident_pct, r.vram_used_mib, r.vram_total_mib, r.pinned_gib, r.ring_rows,
               engine->context());
  if (r.profile_filled) std::fprintf(stderr, "strata-gemma4: the expert slots were filled from %s\n", o.expert_profile.c_str());
  if (r.lent_slots > 0)
    std::fprintf(stderr, "strata-gemma4: a streamed prompt borrows the top %d expert slots for its buffers and refills them after it\n",
                 r.lent_slots);
  if (r.lane_workers > 0)
    std::fprintf(stderr, "strata-gemma4: decode misses on the CPU lane (%d pool threads), %.0f%% of them staged by the copy engine\n",
                 r.lane_workers, 100.0 * (o.pcie_frac >= 0 ? o.pcie_frac : g4::kDefaultPcieFrac));
  if (o.serve) {
    Serve sv(*engine, m.eos_id, m.turn_close_id);
    return sv.run(r);
  }
  if (!o.tokens.empty()) return run_tokens(*engine, o);
  if (o.bench_pp > 0 || o.bench_tg > 0) return run_bench(*engine, o);
  std::fprintf(stderr, "strata-gemma4-engine: nothing to do (--serve, --tokens or --bench-*)\n");
  return 2;
}

}  // namespace

#if defined(_WIN32)
#define G4_ENTRY extern "C" __declspec(dllexport)
#else
#define G4_ENTRY extern "C" __attribute__((visibility("default")))
#endif

G4_ENTRY int gemma4_main(int argc, char** argv) {
  // the launcher's setvbuf reaches this library's stdout only when they share one C runtime (/MD); a static runtime
  // (STRATA_PORTABLE's /MT) gives the library its own
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  // argv is UTF-8 on every platform here: the launcher carries the UTF-8 code page manifest (strata_utf8_manifest)
  std::vector<std::string> args(argv + 1, argv + argc);
  return ::run(args);
}
