// The inflater (plugin spec 2026-10-30 section 12): RFC 1951 streams made by Python's zlib decode to the bytes they were
// made from - checked by length and SHA-256 as they stream (tools/secure/make_inflate_fixtures.py made them, and
// tools/secure/test_inflate_fixtures.py checks the files still decode to this table) - and every malformed stream is
// refused by name. The fixture folder is the case's argument.
#include "secure_frontend/inflate.hpp"
#include "secure_frontend/sha256.hpp"
#include "support/test_support.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {
int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

using secure_frontend::InflateError;
using secure_frontend::InflateResult;

struct Case final {
  std::string_view name;
  std::uint64_t size;
  std::string_view sha256;
};
// (make_inflate_fixtures.py prints these lines)
constexpr std::array<Case, 6> kCases{{
    {"empty", 0U, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
    {"stored", 5000U, "ed9fe0596180bc6dc70745ff113a018bde35a2c8718c0dd53bbb14ccdfc4ebea"},
    {"fixed", 300U, "8fb3113b0a8c43291e488fe546f445f0da0f970c5c6b6f75b6a5ad3f5f240d52"},
    {"dynamic", 100000U, "d8ad6b4934819f02f6824e9454b1db59a26ac7592abc16c6fec1ba9b3eaccefe"},
    {"far_matches", 200000U, "b5b21fb20cc9300216fb477bcd7a45798df44862c3019ab10ff0544719d8fbbc"},
    {"run_of_zeros", 70000U, "f51b279903037b37ea1828a1021499995718d38016cad6c0da30962a41be052f"},
}};

std::filesystem::path fixtures() {
  const auto& args = secure_frontend_test::extra_arguments();
  return args.empty() ? std::filesystem::path{} : std::filesystem::path{args.front()};
}

std::vector<std::uint8_t> fixture(const std::string_view name) {
  std::ifstream in(fixtures() / (std::string{name} + ".deflate"), std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// The stream's result, with what the sink saw: its length, its SHA-256 and the largest chunk.
struct Decoded final {
  std::variant<InflateResult, InflateError> result;
  std::uint64_t seen{0};
  std::string sha256{};
  std::size_t largest_chunk{0};
};

Decoded decode(const std::vector<std::uint8_t>& input, const std::uint64_t max_output = UINT64_MAX) {
  secure_frontend::Sha256 hasher;
  Decoded out{InflateError::truncated};
  out.result = secure_frontend::inflate_raw(input, max_output, [&](const std::span<const std::uint8_t> chunk) {
    hasher.update(chunk);
    out.seen += chunk.size();
    if (chunk.size() > out.largest_chunk) out.largest_chunk = chunk.size();
    return true;
  });
  out.sha256 = secure_frontend::to_hex(hasher.finish());
  return out;
}

// Stored, fixed and dynamic blocks, matches at nearly the window's whole 32 KiB, long runs: each decodes to its bytes,
// the whole stream consumed, in chunks of at most 64 KiB.
void zlibs_streams_decode_to_their_bytes() {
  expect(!fixtures().empty() && std::filesystem::is_directory(fixtures()), "the fixture folder is the case's argument");
  for (const auto& c : kCases) {
    const auto input = fixture(c.name);
    expect(!input.empty(), std::string{c.name} + ": its fixture is there");
    const auto out = decode(input);
    const auto* done = std::get_if<InflateResult>(&out.result);
    expect(done != nullptr, std::string{c.name} + ": decoded" +
                                (done == nullptr ? ": " + std::string{secure_frontend::inflate_error_text(std::get<InflateError>(out.result))} : ""));
    if (done == nullptr) continue;
    expect(done->written == c.size && out.seen == c.size, std::string{c.name} + ": " + std::to_string(out.seen) + " bytes");
    expect(out.sha256 == c.sha256, std::string{c.name} + ": its SHA-256");
    expect(done->consumed == input.size(), std::string{c.name} + ": the whole stream consumed");
    expect(out.largest_chunk <= 65536U, std::string{c.name} + ": chunks of at most 64 KiB");
  }
}

// Each malformed stream is refused by name.
void malformed_streams_are_refused_by_name() {
  const std::array<std::pair<std::string_view, InflateError>, 4> bad{{{"bad_block_type", InflateError::bad_block_type},
                                                                      {"bad_stored_length", InflateError::bad_stored_length},
                                                                      {"bad_distance", InflateError::distance_too_far},
                                                                      {"truncated", InflateError::truncated}}};
  for (const auto& [name, error] : bad) {
    const auto out = decode(fixture(name));
    const auto* refused = std::get_if<InflateError>(&out.result);
    expect(refused != nullptr && *refused == error,
           std::string{name} + ": " + (refused == nullptr ? "decoded" : std::string{secure_frontend::inflate_error_text(*refused)}));
  }
  // no input at all, and a stream with bytes after its last block
  const auto empty = decode({});
  expect(std::holds_alternative<InflateError>(empty.result) && std::get<InflateError>(empty.result) == InflateError::truncated,
         "no input: truncated");
  auto longer = fixture("fixed");
  const std::size_t stream = longer.size();
  longer.push_back(0xAB);
  const auto out = decode(longer);
  const auto* done = std::get_if<InflateResult>(&out.result);
  expect(done != nullptr && done->consumed == stream, "bytes after the last block are not taken (consumed says where it ended)");
}

// The caller's bound holds, and a sink that says no stops the stream.
void the_bounds_hold() {
  const auto input = fixture("dynamic");
  const auto capped = decode(input, 99999U);
  expect(std::holds_alternative<InflateError>(capped.result) && std::get<InflateError>(capped.result) == InflateError::too_large &&
             capped.seen <= 99999U,
         "one byte over the bound: too_large, and no more than the bound reached the sink");
  expect(std::holds_alternative<InflateResult>(decode(input, 100000U).result), "exactly the bound: decoded");
  int calls = 0;
  const auto stopped = secure_frontend::inflate_raw(input, UINT64_MAX, [&calls](const std::span<const std::uint8_t>) { return ++calls < 2; });
  expect(std::holds_alternative<InflateError>(stopped) && std::get<InflateError>(stopped) == InflateError::sink_refused && calls == 2,
         "a sink that says no stops it");
  for (const auto error : {InflateError::truncated, InflateError::bad_block_type, InflateError::bad_stored_length,
                           InflateError::bad_code_lengths, InflateError::bad_symbol, InflateError::distance_too_far,
                           InflateError::too_large, InflateError::sink_refused}) {
    expect(!secure_frontend::inflate_error_text(error).empty() && secure_frontend::inflate_error_text(error) != "RED stub",
           "every error has its words");
  }
}

// Every prefix of a good stream, and every one-byte change of a small one, ends in a result or a named error - never
// past its input or its bound (run under the sanitizers' builds, an out-of-bounds read shows there).
void damaged_streams_end_cleanly() {
  for (const std::string_view name : {"fixed", "dynamic", "stored"}) {
    const auto input = fixture(name);
    const std::size_t step = input.size() > 400U ? input.size() / 200U : 1U;
    for (std::size_t cut = 0; cut < input.size(); cut += step) {
      const std::vector<std::uint8_t> prefix(input.begin(), input.begin() + static_cast<std::ptrdiff_t>(cut));
      (void)decode(prefix, 1U << 20U);
    }
  }
  auto little = fixture("fixed");
  for (std::size_t i = 0; i < little.size(); ++i) {
    for (const std::uint8_t flip : {std::uint8_t{0x01}, std::uint8_t{0x80}, std::uint8_t{0xFF}}) {
      auto changed = little;
      changed[i] = static_cast<std::uint8_t>(changed[i] ^ flip);
      const auto out = decode(changed, 1U << 16U);
      expect(out.seen <= (1U << 16U), "a changed byte never passes the bound");
    }
  }
}
}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"zlibs_streams_decode_to_their_bytes", zlibs_streams_decode_to_their_bytes},
      {"malformed_streams_are_refused_by_name", malformed_streams_are_refused_by_name},
      {"the_bounds_hold", the_bounds_hold},
      {"damaged_streams_end_cleanly", damaged_streams_end_cleanly},
  }, failures);
}
