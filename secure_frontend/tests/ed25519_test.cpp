#include "ed25519_verify.hpp"
#include "sha512.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

// Vectors from RFC 8032 section 7.1 (Ed25519 with SHA-512). Fetched from
// rfc-editor.org; every vector below was parsed from that text and
// independently re-derived - public key from the secret key, signature from
// the message, verification of both, and the negatives below exercised
// against the RFC's own section-6 reference Python before being inlined.
// The group order is L = 2^252 + 27742317777372353535851937790883648493.

std::vector<unsigned char> unhex(const std::string& hex) {
  auto nib = [](const char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
  };
  std::vector<unsigned char> out;
  for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
    out.push_back(static_cast<unsigned char>((nib(hex[i]) << 4) | nib(hex[i + 1])));
  }
  return out;
}

std::array<unsigned char, 32> key32(const std::vector<unsigned char>& v) {
  std::array<unsigned char, 32> out;
  std::copy(v.begin(), v.end(), out.begin());
  return out;
}

std::array<unsigned char, 64> sig64(const std::vector<unsigned char>& v) {
  std::array<unsigned char, 64> out;
  std::copy(v.begin(), v.end(), out.begin());
  return out;
}

// TEST 1: empty message.
std::vector<unsigned char> pubkey1() {
  return unhex(
      "d75a980182b10ab7d54bfed3c964073a"
      "0ee172f3daa62325af021a68f707511a");
}
std::vector<unsigned char> signature1() {
  return unhex(
      "e5564300c360ac729086e2cc806e828a"
      "84877f1eb8e5d974d873e06522490155"
      "5fb8821590a33bacc61e39701cf9b46b"
      "d25bf5f0595bbe24655141438e7a100b");
}

// TEST 2: the single octet 0x72.
std::vector<unsigned char> message2() {
  return unhex("72");
}
std::vector<unsigned char> pubkey2() {
  return unhex(
      "3d4017c3e843895a92b70aa74d1b7ebc"
      "9c982ccf2ec4968cc0cd55f12af4660c");
}
std::vector<unsigned char> signature2() {
  return unhex(
      "92a009a9f0d4cab8720e820b5f642540"
      "a2b27b5416503f8fb3762223ebdb69da"
      "085ac1e43e15996e458f3613d0f11d8c"
      "387b2eaeb4302aeeb00d291612bb0c00");
}

// TEST 3: the two octets 0xaf82.
std::vector<unsigned char> message3() {
  return unhex("af82");
}
std::vector<unsigned char> pubkey3() {
  return unhex(
      "fc51cd8e6218a1a38da47ed00230f058"
      "0816ed13ba3303ac5deb911548908025");
}
std::vector<unsigned char> signature3() {
  return unhex(
      "6291d657deec24024827e69c3abe01a3"
      "0ce548a284743a445e3680d7db5ac3ac"
      "18ff9b538d16f290ae67f760984dc659"
      "4a7c15e9716ed28dc027beceea1ec40a");
}

