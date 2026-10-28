// strata-unified-engine: u::ExpertTier implementation (see u_experts.hpp).
//
// Ported from Maya's expert_cache.{hpp,cpp} (slot storage, residency table,
// per-layer admission, the STRP profile format, warm-start fill) and
// expert_source.hpp (pin-all-in-RAM arena; the resident/non-resident split).
// Trimmed to the one lean facility: single-token decode compute, a reused
// staging slot for non-resident experts, and a simple usage-driven rebalance.
// The DMA/PCIe-overlap machinery and the multi-token (prefill) dispatch in Maya
// are deliberately left for the GPU box.
#include "u_experts.hpp"

#include "kernels/common.cuh"
#include "kernels/u_iq.cuh"  // iq_mmvq_grouped / iq_supported (the device-decided MoE path)

#include <cuda_runtime.h>
#include <immintrin.h>  // _mm_pause / _mm_sfence (the CPU lane's spin)

#include <algorithm>
#include <chrono>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "u_cpu_expert.hpp"  // C3: the host CPU expert-compute lane (f32 reference)
#include "u_cpu_iq.hpp"      // the CPU lane's AVX2 row dots: cpu::have_avx2 (the perf lane; LanePool via u_experts.hpp)

namespace u {
namespace {

constexpr char kMagic[4] = {'S', 'T', 'R', 'P'};
constexpr std::uint32_t kProfileVersion = 1;
constexpr float kDecay = 0.999f;  // per-step decay of the routing usage counts
// The protective heat of a warm-started resident expert (so the first rebalances, run on otherwise-zero
// usage, do not evict the profile's hot set before real routing data accrues) is now the calibratable
// CalibrationProfile::warm_seed, passed into reg_finalize by the family. Default 4.0 (above the 1.0
// rebalance margin; decayed at kDecay it fades over ~a couple thousand tokens).

bool read_u32(std::ifstream& in, std::uint32_t& v) {
  return static_cast<bool>(in.read(reinterpret_cast<char*>(&v), 4));
}

// Ensure a device buffer holds at least `need` bytes, reallocating (and freeing the
// old) when it must grow. `cap` tracks the current size. False (cap left 0) on a
// failed cudaMalloc. Used by the registered path to size its scratch lazily.
bool dev_grow(void** p, std::int64_t& cap, std::int64_t need) {
  if (need <= 0) return true;
  if (*p && need <= cap) return true;
  if (*p) cudaFree(*p);
  *p = nullptr;
  if (cudaMalloc(p, (std::size_t)need) != cudaSuccess) {
    cap = 0;
    return false;
  }
  cap = need;
  return true;
}

}  // namespace

// ================================ profile I/O ================================

bool read_expert_profile(const std::string& path, std::int64_t n_layers, std::int64_t n_expert,
                         std::vector<std::pair<std::int32_t, std::int32_t>>& ranked, std::int64_t& slots,
                         std::string& err) {
  ranked.clear();
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    err = "expert profile: cannot open " + path;
    return false;
  }
  char magic[4];
  if (!in.read(magic, 4) || std::memcmp(magic, kMagic, 4) != 0) {
    err = "expert profile: bad magic (not STRP)";
    return false;
  }
  std::uint32_t version = 0, nl = 0, ne = 0, slots_field = 0, n_ranked = 0;
  if (!read_u32(in, version) || !read_u32(in, nl) || !read_u32(in, ne) || !read_u32(in, slots_field) ||
      !read_u32(in, n_ranked)) {
    err = "expert profile: truncated header";
    return false;
  }
  if (version != kProfileVersion) {
    err = "expert profile: unsupported version";
    return false;
  }
  if ((std::int64_t)nl != n_layers || (std::int64_t)ne != n_expert) {
    err = "expert profile: shape mismatch (layers/experts)";
    return false;
  }
  if ((std::int64_t)n_ranked > n_layers * n_expert) {
    err = "expert profile: n_ranked exceeds layers*experts";
    return false;
  }
  ranked.reserve(n_ranked);
  for (std::uint32_t i = 0; i < n_ranked; ++i) {
    std::uint16_t layer = 0, expert = 0;
    if (!in.read(reinterpret_cast<char*>(&layer), 2) || !in.read(reinterpret_cast<char*>(&expert), 2)) {
      err = "expert profile: truncated ranked pairs";
      return false;
    }
    if (layer >= nl || expert >= ne) {
      err = "expert profile: a ranked pair is out of range";
      return false;
    }
    ranked.emplace_back((std::int32_t)layer, (std::int32_t)expert);
  }
  // The trailing frequency/rank table is the profile's own working; the tier
  // measures heat by running, so it is not read back (Maya expert_cache.hpp).
  slots = (std::int64_t)slots_field;
  return true;
}

bool write_expert_profile(const std::string& path, std::int64_t n_layers, std::int64_t n_expert,
                          const std::vector<std::pair<std::int32_t, std::int32_t>>& ranked, std::string& err) {
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      err = "expert profile: cannot write " + tmp;
      return false;
    }
    out.write(kMagic, 4);
    const std::uint32_t n_ranked = (std::uint32_t)ranked.size();
    const std::uint32_t hdr[5] = {kProfileVersion, (std::uint32_t)n_layers, (std::uint32_t)n_expert, n_ranked,
                                  n_ranked};
    out.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
    for (const auto& [layer, expert] : ranked) {
      const std::uint16_t l = (std::uint16_t)layer, e = (std::uint16_t)expert;
      out.write(reinterpret_cast<const char*>(&l), 2);
      out.write(reinterpret_cast<const char*>(&e), 2);
    }
    // the n_layers*n_expert int32 rank table (-1 = not ranked)
    std::vector<std::int32_t> table((std::size_t)(n_layers * n_expert), -1);
    for (std::size_t r = 0; r < ranked.size(); ++r) {
      const std::int64_t idx = (std::int64_t)ranked[r].first * n_expert + ranked[r].second;
      if (idx >= 0 && idx < (std::int64_t)table.size()) table[(std::size_t)idx] = (std::int32_t)r;
    }
    out.write(reinterpret_cast<const char*>(table.data()), (std::streamsize)(table.size() * sizeof(std::int32_t)));
    if (!out) {
      err = "expert profile: write failed";
      return false;
    }
  }
  std::remove(path.c_str());
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    err = "expert profile: rename over target failed";
    return false;
  }
  return true;
}

std::vector<std::pair<std::int32_t, std::int32_t>> rank_learned_profile(
    std::int64_t n_layers, std::int64_t n_expert, const std::vector<std::uint8_t>& resident,
    const std::vector<double>& heat, const std::vector<std::pair<std::int32_t, std::int32_t>>& prior) {
  const std::int64_t n = n_layers * n_expert;
  std::vector<std::int32_t> prior_rank((std::size_t)n, (std::int32_t)n);  // absent => last
  for (std::size_t r = 0; r < prior.size(); ++r) {
    const std::int64_t idx = (std::int64_t)prior[r].first * n_expert + prior[r].second;
    if (idx >= 0 && idx < n) prior_rank[(std::size_t)idx] = (std::int32_t)r;
  }
  std::vector<std::int64_t> order((std::size_t)n);
  for (std::int64_t i = 0; i < n; ++i) order[(std::size_t)i] = i;
  auto res = [&](std::int64_t i) { return (std::size_t)i < resident.size() ? resident[(std::size_t)i] : 0; };
  auto hot = [&](std::int64_t i) { return (std::size_t)i < heat.size() ? heat[(std::size_t)i] : 0.0; };
  std::stable_sort(order.begin(), order.end(), [&](std::int64_t a, std::int64_t b) {
    if (res(a) != res(b)) return res(a) > res(b);          // resident first
    if (hot(a) != hot(b)) return hot(a) > hot(b);          // then hotter
    if (prior_rank[(std::size_t)a] != prior_rank[(std::size_t)b])
      return prior_rank[(std::size_t)a] < prior_rank[(std::size_t)b];  // then the old place
    return a < b;                                          // then index
  });
  std::vector<std::pair<std::int32_t, std::int32_t>> ranked;
  ranked.reserve((std::size_t)n);
  for (std::int64_t idx : order)
    ranked.emplace_back((std::int32_t)(idx / n_expert), (std::int32_t)(idx % n_expert));
  return ranked;
}

// ================================ ExpertTier ================================

ExpertTier::~ExpertTier() { close(); }

std::int64_t ExpertTier::blob_index(std::int64_t layer, std::int64_t expert) const {
  const std::int64_t stride = cfg_.n_expert + (cfg_.has_shared_expert ? 1 : 0);
  return layer * stride + expert;
}

void ExpertTier::layer_slot_range(std::int64_t layer, std::int64_t& lo, std::int64_t& hi) const {
  if (!cfg_.per_layer_admission) {
    lo = 0;
    hi = vram_slots_;
    return;
  }
  const std::int64_t q = cfg_.n_layers > 0 ? vram_slots_ / cfg_.n_layers : 0;
  lo = layer * q;
  hi = (layer == cfg_.n_layers - 1) ? vram_slots_ : (layer + 1) * q;
}

std::int32_t ExpertTier::slot_of(std::int64_t layer, std::int64_t expert) const {
  if (layer < 0 || layer >= cfg_.n_layers || expert < 0 || expert >= cfg_.n_expert) return kNotResident;
  return residency_[(std::size_t)(layer * cfg_.n_expert + expert)];
}

std::int32_t ExpertTier::admit(std::int64_t layer, std::int64_t expert) {
  if (layer < 0 || layer >= cfg_.n_layers || expert < 0 || expert >= cfg_.n_expert) return kNotResident;
  const std::size_t ri = (std::size_t)(layer * cfg_.n_expert + expert);
  if (residency_[ri] != kNotResident) return residency_[ri];
  std::int64_t lo = 0, hi = 0;
  layer_slot_range(layer, lo, hi);
  std::int64_t& next = cfg_.per_layer_admission ? layer_next_[(std::size_t)layer] : next_free_;
  if (next < lo) next = lo;
  if (next >= hi) return kNotResident;  // this layer's slots are full; never evicts here (warm start)
  const std::int32_t slot = (std::int32_t)next++;
  residency_[ri] = slot;
  ++admitted_;
  return slot;
}

bool ExpertTier::fill_slot_blocking(std::int32_t slot, std::int64_t layer, std::int64_t expert,
                                    std::string& err) {
  const std::uint8_t* src = host_base_ + blob_index(layer, expert) * cfg_.blob_bytes;
  std::uint8_t* dst = vram_base_ + (std::int64_t)slot * cfg_.blob_bytes;
  const cudaError_t e = cudaMemcpy(dst, src, (std::size_t)cfg_.blob_bytes, cudaMemcpyHostToDevice);
  if (e != cudaSuccess) {
    err = std::string("expert tier: slot fill failed: ") + cudaGetErrorString(e);
    return false;
  }
  ++fills_;
  return true;
}

