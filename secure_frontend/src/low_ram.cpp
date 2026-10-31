// Upstream's low-RAM mode (low_ram.hpp): upstream/setup.py's rule, in its own units - the RAM in GiB (ram_gb(): the
// total / 2**30), the experts in GB (MODELS[...]["arena_gb"]: 35.5 for IQ2_XS's 35.45e9 bytes), a GPU's VRAM in GiB
// (nvidia-smi's MiB / 1024).
#include "secure_frontend/low_ram.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <system_error>

namespace secure_frontend {
namespace {

constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;

// upstream KV_CELL_BYTES.get(kv, 1056): one context token's KV cache bytes in each of the 12 QSA layers and the draft layer
double kv_cell_bytes(const std::string_view kv) {
  if (kv == "q4_0") return 576.0;
  if (kv == "k8v4") return 816.0;
  return 1056.0;
}

// upstream low_ram_gpu_gb: the GB of experts a GPU's cache holds - its VRAM less ~5 GB for the dense weights, buffers and
// a 32K context's KV cache, the draft layer's 1 GB, and the KV cache of the context past 32K
double gpu_gb(const double vram_gib, const LowRamFacts& facts, const double arena_gb) {
  const double longer = static_cast<double>(std::max<std::int64_t>(0, facts.context - 32768)) * 13.0 * kv_cell_bytes(facts.kv) / 1e9;
  return std::max(0.0, std::min(arena_gb, vram_gib - 5.0 - (facts.mtp ? kLowRamMtpVramGb : 0.0) - longer));
}

}  // namespace

LowRamChoice choose_low_ram(const LowRamFacts& facts, const std::string_view choice) {
  LowRamChoice out;
  out.arena_gb = static_cast<double>(facts.expert_bytes) / 1e9;
  out.ram_gib = static_cast<double>(facts.ram_bytes) / kGiB;
  const bool known = facts.expert_bytes > 0U && facts.ram_bytes > 0U;   // else auto has nothing to decide by
  // upstream low_ram_wanted: asked for, or auto where the experts do not fit the RAM with the headroom beside them
  const bool asked = choice == "on" || choice == "resident" || choice == "mmap";
  if (!asked && !(choice == "auto" && known && out.ram_gib < out.arena_gb + kLowRamHeadroomGb)) return out;
  // the GPUs' share (each GPU's own; the picture encoder's room on the main one), then upstream's variant rule
  if (facts.expert_bytes > 0U) {
    double held = 0.0;
    for (std::size_t i = 0; i < facts.vram_bytes.size(); ++i) {
      const double vram = static_cast<double>(facts.vram_bytes[i]) / kGiB - (i == 0U && facts.vision_gpu ? kLowRamVisionReserveMib / 1024.0 : 0.0);
      held += gpu_gb(vram, facts, out.arena_gb);
    }
    held = std::min(out.arena_gb, held);
    out.gpu_share = held / out.arena_gb;
    out.rest_gb = out.arena_gb - held;
  }
  const bool resident = choice == "resident" || (choice != "mmap" && known && out.ram_gib >= out.rest_gb + kLowRamHeadroomGb);
  // the engine keeps the resident copy only with a static expert profile (generate.cpp: --resident-cpu-experts)
  out.mode = resident && facts.expert_profile ? LowRamMode::resident : LowRamMode::mmap;
  return out;
}

std::optional<std::uint64_t> pack_expert_bytes(const std::filesystem::path& pack) {
  // native_experts.txt's first line: "# strata native experts v3: ... (n_expert 512, total 35454976000; ...)"
  if (std::ifstream in(pack / "native_experts.txt", std::ios::binary); in) {
    std::string line;
    line.resize(4096);
    in.getline(line.data(), static_cast<std::streamsize>(line.size()));
    line.resize(static_cast<std::size_t>(std::max<std::streamsize>(0, in.gcount())));
    constexpr std::string_view kTotal = "total ";
    if (const auto at = line.find(kTotal); line.rfind("# strata native experts", 0) == 0U && at != std::string::npos) {
      std::uint64_t total = 0;
      const char* first = line.data() + at + kTotal.size();
      const auto [end, error] = std::from_chars(first, line.data() + line.size(), total);
      if (error == std::errc{} && end != first && total > 0U) return total;
      return std::nullopt;   // a total that is no number: not guessed from experts.bin either
    }
  }
  std::error_code ec;
  const auto bin = pack / "experts.bin";
  if (std::filesystem::is_regular_file(bin, ec)) {
    const auto size = std::filesystem::file_size(bin, ec);
    if (!ec && size > 0U) return static_cast<std::uint64_t>(size);
  }
  return std::nullopt;
}

std::string_view low_ram_value(const LowRamMode mode) noexcept {
  switch (mode) {
    case LowRamMode::mmap: return "mmap";
    case LowRamMode::resident: return "resident";
    case LowRamMode::off: break;
  }
  return "off";
}

std::string_view low_ram_flag(const LowRamMode mode) noexcept {
  switch (mode) {
    case LowRamMode::mmap: return "--mmap-experts";
    case LowRamMode::resident: return "--resident-experts";
    case LowRamMode::off: break;
  }
  return {};
}

}  // namespace secure_frontend
