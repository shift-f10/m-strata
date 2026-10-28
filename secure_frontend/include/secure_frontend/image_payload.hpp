#pragma once

// SP5 M1 Task 12 (C-9 carve-out; Rulings T12-a/T12-c): the image_url payload gate and
// the per-request file choreography for the vision child. Everything in the first
// section runs BEFORE any engine or child contact: data-URL shape, strict base64, the
// decoded-byte cap (the pin's IMAGE_URL_MAX = 32 MiB, serve/server.py:2474), and the
// vision.max_side probe for the four formats the pin forwards untouched (JPEG/PNG/
// BMP/GIF, :2710-2728). Only then does the private-scratch write happen, 0600, keyed
// by the pin's hash: sha256 of the image bytes, first 32 hex chars (:2758).
//
// The second section (VisionService) is the request-path choreographer: stage ->
// ENC (through VisionSupervisor) -> combine the .sve files into one GENI file
// (combined_embeddings_path family, :158) -> discard. There is no LRU cache in M1
// (Ruling T12-d): every request encodes fresh and deletes every file it wrote.

#include "secure_frontend/vision_supervisor.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace secure_frontend {

// The pin's per-image URL cap, in decoded bytes (serve/server.py:2474).
inline constexpr std::size_t kVisionImageUrlMaxBytes = std::size_t{32} << 20;

struct ImagePayloadLimits final {
  std::size_t max_image_bytes{kVisionImageUrlMaxBytes};
  std::int64_t max_side{2048};  // vision.max_side
};

struct StagedImage final {
  std::string key;                       // sha256(bytes) hex, first 32 chars (the pin's key, :2758)
  std::filesystem::path path;            // <directory>/<key>.img
  std::uint64_t byte_count{0};
};

enum class ImagePayloadReason { not_a_data_url, bad_data_url_shape, not_base64, too_many_bytes, too_large_side };
struct ImagePayloadError final { ImagePayloadReason reason; };
using ImagePayloadResult = std::variant<StagedImage, ImagePayloadError>;

// Decodes, caps and stages `data_url` into `directory` (which must exist). Every
// refusal writes NOTHING (the plan's "length cap first", T12-c). The strict decoder
// accepts only the base64 alphabet with canonical padding.
[[nodiscard]] ImagePayloadResult stage_image_payload(std::string_view data_url, const std::filesystem::path& directory,
                                                      const ImagePayloadLimits& limits);

// The max_side probe: width and height for PNG/JPEG/GIF/BMP (the formats the pin
// passes to the encoder unconverted); nullopt for anything else, whose dimensions
// only the encoder will know (the kit's ground, Ruling T12-c).
[[nodiscard]] std::optional<std::pair<std::uint32_t, std::uint32_t>> probe_image_dims(std::string_view bytes);

// The request choreographer (chat route). Names handed to the child are RELATIVE to
// the child's cwd, which must be the scratch directory itself (#480: no spaces in
// ENC names — the keys are hex, the suffixes are fixed).
class VisionService final {
 public:
  struct Limits final {
    ImagePayloadLimits payload{};
    std::filesystem::path scratch_dir;  // = the child's working directory
  };

  VisionService(VisionSupervisor& supervisor, Limits limits) : supervisor_(supervisor), limits_(std::move(limits)) {}
  VisionService(const VisionService&) = delete;
  VisionService& operator=(const VisionService&) = delete;

  struct Staged final {
    std::string key;
    std::string child_name;  // <key>.img, relative to the child cwd
    std::filesystem::path path;
  };
  // Refusal reasons map to one fixed 400 in the route; the reason keeps them traceable.
  [[nodiscard]] std::variant<Staged, ImagePayloadReason> stage(std::string_view data_url);
  // Deletes a staged file (every path, always).
  void discard(const Staged& staged);

  struct Encoded final {
    std::string key;
    std::string child_name;  // <key>.sve, relative to the child cwd
    std::filesystem::path path;
    std::size_t image_tokens{0};
  };
  struct EncodeFailure final { VisionEncodeError::Kind kind; };
  // Runs the ENC round trip, then removes the staged .img whatever the answer was
  // (the pin unlinks it after the reply, :2782-2784).
  [[nodiscard]] std::variant<Encoded, EncodeFailure> encode(const Staged& staged);
  void discard(const Encoded& encoded);

  // Concatenates the request's .sve files into one 0600 req-<random>.sve (pin :158,
  // write_temporary :175) and deletes the per-image files (no cache, T12-d).
  [[nodiscard]] std::optional<std::filesystem::path> combine(const std::vector<Encoded>& encoded);
  void discard(const std::filesystem::path& combined);

 private:
  VisionSupervisor& supervisor_;
  Limits limits_;
};

}  // namespace secure_frontend