bool ExpertTier::open(const Config& cfg, std::string& err) {
  if (opened_) {
    err = "expert tier: already open";
    return false;
  }
  if (cfg.n_layers <= 0 || cfg.n_expert <= 0 || cfg.n_embd <= 0 || cfg.n_ff <= 0 || cfg.blob_bytes <= 0) {
    err = "expert tier: bad geometry (layers/experts/embd/ff/blob_bytes must be > 0)";
    return false;
  }
  if (cfg.vram_slots < 0 || cfg.vram_slots > cfg.n_layers * cfg.n_expert) {
    err = "expert tier: vram_slots out of range";
    return false;
  }
  if (cfg.n_embd % 32 != 0 || cfg.n_ff % 32 != 0) {
    err = "expert tier: n_embd and n_ff must be multiples of 32 (Q8_1 activation)";
    return false;
  }
  cfg_ = cfg;
  vram_slots_ = cfg.vram_slots;
  residency_.assign((std::size_t)(cfg.n_layers * cfg.n_expert), kNotResident);
  layer_next_.assign((std::size_t)cfg.n_layers, 0);
  usage_.assign((std::size_t)(cfg.n_layers * cfg.n_expert), 0.0);
  next_free_ = 0;
  admitted_ = 0;
  fills_ = 0;
  stream_misses_ = 0;

  // VRAM slot arena (checked against the card: a short allocation would report
  // hits for slots it does not hold -- Maya ExpertCache::open).
  if (vram_slots_ > 0) {
    const std::size_t bytes = (std::size_t)vram_slots_ * (std::size_t)cfg.blob_bytes;
    if (cudaMalloc(reinterpret_cast<void**>(&vram_base_), bytes) != cudaSuccess) {
      err = "expert tier: cudaMalloc for the VRAM slots failed (" + std::to_string(bytes) + " bytes)";
      return false;
    }
  }

  // decode scratch
  const std::size_t q8_embd = k::q8_1_scratch_bytes((int)cfg.n_embd, 1);
  const std::size_t q8_ff = k::q8_1_scratch_bytes((int)cfg.n_ff, 1);
  bool ok = cudaMalloc(reinterpret_cast<void**>(&d_stage_), (std::size_t)cfg.blob_bytes) == cudaSuccess;
  ok = ok && cudaMalloc(&d_xq8_embd_, q8_embd) == cudaSuccess;
  ok = ok && cudaMalloc(&d_hq8_, q8_ff) == cudaSuccess;
  ok = ok && cudaMalloc(reinterpret_cast<void**>(&d_g_), (std::size_t)cfg.n_ff * sizeof(float)) == cudaSuccess;
  ok = ok && cudaMalloc(reinterpret_cast<void**>(&d_u_), (std::size_t)cfg.n_ff * sizeof(float)) == cudaSuccess;
  ok = ok && cudaMalloc(reinterpret_cast<void**>(&d_h_), (std::size_t)cfg.n_ff * sizeof(float)) == cudaSuccess;
  ok = ok && cudaMalloc(reinterpret_cast<void**>(&d_parts_),
                        (std::size_t)k_max_ * cfg.n_embd * sizeof(float)) == cudaSuccess;
  ok = ok && cudaMalloc(reinterpret_cast<void**>(&d_weights_), (std::size_t)k_max_ * sizeof(float)) == cudaSuccess;
  if (cfg.has_shared_expert)
    ok = ok && cudaMalloc(reinterpret_cast<void**>(&d_shared_), (std::size_t)cfg.n_embd * sizeof(float)) == cudaSuccess;
  if (!ok) {
    err = "expert tier: cudaMalloc for the decode scratch failed";
    close();
    return false;
  }
  opened_ = true;
  return true;
}

bool ExpertTier::set_host_arena(const std::uint8_t* base, std::int64_t bytes, std::string& err) {
  if (!opened_) {
    err = "expert tier: set_host_arena before open";
    return false;
  }
  if (!base || bytes <= 0) {
    err = "expert tier: null/empty host arena";
    return false;
  }
  const std::int64_t stride = cfg_.n_expert + (cfg_.has_shared_expert ? 1 : 0);
  const std::int64_t need = cfg_.n_layers * stride * cfg_.blob_bytes;
  if (bytes < need) {
    err = "expert tier: host arena smaller than the expert layout needs";
    return false;
  }
  host_base_ = base;
  host_bytes_ = bytes;
  // Best-effort page-lock + register for DMA; a failure just means the async
  // staging copy falls back to a pageable copy (slower, still correct).
  if (cudaHostRegister(const_cast<std::uint8_t*>(base), (std::size_t)bytes, cudaHostRegisterDefault) ==
      cudaSuccess)
    host_registered_ = true;
  else
    cudaGetLastError();  // clear the sticky error
  return true;
}

bool ExpertTier::warm_start(const std::vector<std::pair<std::int32_t, std::int32_t>>& ranked,
                            std::string& err) {
  if (!opened_ || !host_base_) {
    err = "expert tier: warm_start before open/set_host_arena";
    return false;
  }
  prior_ = ranked;
  std::fill(residency_.begin(), residency_.end(), kNotResident);
  std::fill(layer_next_.begin(), layer_next_.end(), 0);
  next_free_ = 0;
  admitted_ = 0;
  if (vram_slots_ == 0) return true;  // cold tier: everything streams from RAM
  for (const auto& [layer, expert] : ranked) {
    const std::int32_t slot = admit(layer, expert);
    if (slot == kNotResident) continue;  // that layer's slots are full; others may still have room
    if (!fill_slot_blocking(slot, layer, expert, err)) return false;
  }
  return true;
}

void ExpertTier::note_routing(std::int64_t layer, const std::int32_t* ids, std::int64_t k) {
  if (!opened_ || layer < 0 || layer >= cfg_.n_layers || usage_.empty()) return;
  double* row = usage_.data() + layer * cfg_.n_expert;
  for (std::int64_t e = 0; e < cfg_.n_expert; ++e) row[e] *= kDecay;
  for (std::int64_t i = 0; i < k; ++i) {
    const std::int32_t e = ids[i];
    if (e >= 0 && e < cfg_.n_expert) row[e] += 1.0;
  }
}

std::int64_t ExpertTier::rebalance(float margin, void* stream, std::string& err) {
  if (!opened_ || !host_base_ || vram_slots_ == 0) return 0;
  std::int64_t swaps = 0;
  const auto s = static_cast<cudaStream_t>(stream);
  for (std::int64_t layer = 0; layer < cfg_.n_layers; ++layer) {
    const double* row = usage_.data() + layer * cfg_.n_expert;
    // Repeatedly swap the hottest non-resident expert into the coldest resident
    // slot while the gain clears `margin` (bounded by n_expert iterations).
    for (std::int64_t iter = 0; iter < cfg_.n_expert; ++iter) {
      std::int64_t cold_e = -1, hot_e = -1;
      double cold_v = 0.0, hot_v = -1.0;
      for (std::int64_t e = 0; e < cfg_.n_expert; ++e) {
        const bool resident = residency_[(std::size_t)(layer * cfg_.n_expert + e)] != kNotResident;
        if (resident) {
          if (cold_e < 0 || row[e] < cold_v) { cold_v = row[e]; cold_e = e; }
        } else {
          if (row[e] > hot_v) { hot_v = row[e]; hot_e = e; }
        }
      }
      if (cold_e < 0 || hot_e < 0 || hot_v <= cold_v + (double)margin) break;
      const std::int32_t slot = residency_[(std::size_t)(layer * cfg_.n_expert + cold_e)];
      residency_[(std::size_t)(layer * cfg_.n_expert + cold_e)] = kNotResident;
      residency_[(std::size_t)(layer * cfg_.n_expert + hot_e)] = slot;
      const std::uint8_t* src = host_base_ + blob_index(layer, hot_e) * cfg_.blob_bytes;
      std::uint8_t* dst = vram_base_ + (std::int64_t)slot * cfg_.blob_bytes;
      if (cudaMemcpyAsync(dst, src, (std::size_t)cfg_.blob_bytes, cudaMemcpyHostToDevice, s) != cudaSuccess) {
        err = "expert tier: rebalance refill failed";
        return -1;
      }
      ++fills_;
      ++swaps;
    }
  }
  return swaps;
}

bool ExpertTier::compute(std::int64_t layer, const std::int32_t* ids, const float* weights, const float* x,
                         float* out, std::int64_t k, void* stream, std::string& err) {
  if (!opened_ || !host_base_) {
    err = "expert tier: compute before open/set_host_arena";
    return false;
  }
  if (layer < 0 || layer >= cfg_.n_layers || k < 1 || k > k_max_) {
    err = "expert tier: compute bad layer/k";
    return false;
  }
  const auto s = static_cast<cudaStream_t>(stream);
  if (cudaMemcpyAsync(d_weights_, weights, (std::size_t)k * sizeof(float), cudaMemcpyHostToDevice, s) !=
      cudaSuccess) {
    err = "expert tier: weight upload failed";
    return false;
  }

  // x is shared by every routed expert's gate and up: prepare it once.
  const bool x_needs_q8 = k::is_quantized(cfg_.gate) || k::is_quantized(cfg_.up);
  if (x_needs_q8) k::quantize_q8_1(x, d_xq8_embd_, (int)cfg_.n_embd, 1, stream);

  auto run_ffn = [&](const std::uint8_t* blob_dev, float* parts) {
    const void* gate_w = blob_dev + cfg_.gate_off;
    const void* up_w = blob_dev + cfg_.up_off;
    const void* down_w = blob_dev + cfg_.down_off;
    // gate, up: n_in = n_embd, n_out = n_ff (quantized path only in this lean
    // cut; f32/bf16 experts are a documented box extension).
    k::mmvq(cfg_.gate, gate_w, d_xq8_embd_, d_g_, (int)cfg_.n_embd, (int)cfg_.n_ff, 1, stream);
    k::mmvq(cfg_.up, up_w, d_xq8_embd_, d_u_, (int)cfg_.n_embd, (int)cfg_.n_ff, 1, stream);
    switch (cfg_.act) {
      case Act::swiglu_clamped:
        k::swiglu_clamped(d_g_, d_u_, d_h_, cfg_.n_ff, cfg_.act_limit, stream);
        break;
      case Act::geglu:
        k::geglu(d_g_, d_u_, d_h_, cfg_.n_ff, stream);
        break;
      case Act::swiglu:
      default:
        k::swiglu(d_g_, d_u_, d_h_, cfg_.n_ff, stream);
        break;
    }
    k::quantize_q8_1(d_h_, d_hq8_, (int)cfg_.n_ff, 1, stream);
    // down: n_in = n_ff, n_out = n_embd.
    k::mmvq(cfg_.down, down_w, d_hq8_, parts, (int)cfg_.n_ff, (int)cfg_.n_embd, 1, stream);
  };

  for (std::int64_t i = 0; i < k; ++i) {
    const std::int32_t e = ids[i];
    if (e < 0 || e >= cfg_.n_expert) {
      err = "expert tier: routed id out of range";
      return false;
    }
    const std::int32_t slot = slot_of(layer, e);
    const std::uint8_t* blob_dev;
    if (slot != kNotResident) {
      blob_dev = vram_base_ + (std::int64_t)slot * cfg_.blob_bytes;  // resident: read VRAM
    } else {
      // Non-resident: stream from the host arena into the reused staging slot.
      // Ordered on `stream`, so the FFN below reads it before the next expert
      // overwrites it.
      const std::uint8_t* src = host_base_ + blob_index(layer, e) * cfg_.blob_bytes;
      if (cudaMemcpyAsync(d_stage_, src, (std::size_t)cfg_.blob_bytes, cudaMemcpyHostToDevice, s) !=
          cudaSuccess) {
        err = "expert tier: non-resident stream failed";
        return false;
      }
      blob_dev = d_stage_;
      ++stream_misses_;
    }
    run_ffn(blob_dev, d_parts_ + i * cfg_.n_embd);
  }

  float* shared = nullptr;
  if (cfg_.has_shared_expert) {
    const std::uint8_t* src = host_base_ + blob_index(layer, cfg_.n_expert) * cfg_.blob_bytes;
    if (cudaMemcpyAsync(d_stage_, src, (std::size_t)cfg_.blob_bytes, cudaMemcpyHostToDevice, s) != cudaSuccess) {
      err = "expert tier: shared expert stream failed";
      return false;
    }
    run_ffn(d_stage_, d_shared_);
    shared = d_shared_;
  }

  k::moe_combine(d_parts_, d_weights_, shared, out, cfg_.n_embd, k, stream);
  return true;
}

bool ExpertTier::save_profile(const std::string& path, std::string& err) const {
  std::vector<std::uint8_t> resident((std::size_t)(cfg_.n_layers * cfg_.n_expert), 0);
  for (std::size_t i = 0; i < residency_.size(); ++i) resident[i] = residency_[i] != kNotResident ? 1 : 0;
  const auto ranked = rank_learned_profile(cfg_.n_layers, cfg_.n_expert, resident, usage_, prior_);
  return write_expert_profile(path, cfg_.n_layers, cfg_.n_expert, ranked, err);
}

// ================================ registered per-layer path ================================

