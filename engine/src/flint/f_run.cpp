// The flint family's entry, flint_main - the only export of multiengine_flint.dll, which the multiengine launcher calls
// for a flint model: the command line, the serve loop (strata_0143, as Gemma 4's g4_run.cpp prints it) and the
// test-only modes.
#include "f_cli.hpp"
#include "f_engine.hpp"
#include "f_model.hpp"

#include <algorithm>
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

const char* type_name(fl::TensorType t) {
  return t == fl::TensorType::q8_0 ? "Q8_0" : t == fl::TensorType::bf16 ? "BF16" : t == fl::TensorType::f32 ? "F32" : "F16";
}

int run_check(const fl::ModelDesc& m) {
  int full = 0, rope = 0;
  for (const auto& L : m.layers) {
    full += L.sliding ? 0 : 1;
    rope += L.rope ? 1 : 0;
  }
  const auto& e = m.layers[fl::kDenseLayers];
  std::printf("flint: %d layers (%d dense, %d MoE; %d full, %d sliding of %d; RoPE on %d), %zu shard(s); experts %s/%s/%s, "
              "attention %s, head %s, router %s; eos %d, %zu end-of-generation ids\n",
              fl::kLayers, fl::kDenseLayers, fl::kMoeLayers, full, fl::kLayers - full, fl::kWindow, rope, m.shards.size(),
              type_name(e.gate_exps.type), type_name(e.up_exps.type), type_name(e.down_exps.type), type_name(e.attn_q.type),
              type_name(m.output.type), type_name(e.gate_inp.type), m.eos_id, m.eog_ids.size());
  return 0;
}

struct Serve {
  fl::Engine& E;
  std::mutex mu;
  std::condition_variable cv;
  std::deque<std::string> lines;
  bool eof{false};
  std::atomic<bool> stop{false};
  std::vector<std::int32_t> held;
  std::vector<float> lg;
  std::vector<std::int32_t> eog;

  Serve(fl::Engine& e, std::vector<std::int32_t> stop_ids) : E(e), eog(std::move(stop_ids)) {}

  bool is_eog(std::int32_t t) const { return std::find(eog.begin(), eog.end(), t) != eog.end(); }

  // Returns false when the engine failed (the process then exits non-zero).
  bool request(const std::string& line) {
    const auto parsed = fl::parse_gen(line, fl::kVocab);
    if (const auto* err = std::get_if<std::string>(&parsed)) {
      std::printf("ERR %s\n", err->c_str());
      std::fflush(stdout);
      return true;
    }
    const auto& req = std::get<fl::GenRequest>(parsed);
    const auto& ids = req.ids;
    if (static_cast<long long>(ids.size()) + req.max_new > E.context()) {
      std::printf("ERR prompt (%zu tokens) + max_new (%lld) exceeds the context (%d)\n", ids.size(), req.max_new, E.context());
      std::fflush(stdout);
      return true;
    }
    stop.store(false);
    const std::size_t reuse = fl::reuse_length(held, ids, static_cast<std::size_t>(E.ring_spare()));
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
    std::mt19937_64 rng(fl::request_seed(req.sampling.seed, static_cast<unsigned long long>(Clock::now().time_since_epoch().count())));
    std::vector<std::int32_t> hist = ids;
    const auto d0 = Clock::now();
    long long produced = 0;
    const char* finish = "length";
    const int kk = fl::candidate_count(req.sampling, fl::kVocab);
    const bool gpu_rank = !fl::penalties_on(req.sampling) && kk <= fl::kSampleCandidatesMax;
    std::vector<float> cand_v(static_cast<std::size_t>(fl::kSampleCandidatesMax));
    std::vector<int> cand_i(static_cast<std::size_t>(fl::kSampleCandidatesMax));
    auto pick = [&]() -> int {
      if (!req.sampling.needs_logits()) return E.greedy();
      if (gpu_rank) {
        const int n = E.top_candidates(kk, cand_v.data(), cand_i.data());
        if (n < 0) return n;
        return fl::sample_sorted(cand_v.data(), cand_i.data(), kk, req.sampling, rng);
      }
      if (!E.logits(lg)) return -1;
      for (const float v : lg) {
        if (!std::isfinite(v)) return -2;
      }
      return fl::sample(lg, req.sampling, hist, rng);
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
      if (is_eog(nx)) { finish = "stop"; break; }
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
      if (produced % fl::kDecodeRebalanceEvery == 0) E.rebalance(fl::kDecodeRebalanceSwaps);
      nx = pick();
    }
    const double decode_ms = ms_since(d0);
    std::printf("DONE %lld %zu %.1f %.1f %s 0 0 %zu 0 0 0 0 0.0 %d 0\n", produced, ids.size(), prompt_ms, decode_ms, finish, reuse, fresh);
    std::fflush(stdout);
    std::fprintf(stderr, "multiengine-flint serve: prompt %zu tokens = %zu reused + %d read in %.0f ms (%.1f tok/s), %lld generated in %.0f ms (%.1f tok/s) (%s)\n",
                 ids.size(), reuse, fresh, prompt_ms, prompt_ms > 0 ? 1000.0 * fresh / prompt_ms : 0.0, produced, decode_ms,
                 decode_ms > 0 ? 1000.0 * double(produced) / decode_ms : 0.0, finish);
    std::string why;
    if (!E.save_profile(false, why)) std::fprintf(stderr, "multiengine-flint: the expert profile was not saved: %s\n", why.c_str());
    return true;
  }

