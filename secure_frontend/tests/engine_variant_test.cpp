#include "secure_frontend/engine_variant.hpp"
#include "secure_frontend/sha256.hpp"
#include "support/test_support.hpp"
#include <fstream>
#include <iostream>

namespace {
using namespace secure_frontend;
int failures = 0;
void expect(bool ok, const char* reason) { if (!ok) { ++failures; std::cerr << "FAILED: " << reason << '\n'; } }

void write(const std::filesystem::path& path, const std::string& text) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << text;
}
struct Fixture {
  std::filesystem::path root{secure_frontend_test::make_scratch_directory()};
  std::string basename{
#if defined(_WIN32)
      "multiengine.exe"
#else
      "multiengine"
#endif
  };
  std::string binary{"fast binary"}, library{std::string(150000U, 'x')};
  Fixture() {
    std::filesystem::create_directory(root / "fast-math");
    write(root / basename, "accurate binary");
    write(root / "fast-math" / basename, binary);
    write(root / "fast-math" / "family.dll", library);
  }
  CanonicalFile accurate() const { return std::get<CanonicalFile>(open_executable(root / basename)); }
  std::string record(const std::string& path, const std::string& contents) const {
    return "{\"path\":\"" + path + "\",\"bytes\":" + std::to_string(contents.size()) +
           ",\"sha256\":\"" + to_hex(sha256(contents)) + "\"}";
  }
  std::string manifest(const std::string& files = {}, const std::string& mode = "true", const std::string& directory = "fast-math") const {
    return "{\"schema_version\":1,\"variants\":[{\"name\":\"accurate\",\"directory\":\".\",\"fast_math\":false,\"executable\":\"" +
      basename + "\",\"files\":[" + record(basename, "accurate binary") + "]},{\"name\":\"fast-math\",\"directory\":\"" + directory +
      "\",\"fast_math\":" + mode + ",\"executable\":\"" + basename + "\",\"files\":[" +
      (files.empty() ? record(basename, binary) + "," + record("family.dll", library) : files) + "]}]}";
  }
  bool rejected() const { return std::holds_alternative<std::string>(select_engine_variant(accurate(), true)); }
};

void default_needs_no_variant_installation() {
  Fixture f;
  const auto result = select_engine_variant(f.accurate(), false);
  const auto* file = std::get_if<CanonicalFile>(&result);
  expect(file && file->path() == f.accurate().path(), "accuracy default returns original without manifest");
}
void missing_or_wrong_mode_is_refused() {
  Fixture f;
  expect(f.rejected(), "missing manifest refuses fast selection");
  write(f.root / "variants.json", f.manifest({}, "false"));
  expect(f.rejected(), "false fast-math marker refuses fast selection");
  write(f.root / "variants.json", f.manifest({}, "true", "elsewhere"));
  expect(f.rejected(), "fixed variant directory required");
  write(f.root / "variants.json", std::string(256U * 1024U + 1U, ' '));
  expect(f.rejected(), "oversized manifest refused");
  auto duplicate_key = f.manifest();
  duplicate_key.insert(1U, "\"schema_version\":1,");
  write(f.root / "variants.json", duplicate_key);
  expect(f.rejected(), "duplicate JSON object keys refused by strict parser");
  auto wrong_schema = f.manifest();
  wrong_schema.replace(wrong_schema.find("schema_version\":1"), std::string{"schema_version\":1"}.size(), "schema_version\":2");
  write(f.root / "variants.json", wrong_schema);
  expect(f.rejected(), "unknown manifest schema refused");
}
void valid_fast_variant_verifies_every_file() {
  Fixture f;
  write(f.root / "variants.json", f.manifest());
  const auto result = select_engine_variant(f.accurate(), true);
  const auto* file = std::get_if<CanonicalFile>(&result);
  expect(file && file->path() == f.root / "fast-math" / f.basename, "verified fast executable selected");
  f.library[80000] = 'y';
  write(f.root / "fast-math" / "family.dll", f.library);
  expect(f.rejected(), "same-size sidecar corruption refuses entire variant");
  write(f.root / "fast-math" / "family.dll", "shortened");
  expect(f.rejected(), "sidecar size mismatch refuses entire variant");
}
void invalid_manifest_paths_and_duplicates_are_refused() {
  Fixture f;
  const auto exe = f.record(f.basename, f.binary);
  for (const auto& bad : {exe + "," + exe, exe + "," + f.record("../escape", "x"),
                          f.record("family.dll", f.library), exe + "," + f.record("missing.dll", "x")}) {
    write(f.root / "variants.json", f.manifest(bad));
    expect(f.rejected(), "duplicate, traversal, unlisted executable or missing sidecar refused");
  }
  write(f.root / "variants.json", f.manifest());
  std::filesystem::rename(f.root / "fast-math" / "family.dll", f.root / "absent-sidecar.dll");
  expect(f.rejected(), "missing listed sidecar refuses otherwise valid manifest");
}
void linked_variant_file_is_refused() {
  Fixture f;
  write(f.root / "variants.json", f.manifest());
  std::filesystem::rename(f.root / "fast-math" / "family.dll", f.root / "original.dll");
  std::error_code error;
  std::filesystem::create_hard_link(f.root / "original.dll", f.root / "fast-math" / "family.dll", error);
  expect(!error, "hard-link fixture created");
  if (!error) expect(f.rejected(), "hardlinked sidecar refused");
#if !defined(_WIN32)
  Fixture linked;
  write(linked.root / "variants.json", linked.manifest());
  std::filesystem::rename(linked.root / "fast-math", linked.root / "redirected");
  std::filesystem::create_directory_symlink(linked.root / "redirected", linked.root / "fast-math");
  expect(linked.rejected(), "linked variant directory cannot bypass same-installation containment");
#endif
}
}  // namespace
int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
    {"default_needs_no_variant_installation", default_needs_no_variant_installation},
    {"missing_or_wrong_mode_is_refused", missing_or_wrong_mode_is_refused},
    {"valid_fast_variant_verifies_every_file", valid_fast_variant_verifies_every_file},
    {"invalid_manifest_paths_and_duplicates_are_refused", invalid_manifest_paths_and_duplicates_are_refused},
    {"linked_variant_file_is_refused", linked_variant_file_is_refused}}, failures);
}