bool ExpertTier::reg_ensure(std::int64_t n_embd, std::int64_t n_ff, std::int64_t stage_bytes,
                            std::string& err) {
  bool ok = true;
  ok = ok && dev_grow(reinterpret_cast<void**>(&rg_stage_), rg_stage_cap_, stage_bytes);
  ok = ok && dev_grow(&rg_xq8_, rg_xq8_cap_, (std::int64_t)k::q8_1_scratch_bytes((int)n_embd, 1));
  ok = ok && dev_grow(&rg_hq8_, rg_hq8_cap_, (std::int64_t)k::q8_1_scratch_bytes((int)n_ff, 1));
  ok = ok && dev_grow(reinterpret_cast<void**>(&rg_g_), rg_g_cap_, n_ff * (std::int64_t)sizeof(float));
  ok = ok && dev_grow(reinterpret_cast<void**>(&rg_u_), rg_u_cap_, n_ff * (std::int64_t)sizeof(float));
  ok = ok && dev_grow(reinterpret_cast<void**>(&rg_h_), rg_h_cap_, n_ff * (std::int64_t)sizeof(float));
  ok = ok && dev_grow(reinterpret_cast<void**>(&rg_parts_), rg_parts_cap_,
                      k_max_ * n_embd * (std::int64_t)sizeof(float));
  ok = ok && dev_grow(reinterpret_cast<void**>(&rg_bf16x_), rg_bf16x_cap_,
                      std::max(n_embd, n_ff) * (std::int64_t)sizeof(std::uint16_t));
  if (!ok) {
    err = "expert tier: registered-path scratch alloc failed";
    return false;
  }
  reg_n_embd_ = std::max(reg_n_embd_, n_embd);
  reg_n_ff_ = std::max(reg_n_ff_, n_ff);
  rg_ids_host_.resize((std::size_t)k_max_);
  reg_scratch_ = true;
  return true;
}

bool ExpertTier::register_layer(int layer, const WSlot& gate, const WSlot& up, const WSlot& down,
                                int n_expert, int n_ff_exp, float swiglu_limit, std::string& err, Act act,
                                std::int64_t gate_stride, std::int64_t up_stride, std::int64_t down_stride) {
  if (layer < 0) {
    err = "expert tier: register_layer bad layer";
    return false;
  }
  if (n_expert <= 0 || n_ff_exp <= 0 || gate.n_in <= 0) {
    err = "expert tier: register_layer bad geometry (n_expert/n_ff_exp/gate.n_in must be > 0)";
    return false;
  }
  if (!gate.q || !up.q || !down.q) {
    err = "expert tier: register_layer null expert tensor (missing gate/up/down_exps)";
    return false;
  }
  const std::int64_t n_embd = gate.n_in;
  if (n_embd % 32 != 0 || n_ff_exp % 32 != 0) {
    err = "expert tier: register_layer n_embd and n_ff_exp must be multiples of 32 (Q8_1 activation)";
    return false;
  }
  RegLayer rl;
  rl.gate = gate;
  rl.up = up;
  rl.down = down;
  rl.n_expert = n_expert;
  rl.n_embd = n_embd;
  rl.n_ff = n_ff_exp;
  rl.limit = swiglu_limit;
  rl.act = act;
  // One matrix's bytes (what we STREAM per expert): gate/up are [n_ff, n_embd], down is [n_embd, n_ff].
  rl.gate_bytes = (std::int64_t)k::weight_bytes((k::Quant)gate.type, (int)n_embd, (int)n_ff_exp);
  rl.up_bytes = (std::int64_t)k::weight_bytes((k::Quant)up.type, (int)n_embd, (int)n_ff_exp);
  rl.down_bytes = (std::int64_t)k::weight_bytes((k::Quant)down.type, (int)n_ff_exp, (int)n_embd);
  // Per-expert ADVANCE: an explicit stride (fused gate|up) or, by default, the matrix's own size.
  rl.gate_stride = gate_stride != 0 ? gate_stride : rl.gate_bytes;
  rl.up_stride = up_stride != 0 ? up_stride : rl.up_bytes;
  rl.down_stride = down_stride != 0 ? down_stride : rl.down_bytes;
  rl.valid = true;
  const std::int64_t stage = std::max(rl.gate_bytes, std::max(rl.up_bytes, rl.down_bytes));
  if (!reg_ensure(n_embd, n_ff_exp, stage, err)) return false;
  if ((std::int64_t)reg_.size() <= layer) reg_.resize((std::size_t)layer + 1);
  reg_[(std::size_t)layer] = rl;
  return true;
}

bool ExpertTier::reg_run_token(const RegLayer& rl, int layer, const std::int32_t* ids_host,
                               const float* weights_dev, int k, const float* x, const void* xq, float* out,
                               float limit, void* stream, std::string& err) {
  if (k > 64) { err = "expert tier: reg_run_token k too large"; return false; }
  auto is_q = [](int t) { return t != 0 && t != 30; };  // not F32, not BF16
  const auto caller = static_cast<cudaStream_t>(stream);
  const auto cp = static_cast<cudaStream_t>(rg_copy_);
  // Run the expert sub-block on the compute stream `ce` (not the caller's default stream) so the copy
  // stream `cp` can overlap it. `cs_v` is `ce` as a void* for the u::k kernels. Sync caller->ce at entry.
  void* cs_v = rg_compute_ != nullptr ? rg_compute_ : stream;
  const auto ce = static_cast<cudaStream_t>(cs_v);
  const bool own = (ce != caller) && cp != nullptr;
  if (own) {
    cudaEventRecord(static_cast<cudaEvent_t>(rg_in_ev_), caller);  // x (and everything prior) ready
    cudaStreamWaitEvent(ce, static_cast<cudaEvent_t>(rg_in_ev_), 0);
    cudaStreamWaitEvent(cp, static_cast<cudaEvent_t>(rg_in_ev_), 0);
  }

  // x is shared by every expert's gate and up: quantize it once if either is quantized.
  const void* xq8 = xq;
  if ((is_q(rl.gate.type) || is_q(rl.up.type)) && xq8 == nullptr) {
    k::quantize_q8_1(x, rg_xq8_, (int)rl.n_embd, 1, cs_v);
    xq8 = rg_xq8_;
  }

  const int D = rg_ring_depth_ >= 1 ? rg_ring_depth_ : 1;

  // One expert matrix, read straight from a device base (a resident VRAM slot OR a ring buffer the copy
  // stream has filled); dispatch on quant. No streaming here -- a streamed expert's three matrices are
  // prefetched as a group into a ring buffer (see below), so the copies overlap earlier experts' compute.
  auto gemv = [&](const WSlot& w, const std::uint8_t* wdev, const void* act_q8, const float* act_f32,
                  float* y, std::int64_t n_in, std::int64_t n_out) {
    if (w.type == 0) {  // F32
      k::f32_gemv(act_f32, reinterpret_cast<const float*>(wdev), y, n_in, n_out, cs_v);
    } else if (w.type == 30) {  // BF16
      k::f32_to_bf16(act_f32, rg_bf16x_, n_in, cs_v);
      k::bf16_gemv(rg_bf16x_, reinterpret_cast<const std::uint16_t*>(wdev), y, n_in, n_out, cs_v);
    } else {  // quantized via MMVQ
      k::mmvq((k::Quant)w.type, wdev, act_q8, y, (int)n_in, (int)n_out, 1, cs_v);
    }
  };

  // Prefetch streamed expert `e`'s gate|up|down (resident-slot layout) into ring buffer `r` on the copy
  // stream. Waits for the previous occupant of `r` to be consumed (rg_free_ev_[r]; a no-op the first
  // time), then records rg_ready_ev_[r] so the compute stream can gate its gemvs on the copy.
  auto prefetch = [&](std::int32_t e, int r) -> bool {
    std::uint8_t* d = rg_ring_[(std::size_t)r];
    const std::uint8_t* g = static_cast<const std::uint8_t*>(rl.gate.q) + (std::int64_t)e * rl.gate_stride;
    const std::uint8_t* u = static_cast<const std::uint8_t*>(rl.up.q) + (std::int64_t)e * rl.up_stride;
    const std::uint8_t* dn = static_cast<const std::uint8_t*>(rl.down.q) + (std::int64_t)e * rl.down_stride;
    cudaStreamWaitEvent(cp, static_cast<cudaEvent_t>(rg_free_ev_[(std::size_t)r]), 0);
    if (cudaMemcpyAsync(d, g, (std::size_t)rl.gate_bytes, cudaMemcpyHostToDevice, cp) != cudaSuccess ||
        cudaMemcpyAsync(d + rl.gate_bytes, u, (std::size_t)rl.up_bytes, cudaMemcpyHostToDevice, cp) != cudaSuccess ||
        cudaMemcpyAsync(d + rl.gate_bytes + rl.up_bytes, dn, (std::size_t)rl.down_bytes, cudaMemcpyHostToDevice,
                        cp) != cudaSuccess) {
      err = "expert tier: registered expert prefetch failed";
      return false;
    }
    cudaEventRecord(static_cast<cudaEvent_t>(rg_ready_ev_[(std::size_t)r]), cp);
    return true;
  };

  // Plan the k routed experts into: resident (read from its VRAM slot), CPU-lane (host compute, C3), or
  // GPU-streamed (prefetch ring). Residency is per-expert, so an expert is wholly resident or wholly
  // streamed; a fraction rg_cpu_frac_ of the STREAMED experts are computed on the CPU in parallel.
  const bool cpu_ok = rg_cpu_frac_ > 0.0f && rg_xhost_ != nullptr &&
                      cpu_expert_supported(rl.gate.type, rl.up.type, rl.down.type) &&
                      rl.n_embd % 256 == 0 && rl.n_ff % 256 == 0;
  const std::uint8_t* base_res[64];
  std::int32_t kind[64];   // 0 resident, 1 CPU, 2 GPU-streamed
  std::int32_t gord[64];   // GPU-ring ordinal (kind 2), else -1
  std::int32_t e_of[64];
  int M = 0;
  for (int i = 0; i < k; ++i) {
    const std::int32_t e = ids_host[i];
    if (e < 0 || e >= rl.n_expert) { err = "expert tier: routed id out of range"; return false; }
    e_of[i] = e;
    gord[i] = -1;
    const std::int32_t slot = reg_slot_of(layer, e);
    if (slot != kNotResident) {
      base_res[i] = reg_slot_ptr(layer, slot);
      kind[i] = 0;
      ++reg_resident_hits_;
    } else {
      base_res[i] = nullptr;
      kind[i] = -1;  // streamed (CPU/GPU decided next)
      ++M;
      ++stream_misses_;
    }
  }
  const int k_cpu = cpu_ok ? std::min(M, (int)std::lround((double)rg_cpu_frac_ * M)) : 0;
  std::int32_t gpu_streamed_e[64];
  int M_gpu = 0, cpu_pos[64], n_cpu = 0, seen = 0;
  for (int i = 0; i < k; ++i) {
    if (kind[i] != -1) continue;  // resident
    if (seen < k_cpu) { kind[i] = 1; cpu_pos[n_cpu++] = i; }
    else { kind[i] = 2; gord[i] = M_gpu; gpu_streamed_e[M_gpu++] = e_of[i]; }
    ++seen;
  }

  // Launch the CPU lane first (host threads) so it overlaps the GPU work below.
  std::vector<std::thread> cpu_threads;
  if (n_cpu > 0) {
    if (cudaMemcpy(rg_xhost_, x, (std::size_t)rl.n_embd * sizeof(float), cudaMemcpyDeviceToHost) !=
        cudaSuccess) {
      err = "expert tier: CPU-lane x D2H failed";
      return false;
    }
    const int W = rg_cpu_workers_ > 0 ? std::min(rg_cpu_workers_, n_cpu) : n_cpu;
    const int act_i = rl.act == Act::geglu ? 0 : (rl.act == Act::swiglu ? 1 : 2);
    for (int t = 0; t < W; ++t) {
      cpu_threads.emplace_back([&, t, W, act_i]() {
        for (int jj = t; jj < n_cpu; jj += W) {
          const int i = cpu_pos[jj];
          const std::int32_t e = e_of[i];
          const void* g = static_cast<const std::uint8_t*>(rl.gate.q) + (std::int64_t)e * rl.gate_stride;
          const void* u = static_cast<const std::uint8_t*>(rl.up.q) + (std::int64_t)e * rl.up_stride;
          const void* dn = static_cast<const std::uint8_t*>(rl.down.q) + (std::int64_t)e * rl.down_stride;
          cpu_expert_ffn(rl.gate.type, g, rl.up.type, u, rl.down.type, dn, (int)rl.n_embd, (int)rl.n_ff,
                         act_i, limit, rg_xhost_, rg_cpu_parts_ + (std::int64_t)i * rl.n_embd,
                         rg_cpu_scratch_ + (std::int64_t)t * 2 * rl.n_ff);
        }
      });
    }
  }

  // GPU path: prefetch the first min(D, M_gpu) gpu-streamed experts, then compute resident + gpu-streamed.
  for (int n = 0; n < M_gpu && n < D; ++n)
    if (!prefetch(gpu_streamed_e[n], n % D)) return false;
  for (int i = 0; i < k; ++i) {
    if (kind[i] == 1) continue;  // CPU lane fills this part
    const std::uint8_t* base;
    int r = -1;
    if (kind[i] == 0) {
      base = base_res[i];  // resident slot
    } else {
      r = gord[i] % D;
      cudaStreamWaitEvent(ce, static_cast<cudaEvent_t>(rg_ready_ev_[(std::size_t)r]), 0);  // copy done
      base = rg_ring_[(std::size_t)r];
    }
    const std::uint8_t* g = base;
    const std::uint8_t* u = base + rl.gate_bytes;
    const std::uint8_t* dn = base + rl.gate_bytes + rl.up_bytes;
    gemv(rl.gate, g, xq8, x, rg_g_, rl.n_embd, rl.n_ff);
    gemv(rl.up, u, xq8, x, rg_u_, rl.n_embd, rl.n_ff);
    switch (rl.act) {  // gated activation: gemma4 geglu, GLM/ds4/raon1 clamped SwiGLU, qwen plain SwiGLU
      case Act::geglu: k::geglu(rg_g_, rg_u_, rg_h_, rl.n_ff, cs_v); break;
      case Act::swiglu: k::swiglu(rg_g_, rg_u_, rg_h_, rl.n_ff, cs_v); break;
      case Act::swiglu_clamped:
      default: k::swiglu_clamped(rg_g_, rg_u_, rg_h_, rl.n_ff, limit, cs_v); break;
    }
    if (is_q(rl.down.type)) k::quantize_q8_1(rg_h_, rg_hq8_, (int)rl.n_ff, 1, cs_v);
    gemv(rl.down, dn, rg_hq8_, rg_h_, rg_parts_ + (std::int64_t)i * rl.n_embd, rl.n_ff, rl.n_embd);
    if (kind[i] == 2) {
      cudaEventRecord(static_cast<cudaEvent_t>(rg_free_ev_[(std::size_t)r]), ce);  // ring[r] consumed
      const int next = gord[i] + D;
      if (next < M_gpu && !prefetch(gpu_streamed_e[next], r)) return false;  // refill this slot
    }
  }

  // Join the CPU lane and H2D its parts into rg_parts_ on `ce`, before the combine below.
  for (auto& th : cpu_threads) th.join();
  for (int jj = 0; jj < n_cpu; ++jj) {
    const int i = cpu_pos[jj];
    cudaMemcpyAsync(rg_parts_ + (std::int64_t)i * rl.n_embd, rg_cpu_parts_ + (std::int64_t)i * rl.n_embd,
                    (std::size_t)rl.n_embd * sizeof(float), cudaMemcpyHostToDevice, ce);
  }

  // Combine the k routed parts by their (device) weights; no shared expert here.
  k::moe_combine(rg_parts_, weights_dev, nullptr, out, rl.n_embd, k, cs_v);
  // out is produced on `ce`; make the caller's stream wait for it before it reads out.
  if (own) {
    cudaEventRecord(static_cast<cudaEvent_t>(rg_out_ev_), ce);
    cudaStreamWaitEvent(caller, static_cast<cudaEvent_t>(rg_out_ev_), 0);
  }
  return true;
}

