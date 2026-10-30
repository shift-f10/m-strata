// The Gemma 4 family's entry, gemma4_main - the only export of multiengine_gemma4.dll, which the multiengine launcher
// calls for a gemma4 model: the command line, the serve loop (strata_0143, as upstream's q36 engine prints it) and
// the test-only modes (plan Task 7).  Formerly strata-gemma4-engine's main.
#include "g4_cli.hpp"
#include "g4_engine.hpp"
#include "g4_gguf.hpp"
#include "g4_model.hpp"
#include "g4_spec.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
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

int run_check(const g4::ModelDesc& m, const g4::AssistantDesc* d) {
  int full = 0, bf16 = 0;
  for (const auto& L : m.layers) {
    full += L.sliding ? 0 : 1;
    bf16 += L.attn_q.type == g4::TensorType::bf16 ? 1 : 0;
  }
  std::printf("gemma4 26B-A4B: 30 layers (%d full, %d sliding), %d layers with BF16 attention, eos %d, <turn|> %d, softcap %.1f\n",
              full, 30 - full, bf16, m.eos_id, m.turn_close_id, double(m.softcap));
  if (d != nullptr) {
    int dfull = 0;
    for (const auto& L : d->layers) dfull += L.sliding ? 0 : 1;
    std::printf("gemma4 drafter: %zu layers (%d full, %zu sliding), width %d, FFN %d, over target layers %d and %d\n", d->layers.size(),
                dfull, d->layers.size() - static_cast<std::size_t>(dfull), d->n_embd, d->layers.empty() ? 0 : d->layers[0].n_ff, g4::kLayers - 2,
                g4::kLayers - 1);
  }
  return 0;
}

// The decode loop after a prompt (the serve loop's and the MTP test mode's), plain or speculative (spec §12): `nx` is
// the first token, picked from the prompt's last row. Each token is emitted (emit: false ends the answer there); with
// a drafter, each step drafts up to `spec` tokens after it, verifies them in one window, keeps the drafts the request's
// own sampler agrees with (llama.cpp's sample-and-accept), and the engine keeps exactly the tokens plain decode keeps:
// every emitted token but the last. Returns false when the engine failed.
struct DecodeLoop {
  g4::Engine& E;
  const g4::Sampling& s;
  std::mt19937_64& rng;
  std::vector<std::int32_t>& hist;  // the prompt and the emitted tokens (the penalties' history)
  std::vector<std::int32_t>& held;  // the tokens the engine holds
  int spec{0};                      // drafts a step at most (0: plain decode)
  float p_min{0.0f};
  long long produced{0}, offered{0}, accepted{0};
  const char* finish{"length"};
  std::vector<float> lg{};
  std::vector<float> cand_v = std::vector<float>(static_cast<std::size_t>(g4::kSampleCandidatesMax));
  std::vector<int> cand_i = std::vector<int>(static_cast<std::size_t>(g4::kSampleCandidatesMax));

  int kk() const { return g4::candidate_count(s, g4::kVocab); }
  bool gpu_rank() const { return !g4::penalties_on(s) && kk() <= g4::kSampleCandidatesMax; }

  // the next token from the last read row's logits
  int pick() {
    if (!s.needs_logits()) return E.greedy();
    if (gpu_rank()) {
      const int got = E.top_candidates(kk(), cand_v.data(), cand_i.data());
      if (got < 0) return got;
      return g4::sample_sorted(cand_v.data(), cand_i.data(), kk(), s, rng);
    }
    if (!E.logits(lg)) return -1;
    for (const float v : lg) {
      if (!std::isfinite(v)) return -2;
    }
    return g4::sample(lg, s, hist, rng);
  }
  // a window row's token, with `h` the history up to it
  int pick_row(int row, const std::vector<std::int32_t>& h) {
    if (gpu_rank()) {
      const int got = E.row_candidates(row, kk(), cand_v.data(), cand_i.data());
      if (got < 0) return got;
      return g4::sample_sorted(cand_v.data(), cand_i.data(), kk(), s, rng);
    }
    if (!E.row_logits(row, lg)) return -1;
    for (const float v : lg) {
      if (!std::isfinite(v)) return -2;
    }
    return g4::sample(lg, s, h, rng);
  }

  bool rebalance_on{true};          // false: the tier never moves (the test modes, as --tokens runs)

