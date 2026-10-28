// SP5 M1 Task 12 (C-9; Rulings T12-a/T12-c/T12-d): the image_url payload gate and the
// per-request vision file choreography. See image_payload.hpp for the pinned shapes.
#include "secure_frontend/image_payload.hpp"

#include "secure_frontend/sha256.hpp"
#include "secure_frontend/token_generator.hpp"  // fill_random for the req-<random>.sve name

#include <array>
#include <cerrno>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <utility>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace secure_frontend {
namespace {

constexpr std::string_view kBase64Prefix = ";base64,";

// Strict base64: alphabet only, a multiple of four, canonical padding at the end.
// Whitespace or any other byte refuses (the payload never "fixes" client bytes).
[[nodiscard]] std::optional<std::string> decode_base64(const std::string_view encoded) {
  if (encoded.empty() || encoded.size() % 4U != 0U) return std::nullopt;
  const auto sextet = [](const char c) noexcept -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  std::string out;
  out.reserve(encoded.size() / 4U * 3U);
  for (std::size_t group = 0U; group < encoded.size(); group += 4U) {
    const bool pad_one = encoded[group + 3U] == '=';
    const bool pad_two = encoded[group + 2U] == '=';
    if (pad_two && !pad_one) return std::nullopt;  // "=" then a character: not canonical
    const std::size_t pad = pad_two ? 2U : (pad_one ? 1U : 0U);
    if (pad != 0U && group + 4U != encoded.size()) return std::nullopt;  // padding only at the end
    const char bytes[3] = {encoded[group], encoded[group + 1U], encoded[group + 2U]};
    int values[3] = {};
    // Padding lives at the END of the group only: with one '=', the first three
    // characters are all real; with two '=', characters 0 and 1 are.
    const std::size_t real = pad_two ? 2U : 3U;
    for (std::size_t index = 0U; index < real; ++index) {
      values[index] = sextet(bytes[index]);
      if (values[index] < 0) return std::nullopt;
    }
    out.push_back(static_cast<char>((static_cast<unsigned>(values[0]) << 2U) | (static_cast<unsigned>(values[1]) >> 4U)));
    if (pad < 2U)
      out.push_back(static_cast<char>(((static_cast<unsigned>(values[1]) & 0x0fU) << 4U) |
                                      (static_cast<unsigned>(values[2]) >> 2U)));
    if (pad < 1U) out.push_back(static_cast<char>(((static_cast<unsigned>(values[2]) & 0x03U) << 6U) |
                                                  static_cast<unsigned>(sextet(encoded[group + 3U]))));
    if (pad < 1U && sextet(encoded[group + 3U]) < 0) return std::nullopt;
  }
  return out;
}

std::uint32_t be32(const std::string& b, const std::size_t at) {
  return (static_cast<std::uint32_t>(static_cast<unsigned char>(b[at])) << 24U) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 1U])) << 16U) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 2U])) << 8U) |
         static_cast<unsigned char>(b[at + 3U]);
}
std::uint32_t be16(const std::string& b, const std::size_t at) {
  return (static_cast<std::uint32_t>(static_cast<unsigned char>(b[at])) << 8U) | static_cast<unsigned char>(b[at + 1U]);
}
std::uint32_t le16(const std::string& b, const std::size_t at) {
  return static_cast<std::uint32_t>(static_cast<unsigned char>(b[at])) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 1U])) << 8U);
}
std::uint32_t le32(const std::string& b, const std::size_t at) {
  return static_cast<std::uint32_t>(static_cast<unsigned char>(b[at])) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 1U])) << 8U) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 2U])) << 16U) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 3U])) << 24U);
}

// The private 0600 write (the pin's scratch files open with 0o600, :179/:2779).
[[nodiscard]] bool write_private_file(const std::filesystem::path& path, const std::string& bytes) {
#if defined(_WIN32)
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  out.close();
  return out.good();
#else
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) return false;
  bool ok = ::fchmod(fd, 0600) == 0;
  std::size_t written = 0U;
  while (ok && written < bytes.size()) {
    const ssize_t count = ::write(fd, bytes.data() + written, bytes.size() - written);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      ok = false;
    } else {
      written += static_cast<std::size_t>(count);
    }
  }
  if (::close(fd) != 0) ok = false;
  return ok;
#endif
}

[[nodiscard]] std::optional<std::string> read_whole(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

}  // namespace