bool ExpertTier::run_moe(int layer, const int* ids, const float* weights, int k, const float* x,
                         const void* xq, float* out, void* stream, std::string& err) {
  if (layer < 0 || (std::int64_t)reg_.size() <= layer || !reg_[(std::size_t)layer].valid) {
    err = "expert tier: run_moe on an unregistered layer";
    return false;
  }
  if (k < 1 || k > k_max_) {
    err = "expert tier: run_moe bad k";
    return false;
  }
  const RegLayer& rl = reg_[(std::size_t)layer];
  const auto s = static_cast<cudaStream_t>(stream);
  // The router produced `ids` on the device; bring this token's k ids to the host
  // (one sync) to index the stacked tensors, then run the FFN.
  if (cudaMemcpyAsync(rg_ids_host_.data(), ids, (std::size_t)k * sizeof(std::int32_t),
                      cudaMemcpyDeviceToHost, s) != cudaSuccess) {
    err = "expert tier: run_moe id copy failed";
    return false;
  }
  cudaStreamSynchronize(s);
  // Track this layer's routing heat so reg_rebalance can converge the resident set onto the hot experts.
  // A no-op until reg_finalize has sized reg_usage_ (i.e. when a VRAM residency budget was given).
  reg_note_routing(layer, rg_ids_host_.data(), k);
  return reg_run_token(rl, layer, rg_ids_host_.data(), weights, k, x, xq, out, rl.limit, stream, err);
}

bool ExpertTier::run_window(int layer, const float* x, const int* ids, const float* weights, int T, int k,
                            int n_embd, int n_ff, float swiglu_limit, float* out, void* stream,
                            std::string& err) {
  if (layer < 0 || (std::int64_t)reg_.size() <= layer || !reg_[(std::size_t)layer].valid) {
    err = "expert tier: run_window on an unregistered layer";
    return false;
  }
  if (T < 1 || k < 1 || k > k_max_) {
    err = "expert tier: run_window bad T/k";
    return false;
  }
  const RegLayer& rl = reg_[(std::size_t)layer];
  if (n_embd != rl.n_embd || n_ff != rl.n_ff) {
    err = "expert tier: run_window geometry mismatch vs registration";
    return false;
  }
  const auto s = static_cast<cudaStream_t>(stream);
  // Per-token: copy the row's k routed ids to the host, then run its FFN. (The box
  // prefill dispatch will batch this; the lean cut runs the window one row at a time.)
  for (int t = 0; t < T; ++t) {
    if (cudaMemcpyAsync(rg_ids_host_.data(), ids + (std::int64_t)t * k,
                        (std::size_t)k * sizeof(std::int32_t), cudaMemcpyDeviceToHost, s) != cudaSuccess) {
      err = "expert tier: run_window id copy failed";
      return false;
    }
    cudaStreamSynchronize(s);
    if (!reg_run_token(rl, layer, rg_ids_host_.data(), weights + (std::int64_t)t * k, k,
                       x + (std::int64_t)t * n_embd, nullptr, out + (std::int64_t)t * n_embd, swiglu_limit,
                       stream, err))
      return false;
  }
  return true;
}

// ---- registered-path VRAM residency ----

std::int32_t ExpertTier::reg_slot_of(int layer, std::int32_t e) const {
  if (reg_vram_slots_ == 0 || layer < 0 || layer >= reg_n_layers_ || e < 0 || e >= reg_n_expert_)
    return kNotResident;
  return reg_residency_[(std::size_t)((std::int64_t)layer * reg_n_expert_ + e)];
}

std::int64_t ExpertTier::reg_pin_host_arena(std::int64_t budget_bytes) {
  // Copy `n` bytes with a few threads: the source is the mapped GGUF (page cache or SSD), a single thread
  // copies well under the RAM bandwidth.
  auto parallel_copy = [](void* dst, const void* src, std::size_t n) {
    constexpr int kThreadsCopy = 8;
    const std::size_t per = (n + kThreadsCopy - 1) / kThreadsCopy;
    std::vector<std::thread> th;
    for (int t = 0; t < kThreadsCopy; ++t) {
      const std::size_t lo = (std::size_t)t * per;
      if (lo >= n) break;
      const std::size_t len = std::min(per, n - lo);
      th.emplace_back([=]() { std::memcpy(static_cast<std::uint8_t*>(dst) + lo, static_cast<const std::uint8_t*>(src) + lo, len); });
    }
    for (auto& x : th) x.join();
  };
  std::int64_t pinned = 0;
  reg_dev_.assign(reg_.size(), RegDev{});
  for (std::size_t l = 0; l < reg_.size(); ++l) {
    RegLayer& rl = reg_[l];
    if (!rl.valid) continue;
    WSlot* ws[3] = {&rl.gate, &rl.up, &rl.down};
    const void** dev[3] = {&reg_dev_[l].gate, &reg_dev_[l].up, &reg_dev_[l].down};
    const std::int64_t stride[3] = {rl.gate_stride, rl.up_stride, rl.down_stride};
    const std::int64_t bytes[3] = {rl.gate_bytes, rl.up_bytes, rl.down_bytes};
    for (int m = 0; m < 3; ++m) {
      if (stride[m] != bytes[m]) continue;  // fused/strided (e.g. gate|up in one tensor): keep the mmap
      const std::int64_t span = rl.n_expert * bytes[m];
      if (pinned + span > budget_bytes) return pinned;
      void* p = nullptr;
      // Mapped: the device-decided MoE path reads a non-resident expert straight from here (zero-copy).
      // Write-combined: GPU reads across PCIe then do not snoop the CPU caches (the CPU only writes it here) --
      // unless the CPU expert lane reads it (reg_set_host_cached): uncached WC reads would starve it.
      const unsigned flags = cudaHostAllocMapped | (rg_host_cached_ ? 0u : (unsigned)cudaHostAllocWriteCombined);
      if (cudaHostAlloc(&p, (std::size_t)span, flags) != cudaSuccess ||
          p == nullptr) {
        (void)cudaGetLastError();  // clear it: the remaining tensors simply stay on the mmap
        return pinned;
      }
      parallel_copy(p, ws[m]->q, (std::size_t)span);
      rg_pinned_.push_back(p);
      ws[m]->q = p;
      if (ws[m]->type == 0) ws[m]->f32 = static_cast<const float*>(p);
      void* dp = nullptr;
      if (cudaHostGetDevicePointer(&dp, p, 0) == cudaSuccess) *dev[m] = dp;
      else (void)cudaGetLastError();  // not mapped: this layer just stays on the host-planned path
      pinned += span;
    }
  }
  return pinned;
}

bool ExpertTier::reg_publish_device_residency(std::string& err) {
  if (reg_residency_.empty()) return true;
  const std::size_t n = reg_residency_.size();
  if (d_res_ == nullptr && cudaMalloc(reinterpret_cast<void**>(&d_res_), n * sizeof(int)) != cudaSuccess) {
    d_res_ = nullptr;
    err = "expert tier: device residency table alloc failed";
    return false;
  }
  std::vector<int> local;
  reg_local_residency(local);
  if (cudaMemcpy(d_res_, local.data(), n * sizeof(int), cudaMemcpyHostToDevice) != cudaSuccess) {
    err = "expert tier: device residency table upload failed";
    return false;
  }
  if (gr_tail_dev_ == nullptr) {  // the tail-skip rank the device path reads (0 = off until set)
    if (cudaMalloc(reinterpret_cast<void**>(&gr_tail_dev_), sizeof(int)) != cudaSuccess) {
      gr_tail_dev_ = nullptr;
      err = "expert tier: tail-skip rank alloc failed";
      return false;
    }
    return reg_set_tail_skip(0, err);
  }
  return true;
}

bool ExpertTier::reg_set_tail_skip(int rank, std::string& err) {
  if (gr_tail_dev_ == nullptr) {
    err = "expert tier: reg_set_tail_skip before reg_publish_device_residency";
    return false;
  }
  if (cudaMemcpy(gr_tail_dev_, &rank, sizeof(int), cudaMemcpyHostToDevice) != cudaSuccess) {
    err = "expert tier: tail-skip rank upload failed";
    return false;
  }
  gr_tail_rank_ = rank;
  return true;
}

std::uint8_t* ExpertTier::reg_slot_ptr(int layer, std::int32_t slot) const {
  const std::size_t l = (std::size_t)layer;
  return reg_vram_ + reg_lbase_[l] + ((std::int64_t)slot - reg_lfirst_[l]) * reg_lslot_bytes_[l];
}