  bool run(int nx, long long max_new, const std::function<bool(std::int32_t)>& emit_token, const std::atomic<bool>* stop) {
    long long bucket = 0;   // the expert tier moves every kDecodeRebalanceEvery emitted tokens
    auto rebalance = [&] {
      if (rebalance_on && produced / g4::kDecodeRebalanceEvery > bucket) {
        bucket = produced / g4::kDecodeRebalanceEvery;
        E.rebalance(g4::kDecodeRebalanceSwaps);
      }
    };
    // shows a token; false when the answer ends with it
    auto emit = [&](std::int32_t t) {
      ++produced;
      hist.push_back(t);
      const bool more = emit_token(t);
      if (!more) {
        finish = "stop";
        return false;
      }
      if (produced >= max_new) return false;
      if (stop != nullptr && stop->load()) {
        finish = "cancel";
        return false;
      }
      return true;
    };
    std::vector<std::int32_t> drafts(static_cast<std::size_t>(g4::kSpecMaxRows));
    std::vector<float> probs(static_cast<std::size_t>(g4::kSpecMaxRows));
    std::vector<std::int32_t> rows, greedy_rows(static_cast<std::size_t>(g4::kSpecMaxRows));
    for (;;) {
      if (nx == -2) {
        finish = "nonfinite";
        return true;
      }
      if (nx < 0) return false;
      if (!emit(nx)) return true;
      if (E.position() + 1 > E.context()) return true;
      int nd = 0;
      if (spec > 0) {
        const int budget = g4::draft_budget(spec, produced, max_new, E.position(), E.context());
        if (budget > 0) {
          const int got = E.draft(nx, budget, p_min, drafts.data(), probs.data());
          if (got < 0) return false;
          nd = g4::drafts_kept(probs.data(), got, p_min);
        }
      }
      int window = 0;
      if (nd > 0) {   // the window [nx, d0 .. d(nd-1)]
        rows.assign(1, nx);
        rows.insert(rows.end(), drafts.begin(), drafts.begin() + nd);
        window = E.verify(rows.data(), nd + 1);
        if (window < 0) return false;
      }
      if (window == 0) {   // a plain decode step (no drafts, or a window the engine does not read)
        const std::int32_t one = nx;
        if (E.process(&one, 1, {}) != 1) return false;
        held.push_back(nx);
        rebalance();
        nx = pick();
        continue;
      }
      offered += nd;
      const bool greedy = !s.needs_logits();
      if (greedy && E.window_greedy(greedy_rows.data()) != nd + 1) return false;
      // the sampler's history, extended row by row: only the penalties read it
      const bool penalties = g4::penalties_on(s);
      std::vector<std::int32_t> h;
      if (penalties) h = hist;
      const std::vector<std::int32_t> offer(drafts.begin(), drafts.begin() + nd);
      const auto samples = g4::accept_drafts(offer, [&](int row) {
        const int t = greedy ? greedy_rows[static_cast<std::size_t>(row)] : pick_row(row, penalties ? h : hist);
        if (t >= 0 && penalties) h.push_back(t);
        return t;
      });
      // samples: the accepted drafts, then the next token (or a failure); the accepted drafts are shown in turn, and
      // the engine keeps nx and the shown drafts but the last (as plain decode keeps every shown token but the last).
      // `accepted` counts every draft the sampler agreed with, as the Qwen engine and llama.cpp count them, also when
      // the answer ends inside them.
      const std::size_t k = samples.size() - 1;
      accepted += static_cast<long long>(std::min<std::size_t>(k, static_cast<std::size_t>(nd)));
      int kept = 1;
      bool ended = false;
      for (std::size_t i = 0; i < k && samples[i] >= 0; ++i) {
        if (!emit(samples[i])) {
          ended = true;
          break;
        }
        ++kept;
      }
      if (!E.accept(kept)) return false;
      held.push_back(nx);
      held.insert(held.end(), samples.begin(), samples.begin() + (kept - 1));
      rebalance();
      if (ended) return true;
      nx = samples.back();
    }
  }
};