// TEST 1024: the 1023-bit message ("1024" names the message size in bits).
std::vector<unsigned char> message1024() {
  return unhex(
      "08b8b2b733424243760fe426a4b54908"
      "632110a66c2f6591eabd3345e3e4eb98"
      "fa6e264bf09efe12ee50f8f54e9f77b1"
      "e355f6c50544e23fb1433ddf73be84d8"
      "79de7c0046dc4996d9e773f4bc9efe57"
      "38829adb26c81b37c93a1b270b20329d"
      "658675fc6ea534e0810a4432826bf58c"
      "941efb65d57a338bbd2e26640f89ffbc"
      "1a858efcb8550ee3a5e1998bd177e93a"
      "7363c344fe6b199ee5d02e82d522c4fe"
      "ba15452f80288a821a579116ec6dad2b"
      "3b310da903401aa62100ab5d1a36553e"
      "06203b33890cc9b832f79ef80560ccb9"
      "a39ce767967ed628c6ad573cb116dbef"
      "efd75499da96bd68a8a97b928a8bbc10"
      "3b6621fcde2beca1231d206be6cd9ec7"
      "aff6f6c94fcd7204ed3455c68c83f4a4"
      "1da4af2b74ef5c53f1d8ac70bdcb7ed1"
      "85ce81bd84359d44254d95629e9855a9"
      "4a7c1958d1f8ada5d0532ed8a5aa3fb2"
      "d17ba70eb6248e594e1a2297acbbb39d"
      "502f1a8c6eb6f1ce22b3de1a1f40cc24"
      "554119a831a9aad6079cad88425de6bd"
      "e1a9187ebb6092cf67bf2b13fd65f270"
      "88d78b7e883c8759d2c4f5c65adb7553"
      "878ad575f9fad878e80a0c9ba63bcbcc"
      "2732e69485bbc9c90bfbd62481d9089b"
      "eccf80cfe2df16a2cf65bd92dd597b07"
      "07e0917af48bbb75fed413d238f5555a"
      "7a569d80c3414a8d0859dc65a46128ba"
      "b27af87a71314f318c782b23ebfe808b"
      "82b0ce26401d2e22f04d83d1255dc51a"
      "ddd3b75a2b1ae0784504df543af8969b"
      "e3ea7082ff7fc9888c144da2af58429e"
      "c96031dbcad3dad9af0dcbaaaf268cb8"
      "fcffead94f3c7ca495e056a9b47acdb7"
      "51fb73e666c6c655ade8297297d07ad1"
      "ba5e43f1bca32301651339e22904cc8c"
      "42f58c30c04aafdb038dda0847dd988d"
      "cda6f3bfd15c4b4c4525004aa06eeff8"
      "ca61783aacec57fb3d1f92b0fe2fd1a8"
      "5f6724517b65e614ad6808d6f6ee34df"
      "f7310fdc82aebfd904b01e1dc54b2927"
      "094b2db68d6f903b68401adebf5a7e08"
      "d78ff4ef5d63653a65040cf9bfd4aca7"
      "984a74d37145986780fc0b16ac451649"
      "de6188a7dbdf191f64b5fc5e2ab47b57"
      "f7f7276cd419c17a3ca8e1b939ae49e4"
      "88acba6b965610b5480109c8b17b80e1"
      "b7b750dfc7598d5d5011fd2dcc5600a3"
      "2ef5b52a1ecc820e308aa342721aac09"
      "43bf6686b64b2579376504ccc493d97e"
      "6aed3fb0f9cd71a43dd497f01f17c0e2"
      "cb3797aa2a2f256656168e6c496afc5f"
      "b93246f6b1116398a346f1a641f3b041"
      "e989f7914f90cc2c7fff357876e506b5"
      "0d334ba77c225bc307ba537152f3f161"
      "0e4eafe595f6d9d90d11faa933a15ef1"
      "369546868a7f3a45a96768d40fd9d034"
      "12c091c6315cf4fde7cb68606937380d"
      "b2eaaa707b4c4185c32eddcdd306705e"
      "4dc1ffc872eeee475a64dfac86aba41c"
      "0618983f8741c5ef68d3a101e8a3b8ca"
      "c60c905c15fc910840b94c00a0b9d0");
}
std::vector<unsigned char> pubkey1024() {
  return unhex(
      "278117fc144c72340f67d0f2316e8386"
      "ceffbf2b2428c9c51fef7c597f1d426e");
}
std::vector<unsigned char> signature1024() {
  return unhex(
      "0aab4c900501b3e24d7cdf4663326a3a"
      "87df5e4843b2cbdb67cbf6e460fec350"
      "aa5371b1508f9f4528ecea23c436d94b"
      "5e8fcd4f681e30a6ac00a9704a188a03");
}

// TEST 3's signature with S replaced by S + L: the same group scalar encoded
// non-canonically. The RFC reference accepts it when the S < L check is
// removed and rejects it with the check in place; RFC 8032 section 8.4 names
// exactly this malleation as the reason the check is required.
std::vector<unsigned char> signature3_noncanonical() {
  return unhex(
      "6291d657deec24024827e69c3abe01a3"
      "0ce548a284743a445e3680d7db5ac3ac"
      "05d391b0a77904e98404ef037747a56e"
      "4a7c15e9716ed28dc027beceea1ec41a");
}

void sha512_fips_example_digests() {
  // FIPS 180-4 examples (repeated by RFC 6234 section 8.5).
  expect(secure_frontend::to_hex(secure_frontend::sha512(std::string_view{"abc", 3})) ==
             "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
             "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
         "SHA-512(\"abc\")");
  expect(secure_frontend::to_hex(secure_frontend::sha512(std::string_view{})) ==
             "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
             "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e",
         "SHA-512(\"\")");
}

void sha512_million_a_streaming() {
  // FIPS 180-4: one million 'a' octets fed through the streaming interface in
  // 1000-octet pieces.
  secure_frontend::Sha512 hash;
  const std::vector<unsigned char> chunk(1000, 'a');
  for (int i = 0; i < 1000; ++i) {
    hash.update(chunk);
  }
  expect(hash.bytes_hashed() == 1000000, "the byte counter");
  expect(secure_frontend::to_hex(hash.finish()) ==
             "e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973eb"
             "de0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b",
         "SHA-512(1000000 a's)");
}

void sha512_chunk_boundaries_do_not_change_digest() {
  // The SHA-512 block is 128 octets; the digest must not depend on where the
  // update() calls are cut.
  std::vector<unsigned char> data(1000000, 'a');
  const std::string one_shot = secure_frontend::to_hex(secure_frontend::sha512(
      std::string_view{reinterpret_cast<const char*>(data.data()), data.size()}));
  for (const std::size_t chunk : {std::size_t{1}, std::size_t{3}, std::size_t{63}, std::size_t{64},
                                  std::size_t{65}, std::size_t{127}, std::size_t{128}, std::size_t{129}}) {
    secure_frontend::Sha512 hash;
    for (std::size_t off = 0; off < data.size(); off += chunk) {
      hash.update(std::span<const std::uint8_t>{data.data() + off, std::min(chunk, data.size() - off)});
    }
    expect(secure_frontend::to_hex(hash.finish()) == one_shot, "chunk size");
  }
}

