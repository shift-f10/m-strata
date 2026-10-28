#include "secure_frontend/secret_bytes.hpp"
#include "secure_frontend/token_generator.hpp"
#include "secure_frontend/token_verifier.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <new>
#include <set>
#include <string>
#include <utility>
#include <variant>

namespace {
int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}

std::string encode(const std::string& bytes) {
  secure_frontend::SecretBytes out;
  expect(secure_frontend::base64url_encode(bytes, out), "encode fits");
  return std::string{out.view()};
}

void base64url_known_vectors() {
  // RFC 4648 §10 vectors, unpadded, plus the two URL-safe characters.
  expect(encode("").empty(), "empty");
  expect(encode("f") == "Zg", "f");
  expect(encode("fo") == "Zm8", "fo");
  expect(encode("foo") == "Zm9v", "foo");
  expect(encode("foob") == "Zm9vYg", "foob");
  expect(encode("fooba") == "Zm9vYmE", "fooba");
  expect(encode("foobar") == "Zm9vYmFy", "foobar");
  expect(encode(std::string{"\xfb\xff"}) == "-_8", "uses - and _ instead of + and /");
}

void token_generate_is_43_base64url_chars() {
  const auto result = secure_frontend::generate_token();
  const auto* token = std::get_if<secure_frontend::SecretBytes>(&result);
  expect(token != nullptr, "the OS CSPRNG is available");
  if (token == nullptr) return;
  expect(token->size() == secure_frontend::kGeneratedTokenCharacters, "32 random bytes encode to 43 characters");
  const std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  expect(std::all_of(token->view().begin(), token->view().end(),
                     [&](const char c) { return alphabet.find(c) != std::string_view::npos; }),
         "only base64url characters, no padding, no prefix");
}

void token_generate_differs_across_calls() {
  std::set<std::string> seen;
  for (int i = 0; i < 32; ++i) {
    auto result = secure_frontend::generate_token();
    seen.insert(std::string{std::get<secure_frontend::SecretBytes>(result).view()});
  }
  expect(seen.size() == 32U, "32 generated tokens are all distinct");
}

bool all_zero(const unsigned char* bytes, const std::size_t size) {
  return std::all_of(bytes, bytes + size, [](const unsigned char b) { return b == 0U; });
}

void secret_bytes_wiped_on_destroy_and_move() {
  alignas(secure_frontend::SecretBytes) unsigned char storage[sizeof(secure_frontend::SecretBytes)];
  auto* secret = new (storage) secure_frontend::SecretBytes();
  expect(secret->assign("SENSITIVE-0123456789"), "assign fits");
  secure_frontend::SecretBytes moved = std::move(*secret);
  expect(moved.view() == "SENSITIVE-0123456789", "the value moves");
  expect(secret->empty() && all_zero(secret->storage_for_tests(), secure_frontend::SecretBytes::kCapacity),
         "a moved-from SecretBytes holds only zeros (no SSO remnant)");
  expect(secret->assign("THIRD-SECRET-VALUE"), "reassign");
  secret->~SecretBytes();
  const std::string_view marker = "THIRD-SECRET";
  expect(std::search(storage, storage + sizeof(storage), marker.begin(), marker.end()) == storage + sizeof(storage),
         "destroyed storage no longer holds the secret");
  moved.clear();
  expect(moved.empty() && all_zero(moved.storage_for_tests(), secure_frontend::SecretBytes::kCapacity), "clear wipes");
}

void secret_bytes_rejects_over_capacity() {
  secure_frontend::SecretBytes secret;
  expect(!secret.assign(std::string(secure_frontend::SecretBytes::kCapacity + 1U, 'x')) && secret.empty(),
         "a value over 256 bytes is refused, not truncated");
  expect(secret.assign(std::string(secure_frontend::SecretBytes::kCapacity, 'x')), "exactly 256 bytes fits");
}

void auth03_compare_touches_fixed_window() {
  const std::string stored(43, 's');
  for (const std::size_t length : {std::size_t{1}, std::size_t{43}, std::size_t{256}}) {
    std::size_t compared = 0;
    (void)secure_frontend::detail::fixed_window_equals(std::string(length, 'p'), stored, &compared);
    expect(compared == secure_frontend::kTokenWindowBytes,
           "every comparison touches all 256 bytes, input length " + std::to_string(length));
  }
  std::size_t compared = 99;
  expect(!secure_frontend::detail::fixed_window_equals(std::string(257, 'p'), stored, &compared) && compared == 0U,
         "a value over the public 256-byte maximum is rejected before comparing");
}

void auth03_prefix_and_suffix_variants_rejected() {
  const std::string stored = "auth03-token-0123456789abcdefghijklmnopqrs";
  secure_frontend::SecretBytes secret;
  expect(secret.assign(stored), "fits");
  const secure_frontend::TokenVerifier verifier(std::move(secret));
  expect(verifier.matches(stored), "the exact token matches");
  std::string first = stored; first.front() = 'X';
  std::string last = stored; last.back() = 'X';
  for (const std::string& variant : {stored.substr(0, stored.size() - 1), stored + "x", "x" + stored, first, last,
                                     std::string{}, stored + std::string(1, '\0')}) {
    expect(!verifier.matches(variant), "a prefix, suffix or one-byte variant must not match");
  }
  const secure_frontend::TokenVerifier empty{secure_frontend::SecretBytes{}};
  expect(!empty.matches(""), "an empty stored token never matches, not even an empty one");
}
}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"base64url_known_vectors", base64url_known_vectors},
      {"token_generate_is_43_base64url_chars", token_generate_is_43_base64url_chars},
      {"token_generate_differs_across_calls", token_generate_differs_across_calls},
      {"secret_bytes_wiped_on_destroy_and_move", secret_bytes_wiped_on_destroy_and_move},
      {"secret_bytes_rejects_over_capacity", secret_bytes_rejects_over_capacity},
      {"auth03_compare_touches_fixed_window", auth03_compare_touches_fixed_window},
      {"auth03_prefix_and_suffix_variants_rejected", auth03_prefix_and_suffix_variants_rejected},
  }, failures);
}