void ExpertTier::reg_local_residency(std::vector<int>& out) const {
  out.assign(reg_residency_.size(), -1);
  for (std::int64_t l = 0; l < reg_n_layers_; ++l) {
    const std::int64_t first = (std::size_t)l < reg_lfirst_.size() ? reg_lfirst_[(std::size_t)l] : 0;
    for (std::int64_t e = 0; e < reg_n_expert_; ++e) {
      const std::size_t i = (std::size_t)(l * reg_n_expert_ + e);
      if (reg_residency_[i] != kNotResident) out[i] = (int)(reg_residency_[i] - first);
    }
  }
}

bool ExpertTier::reg_device_ready(int layer) const {
  if (d_res_ == nullptr || layer < 0 || (std::size_t)layer >= reg_.size() || (std::size_t)layer >= reg_dev_.size())
    return false;
  const RegLayer& rl = reg_[(std::size_t)layer];
  const RegDev& d = reg_dev_[(std::size_t)layer];
  return rl.valid && d.gate != nullptr && d.up != nullptr && d.down != nullptr && k::iq_supported(rl.gate.type) &&
         k::iq_supported(rl.up.type) && k::iq_supported(rl.down.type);
}

bool ExpertTier::gr_ensure(int k, std::int64_t n_embd, std::int64_t n_ff, std::string& err, int n_tok) {
  if (gr_g_ != nullptr && k <= gr_k_ && n_tok <= gr_ntok_ && n_embd <= gr_n_embd_ && n_ff <= gr_n_ff_) return true;
  k = std::max(k, (int)gr_k_);  // only ever grows (the window and the single-token step share it)
  n_tok = std::max(n_tok, (int)gr_ntok_);
  for (float* b : {gr_g_, gr_u_, gr_h_, gr_parts_})
    if (b) cudaFree(b);
  if (gr_xq8_) cudaFree(gr_xq8_);
  if (gr_hq8_) cudaFree(gr_hq8_);
  if (gr_eff_) cudaFree(gr_eff_);
  if (gr_stage_) cudaFree(gr_stage_);
  gr_g_ = gr_u_ = gr_h_ = gr_parts_ = nullptr;
  gr_xq8_ = gr_hq8_ = nullptr;
  gr_eff_ = nullptr;
  gr_stage_ = nullptr;
  const std::size_t kf = (std::size_t)k * (std::size_t)n_ff * sizeof(float);
  const std::size_t q81 = 36;  // one Q8_1 block (half2 ds + 32 int8) per 32 values
  // a staging slot holds one expert of the largest layer, 256-aligned so each matrix's copy stays 16-aligned
  gr_stage_stride_ = (reg_slot_bytes_ + 255) / 256 * 256;
  if (cudaMalloc(reinterpret_cast<void**>(&gr_g_), kf) != cudaSuccess ||
      cudaMalloc(reinterpret_cast<void**>(&gr_u_), kf) != cudaSuccess ||
      cudaMalloc(reinterpret_cast<void**>(&gr_h_), kf) != cudaSuccess ||
      cudaMalloc(reinterpret_cast<void**>(&gr_parts_), (std::size_t)k * (std::size_t)n_embd * sizeof(float)) !=
          cudaSuccess ||
      cudaMalloc(&gr_xq8_, (std::size_t)n_tok * (std::size_t)(n_embd / 32) * q81) != cudaSuccess ||
      cudaMalloc(&gr_hq8_, (std::size_t)k * (std::size_t)(n_ff / 32) * q81) != cudaSuccess ||
      cudaMalloc(reinterpret_cast<void**>(&gr_eff_), (std::size_t)k * sizeof(int)) != cudaSuccess ||
      (gr_stage_on_ && cudaMalloc(reinterpret_cast<void**>(&gr_stage_),
                                  (std::size_t)gr_stage_k_ * (std::size_t)gr_stage_stride_) != cudaSuccess)) {
    err = "expert tier: grouped MoE scratch alloc failed";
    return false;
  }
  gr_k_ = k;
  gr_ntok_ = n_tok;
  gr_n_embd_ = n_embd;
  gr_n_ff_ = n_ff;
  return true;
}

bool ExpertTier::reg_run_token_device(int layer, const int* ids_dev, const float* weights_dev, int k,
                                      const float* x, float* out, void* stream, std::string& err, int n_tok) {
  if (n_tok > 1 && gr_stage_on_) {  // staging holds one token's misses: the window runs it token by token
    for (int t = 0; t < n_tok; ++t)
      if (!reg_run_token_device(layer, ids_dev + (std::int64_t)t * k, weights_dev + (std::int64_t)t * k, k,
                                x + (std::int64_t)t * reg_[(std::size_t)layer].n_embd,
                                out + (std::int64_t)t * reg_[(std::size_t)layer].n_embd, stream, err, 1))
        return false;
    return true;
  }
  if (!reg_device_ready(layer)) {
    err = "expert tier: reg_run_token_device on a layer that is not device-ready";
    return false;
  }
  const RegLayer& rl = reg_[(std::size_t)layer];
  const RegDev& d = reg_dev_[(std::size_t)layer];
  const int kt = k * n_tok;  // the window's tokens' entries, token-major (T = 1: one token)
  if (gr_stage_k_ == 0) gr_stage_k_ = k;  // staging slots: one token's misses
  if (!gr_ensure(kt, rl.n_embd, rl.n_ff, err, n_tok)) return false;
  const int* res = d_res_ + (std::int64_t)layer * reg_n_expert_;
  const int ne = (int)rl.n_embd, nf = (int)rl.n_ff;
  // this layer's slot region and slot size (d_res_ holds layer-local slot indices)
  const bool pooled = reg_vram_ != nullptr && (std::size_t)layer < reg_lbase_.size();
  const std::uint8_t* vbase = pooled ? reg_vram_ + reg_lbase_[(std::size_t)layer] : nullptr;
  const long long sbytes = pooled ? (long long)reg_lslot_bytes_[(std::size_t)layer] : 0;
  // The CPU expert lane on this layer: the device plans resident / skipped / GPU-miss / CPU-miss and rings the
  // host, whose pool computes the CPU misses while the grouped launches below compute the rest.
  const bool lane = lane_pool_ != nullptr && !gr_stage_on_ && n_tok <= lane_tmax_ && k == lane_k_ &&
                    (std::size_t)layer < lane_ok_.size() && lane_ok_[(std::size_t)layer] != 0;
  auto* lane_dev8 = static_cast<std::uint8_t*>(d_lane_host_);
  if (lane) {
    if (std::find(lane_layers_.begin(), lane_layers_.end(), layer) == lane_layers_.end()) {
      lane_layers_.push_back(layer);
      std::sort(lane_layers_.begin(), lane_layers_.end());
    }
    k::expert_lane_plan(ids_dev, res, kt, k, gr_tail_dev_, lane_dev_, gr_eff_, lane_ncpu_dev_, stream);
    k::lane_doorbell(x, n_tok * ne, ids_dev, gr_eff_, kt, lane_dev_, lane_ncpu_dev_,
                     reinterpret_cast<float*>(lane_dev8 + lane_off_x_),
                     reinterpret_cast<int*>(lane_dev8 + lane_off_ids_) + (std::int64_t)layer * lane_slot_,
                     reinterpret_cast<int*>(lane_dev8 + lane_off_eff_) + (std::int64_t)layer * lane_slot_,
                     reinterpret_cast<unsigned*>(lane_dev8), (unsigned)layer + 1u, stream);
  } else {
    // Where each routed expert is read (cache slot / staged copy / skipped tail), decided on the device; the
    // misses are staged into VRAM here, by one wide copy kernel, unless staging is off (then zero-copy).
    k::expert_route_plan(ids_dev, res, kt, gr_tail_dev_, d.gate, rl.gate_stride, rl.gate_bytes, d.up, rl.up_stride,
                         rl.up_bytes, d.down, rl.down_stride, rl.down_bytes, gr_stage_, gr_stage_stride_, gr_eff_,
                         stream, k);
  }
  // a token's x is shared by its k experts' gate and up: one Q8_1 column a token (entries j / k)
  k::quantize_q8_1(x, gr_xq8_, ne, n_tok, stream);
  k::iq_mmvq_grouped(rl.gate.type, ids_dev, gr_eff_, vbase, sbytes, 0, gr_stage_, gr_stage_stride_, d.gate,
                     rl.gate_stride, gr_xq8_, ne / 32, gr_g_, nf, ne, nf, kt, stream, k);
  k::iq_mmvq_grouped(rl.up.type, ids_dev, gr_eff_, vbase, sbytes, rl.gate_bytes, gr_stage_, gr_stage_stride_,
                     d.up, rl.up_stride, gr_xq8_, ne / 32, gr_u_, nf, ne, nf, kt, stream, k);
  const std::int64_t kn = (std::int64_t)kt * nf;
  switch (rl.act) {  // the same gated activation as reg_run_token, over all k joins at once
    case Act::geglu: k::geglu(gr_g_, gr_u_, gr_h_, kn, stream); break;
    case Act::swiglu: k::swiglu(gr_g_, gr_u_, gr_h_, kn, stream); break;
    case Act::swiglu_clamped:
    default: k::swiglu_clamped(gr_g_, gr_u_, gr_h_, kn, rl.limit, stream); break;
  }
  k::quantize_q8_1(gr_h_, gr_hq8_, nf, kt, stream);  // kt columns, column j at block j*nf/32
  k::iq_mmvq_grouped(rl.down.type, ids_dev, gr_eff_, vbase, sbytes, rl.gate_bytes + rl.up_bytes, gr_stage_,
                     gr_stage_stride_, d.down, rl.down_stride, gr_hq8_, nf / 32, gr_parts_, ne, nf, ne, kt, stream, 1);
  if (lane)  // the CPU's rows over its entries' zero rows, once the host has served the layer
    k::lane_merge(gr_eff_, kt, lane_dev_, lane_ncpu_dev_, reinterpret_cast<const unsigned*>(lane_dev8 + 64),
                  (unsigned)layer + 1u, reinterpret_cast<const float*>(lane_dev8 + lane_off_ymiss_), gr_parts_, ne,
                  stream);
  if (n_tok == 1) k::moe_combine(gr_parts_, weights_dev, nullptr, out, ne, k, stream);
  else k::moe_combine_multi(gr_parts_, weights_dev, nullptr, out, ne, k, n_tok, stream);
  return true;
}

