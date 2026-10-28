#pragma once

// Parallel ranged downloads (spec docs/superpowers/specs/2026-10-23-parallel-downloader-design.md):
// a file is fetched as fixed-size pieces over several connections. PieceMap tracks which pieces are
// on disk, so a stopped download (pause, a dropped connection) resumes only the missing ones. It is
// serialized to the `<name>.part.chunks` sidecar beside the `.part` file (owner-only). The ETag and
// total guard a resume: if the object on the server changed, the stored ETag/total no longer match
// and the caller discards the partial and restarts.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace secure_frontend {

// 32 MiB per piece, matching upstream's DL_CHUNK (upstream/setup.py); the most a failed request
// loses, and the unit the piece bitmap records.
inline constexpr std::uint64_t kDownloadPieceBytes = std::uint64_t{32} << 20U;
// Pieces one request asks for (128 MiB), and tries per run, matching upstream's DL_RUN/DL_CHUNK_TRIES.
inline constexpr std::uint64_t kDownloadRun = 4;
inline constexpr std::uint32_t kDownloadRunTries = 5;

struct PieceSpan final {
  std::uint64_t offset;
  std::uint64_t length;  // the last piece is short when total is not a multiple of piece_bytes
};

struct PieceMap final {
  std::uint64_t total{0};
  std::uint64_t piece_bytes{kDownloadPieceBytes};
  std::string etag;         // the server's ETag for the object these pieces belong to
  std::vector<bool> done;   // size() == piece count; done[i] = piece i is fully on disk
};

// ceil(total / piece_bytes); 0 pieces for a 0-byte file or a 0 piece size.
[[nodiscard]] std::uint64_t piece_count(std::uint64_t total, std::uint64_t piece_bytes) noexcept;
// A fresh map with every piece unset.
[[nodiscard]] PieceMap piece_map_new(std::uint64_t total, std::string etag,
                                     std::uint64_t piece_bytes = kDownloadPieceBytes);
// The byte range of piece `index` ({0,0} when out of range); the last piece is short.
[[nodiscard]] PieceSpan piece_span(const PieceMap& map, std::uint64_t index) noexcept;
[[nodiscard]] bool all_done(const PieceMap& map) noexcept;
// The bytes already on disk (full pieces only), for progress and resume accounting.
[[nodiscard]] std::uint64_t done_bytes(const PieceMap& map) noexcept;

// A single line, owner-only, written to `<name>.part.chunks`; see the .cpp for the grammar.
[[nodiscard]] std::string serialize_piece_map(const PieceMap& map);
// Parses it back; std::nullopt on anything malformed (a corrupt sidecar restarts from zero).
[[nodiscard]] std::optional<PieceMap> parse_piece_map(std::string_view text);

}  // namespace secure_frontend