void rfc8032_ed25519_official_vectors() {
  expect(secure_frontend::ed25519_verify(key32(pubkey1()), std::vector<unsigned char>{}, sig64(signature1())),
         "RFC 8032 section 7.1 TEST 1");
  expect(secure_frontend::ed25519_verify(key32(pubkey2()), message2(), sig64(signature2())),
         "RFC 8032 section 7.1 TEST 2");
  expect(secure_frontend::ed25519_verify(key32(pubkey3()), message3(), sig64(signature3())),
         "RFC 8032 section 7.1 TEST 3");
  expect(secure_frontend::ed25519_verify(key32(pubkey1024()), message1024(), sig64(signature1024())),
         "RFC 8032 section 7.1 TEST 1024");
}

void ed25519_rejects_tampered_message() {
  auto tampered = message2();
  tampered[0] = static_cast<unsigned char>(tampered[0] ^ 0x01);
  expect(!secure_frontend::ed25519_verify(key32(pubkey2()), tampered, sig64(signature2())), "flipped message bit");
  std::vector<unsigned char> longer = message2();
  longer.push_back(0x00);
  expect(!secure_frontend::ed25519_verify(key32(pubkey2()), longer, sig64(signature2())), "extended message");
  const std::vector<unsigned char> empty;
  expect(!secure_frontend::ed25519_verify(key32(pubkey1()), empty, sig64(signature2())),
         "TEST 2's signature over TEST 1's (empty) message");
}

void ed25519_rejects_tampered_signature() {
  auto tampered = signature2();
  tampered[0] = static_cast<unsigned char>(tampered[0] ^ 0x01);    // flips R
  expect(!secure_frontend::ed25519_verify(key32(pubkey2()), message2(), sig64(tampered)), "flipped R bit");
  tampered = signature2();
  tampered[40] = static_cast<unsigned char>(tampered[40] ^ 0x01);  // flips S
  expect(!secure_frontend::ed25519_verify(key32(pubkey2()), message2(), sig64(tampered)), "flipped S bit");
}

void ed25519_rejects_swapped_pubkey() {
  // Valid keys, valid signatures - just not matching pairs.
  expect(!secure_frontend::ed25519_verify(key32(pubkey2()), message3(), sig64(signature3())), "TEST 2 key, TEST 3 pair");
  expect(!secure_frontend::ed25519_verify(key32(pubkey3()), message2(), sig64(signature2())), "TEST 3 key, TEST 2 pair");
}

void ed25519_rejects_noncanonical_signature() {
  // S + L is the same group scalar in a non-canonical encoding; RFC 8032's
  // S < L check must reject it (§8.4). The reference implementation accepts
  // it only when that check is removed.
  expect(secure_frontend::ed25519_verify(key32(pubkey3()), message3(), sig64(signature3())), "canonical first");
  expect(!secure_frontend::ed25519_verify(key32(pubkey3()), message3(), sig64(signature3_noncanonical())),
         "S + L malleation");
}

void ed25519_rejects_invalid_encodings() {
  expect(!secure_frontend::ed25519_verify(key32(std::vector<unsigned char>(32, 0)), message2(), sig64(signature2())),
         "all-zero public key");
  std::vector<unsigned char> bad_pk = pubkey2();
  bad_pk[31] = static_cast<unsigned char>(bad_pk[31] ^ 0x80);  // flips the encoded x sign bit
  expect(!secure_frontend::ed25519_verify(key32(bad_pk), message2(), sig64(signature2())), "flipped key sign bit");
  std::vector<unsigned char> bad_sig = signature2();
  bad_sig[31] = static_cast<unsigned char>(bad_sig[31] ^ 0x80);  // an S of 2^255 scale is never < L
  expect(!secure_frontend::ed25519_verify(key32(pubkey2()), message2(), sig64(bad_sig)), "huge S encoding");
  expect(!secure_frontend::ed25519_verify(key32(pubkey2()), message2(), sig64(std::vector<unsigned char>(64, 0))),
         "all-zero signature");
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"sha512_fips_example_digests", sha512_fips_example_digests},
      {"sha512_million_a_streaming", sha512_million_a_streaming},
      {"sha512_chunk_boundaries_do_not_change_digest", sha512_chunk_boundaries_do_not_change_digest},
      {"rfc8032_ed25519_official_vectors", rfc8032_ed25519_official_vectors},
      {"ed25519_rejects_tampered_message", ed25519_rejects_tampered_message},
      {"ed25519_rejects_tampered_signature", ed25519_rejects_tampered_signature},
      {"ed25519_rejects_swapped_pubkey", ed25519_rejects_swapped_pubkey},
      {"ed25519_rejects_noncanonical_signature", ed25519_rejects_noncanonical_signature},
      {"ed25519_rejects_invalid_encodings", ed25519_rejects_invalid_encodings},
  }, failures);
}
