#include "secure_frontend/sha256.hpp"
#include "secure_frontend/test_hooks.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

std::string hex_of(const std::string_view bytes) { return secure_frontend::to_hex(secure_frontend::sha256(bytes)); }

// FIPS 180-4 / NIST CSRC "SHA-256 examples" and the NIST long-message vectors.
void fips_180_4_short_vectors() {
  expect(hex_of("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "empty message");
  expect(hex_of("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "abc (one block)");
  expect(hex_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
             "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
         "448-bit message (two blocks)");
  expect(hex_of("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu") ==
             "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1",
         "896-bit message");
}

void fips_180_4_million_a() {
  secure_frontend::Sha256 even;
  const std::string thousand(1000, 'a');
  for (int i = 0; i < 1000; ++i) even.update(thousand);
  expect(secure_frontend::to_hex(even.finish()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
         "one million 'a' in 1000-byte chunks");
  secure_frontend::Sha256 uneven;
  const std::string source(4099, 'a');
  std::size_t left = 1000000;
  std::size_t step = 1;
  while (left != 0) {
    const std::size_t take = std::min(left, step);
    uneven.update(std::string_view{source}.substr(0, take));
    left -= take;
    step = step * 7 % 4099 + 1;
  }
  expect(secure_frontend::to_hex(uneven.finish()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
         "one million 'a' in uneven chunks");
}

// NIST "extremely long message": the 64-byte pattern repeated 16 777 216 times (2^30 bytes, 2^33 bits).
void long_message_one_gibibyte() {
  const std::string pattern = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno";
  expect(pattern.size() == 64, "pattern is 64 bytes");
  std::string chunk;
  chunk.reserve(secure_frontend::kSha256ReadChunkBytes);
  while (chunk.size() < secure_frontend::kSha256ReadChunkBytes) chunk += pattern;
  secure_frontend::Sha256 hasher;
  const std::uint64_t total = std::uint64_t{1} << 30U;
  for (std::uint64_t done = 0; done < total; done += chunk.size()) hasher.update(chunk);
  expect(hasher.bytes_hashed() == total, "length counter reaches 2^30");
  expect(secure_frontend::to_hex(hasher.finish()) == "50e72a0e26442fe2552dc3938ac58658228c0cbfb1d2ca872ae435266fcd055e",
         "2^30-byte NIST long message");
}

void chunk_boundaries_do_not_change_digest() {
  std::string data(300, '\0');
  for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<char>((i * 31U + 7U) & 0xFFU);
  for (const std::size_t n : {0U, 1U, 55U, 56U, 63U, 64U, 65U, 119U, 120U, 127U, 128U, 129U, 300U}) {
    const auto whole = secure_frontend::sha256(std::string_view{data}.substr(0, n));
    for (std::size_t split = 0; split <= n; ++split) {
      secure_frontend::Sha256 hasher;
      hasher.update(std::string_view{data}.substr(0, split));
      hasher.update(std::string_view{data}.substr(split, n - split));
      if (hasher.finish() != whole) {
        expect(false, "split at " + std::to_string(split) + " of " + std::to_string(n));
        break;
      }
    }
  }
  secure_frontend::Sha256 reused;
  reused.update("abc");
  (void)reused.finish();
  expect(secure_frontend::to_hex(reused.finish()) == hex_of(""), "finish() resets to the initial state");
}

void length_field_encodes_bits_big_endian() {
  using Field = std::array<std::uint8_t, 8>;
  expect(secure_frontend::sha256_length_field(0) == Field{0, 0, 0, 0, 0, 0, 0, 0}, "zero length");
  expect(secure_frontend::sha256_length_field(3) == Field{0, 0, 0, 0, 0, 0, 0, 0x18}, "3 bytes = 24 bits");
  expect(secure_frontend::sha256_length_field((std::uint64_t{1} << 32U) + 5U) == Field{0, 0, 0, 0x08, 0, 0, 0, 0x28},
         "2^32+5 bytes = 2^35+40 bits, big endian");
}

void hex_helpers_are_strict() {
  const std::string digest = hex_of("abc");
  expect(secure_frontend::is_lower_hex_digest(digest), "a digest is lowercase hex");
  std::string upper = digest;
  upper[0] = 'B';
  expect(!secure_frontend::is_lower_hex_digest(upper), "uppercase rejected");
  expect(!secure_frontend::is_lower_hex_digest(digest.substr(1)), "63 characters rejected");
  expect(!secure_frontend::is_lower_hex_digest(digest + "0"), "65 characters rejected");
}

#if defined(SECURE_FRONTEND_TEST_HOOKS)
void length_counter_past_2_32_bytes() {
  secure_frontend::Sha256 hasher;
  secure_frontend::sha256_add_length_for_test(hasher, std::uint64_t{1} << 32U);
  hasher.update("abc");
  expect(hasher.bytes_hashed() == (std::uint64_t{1} << 32U) + 3U, "the 64-bit counter carries past 2^32 bytes");
  const auto first = hasher.finish();
  secure_frontend::Sha256 again;
  secure_frontend::sha256_add_length_for_test(again, std::uint64_t{1} << 32U);
  again.update("abc");
  expect(again.finish() == first, "deterministic");
  expect(first != secure_frontend::sha256("abc"), "the length field participates in the digest");
}
#endif

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"fips_180_4_short_vectors", fips_180_4_short_vectors},
      {"fips_180_4_million_a", fips_180_4_million_a},
      {"long_message_one_gibibyte", long_message_one_gibibyte},
      {"chunk_boundaries_do_not_change_digest", chunk_boundaries_do_not_change_digest},
      {"length_field_encodes_bits_big_endian", length_field_encodes_bits_big_endian},
      {"hex_helpers_are_strict", hex_helpers_are_strict},
#if defined(SECURE_FRONTEND_TEST_HOOKS)
      {"length_counter_past_2_32_bytes", length_counter_past_2_32_bytes},
#endif
  }, failures);
}
