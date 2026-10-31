// Trust by key (plugin spec 2026-10-30, section 11; the owner's decision 8): a plugin.sig written by
// scripts/plugin_signing.py (the committed fixtures, signed with RFC 8032's TEST 1 key) is read and verified here - taken
// only under a key this server trusts and only for the folder it was made for - and every other one says why.
#include "secure_frontend/plugin_signature.hpp"
#include "secure_frontend/sha256.hpp"

#include "support/test_support.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

std::string fixture(const std::string& name) {
  std::ifstream in(std::string{SECURE_FRONTEND_PLUGIN_FIXTURES} + "/signedtool/" + name, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string sha256_hex(const std::string& bytes) {
  secure_frontend::Sha256 hash;
  hash.update(bytes);
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  for (const auto byte : hash.finish()) {
    out += kDigits[byte >> 4U];
    out += kDigits[byte & 0x0FU];
  }
  return out;
}

const std::string kRfcPublic = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";
const std::string kOtherPublic = "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c";  // RFC 8032 TEST 2

#if defined(_WIN32)
const std::string kProgram = "signed-tool.exe";
#else
const std::string kProgram = "signed-tool";
#endif

struct Folder final {
  std::string envelope = fixture(std::string{"plugin.sig."} + std::string{secure_frontend::plugin_signature_platform()});
  std::string manifest = sha256_hex(fixture("plugin.json"));
  std::map<std::string, std::string> files{{kProgram, sha256_hex(fixture("program"))}};
};

const std::vector<secure_frontend::TrustedKey> kTrusted{{.key = kRfcPublic, .name = "Example Labs", .install = false}};

void the_statement_is_the_signers() {
  const Folder folder;
  expect(!folder.envelope.empty(), "the committed fixture is there");
  const std::string statement = secure_frontend::plugin_signature_statement(kRfcPublic, "signedtool", secure_frontend::plugin_signature_platform(),
                                                                            folder.manifest, folder.files);
  expect(folder.envelope.rfind(statement + "\n--STRATA-SIGNATURE-V1\n", 0) == 0U, "the statement is the one plugin_signing.py signed: " + statement);
  expect(secure_frontend::plugin_signature_statement(kRfcPublic, "lookup", "linux", std::string(64, 'a'),
                                                     {{"lookup", std::string(64, 'b')}, {"data/x.bin", std::string(64, 'c')}}) ==
             "m-strata.plugin-signature/1\nkey " + kRfcPublic + "\nid lookup\nplatform linux\nmanifest " + std::string(64, 'a') +
                 "\nfile data/x.bin " + std::string(64, 'c') + "\nfile lookup " + std::string(64, 'b') + "\n",
         "files sorted by name, each line ending in LF");
  expect(secure_frontend::valid_signing_key(kRfcPublic) && !secure_frontend::valid_signing_key(kRfcPublic.substr(1)) &&
             !secure_frontend::valid_signing_key(std::string(64, 'A')),
         "a key is 64 lowercase hex");
}

void a_signature_under_a_trusted_key_is_taken() {
  const Folder folder;
  const auto checked = secure_frontend::check_plugin_signature(folder.envelope, "signedtool", folder.manifest, folder.files, kTrusted);
  expect(checked.key == kRfcPublic && checked.signer == "Example Labs" && checked.problem.empty(),
         "verified, by the key's name: " + checked.signer + " / " + checked.problem);
}

void every_other_signature_says_why() {
  const Folder folder;
  const auto untrusted = secure_frontend::check_plugin_signature(folder.envelope, "signedtool", folder.manifest, folder.files,
                                                                 {{.key = kOtherPublic, .name = "Someone else", .install = false}});
  expect(untrusted.key == kRfcPublic && untrusted.signer.empty() && has(untrusted.problem, "does not trust"),
         "a key this server does not trust: " + untrusted.problem);
  auto changed = folder.files;
  changed[kProgram] = std::string(64, '0');
  const auto other_files = secure_frontend::check_plugin_signature(folder.envelope, "signedtool", folder.manifest, changed, kTrusted);
  expect(other_files.signer.empty() && has(other_files.problem, "changed since it was signed"), "a file changed: " + other_files.problem);
  const auto other_manifest = secure_frontend::check_plugin_signature(folder.envelope, "signedtool", std::string(64, '1'), folder.files, kTrusted);
  expect(other_manifest.signer.empty() && has(other_manifest.problem, "changed since it was signed"), "the manifest changed: " + other_manifest.problem);
  const auto other_id = secure_frontend::check_plugin_signature(folder.envelope, "lookup", folder.manifest, folder.files, kTrusted);
  expect(other_id.signer.empty() && has(other_id.problem, "another plugin"), "signed for another plugin: " + other_id.problem);
  const std::string other_platform_envelope =
      fixture(secure_frontend::plugin_signature_platform() == "windows" ? "plugin.sig.linux" : "plugin.sig.windows");
  const auto other_platform = secure_frontend::check_plugin_signature(other_platform_envelope, "signedtool", folder.manifest, folder.files, kTrusted);
  expect(other_platform.signer.empty() && has(other_platform.problem, "another platform"), "signed for the other platform: " + other_platform.problem);
  std::string flipped = folder.envelope;
  char& changed_char = flipped[flipped.find("--STRATA-SIGNATURE-V1\n") + 26U];  // another valid base64 character
  changed_char = changed_char == 'A' ? 'B' : 'A';
  const auto forged = secure_frontend::check_plugin_signature(flipped, "signedtool", folder.manifest, folder.files, kTrusted);
  expect(forged.signer.empty() && has(forged.problem, "does not verify"), "a changed signature: " + forged.problem);
  std::string edited = folder.envelope;
  edited.replace(edited.find("id signedtool"), 13, "id signedtoox");
  const auto forged_statement = secure_frontend::check_plugin_signature(edited, "signedtoox", folder.manifest, folder.files, kTrusted);
  expect(forged_statement.signer.empty() && has(forged_statement.problem, "does not verify"), "an edited statement: " + forged_statement.problem);
  for (const std::string& junk : {std::string{"not a signature"}, std::string{"m-strata.plugin-signature/1\nkey x\n"},
                                 "m-strata.plugin-signature/1\n--STRATA-SIGNATURE-V1\n" + std::string(88, 'A') + "\n"}) {
    const auto read = secure_frontend::check_plugin_signature(junk, "signedtool", folder.manifest, folder.files, kTrusted);
    expect(read.signer.empty() && !read.problem.empty(), "not a signature: " + read.problem);
  }
  const auto huge = secure_frontend::check_plugin_signature(std::string(secure_frontend::kMaxPluginSignatureBytes + 1U, 'x'), "signedtool",
                                                            folder.manifest, folder.files, kTrusted);
  expect(has(huge.problem, "16 KiB"), "a file larger than 16 KiB: " + huge.problem);
}

// Before any file is looked at (spec section 12: a plugin added from the page is trusted first): a signature under a
// trusted key, for this plugin and this platform, gives the manifest's and files' SHA-256s they must have - and anything
// else says why, in check_plugin_signature's words.
void a_trusted_statement_is_read_before_the_files() {
  const Folder folder;
  const auto read = secure_frontend::read_trusted_statement(folder.envelope, "signedtool", kTrusted);
  const auto* statement = std::get_if<secure_frontend::TrustedStatement>(&read);
  expect(statement != nullptr, "read: " + (statement == nullptr ? std::get<std::string>(read) : std::string{}));
  if (statement != nullptr) {
    expect(statement->key == kRfcPublic && statement->signer == "Example Labs", "its key and signer");
    expect(statement->manifest_sha256 == folder.manifest && statement->files == folder.files, "the SHA-256s the files must have");
  }
  const auto problem = [](const std::variant<secure_frontend::TrustedStatement, std::string>& r) {
    return std::holds_alternative<std::string>(r) ? std::get<std::string>(r) : std::string{"(read)"};
  };
  std::string named;
  expect(has(problem(secure_frontend::read_trusted_statement(
                 folder.envelope, "signedtool", {{.key = kOtherPublic, .name = "Someone", .install = false}}, &named)),
             "does not trust"),
         "under a key not trusted");
  expect(named == kRfcPublic, "a refusal can name the key it was signed with");
  expect(has(problem(secure_frontend::read_trusted_statement(folder.envelope, "lookup", kTrusted)), "another plugin"), "for another plugin");
  const std::string other_platform =
      fixture(secure_frontend::plugin_signature_platform() == "windows" ? "plugin.sig.linux" : "plugin.sig.windows");
  expect(has(problem(secure_frontend::read_trusted_statement(other_platform, "signedtool", kTrusted)), "another platform"),
         "for another platform");
  std::string flipped = folder.envelope;
  char& changed_char = flipped[flipped.find("--STRATA-SIGNATURE-V1\n") + 26U];
  changed_char = changed_char == 'A' ? 'B' : 'A';
  expect(has(problem(secure_frontend::read_trusted_statement(flipped, "signedtool", kTrusted)), "does not verify"), "a changed signature");
  expect(has(problem(secure_frontend::read_trusted_statement("not a signature", "signedtool", kTrusted)), "not a signature"),
         "not a signature");
  expect(has(problem(secure_frontend::read_trusted_statement(std::string(secure_frontend::kMaxPluginSignatureBytes + 1U, 'x'),
                                                             "signedtool", kTrusted)),
             "16 KiB"),
         "larger than 16 KiB");
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"the_statement_is_the_signers", the_statement_is_the_signers},
      {"a_signature_under_a_trusted_key_is_taken", a_signature_under_a_trusted_key_is_taken},
      {"a_trusted_statement_is_read_before_the_files", a_trusted_statement_is_read_before_the_files},
      {"every_other_signature_says_why", every_other_signature_says_why},
  }, failures);
}