bool ExpertTier::reg_lane_enable(int t_max, int k, int workers, int pcie_num, std::string& err) {
  if (d_res_ == nullptr || gr_tail_dev_ == nullptr) {
    err = "expert tier: reg_lane_enable before reg_publish_device_residency";
    return false;
  }
  if (t_max < 1 || k < 1 || t_max * k > k::kLaneMaxEntries) {
    err = "expert tier: reg_lane_enable: t_max * k must be in [1, " + std::to_string(k::kLaneMaxEntries) + "]";
    return false;
  }
  const std::size_t nl = reg_.size();
  lane_L_.assign(nl, cpu::LaneLayer{});
  lane_ok_.assign(nl, 0);
  std::int64_t ne_max = 0;
  int n_ok = 0;
  for (std::size_t l = 0; l < nl; ++l) {
    const RegLayer& rl = reg_[l];
    if (!rl.valid) continue;
    cpu::LaneLayer& L = lane_L_[l];
    L.gate_type = rl.gate.type;
    L.up_type = rl.up.type;
    L.down_type = rl.down.type;
    L.gate = static_cast<const std::uint8_t*>(rl.gate.q);
    L.up = static_cast<const std::uint8_t*>(rl.up.q);
    L.down = static_cast<const std::uint8_t*>(rl.down.q);
    L.gate_stride = rl.gate_stride;
    L.up_stride = rl.up_stride;
    L.down_stride = rl.down_stride;
    L.n_embd = (int)rl.n_embd;
    L.n_ff = (int)rl.n_ff;
    if (L.usable() && rl.act == Act::swiglu && reg_device_ready((int)l)) {
      lane_ok_[l] = 1;
      ++n_ok;
      ne_max = std::max(ne_max, rl.n_embd);
    }
  }
  if (n_ok == 0) {
    err = "expert tier: reg_lane_enable: no layer's expert quants have a CPU dot";
    return false;
  }
  lane_tmax_ = t_max;
  lane_k_ = k;
  lane_slot_ = t_max * k;
  // one mapped allocation: seq @0, flag @64, then x, ids, eff and ymiss, each 64-aligned
  auto up64 = [](std::int64_t v) { return (v + 63) / 64 * 64; };
  lane_off_x_ = 128;
  lane_off_ids_ = up64(lane_off_x_ + (std::int64_t)t_max * ne_max * (std::int64_t)sizeof(float));
  lane_off_eff_ = up64(lane_off_ids_ + (std::int64_t)nl * lane_slot_ * (std::int64_t)sizeof(int));
  lane_off_ymiss_ = up64(lane_off_eff_ + (std::int64_t)nl * lane_slot_ * (std::int64_t)sizeof(int));
  const std::int64_t bytes = up64(lane_off_ymiss_ + (std::int64_t)lane_slot_ * ne_max * (std::int64_t)sizeof(float));
  if (cudaHostAlloc(&lane_host_, (std::size_t)bytes, cudaHostAllocMapped) != cudaSuccess ||
      cudaHostGetDevicePointer(&d_lane_host_, lane_host_, 0) != cudaSuccess) {
    err = "expert tier: the CPU lane's mapped buffers";
    return false;
  }
  std::memset(lane_host_, 0, (std::size_t)bytes);
  auto* h8 = static_cast<std::uint8_t*>(lane_host_);
  h_lane_seq_ = reinterpret_cast<volatile unsigned*>(h8);
  h_lane_flag_ = reinterpret_cast<volatile unsigned*>(h8 + 64);
  h_lane_x_ = reinterpret_cast<float*>(h8 + lane_off_x_);
  h_lane_ids_ = reinterpret_cast<int*>(h8 + lane_off_ids_);
  h_lane_eff_ = reinterpret_cast<int*>(h8 + lane_off_eff_);
  h_lane_ymiss_ = reinterpret_cast<float*>(h8 + lane_off_ymiss_);
  if (cudaMalloc(reinterpret_cast<void**>(&lane_dev_), 2 * sizeof(int)) != cudaSuccess ||
      cudaMalloc(reinterpret_cast<void**>(&lane_ncpu_dev_), sizeof(int)) != cudaSuccess ||
      cudaMemset(lane_ncpu_dev_, 0, sizeof(int)) != cudaSuccess) {
    err = "expert tier: the CPU lane's device words";
    return false;
  }
  const int words[2] = {0, std::clamp(pcie_num, 0, 256)};
  if (cudaMemcpy(lane_dev_, words, sizeof(words), cudaMemcpyHostToDevice) != cudaSuccess) {
    err = "expert tier: the CPU lane's device words upload";
    return false;
  }
  lane_pcie_num_ = words[1];
  lane_dev_on_ = false;
  lane_pool_ = std::make_unique<cpu::LanePool>(workers, true);
  std::fprintf(stderr,
               "expert tier: CPU expert lane on %d of %zu layers: %d threads (%s), pcie_frac %.2f, %.1f MiB mapped\n",
               n_ok, nl, lane_pool_->threads(), cpu::have_avx2() ? "AVX2 dots" : "scalar dots",
               (double)lane_pcie_num_ / 256.0, (double)bytes / (1024.0 * 1024.0));
  return true;
}

bool ExpertTier::reg_lane_set_pcie(int pcie_num, std::string& err) {
  if (lane_dev_ == nullptr) return true;
  const int v = std::clamp(pcie_num, 0, 256);
  if (cudaMemcpy(lane_dev_ + 1, &v, sizeof(int), cudaMemcpyHostToDevice) != cudaSuccess) {
    err = "expert tier: the CPU lane's pcie share upload";
    return false;
  }
  lane_pcie_num_ = v;
  return true;
}

bool ExpertTier::reg_lane_arm(bool on, void* stream, std::string& err) {
  if (lane_pool_ == nullptr) return true;
  if (on) {
    *h_lane_seq_ = 0u;
    *h_lane_flag_ = 0u;
    std::atomic_thread_fence(std::memory_order_seq_cst);
  }
  if (on != lane_dev_on_) {  // stream-ordered before the launch; any nonzero word is "on"
    if (cudaMemsetAsync(lane_dev_, on ? 1 : 0, sizeof(int), static_cast<cudaStream_t>(stream)) != cudaSuccess) {
      err = "expert tier: the CPU lane's on word";
      return false;
    }
    lane_dev_on_ = on;
  }
  lane_on_ = on;
  return true;
}

