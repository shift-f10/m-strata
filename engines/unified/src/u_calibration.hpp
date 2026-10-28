// engines/unified/src/u_calibration.hpp - the shared per-box hardware-calibration profile.
//
// Spec: docs/superpowers/specs/2026-10-24-engine-calibration-design.md (Stage C1). One profile per box,
// keyed by a hardware signature (GPU name + VRAM + model + context), measured once by --calibrate and
// reloaded on start. It holds the knobs of the ONE perf subsystem the unified engine has today -- the
// adaptive expert tier (u::ExpertTier residency + rebalance). Every family reads it, so calibration is
// engine-wide, not per-family. The struct defaults equal the values the families used as literals
// before C1, so an un-calibrated box behaves exactly as before.
//
// This header is host-safe (no CUDA): the store (file + env) is plain C++. The caller supplies the GPU
// name + VRAM (which it queries with cudaGetDeviceProperties), so this stays out of the CUDA build.
#pragma once

#include <cstdint>
#include <string>

namespace u {

// The calibratable knobs (C1: the adaptive expert tier only). C2+ add PCIe-overlap / CPU-lane fields.
struct CalibrationProfile final {
  std::int64_t residency_headroom_mib = 1024;  // VRAM kept free below the resident-expert budget
  int rebalance_every = 8;                      // decode tokens between reg_rebalance() calls
  float rebalance_margin = 1.0f;                // the swap-in usage-gain threshold
  double warm_seed = 4.0;                        // protective heat for a warm-started resident expert
  int stream_prefetch = 1;                       // C2: streamed-expert prefetch ring depth (1 = no copy/
                                                 // compute overlap). Default 1: on this box/GLM the
                                                 // streaming is copy-BW-bound, so depth>1 showed no gain
                                                 // (parity-verified); calibration raises it where it helps.
  float cpu_expert_frac = 0.0f;                  // C3: fraction of a token's STREAMED experts computed on
                                                 // the CPU lane instead of over PCIe. Default 0 = off
                                                 // (deterministic; CPU-computed experts are not bit-exact
                                                 // with the GPU). Calibration raises it where the CPU wins.
  int cpu_workers = 0;                           // C3: max CPU-lane worker threads (0 = one per CPU expert)
  int version = 1;                               // bump to invalidate stored records
};

// The process-wide calibration. init() once at start (after the device is chosen), then profile()
// everywhere. Thread-unsafe by design: init() runs before any family graph loads.
class Calibration final {
 public:
  // Load the profile for this box+model+context: the store file (STRATA_CAL_FILE, else the default
  // path) keyed by hardware_key(), then env overrides on top. A missing file / absent key / version
  // mismatch leaves the defaults (so behaviour is unchanged until a calibration is written).
  static void init(const std::string& gpu_name, std::uint64_t vram_bytes, const std::string& model_path,
                   std::uint32_t context);

  // The loaded profile (defaults until init()).
  static const CalibrationProfile& profile();

  // "<sanitized gpu>__<vram_gib>gib__<model basename>__ctx<context>" -- one record per box+model+context.
  static std::string hardware_key(const std::string& gpu_name, std::uint64_t vram_bytes,
                                  const std::string& model_path, std::uint32_t context);

  // The store file path: $STRATA_CAL_FILE, else <home>/.strata/unified-calibration.cal.
  static std::string store_path();

  // Upsert `p` under `key` in the store (atomic tmp+rename). Used by --calibrate. False with `err`.
  [[nodiscard]] static bool save(const std::string& key, const CalibrationProfile& p, std::string& err);

  // Read the record for `key` from the store into `out`. False (out unchanged) if absent/malformed.
  [[nodiscard]] static bool load_record(const std::string& key, CalibrationProfile& out);
};

}  // namespace u
