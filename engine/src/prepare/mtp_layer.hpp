#pragma once
// strata-prepare mtp: Qwen3.8-Flash-Next's MTP draft layer for the engine (--mtp <rt>), with no Python (owner
// 2026-10-29: "create native versions ... add it to strata prepare"). Upstream Strata's tools/mtp_pack.py (the routed
// experts as Q2_0 with its per-block MSE scale search, the dense tensors BF16, the 1-D ones F32) and tools/mtp_rt.py
// (experts.bin in the engine's blob layout, dense.bin with the large projections as Q8_0 and the norms +1, dense.txt)
// fused into one step from the fetched tensors to the runtime folder, byte for byte as the two tools write it; the
// draft layer's token subset (upstream's data/draft_vocab.bin) is copied in last. The mtp-q2_0.gguf between the two
// tools is not written: only its runtime files are read by the engine. The tensors are upstream's tools/mtp_fetch.py
// layout: <src>/tensors/<name>.bin, each one tensor's raw little-endian BF16 bytes from the pinned checkpoint.

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>

namespace strata_prepare {

// The checkpoint the MTP tensors come from (tools/mtp_fetch.py's PINNED_REVISION) and where each one is: its shard of
// the BF16 safetensors, its byte offset there and size (mtp-inventory.json at that revision), its shape, and the
// SHA-256 of its bytes (mtp_fetch.py's SHA256 table, #327). A fetch reads exactly these ranges.
inline constexpr std::string_view kMtpRepo = "Qwen/Qwen3.8-Flash-Next";
inline constexpr std::string_view kMtpRevision = "de4b8e4d43b917e7706784d8bb445c9af86a3540";

struct MtpTensor {
  std::string_view name;
  std::string_view shard;
  std::uint64_t start = 0;            // the first byte in the shard
  std::uint64_t bytes = 0;
  std::array<std::int64_t, 3> shape{};
  int dims = 0;
  std::string_view sha256;            // empty: not checked (the unit tests' small tensors)
};

// The 31 mtp.* tensors of kMtpRevision, sorted by name.
[[nodiscard]] std::span<const MtpTensor> mtp_tensors() noexcept;

struct MtpOptions {
  std::filesystem::path src;                       // holds tensors/<name>.bin
  std::filesystem::path out;                       // the runtime folder (created when missing)
  std::optional<std::filesystem::path> draft_vocab;   // copied in as draft_vocab.bin, last
  unsigned threads = 0;                            // 0: the hardware's
};

// The runtime folder from the tensors: `STEP verify`, `STEP experts`, `STEP dense`, `STEP vocab` on `steps` as each
// starts, messages on `log`. Returns "" when the folder is complete, else the reason: missing_tensor, wrong_size,
// wrong_sha256 (a tensor that is not the checkpoint's), bad_shape, missing_vocab, write_failed. Each file is written
// under a temporary name and renamed when whole, draft_vocab.bin last, so a folder with all three runtime files is a
// complete one.
[[nodiscard]] std::string mtp_runtime(const MtpOptions& options, std::span<const MtpTensor> tensors, std::ostream& steps,
                                      std::ostream& log);

// mtp_pack.py's q2_0() for one block of 64 values: the fp16 scale chosen among 17 candidates (amax x 0.5 ... 1.0) for
// the least squared error over the grid {-1, 0, 1, 2}, numpy's float32 arithmetic and pairwise sum; 18 bytes out.
void q2_0_block(const float* x, unsigned char* out) noexcept;
// mtp_rt.py's q8_0() for one block of 32 values (ggml's reference: d = amax / 127, half away from zero); 34 bytes out.
void q8_0_block(const float* x, unsigned char* out) noexcept;
// mtp_rt.py's blob_of(): one expert's blob from its Q2_0 rows - gate_up (2 x ff rows of h / 64 blocks: gate rows, then
// up rows) and down (h rows of ff / 64 blocks) - gate and up rows interleaved, the codes plane, then the scales plane.
[[nodiscard]] std::string expert_blob(const unsigned char* gate_up, const unsigned char* down, std::int64_t h, std::int64_t ff);

}  // namespace strata_prepare
