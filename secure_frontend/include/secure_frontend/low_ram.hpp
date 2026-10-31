#pragma once

// Upstream's low-RAM mode (owner 2026-10-31, the Azure VM: a Qwen3.8 IQ2_XS card on 27.4 GiB of RAM never started, its
// engine could not map the 35.4 GB expert arena; "Yes, build it"). Upstream's setup (upstream/setup.py: low_ram_needed,
// low_ram_gpu_gb, low_ram_resident; step 5's variant) puts a PC whose RAM cannot hold the model's experts with
// LOW_RAM_HEADROOM_GB beside them in the low-RAM mode: the engine reads the experts from the pack (its experts.bin, or
// a native pack's GGUF shards in place, generate.cpp's CS-T) instead of copying them all into RAM -
//   resident (--resident-experts): the ones the GPUs' caches do not hold are copied into RAM once, when they fit the RAM
//     with the headroom beside them;
//   mmap (--mmap-experts): they are read through the OS file cache.
// The card's low_ram option: auto (upstream's rule), on (the mode, its variant by the rule), resident, mmap, off.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace secure_frontend {

// upstream/setup.py LOW_RAM_HEADROOM_GB, MTP_VRAM_GB, VISION["gpu"]["reserve_mib"], KV_CELL_BYTES
inline constexpr double kLowRamHeadroomGb = 10.0;
inline constexpr double kLowRamMtpVramGb = 1.0;
inline constexpr double kLowRamVisionReserveMib = 700.0;

enum class LowRamMode { off, mmap, resident };

struct LowRamFacts final {
  std::uint64_t ram_bytes{0};                // this PC's RAM
  std::vector<std::uint64_t> vram_bytes{};   // the GPUs the model runs on, the main one first
  std::uint64_t expert_bytes{0};             // the pack's experts (pack_expert_bytes)
  std::int64_t context{32768};               // max_context
  std::string kv{"int8"};                    // --kv
  bool mtp{false};                           // the draft layer is used (its ~1 GB comes out of each GPU's cache)
  bool vision_gpu{false};                    // the picture encoder runs on the main GPU
  bool expert_profile{false};                // the card names a static --expert-profile (--resident-experts needs one)
};

struct LowRamChoice final {
  LowRamMode mode{LowRamMode::off};
  double gpu_share{0.0};   // the share of the experts the GPUs hold
  double rest_gb{0.0};     // GB of experts the GPUs do not hold
  double arena_gb{0.0};    // the experts, GB
  double ram_gib{0.0};     // the RAM, GiB (upstream's ram_gb())
};

// The choice for the card's low_ram value ("auto", "on", "resident", "mmap", "off"; any other is "off").
[[nodiscard]] LowRamChoice choose_low_ram(const LowRamFacts& facts, std::string_view choice);

// The pack's expert bytes: native_experts.txt's header total (strata native experts v3: "total N"), else experts.bin's
// size; nullopt when neither is there or readable.
[[nodiscard]] std::optional<std::uint64_t> pack_expert_bytes(const std::filesystem::path& pack);

// "resident", "mmap" or "off" (the option's value a start writes); and the engine flag of each ("" for off).
[[nodiscard]] std::string_view low_ram_value(LowRamMode mode) noexcept;
[[nodiscard]] std::string_view low_ram_flag(LowRamMode mode) noexcept;

}  // namespace secure_frontend