bool ExpertTier::reg_lane_serve(int n_tok, void* stream, std::string& err) {
  if (!lane_on_) return true;
  lane_on_ = false;
  using Clk = std::chrono::steady_clock;
  const auto cs = static_cast<cudaStream_t>(stream);
  auto release = [&]() {  // no GPU wait may outlive a failed serve
    std::atomic_thread_fence(std::memory_order_seq_cst);
    *h_lane_flag_ = 0xFFFFFFFFu;
  };
  ++lane_launches_;
  for (const int il : lane_layers_) {
    const unsigned want = (unsigned)il + 1u;
    const auto a = Clk::now();
    auto last = a;
    std::uint32_t spins = 0;
    while (*h_lane_seq_ < want) {
      _mm_pause();
      if ((++spins & 1023u) != 0) continue;
      const auto now = Clk::now();
      if (now - last > std::chrono::microseconds(2000)) {
        last = now;
        const cudaError_t q = cudaStreamQuery(cs);
        if (q != cudaErrorNotReady && *h_lane_seq_ < want) {
          release();
          err = "expert tier: CPU lane: layer " + std::to_string(il) + " never rang (" +
                (q == cudaSuccess ? std::string("the launch finished") : std::string(cudaGetErrorString(q))) + ")";
          return false;
        }
      }
      if (now - a > std::chrono::seconds(20)) {
        release();
        err = "expert tier: CPU lane: timed out at layer " + std::to_string(il);
        return false;
      }
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    const auto b = Clk::now();
    const int n = n_tok * lane_k_;
    const int* ids = h_lane_ids_ + (std::int64_t)il * lane_slot_;
    const int* eff = h_lane_eff_ + (std::int64_t)il * lane_slot_;
    int ncpu = 0;
    for (int j = 0; j < n; ++j) ncpu += eff[j] == k::kEffCpu ? 1 : 0;
    if (ncpu > 0) {
      if (!lane_pool_->serve(lane_L_[(std::size_t)il], n_tok, lane_k_, h_lane_x_, ids, eff, k::kEffCpu,
                             h_lane_ymiss_)) {
        release();
        err = "expert tier: CPU lane: serve refused " + std::to_string(n_tok) + " tokens";
        return false;
      }
      ++lane_cpu_layers_;
      lane_cpu_entries_ += ncpu;
    }
    std::atomic_thread_fence(std::memory_order_seq_cst);
    _mm_sfence();
    *h_lane_flag_ = want;
    const auto c = Clk::now();
    lane_ms_wait_ += std::chrono::duration<double, std::milli>(b - a).count();
    lane_ms_serve_ += std::chrono::duration<double, std::milli>(c - b).count();
  }
  return true;
}

void ExpertTier::reg_lane_reset_stats() {
  lane_launches_ = lane_cpu_layers_ = lane_cpu_entries_ = 0;
  lane_ms_wait_ = lane_ms_serve_ = 0.0;
  if (lane_pool_ != nullptr) {
    lane_pool_->experts = lane_pool_->rows_tokens = lane_pool_->bytes = 0;
    lane_pool_->ms_gu = lane_pool_->ms_q = lane_pool_->ms_down = 0.0;
  }
}

void ExpertTier::reg_lane_report() const {
  if (lane_pool_ == nullptr) return;
  const cpu::LanePool& P = *lane_pool_;
  const double ms_cpu = P.ms_gu + P.ms_q + P.ms_down;
  std::fprintf(stderr,
               "expert tier: CPU lane: %lld launches, %lld layers served %lld entries = %lld experts (%.0f us an "
               "expert, %.1f GB/s of expert bytes; gate/up %.0f ms, h %.0f ms, down %.0f ms); host waited %.0f ms, "
               "served %.0f ms\n",
               (long long)lane_launches_, (long long)lane_cpu_layers_, (long long)lane_cpu_entries_,
               (long long)P.experts, P.experts > 0 ? 1000.0 * ms_cpu / (double)P.experts : 0.0,
               ms_cpu > 0 ? (double)P.bytes / (ms_cpu * 1e6) : 0.0, P.ms_gu, P.ms_q, P.ms_down, lane_ms_wait_,
               lane_ms_serve_);
}

void ExpertTier::reg_note_device_token(const std::int32_t* ids_all, int k, const std::vector<char>& mask) {
  for (std::size_t l = 0; l < mask.size() && (std::int64_t)l < reg_n_layers_; ++l) {
    if (mask[l] == 0) continue;
    const std::int32_t* ids = ids_all + (std::int64_t)l * k;
    reg_note_routing((int)l, ids, k);
    for (int j = 0; j < k; ++j) {
      if (reg_slot_of((int)l, ids[j]) != kNotResident) ++reg_resident_hits_;
      else if (gr_tail_rank_ > 0 && j >= gr_tail_rank_) ++reg_tail_skipped_;  // the device skipped it
      else ++stream_misses_;
    }
  }
}

double ExpertTier::reg_cached_fraction() const {
  std::int64_t pairs = 0;
  for (const auto& rl : reg_)
    if (rl.valid) pairs += rl.n_expert;
  return pairs > 0 ? (double)reg_vram_slots_ / (double)pairs : 0.0;
}

bool ExpertTier::reg_fill_slot(std::int32_t slot, int layer, std::int32_t e, std::string& err) {
  const RegLayer& rl = reg_[(std::size_t)layer];
  std::uint8_t* dst = reg_slot_ptr(layer, slot);
  const std::uint8_t* g = static_cast<const std::uint8_t*>(rl.gate.q) + (std::int64_t)e * rl.gate_stride;
  const std::uint8_t* u = static_cast<const std::uint8_t*>(rl.up.q) + (std::int64_t)e * rl.up_stride;
  const std::uint8_t* d = static_cast<const std::uint8_t*>(rl.down.q) + (std::int64_t)e * rl.down_stride;
  if (cudaMemcpy(dst, g, (std::size_t)rl.gate_bytes, cudaMemcpyHostToDevice) != cudaSuccess ||
      cudaMemcpy(dst + rl.gate_bytes, u, (std::size_t)rl.up_bytes, cudaMemcpyHostToDevice) != cudaSuccess ||
      cudaMemcpy(dst + rl.gate_bytes + rl.up_bytes, d, (std::size_t)rl.down_bytes, cudaMemcpyHostToDevice) !=
          cudaSuccess) {
    err = "expert tier: registered slot fill failed";
    return false;
  }
  ++reg_fills_;
  return true;
}

bool ExpertTier::reg_finalize(std::int64_t vram_budget_bytes,
                              const std::vector<std::pair<std::int32_t, std::int32_t>>& ranked,
                              double warm_seed, int stream_prefetch, std::string& err) {
  if (reg_.empty()) {
    err = "expert tier: reg_finalize before register_layer";
    return false;
  }
  reg_n_layers_ = (std::int64_t)reg_.size();
  // n_expert from the first VALID layer: a family with leading dense layers (GLM dense_lead=3) leaves the
  // early reg_ entries unregistered (n_expert 0), so reg_[0] is not a safe source.
  reg_n_expert_ = 0;
  reg_slot_bytes_ = 0;
  for (const auto& rl : reg_)
    if (rl.valid) {
      if (reg_n_expert_ == 0) reg_n_expert_ = rl.n_expert;
      reg_slot_bytes_ = std::max(reg_slot_bytes_, rl.gate_bytes + rl.up_bytes + rl.down_bytes);
    }
  reg_residency_.assign((std::size_t)(reg_n_layers_ * reg_n_expert_), kNotResident);
  reg_layer_next_.assign((std::size_t)reg_n_layers_, 0);
  reg_usage_.assign((std::size_t)(reg_n_layers_ * reg_n_expert_), 0.0);
  reg_vram_slots_ = 0;

  // ---- C2 prefetch ring (async PCIe overlap for STREAMED experts): a copy stream + `stream_prefetch`
  // device buffers sized to one resident slot (gate|up|down) + ready/free events per buffer. Allocated
  // here (before the residency slot pool) so it exists on every path, including fully-streaming. Depth 1
  // = serial copy->compute (correct, no overlap); depth N keeps up to N streamed experts' copies in
  // flight ahead of the compute stream. Tiny (N * one expert); it comes out of the free-VRAM budget.
  rg_ring_depth_ = std::max(1, std::min(stream_prefetch, 16));
  if (reg_slot_bytes_ > 0) {
    if (rg_copy_ == nullptr) {
      cudaStream_t cs = nullptr, ce = nullptr;
      if (cudaStreamCreate(&cs) != cudaSuccess || cudaStreamCreate(&ce) != cudaSuccess) {
        err = "expert tier: reg_finalize prefetch stream create failed";
        return false;
      }
      rg_copy_ = cs;
      rg_compute_ = ce;
      cudaEvent_t ie = nullptr, oe = nullptr;
      if (cudaEventCreateWithFlags(&ie, cudaEventDisableTiming) != cudaSuccess ||
          cudaEventCreateWithFlags(&oe, cudaEventDisableTiming) != cudaSuccess) {
        err = "expert tier: reg_finalize boundary event create failed";
        return false;
      }
      rg_in_ev_ = ie;
      rg_out_ev_ = oe;
    }
    rg_ring_cap_ = reg_slot_bytes_;
    rg_ring_.assign((std::size_t)rg_ring_depth_, nullptr);
    rg_ready_ev_.assign((std::size_t)rg_ring_depth_, nullptr);
    rg_free_ev_.assign((std::size_t)rg_ring_depth_, nullptr);
    for (int r = 0; r < rg_ring_depth_; ++r) {
      if (cudaMalloc(reinterpret_cast<void**>(&rg_ring_[(std::size_t)r]), (std::size_t)rg_ring_cap_) !=
          cudaSuccess) {
        err = "expert tier: reg_finalize prefetch-ring buffer alloc failed";
        return false;
      }
      cudaEvent_t re = nullptr, fe = nullptr;
      if (cudaEventCreateWithFlags(&re, cudaEventDisableTiming) != cudaSuccess ||
          cudaEventCreateWithFlags(&fe, cudaEventDisableTiming) != cudaSuccess) {
        err = "expert tier: reg_finalize prefetch-ring event create failed";
        return false;
      }
      rg_ready_ev_[(std::size_t)r] = re;
      rg_free_ev_[(std::size_t)r] = fe;
    }
    // C3 CPU-lane host buffers (pinned x + per-expert parts for fast H2D; plain per-expert scratch).
    if (rg_xhost_ == nullptr && reg_n_embd_ > 0 && reg_n_ff_ > 0) {
      cudaMallocHost(reinterpret_cast<void**>(&rg_xhost_), (std::size_t)reg_n_embd_ * sizeof(float));
      cudaMallocHost(reinterpret_cast<void**>(&rg_cpu_parts_),
                     (std::size_t)k_max_ * reg_n_embd_ * sizeof(float));
      rg_cpu_scratch_ = new (std::nothrow) float[(std::size_t)k_max_ * 2 * reg_n_ff_];
      if (rg_xhost_ == nullptr || rg_cpu_parts_ == nullptr || rg_cpu_scratch_ == nullptr) {
        err = "expert tier: reg_finalize CPU-lane host buffer alloc failed";
        return false;
      }
    }
  }

  if (vram_budget_bytes <= 0 || reg_slot_bytes_ <= 0) return true;  // fully streaming (no residency)
  // Per-layer slot geometry: a slot holds one expert of ITS layer (gate|up|down at that layer's quants),
  // so a mixed-quant model (qwen4exp: 1.5-2.7 MB experts) does not pay the largest layer's slot size in
  // every layer. Each MoE layer gets an equal share of the budget in BYTES -- more slots where experts are
  // smaller -- and dense layers (no experts) get none.
  const std::size_t nl = (std::size_t)reg_n_layers_;
  reg_lslot_bytes_.assign(nl, 0);
  reg_lfirst_.assign(nl, 0);
  reg_lcount_.assign(nl, 0);
  reg_lbase_.assign(nl, 0);
  std::int64_t n_valid = 0;
  for (const auto& rl : reg_)
    if (rl.valid) ++n_valid;
  if (n_valid == 0) return true;
  const std::int64_t share = vram_budget_bytes / n_valid;
  std::int64_t slots = 0, pool_bytes = 0;
  for (std::size_t l = 0; l < nl; ++l) {
    reg_lfirst_[l] = slots;
    reg_lbase_[l] = pool_bytes;
    const RegLayer& rl = reg_[l];
    if (!rl.valid) continue;
    const std::int64_t b = rl.gate_bytes + rl.up_bytes + rl.down_bytes;
    const std::int64_t c = b > 0 ? std::min<std::int64_t>(share / b, rl.n_expert) : 0;
    reg_lslot_bytes_[l] = b;
    reg_lcount_[l] = c;
    slots += c;
    pool_bytes += c * b;
  }
  if (slots <= 0) return true;
  if (cudaMalloc(reinterpret_cast<void**>(&reg_vram_), (std::size_t)pool_bytes) != cudaSuccess) {
    std::fprintf(stderr, "expert tier: reg_finalize cudaMalloc %lld slots (%.2f GiB) FAILED; fully streaming\n",
                 (long long)slots, (double)pool_bytes / (1024.0 * 1024.0 * 1024.0));
    reg_vram_ = nullptr;
    reg_lcount_.assign(nl, 0);
    cudaGetLastError();  // clear the sticky error; run fully streaming (not fatal)
    return true;
  }
  reg_vram_slots_ = slots;

  // Per-layer admission within the layer's own slot range; admit returns the next free slot.
  auto admit = [&](int layer, std::int32_t e) -> std::int32_t {
    if (layer < 0 || layer >= reg_n_layers_ || e < 0 || e >= reg_n_expert_) return kNotResident;
    std::int32_t& cur = reg_residency_[(std::size_t)((std::int64_t)layer * reg_n_expert_ + e)];
    if (cur != kNotResident) return cur;
    const std::int64_t lo = reg_lfirst_[(std::size_t)layer];
    const std::int64_t hi = lo + reg_lcount_[(std::size_t)layer];
    std::int64_t& next = reg_layer_next_[(std::size_t)layer];
    if (next < lo) next = lo;
    if (next >= hi) return kNotResident;  // this layer's slots are full
    const std::int32_t slot = (std::int32_t)next++;
    cur = slot;
    return slot;
  };

  // WARM START: fill from `ranked` (hottest first) when given, else cold (experts 0..quota-1 per layer).
  if (!ranked.empty()) {
    for (const auto& [l, e] : ranked) {
      const std::int32_t slot = admit(l, e);
      if (slot == kNotResident) continue;
      if (!reg_fill_slot(slot, l, e, err)) return false;
      // Seed the resident expert's heat so the early rebalances keep the profile's hot set instead of
      // churning it out against zero usage; it decays back toward real routing data over the first run.
      reg_usage_[(std::size_t)((std::int64_t)l * reg_n_expert_ + e)] = warm_seed;
    }
  } else {
    for (int l = 0; l < (int)reg_n_layers_; ++l) {
      if (!reg_[(std::size_t)l].valid) continue;  // dense layer: no experts to make resident
      for (std::int32_t e = 0; e < (std::int32_t)reg_n_expert_; ++e) {
        const std::int32_t slot = admit(l, e);
        if (slot == kNotResident) break;  // this layer's quota is full
        if (!reg_fill_slot(slot, l, e, err)) return false;
      }
    }
  }
  return true;
}

void ExpertTier::reg_note_routing(int layer, const std::int32_t* ids, int k) {
  if (layer < 0 || layer >= reg_n_layers_ || reg_usage_.empty()) return;
  double* row = reg_usage_.data() + (std::int64_t)layer * reg_n_expert_;
  for (std::int64_t e = 0; e < reg_n_expert_; ++e) row[e] *= kDecay;
  for (int i = 0; i < k; ++i) {
    const std::int32_t e = ids[i];
    if (e >= 0 && e < reg_n_expert_) row[(std::size_t)e] += 1.0;
  }
}

std::int64_t ExpertTier::reg_rebalance(float margin, void* stream, std::string& err) {
  (void)stream;  // fills are blocking here (simple + correct); the box can overlap on `stream` later
  if (reg_vram_slots_ == 0 || reg_usage_.empty()) return 0;
  std::int64_t swaps = 0;
  for (std::int64_t layer = 0; layer < reg_n_layers_; ++layer) {
    const double* row = reg_usage_.data() + layer * reg_n_expert_;
    for (std::int64_t iter = 0; iter < reg_n_expert_; ++iter) {
      std::int64_t cold_e = -1, hot_e = -1;
      double cold_v = 0.0, hot_v = -1.0;
      for (std::int64_t e = 0; e < reg_n_expert_; ++e) {
        const bool resident = reg_residency_[(std::size_t)(layer * reg_n_expert_ + e)] != kNotResident;
        if (resident) {
          if (cold_e < 0 || row[e] < cold_v) { cold_v = row[e]; cold_e = e; }
        } else {
          if (row[e] > hot_v) { hot_v = row[e]; hot_e = e; }
        }
      }
      if (cold_e < 0 || hot_e < 0 || hot_v <= cold_v + (double)margin) break;
      const std::int32_t slot = reg_residency_[(std::size_t)(layer * reg_n_expert_ + cold_e)];
      reg_residency_[(std::size_t)(layer * reg_n_expert_ + cold_e)] = kNotResident;
      if (!reg_fill_slot(slot, (int)layer, (std::int32_t)hot_e, err)) return -1;
      reg_residency_[(std::size_t)(layer * reg_n_expert_ + hot_e)] = slot;
      ++swaps;
    }
  }
  // the device-decided MoE path reads residency on the device: republish it (blocking, like the fills;
  // the legacy-stream copy waits for the in-flight token's kernels on any blocking stream).
  if (swaps > 0 && d_res_ != nullptr) {
    std::vector<int> local;
    reg_local_residency(local);
    if (cudaMemcpy(d_res_, local.data(), local.size() * sizeof(int), cudaMemcpyHostToDevice) != cudaSuccess) {
      err = "expert tier: device residency table re-upload failed";
      return -1;
    }
  }
  return swaps;
}

bool ExpertTier::reg_upload_residency_async(void* stream, std::string& err) {
  const std::size_t n = reg_residency_.size();
  // the previous upload read the mirror; it must be done before the mirror is rewritten
  if (cudaEventSynchronize(static_cast<cudaEvent_t>(rg_res_ev_)) != cudaSuccess) {
    err = "expert tier: residency upload wait failed";
    return false;
  }
  for (std::int64_t l = 0; l < reg_n_layers_; ++l) {  // layer-local slots, as reg_local_residency
    const std::int64_t first = (std::size_t)l < reg_lfirst_.size() ? reg_lfirst_[(std::size_t)l] : 0;
    for (std::int64_t e = 0; e < reg_n_expert_; ++e) {
      const std::size_t i = (std::size_t)(l * reg_n_expert_ + e);
      h_res_[i] = reg_residency_[i] != kNotResident ? (int)(reg_residency_[i] - first) : -1;
    }
  }
  const auto cs = static_cast<cudaStream_t>(stream);
  if (cudaMemcpyAsync(d_res_, h_res_, n * sizeof(int), cudaMemcpyHostToDevice, cs) != cudaSuccess ||
      cudaEventRecord(static_cast<cudaEvent_t>(rg_res_ev_), cs) != cudaSuccess) {
    err = "expert tier: device residency table upload failed";
    return false;
  }
  return true;
}

int ExpertTier::reg_land_pending(std::string& err) {
  if (rg_pending_.empty()) return 0;
  const cudaError_t q = cudaEventQuery(static_cast<cudaEvent_t>(rg_fill_ev_));
  if (q == cudaErrorNotReady) return 0;
  if (q != cudaSuccess) {
    err = "expert tier: async slot fill failed";
    return -1;
  }
  for (const PendingFill& p : rg_pending_)
    reg_residency_[(std::size_t)((std::int64_t)p.layer * reg_n_expert_ + p.e)] = p.slot;
  rg_pending_.clear();
  return 1;
}

bool ExpertTier::reg_land_async(void* stream, std::string& err) {
  const int landed = reg_land_pending(err);
  if (landed < 0) return false;
  return landed == 0 || reg_upload_residency_async(stream, err);
}

std::int64_t ExpertTier::reg_rebalance_async(float margin, int max_swaps, void* stream, std::string& err) {
  if (reg_vram_slots_ == 0 || reg_usage_.empty() || d_res_ == nullptr || rg_copy_ == nullptr) return 0;
  if (rg_fill_ev_ == nullptr) {
    cudaEvent_t fe = nullptr, re = nullptr;
    if (cudaEventCreateWithFlags(&fe, cudaEventDisableTiming) != cudaSuccess ||
        cudaEventCreateWithFlags(&re, cudaEventDisableTiming) != cudaSuccess ||
        cudaHostAlloc(reinterpret_cast<void**>(&h_res_), reg_residency_.size() * sizeof(int),
                      cudaHostAllocDefault) != cudaSuccess) {
      err = "expert tier: async rebalance setup failed";
      return -1;
    }
    rg_fill_ev_ = fe;
    rg_res_ev_ = re;
    cudaEventRecord(re, static_cast<cudaStream_t>(stream));  // "no upload pending"
  }
  // 1. land the batch in flight: its experts turn resident (published below)
  bool publish = false;
  if (!rg_pending_.empty()) {
    const int landed = reg_land_pending(err);
    if (landed < 0) return -1;
    if (landed == 0) return 0;  // still copying: plan nothing new
    publish = true;
  }
  // 2. plan, as reg_rebalance: per layer, the hottest non-resident expert into the coldest resident slot
  // while the gain clears the margin. The cold expert is evicted now; the hot one is pending until landed.
  // A capped round starts one layer further each time, so a binding cap does not starve the last layers
  // (measured: starting at layer 0 with a cap of 8 a round, the hit rate fell to 43%).
  std::vector<std::uint8_t> reserved((std::size_t)reg_n_expert_, 0);
  std::int64_t swaps = 0;
  const std::int64_t first = reg_n_layers_ > 0 ? rg_round_++ % reg_n_layers_ : 0;
  for (std::int64_t li = 0; li < reg_n_layers_ && (max_swaps <= 0 || swaps < max_swaps); ++li) {
    const std::int64_t layer = (first + li) % reg_n_layers_;
    const double* row = reg_usage_.data() + layer * reg_n_expert_;
    std::fill(reserved.begin(), reserved.end(), (std::uint8_t)0);
    while (max_swaps <= 0 || swaps < max_swaps) {
      std::int64_t cold_e = -1, hot_e = -1;
      double cold_v = 0.0, hot_v = -1.0;
      for (std::int64_t e = 0; e < reg_n_expert_; ++e) {
        const bool resident = reg_residency_[(std::size_t)(layer * reg_n_expert_ + e)] != kNotResident;
        if (resident) {
          if (cold_e < 0 || row[e] < cold_v) { cold_v = row[e]; cold_e = e; }
        } else if (!reserved[(std::size_t)e]) {
          if (row[e] > hot_v) { hot_v = row[e]; hot_e = e; }
        }
      }
      if (cold_e < 0 || hot_e < 0 || hot_v <= cold_v + (double)margin) break;
      const std::size_t ci = (std::size_t)(layer * reg_n_expert_ + cold_e);
      rg_pending_.push_back({(int)layer, (std::int32_t)hot_e, reg_residency_[ci]});
      reg_residency_[ci] = kNotResident;
      reserved[(std::size_t)hot_e] = 1;
      ++swaps;
    }
  }
  if (swaps > 0) publish = true;
  // 3. publish (landings + evictions) on the decode's stream, then 4. refill the evicted slots on the copy
  // stream once that upload -- and with it every step still reading the evicted experts -- is done.
  if (publish && !reg_upload_residency_async(stream, err)) return -1;
  if (swaps > 0) {
    const auto cp = static_cast<cudaStream_t>(rg_copy_);
    if (cudaStreamWaitEvent(cp, static_cast<cudaEvent_t>(rg_res_ev_), 0) != cudaSuccess) {
      err = "expert tier: async fill ordering failed";
      return -1;
    }
    for (const PendingFill& p : rg_pending_) {
      const RegLayer& rl = reg_[(std::size_t)p.layer];
      std::uint8_t* dst = reg_slot_ptr(p.layer, p.slot);
      const auto* g = static_cast<const std::uint8_t*>(rl.gate.q) + (std::int64_t)p.e * rl.gate_stride;
      const auto* u = static_cast<const std::uint8_t*>(rl.up.q) + (std::int64_t)p.e * rl.up_stride;
      const auto* d = static_cast<const std::uint8_t*>(rl.down.q) + (std::int64_t)p.e * rl.down_stride;
      if (cudaMemcpyAsync(dst, g, (std::size_t)rl.gate_bytes, cudaMemcpyHostToDevice, cp) != cudaSuccess ||
          cudaMemcpyAsync(dst + rl.gate_bytes, u, (std::size_t)rl.up_bytes, cudaMemcpyHostToDevice, cp) !=
              cudaSuccess ||
          cudaMemcpyAsync(dst + rl.gate_bytes + rl.up_bytes, d, (std::size_t)rl.down_bytes, cudaMemcpyHostToDevice,
                          cp) != cudaSuccess) {
        err = "expert tier: async slot fill failed";
        return -1;
      }
      ++reg_fills_;
    }
    if (cudaEventRecord(static_cast<cudaEvent_t>(rg_fill_ev_), cp) != cudaSuccess) {
      err = "expert tier: async fill event failed";
      return -1;
    }
  }
  return swaps;
}

std::vector<std::pair<std::int32_t, std::int32_t>> ExpertTier::reg_ranked() const {
  std::vector<std::uint8_t> resident((std::size_t)(reg_n_layers_ * reg_n_expert_), 0);
  for (std::size_t i = 0; i < reg_residency_.size(); ++i) resident[i] = reg_residency_[i] != kNotResident ? 1 : 0;
  return rank_learned_profile(reg_n_layers_, reg_n_expert_, resident, reg_usage_, {});
}

void ExpertTier::close() {
  if (vram_base_) cudaFree(vram_base_);
  if (d_stage_) cudaFree(d_stage_);
  if (d_xq8_embd_) cudaFree(d_xq8_embd_);
  if (d_hq8_) cudaFree(d_hq8_);
  if (d_g_) cudaFree(d_g_);
  if (d_u_) cudaFree(d_u_);
  if (d_h_) cudaFree(d_h_);
  if (d_parts_) cudaFree(d_parts_);
  if (d_shared_) cudaFree(d_shared_);
  if (d_weights_) cudaFree(d_weights_);
  if (host_registered_ && host_base_) cudaHostUnregister(const_cast<std::uint8_t*>(host_base_));
  vram_base_ = nullptr;
  d_stage_ = nullptr;
  d_xq8_embd_ = d_hq8_ = nullptr;
  d_g_ = d_u_ = d_h_ = d_parts_ = d_shared_ = d_weights_ = nullptr;
  host_base_ = nullptr;
  host_registered_ = false;
  opened_ = false;

  // registered per-layer path
  if (rg_stage_) cudaFree(rg_stage_);
  if (rg_xq8_) cudaFree(rg_xq8_);
  if (rg_hq8_) cudaFree(rg_hq8_);
  if (rg_g_) cudaFree(rg_g_);
  if (rg_u_) cudaFree(rg_u_);
  if (rg_h_) cudaFree(rg_h_);
  if (rg_parts_) cudaFree(rg_parts_);
  if (rg_bf16x_) cudaFree(rg_bf16x_);
  rg_stage_ = nullptr;
  rg_xq8_ = rg_hq8_ = nullptr;
  rg_g_ = rg_u_ = rg_h_ = rg_parts_ = nullptr;
  rg_bf16x_ = nullptr;
  rg_stage_cap_ = rg_xq8_cap_ = rg_hq8_cap_ = 0;
  rg_g_cap_ = rg_u_cap_ = rg_h_cap_ = rg_parts_cap_ = rg_bf16x_cap_ = 0;
  reg_.clear();
  reg_scratch_ = false;
  reg_n_embd_ = reg_n_ff_ = 0;
  for (void* p : rg_pinned_)
    if (p) cudaFreeHost(p);
  rg_pinned_.clear();
  reg_dev_.clear();
  if (rg_fill_ev_) cudaEventSynchronize(static_cast<cudaEvent_t>(rg_fill_ev_));  // no fill into a freed pool
  if (rg_fill_ev_) cudaEventDestroy(static_cast<cudaEvent_t>(rg_fill_ev_));
  if (rg_res_ev_) cudaEventDestroy(static_cast<cudaEvent_t>(rg_res_ev_));
  if (h_res_) cudaFreeHost(h_res_);
  rg_fill_ev_ = rg_res_ev_ = nullptr;
  h_res_ = nullptr;
  rg_pending_.clear();
  if (d_res_) cudaFree(d_res_);
  d_res_ = nullptr;
  if (gr_tail_dev_) cudaFree(gr_tail_dev_);
  gr_tail_dev_ = nullptr;
  lane_pool_.reset();  // the workers stop before their buffers go
  if (lane_host_) cudaFreeHost(lane_host_);
  lane_host_ = d_lane_host_ = nullptr;
  h_lane_seq_ = h_lane_flag_ = nullptr;
  if (lane_dev_) cudaFree(lane_dev_);
  if (lane_ncpu_dev_) cudaFree(lane_ncpu_dev_);
  lane_dev_ = lane_ncpu_dev_ = nullptr;
  lane_layers_.clear();
  gr_tail_rank_ = 0;
  for (float* b : {gr_g_, gr_u_, gr_h_, gr_parts_})
    if (b) cudaFree(b);
  if (gr_xq8_) cudaFree(gr_xq8_);
  if (gr_hq8_) cudaFree(gr_hq8_);
  if (gr_eff_) cudaFree(gr_eff_);
  if (gr_stage_) cudaFree(gr_stage_);
  gr_g_ = gr_u_ = gr_h_ = gr_parts_ = nullptr;
  gr_xq8_ = gr_hq8_ = nullptr;
  gr_eff_ = nullptr;
  gr_stage_ = nullptr;
  gr_stage_stride_ = 0;
  gr_k_ = gr_ntok_ = gr_n_embd_ = gr_n_ff_ = 0;
  gr_stage_k_ = 0;

  // registered-path VRAM residency
  if (reg_vram_) cudaFree(reg_vram_);
  reg_vram_ = nullptr;
  reg_vram_slots_ = reg_slot_bytes_ = 0;
  reg_lslot_bytes_.clear();
  reg_lfirst_.clear();
  reg_lcount_.clear();
  reg_lbase_.clear();
  reg_n_layers_ = reg_n_expert_ = 0;
  reg_residency_.clear();
  reg_layer_next_.clear();
  reg_usage_.clear();
  reg_fills_ = reg_resident_hits_ = 0;

  // C2 prefetch ring
  for (std::uint8_t* b : rg_ring_)
    if (b) cudaFree(b);
  for (void* e : rg_ready_ev_)
    if (e) cudaEventDestroy(static_cast<cudaEvent_t>(e));
  for (void* e : rg_free_ev_)
    if (e) cudaEventDestroy(static_cast<cudaEvent_t>(e));
  rg_ring_.clear();
  rg_ready_ev_.clear();
  rg_free_ev_.clear();
  if (rg_in_ev_) cudaEventDestroy(static_cast<cudaEvent_t>(rg_in_ev_));
  if (rg_out_ev_) cudaEventDestroy(static_cast<cudaEvent_t>(rg_out_ev_));
  if (rg_copy_) cudaStreamDestroy(static_cast<cudaStream_t>(rg_copy_));
  if (rg_compute_) cudaStreamDestroy(static_cast<cudaStream_t>(rg_compute_));
  rg_in_ev_ = rg_out_ev_ = nullptr;
  rg_copy_ = rg_compute_ = nullptr;
  rg_ring_cap_ = 0;
  rg_ring_depth_ = 1;

  // C3 CPU-lane host buffers
  if (rg_xhost_) cudaFreeHost(rg_xhost_);
  if (rg_cpu_parts_) cudaFreeHost(rg_cpu_parts_);
  delete[] rg_cpu_scratch_;
  rg_xhost_ = rg_cpu_parts_ = rg_cpu_scratch_ = nullptr;
  rg_cpu_frac_ = 0.0f;
  rg_cpu_workers_ = 0;
}

}  // namespace u