  int run(const fl::LoadReport& r) {
    std::printf("%s\n", fl::engine_info_line(r, E.context()).c_str());
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

int run_tokens(fl::Engine& E, const fl::EngineOptions& o) {
  bool ok = false;
  const auto ids = fl::parse_id_list(o.tokens, ok);
  bool fok = true;
  const auto forced = o.force.empty() ? std::vector<std::int32_t>{} : fl::parse_id_list(o.force, fok);
  if (!ok || !fok) {
    std::fprintf(stderr, "bad --tokens or --force\n");
    return 2;
  }
  for (auto t : ids) {
    if (t < 0 || t >= fl::kVocab) { std::fprintf(stderr, "a token id is outside the vocabulary\n"); return 2; }
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

int run_bench(fl::Engine& E, const fl::EngineOptions& o) {
  std::vector<std::int32_t> ids;
  for (int i = 0; i < std::max(o.bench_pp, 1); ++i) ids.push_back(1000 + (i * 7919) % 140000);
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
      if ((i + 1) % fl::kDecodeRebalanceEvery == 0) E.rebalance(fl::kDecodeRebalanceSwaps);
    }
    const double ms = ms_since(t0);
    std::printf("bench-tg %d tokens: %.0f ms, %.1f tok/s\n", o.bench_tg, ms, 1000.0 * o.bench_tg / ms);
  }
  std::fflush(stdout);
  return 0;
}

int run(const std::vector<std::string>& args) {
  const auto parsed = fl::parse_cli(args);
  if (const auto* err = std::get_if<std::string>(&parsed)) {
    std::fprintf(stderr, "multiengine-flint: %s\n", err->c_str());
    return 2;
  }
  const auto& o = std::get<fl::EngineOptions>(parsed);
  if (!o.gpus.empty()) {
    std::string list;
    for (int g : o.gpus) list += (list.empty() ? "" : ",") + std::to_string(g);
    set_env("CUDA_DEVICE_ORDER", "PCI_BUS_ID");
    set_env("CUDA_VISIBLE_DEVICES", list);
  }
  const auto model = fl::read_model(o.model);
  if (const auto* err = std::get_if<fl::ModelError>(&model)) {
    std::fprintf(stderr, "multiengine-flint: %s\n", err->message.c_str());
    return 1;
  }
  const auto& m = std::get<fl::ModelDesc>(model);
  if (o.check) return run_check(m);
  fl::EngineConfig cfg;
  cfg.shards = m.shards;
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
  cfg.layers = o.layers;
  std::string error;
  auto engine = fl::Engine::create(cfg, m, error);
  if (!engine) {
    std::fprintf(stderr, "multiengine-flint: %s\n", error.c_str());
    return 1;
  }
  const auto r = engine->report();
  std::fprintf(stderr, "multiengine-flint: loaded in %.1f s; %d expert slots a layer (%.1f%% resident), VRAM %.0f of %.0f MiB, pinned RAM %.2f GiB, ring %d rows, context %d\n",
               r.load_seconds, r.slots_per_layer, r.resident_pct, r.vram_used_mib, r.vram_total_mib, r.pinned_gib, r.ring_rows,
               engine->context());
  if (r.profile_filled) std::fprintf(stderr, "multiengine-flint: the expert slots were filled from %s\n", o.expert_profile.c_str());
  if (r.lent_slots > 0)
    std::fprintf(stderr, "multiengine-flint: a streamed prompt borrows the top %d expert slots for its buffers and refills them after it\n",
                 r.lent_slots);
  if (r.lane_workers > 0)
    std::fprintf(stderr, "multiengine-flint: decode misses on the CPU lane (%d pool threads), %.0f%% of them staged by the copy engine\n",
                 r.lane_workers, 100.0 * (o.pcie_frac >= 0 ? o.pcie_frac : fl::kDefaultPcieFrac));
  if (o.serve) {
    Serve sv(*engine, m.eog_ids);
    return sv.run(r);
  }
  if (!o.tokens.empty()) return run_tokens(*engine, o);
  if (o.bench_pp > 0 || o.bench_tg > 0) return run_bench(*engine, o);
  std::fprintf(stderr, "multiengine-flint: nothing to do (--serve, --tokens or --bench-*)\n");
  return 2;
}

}  // namespace

#if defined(_WIN32)
#define FL_ENTRY extern "C" __declspec(dllexport)
#else
#define FL_ENTRY extern "C" __attribute__((visibility("default")))
#endif

FL_ENTRY int flint_main(int argc, char** argv) {
  // the launcher's setvbuf reaches this library's stdout only when they share one C runtime (/MD)
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  // argv is UTF-8 on every platform here: the launcher carries the UTF-8 code page manifest (strata_utf8_manifest)
  std::vector<std::string> args(argv + 1, argv + argc);
  return ::run(args);
}
