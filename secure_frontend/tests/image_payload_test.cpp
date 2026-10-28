// SP5 M1 Task 12 (C-9 carve-out; Rulings T12-a/T12-c): the image_url payload gate.
// Everything here runs BEFORE any engine contact: data-URL shape, strict base64, the
// decoded-byte cap (the pin's IMAGE_URL_MAX = 32 MiB, serve/server.py:2474), the
// vision.max_side probe for the four formats the pin forwards untouched (JPEG/PNG/BMP/
// GIF, :2710-2728), then the private-scratch write at 0600 keyed by the pin's hash
// (sha256 of the image bytes, first 32 hex chars, :2758).
#include "secure_frontend/image_payload.hpp"
#include "secure_frontend/sha256.hpp"
#include "support/test_support.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <variant>
#include <vector>

namespace {

int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}

std::string b64(const std::string& raw) {
  static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (std::size_t i = 0; i < raw.size(); i += 3U) {
    const unsigned b0 = static_cast<unsigned char>(raw[i]);
    const unsigned b1 = i + 1U < raw.size() ? static_cast<unsigned char>(raw[i + 1U]) : 0U;
    const unsigned b2 = i + 2U < raw.size() ? static_cast<unsigned char>(raw[i + 2U]) : 0U;
    out += kTable[b0 >> 2U];
    out += kTable[((b0 & 3U) << 4U) | (b1 >> 4U)];
    out += i + 1U < raw.size() ? kTable[((b1 & 15U) << 2U) | (b2 >> 6U)] : '=';
    out += i + 2U < raw.size() ? kTable[b2 & 63U] : '=';
  }
  return out;
}

std::string png(const unsigned width, const unsigned height) {
  std::string bytes = "\x89PNG\r\n\x1a\n";
  const auto be32 = [](const unsigned v) {
    return std::string{static_cast<char>(v >> 24), static_cast<char>(v >> 16), static_cast<char>(v >> 8), static_cast<char>(v)};
  };
  bytes += be32(13) + "IHDR" + be32(width) + be32(height) + std::string(9U, '\0');
  return bytes;
}

std::string jpeg(const unsigned width, const unsigned height) {
  const auto be16 = [](const unsigned v) { return std::string{static_cast<char>(v >> 8), static_cast<char>(v)}; };
  return std::string("\xff\xd8\xff\xc0", 4) + be16(17) + std::string(1U, '\x08') + be16(height) + be16(width) +
         std::string(13U, '\0') + "\xff\xd9";
}

std::string gif(const unsigned width, const unsigned height) {
  const auto le16 = [](const unsigned v) { return std::string{static_cast<char>(v & 0xffU), static_cast<char>(v >> 8)}; };
  return "GIF89a" + le16(width) + le16(height) + std::string(7U, '\0');
}

std::string bmp(const unsigned width, const unsigned height) {
  const auto le32 = [](const unsigned v) {
    return std::string{static_cast<char>(v & 0xffU), static_cast<char>((v >> 8) & 0xffU), static_cast<char>(v >> 16),
                       static_cast<char>(v >> 24)};
  };
  return "BM" + std::string(16U, '\0') + le32(width) + le32(height) + std::string(8U, '\0');
}

std::string hex_of(const std::string& bytes) {
  secure_frontend::Sha256 hasher;
  hasher.update(bytes);
  return secure_frontend::to_hex(hasher.finish());
}

secure_frontend::ImagePayloadLimits small_limits() {
  secure_frontend::ImagePayloadLimits limits;
  limits.max_image_bytes = 1U << 20U;
  limits.max_side = 2048;
  return limits;
}

std::size_t files_in(const std::filesystem::path& directory) {
  std::size_t count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (entry.is_regular_file()) ++count;
  }
  return count;
}

void image_payload_stages_valid_images_with_the_pin_key() {
  const auto directory = secure_frontend_test::make_scratch_directory() / "vision";
  std::filesystem::create_directories(directory);
  const auto limits = small_limits();
  for (const auto& bytes : {png(8, 6), jpeg(1024, 768), gif(16, 16), bmp(32, 1)}) {
    const auto result =
        secure_frontend::stage_image_payload("data:image/png;base64," + b64(bytes), directory, limits);
    const auto* staged = std::get_if<secure_frontend::StagedImage>(&result);
    expect(staged != nullptr, "the four pass-through formats stage (all sides <= max_side)");
    if (staged == nullptr) continue;
    expect(staged->key == hex_of(bytes).substr(0, 32U), "the key is sha256(bytes)[:32], like the pin (:2758)");
    expect(staged->byte_count == bytes.size(), "the byte count is the decoded length");
    std::ifstream in(staged->path, std::ios::binary);
    expect(std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}} == bytes,
           "the staged file carries the image bytes exactly");