std::optional<std::pair<std::uint32_t, std::uint32_t>> probe_image_dims(const std::string_view bytes) {
  const std::string b{bytes};
  if (b.size() >= 24U && b.rfind(std::string("\x89PNG\r\n\x1a\n", 8), 0) == 0 && b.compare(12U, 4U, "IHDR") == 0)
    return std::pair{be32(b, 16U), be32(b, 20U)};
  if (b.rfind("GIF8", 0) == 0 && b.size() >= 10U) return std::pair{le16(b, 6U), le16(b, 8U)};
  if (b.rfind("BM", 0) == 0 && b.size() >= 26U) return std::pair{le32(b, 18U), le32(b, 22U)};
  if (b.size() >= 9U && static_cast<unsigned char>(b[0]) == 0xffU && static_cast<unsigned char>(b[1]) == 0xd8U) {
    // JPEG: scan the marker chain to a start-of-frame (C0-CF except C4/C8/CC — DHT/DRI
    // tables and restart markers carry data we skip).
    std::size_t at = 2U;
    while (at + 1U < b.size()) {
      if (static_cast<unsigned char>(b[at]) != 0xffU) {
        ++at;
        continue;
      }
      const auto marker = static_cast<unsigned char>(b[at + 1U]);
      if (marker >= 0xc0U && marker <= 0xcfU && marker != 0xc4U && marker != 0xc8U && marker != 0xccU) {
        if (at + 9U <= b.size()) return std::pair{be16(b, at + 7U), be16(b, at + 5U)};  // height then width
        return std::nullopt;
      }
      if (marker == 0xd8U || marker == 0x01U || (marker >= 0xd0U && marker <= 0xd7U)) {
        at += 2U;
        continue;
      }
      if (marker == 0xd9U) break;  // end of image: no SOF found
      if (at + 3U >= b.size()) break;
      at += 2U + be16(b, at + 2U);  // skip this segment (length includes itself)
    }
  }
  return std::nullopt;  // WebP/TIFF/AVIF...: the encoder (stb) decides, and the kit proves it
}

ImagePayloadResult stage_image_payload(const std::string_view data_url, const std::filesystem::path& directory,
                                       const ImagePayloadLimits& limits) {
  if (data_url.rfind("data:", 0) != 0) return ImagePayloadError{ImagePayloadReason::not_a_data_url};
  const auto semicolon = data_url.find(kBase64Prefix);
  if (semicolon == std::string_view::npos || semicolon <= std::string_view{"data:"}.size())
    return ImagePayloadError{ImagePayloadReason::bad_data_url_shape};
  const auto decoded = decode_base64(data_url.substr(semicolon + kBase64Prefix.size()));
  if (!decoded.has_value()) return ImagePayloadError{ImagePayloadReason::not_base64};
  if (decoded->size() > limits.max_image_bytes) return ImagePayloadError{ImagePayloadReason::too_many_bytes};
  if (const auto dims = probe_image_dims(*decoded); dims.has_value() &&
      (static_cast<std::int64_t>(dims->first) > limits.max_side || static_cast<std::int64_t>(dims->second) > limits.max_side)) {
    return ImagePayloadError{ImagePayloadReason::too_large_side};
  }
  Sha256 hasher;
  hasher.update(*decoded);
  const std::string key = to_hex(hasher.finish()).substr(0U, 32U);  // the pin's key (:2758)
  const auto path = directory / (key + ".img");
  if (!write_private_file(path, *decoded)) return ImagePayloadError{ImagePayloadReason::bad_data_url_shape};
  return StagedImage{key, path, static_cast<std::uint64_t>(decoded->size())};
}

std::variant<VisionService::Staged, ImagePayloadReason> VisionService::stage(const std::string_view data_url) {
  const auto staged = stage_image_payload(data_url, limits_.scratch_dir, limits_.payload);
  if (const auto* error = std::get_if<ImagePayloadError>(&staged)) return error->reason;
  const auto& image = std::get<StagedImage>(staged);
  return Staged{image.key, image.key + ".img", image.path};
}

void VisionService::discard(const Staged& staged) { discard(staged.path); }

std::variant<VisionService::Encoded, VisionService::EncodeFailure> VisionService::encode(const Staged& staged) {
  const auto result = supervisor_.encode(staged.child_name, staged.key + ".sve");
  discard(staged);  // the pin unlinks the staged .img after the reply, whatever it said (:2782-2784)
  if (const auto* error = std::get_if<VisionEncodeError>(&result)) return EncodeFailure{error->kind};
  const auto& encoded = std::get<VisionEncoded>(result);
  return Encoded{staged.key, staged.key + ".sve", limits_.scratch_dir / (staged.key + ".sve"),
                 static_cast<std::size_t>(encoded.image_tokens)};
}

void VisionService::discard(const Encoded& encoded) { discard(encoded.path); }

std::optional<std::filesystem::path> VisionService::combine(const std::vector<Encoded>& encoded) {
  if (encoded.empty()) return std::nullopt;
  std::array<unsigned char, 6> randomness{};
  if (!fill_random(randomness.data(), randomness.size())) return std::nullopt;
  static constexpr char kHex[] = "0123456789abcdef";
  std::string name{"req-"};
  for (const unsigned char byte : randomness) {
    name.push_back(kHex[byte >> 4U]);
    name.push_back(kHex[byte & 0x0fU]);
  }
  name += ".sve";
  const auto combined_path = limits_.scratch_dir / name;
  std::string bytes;
  for (const auto& image : encoded) {
    const auto part = read_whole(image.path);
    if (!part.has_value()) return std::nullopt;
    bytes.append(*part);
  }
  if (!write_private_file(combined_path, bytes)) return std::nullopt;
  for (const auto& image : encoded) discard(image);  // no cache in M1 (T12-d)
  return combined_path;
}

void VisionService::discard(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
}

}  // namespace secure_frontend