struct Serve {
  g4::Engine& E;
  std::mutex mu;
  std::condition_variable cv;
  std::deque<std::string> lines;
  bool eof{false};
  std::atomic<bool> stop{false};
  std::vector<std::int32_t> held;
  std::int32_t eos_a, eos_b;
  int spec{0};          // MTP: drafts a step at most (--spec - 1), with a drafter
  float p_min{0.0f};    // --spec-min-p

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
    DecodeLoop loop{E, req.sampling, rng, hist, held};
    loop.spec = E.has_drafter() ? spec : 0;
    loop.p_min = p_min;
    const bool ok = loop.run(loop.pick(), req.max_new, [&](std::int32_t t) {
      std::printf("T %d\n", t);
      std::fflush(stdout);
      return !is_eos(t);
    }, &stop);
    if (!ok) {
      std::printf("ERR the engine failed (CUDA)\n");
      std::fflush(stdout);
      return false;
    }
    if (held.size() != static_cast<std::size_t>(E.position())) {
      // the reuse rule matches prompts against `held`: tokens the engine does not hold would be reused as if it did
      std::fprintf(stderr, "strata-gemma4: the held tokens (%zu) are not the engine's position (%d); the next prompt is read whole\n",
                   held.size(), E.position());
      held.clear();
      E.truncate(0);
    }
    const long long produced = loop.produced;
    const char* finish = loop.finish;
    const double decode_ms = ms_since(d0);
    // the Qwen engine's DONE shape: field 6 the drafts accepted, field 7 the drafts offered (0 0 without MTP)
    std::printf("DONE %lld %zu %.1f %.1f %s %lld %lld %zu 0 0 0 0 0.0 %d 0\n", produced, ids.size(), prompt_ms, decode_ms, finish,
                loop.accepted, loop.offered, reuse, fresh);
    std::fflush(stdout);
    std::fprintf(stderr, "strata-gemma4 serve: prompt %zu tokens = %zu reused + %d read in %.0f ms (%.1f tok/s), %lld generated in %.0f ms (%.1f tok/s)%s (%s)\n",
                 ids.size(), reuse, fresh, prompt_ms, prompt_ms > 0 ? 1000.0 * fresh / prompt_ms : 0.0, produced, decode_ms,
                 decode_ms > 0 ? 1000.0 * double(produced) / decode_ms : 0.0,
                 loop.offered > 0 ? (", drafts accepted " + std::to_string(loop.accepted) + " of " + std::to_string(loop.offered)).c_str() : "",
                 finish);
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

// --spec-check N (test-only, after the prompt and a settled tier): N greedy tokens in windows of --spec rows. Each
// window's tokens are first read as single decode steps (their logits kept), then the engine goes back and reads the
// same tokens as one verify window: every row's logits must be the steps' bit for bit. Exit 0 when all are.
int run_spec_check(g4::Engine& E, const g4::EngineOptions& o) {
  const int T = o.spec;
  std::vector<std::vector<float>> want(static_cast<std::size_t>(T));
  std::vector<float> got;
  std::vector<std::int32_t> toks;
  long long rows = 0, same_rows = 0, windows = 0;
  double worst = 0.0;
  int a = E.greedy();
  while (rows < o.spec_check) {
    const int p0 = E.position();
    if (a < 0 || p0 + T + 1 > E.context()) break;
    toks.assign(1, a);
    for (int t = 0; t < T; ++t) {
      if (E.process(&toks[static_cast<std::size_t>(t)], 1, {}) != 1 || !E.logits(want[static_cast<std::size_t>(t)])) return 1;
      toks.push_back(E.greedy());
    }
    E.truncate(p0);
    if (E.verify(toks.data(), T) != T) {
      std::fprintf(stderr, "the verify window failed\n");
      return 1;
    }
    for (int t = 0; t < T; ++t) {
      if (!E.row_logits(t, got)) return 1;
      const auto& w = want[static_cast<std::size_t>(t)];
      const bool same = got.size() == w.size() && std::memcmp(got.data(), w.data(), got.size() * sizeof(float)) == 0;
      same_rows += same ? 1 : 0;
      if (!same) {
        for (std::size_t i = 0; i < got.size() && i < w.size(); ++i) worst = std::max(worst, std::fabs(double(got[i]) - double(w[i])));
      }
    }
    if (!E.accept(T)) return 1;
    a = toks[static_cast<std::size_t>(T)];
    rows += T;
    ++windows;
  }
  std::printf("spec-check: %lld windows of %d rows, %lld of %lld rows bitwise identical to single decode steps, worst |diff| %.3g\n",
              windows, T, same_rows, rows, worst);
  std::fflush(stdout);
  return rows > 0 && same_rows == rows ? 0 : 1;
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
  if (E.has_drafter() && (!forced.empty() || !o.dump_layers.empty() || !o.dump_full_logits.empty())) {
    std::fprintf(stderr, "--force and --dump-* are not taken with --mtp\n");
    return 2;
  }
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
  if ((o.settle || o.spec_check > 0) && !E.settle()) {
    std::fprintf(stderr, "the expert swaps did not land\n");
    return 1;
  }
  if (o.spec_check > 0) return run_spec_check(E, o);
  if (E.has_drafter()) {
    // greedy through the speculative loop (the tier still, as the plain loop below keeps it)
    g4::Sampling greedy;
    std::mt19937_64 rng(1);
    std::vector<std::int32_t> hist = ids, held;
    DecodeLoop loop{E, greedy, rng, hist, held};
    loop.spec = o.spec - 1;
    loop.p_min = static_cast<float>(o.spec_min_p);
    loop.rebalance_on = false;
    std::vector<std::int32_t> gen;
    const auto d0 = Clock::now();
    if (!loop.run(E.greedy(), n_gen, [&](std::int32_t t) {
          gen.push_back(t);
          return true;
        }, nullptr)) {
      std::fprintf(stderr, "the engine failed\n");
      return 1;
    }
    const double tg = ms_since(d0);
    std::printf("gen:");
    for (auto t : gen) std::printf(" %d", t);
    std::printf("\n");
    std::fflush(stdout);
    std::fprintf(stderr, "prompt %zu tokens in %.0f ms (%.1f tok/s); %zu tokens in %.0f ms (%.1f tok/s), drafts accepted %lld of %lld\n",
                 ids.size(), pp, 1000.0 * double(ids.size()) / std::max(pp, 1e-9), gen.size(), tg,
                 1000.0 * double(gen.size()) / std::max(tg, 1e-9), loop.accepted, loop.offered);
    // as the plain loop below: non-finite logits or the context's end before --n tokens fail the run
    return gen.size() == static_cast<std::size_t>(n_gen) ? 0 : 1;
  }
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
  // MTP (spec §12): the drafter's header through its guard, its vocabulary against the target's (the file's bytes are
  // data: only the bounded reader and the guard look at them)
  std::optional<g4::AssistantDesc> drafter;
  if (!o.mtp.empty()) {
    const auto dh = g4::read_gguf_header(o.mtp);
    if (const auto* err = std::get_if<g4::GgufError>(&dh)) {
      std::fprintf(stderr, "strata-gemma4-engine: the drafter file: %s\n", err->message.c_str());
      return 1;
    }
    const auto dd = g4::check_assistant(std::get<g4::GgufFile>(dh));
    if (const auto* err = std::get_if<g4::ModelError>(&dd)) {
      std::fprintf(stderr, "strata-gemma4-engine: %s\n", err->message.c_str());
      return 1;
    }
    if (!g4::same_vocabulary(std::get<g4::GgufFile>(header), std::get<g4::GgufFile>(dh))) {
      std::fprintf(stderr, "strata-gemma4-engine: gemma4 drafter: its vocabulary is not the model's\n");
      return 1;
    }
    drafter = std::get<g4::AssistantDesc>(dd);
  } else if (o.spec_check > 0) {
    std::fprintf(stderr, "strata-gemma4-engine: --spec-check needs --mtp\n");
    return 2;
  }
  if (o.check) return run_check(m, drafter ? &*drafter : nullptr);
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
  cfg.mtp_model = o.mtp;
  cfg.spec = o.spec;
  std::string error;
  auto engine = g4::Engine::create(cfg, m, error, drafter ? &*drafter : nullptr);
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
  if (r.mtp_layers > 0)
    std::fprintf(stderr, "strata-gemma4: MTP drafter of %d layers (%.0f MiB of VRAM): up to %d drafts a step, verified in one window; "
                 "drafting stops below p %.2f\n", r.mtp_layers, r.mtp_mib, o.spec - 1, o.spec_min_p);
  if (o.serve) {
    Serve sv(*engine, m.eos_id, m.turn_close_id);
    sv.spec = engine->has_drafter() ? o.spec - 1 : 0;
    sv.p_min = static_cast<float>(o.spec_min_p);
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