#if !defined(_WIN32)
    const auto perms = std::filesystem::status(staged->path).permissions();
    expect(perms == (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write), "the staged file is 0600");
#endif
    expect(std::filesystem::exists(staged->path) && staged->path.parent_path() == directory, "the file sits in the scratch dir");
  }
  // Unknown formats pass the probe (stb decides; the kit proves it, Ruling T12-c).
  const auto webp = secure_frontend::stage_image_payload("data:image/webp;base64," + b64("RIFF\x00\x00\x00\x00WEBVP2LP"),
                                                         directory, limits);
  expect(std::holds_alternative<secure_frontend::StagedImage>(webp), "an unprobed format stages on the byte caps alone");
}

void image_payload_rejects_bad_shape_before_any_write() {
  const auto directory = secure_frontend_test::make_scratch_directory() / "vision";
  std::filesystem::create_directories(directory);
  const auto limits = small_limits();
  for (const std::string bad : {"https://example.test/a.png",               // not a data URL (T12-a)
                                "data:image/png,YWJj",                      // no ;base64
                                "data:image/png;base64,@@@@",               // not base64
                                "data:image/png;base64,YW J",               // whitespace inside
                                "data:image/png;base64,YW===",              // bad padding
                                "not-a-url", "data:;base64,YWJj"}) {        // no media type
    const auto result = secure_frontend::stage_image_payload(bad, directory, limits);
    const auto* error = std::get_if<secure_frontend::ImagePayloadError>(&result);
    expect(error != nullptr, "refused before any write: " + bad);
  }
  expect(files_in(directory) == 0U, "nothing was written for any refused URL");
}

void image_payload_caps_bytes_and_sides() {
  const auto directory = secure_frontend_test::make_scratch_directory() / "vision";
  std::filesystem::create_directories(directory);
  auto limits = small_limits();
  limits.max_image_bytes = 4U;
  const auto big = secure_frontend::stage_image_payload("data:image/png;base64," + b64(std::string(5U, 'a')), directory, limits);
  const auto* byte_error = std::get_if<secure_frontend::ImagePayloadError>(&big);
  expect(byte_error != nullptr && byte_error->reason == secure_frontend::ImagePayloadReason::too_many_bytes,
         "the decoded-byte cap (IMAGE_URL_MAX family, :2474)");
  limits.max_image_bytes = 1U << 20U;
  limits.max_side = 100;
  const auto wide = secure_frontend::stage_image_payload("data:image/jpeg;base64," + b64(jpeg(3000, 40)), directory, limits);
  const auto* side_error = std::get_if<secure_frontend::ImagePayloadError>(&wide);
  expect(side_error != nullptr && side_error->reason == secure_frontend::ImagePayloadReason::too_large_side,
         "vision.max_side fires on a JPEG whose SOF says 3000 wide");
  const auto tall_png = secure_frontend::stage_image_payload("data:image/png;base64," + b64(png(99, 101)), directory, limits);
  expect(std::holds_alternative<secure_frontend::ImagePayloadError>(tall_png), "PNG height counts too");
  const auto tall_gif = secure_frontend::stage_image_payload("data:image/gif;base64," + b64(gif(101, 2)), directory, limits);
  expect(std::holds_alternative<secure_frontend::ImagePayloadError>(tall_gif), "GIF width counts");
  const auto tall_bmp = secure_frontend::stage_image_payload("data:image/bmp;base64," + b64(bmp(1, 101)), directory, limits);
  expect(std::holds_alternative<secure_frontend::ImagePayloadError>(tall_bmp), "BMP height counts");
  expect(files_in(directory) == 0U, "refusals leave no files behind");
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"image_payload_stages_valid_images_with_the_pin_key", image_payload_stages_valid_images_with_the_pin_key},
      {"image_payload_rejects_bad_shape_before_any_write", image_payload_rejects_bad_shape_before_any_write},
      {"image_payload_caps_bytes_and_sides", image_payload_caps_bytes_and_sides},
  }, failures);
}
