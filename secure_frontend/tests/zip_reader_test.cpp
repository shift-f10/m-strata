// The zip reader (plugin spec 2026-10-30 section 12): archives Python's zipfile wrote list and extract to their bytes
// (by size and SHA-256), and every hostile archive - made byte by byte by tools/secure/make_zip_fixtures.py - is refused
// with its reason, before anything is extracted. The fixture folder is the case's argument.
#include "secure_frontend/sha256.hpp"
#include "secure_frontend/zip_reader.hpp"
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

std::vector<std::uint8_t> fixture(const std::string_view name) {
  const auto& args = secure_frontend_test::extra_arguments();
  const std::filesystem::path folder = args.empty() ? std::filesystem::path{} : std::filesystem::path{args.front()};
  std::ifstream in(folder / (std::string{name} + ".zip"), std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

struct Expected final {
  std::string_view file;
  std::uint64_t size;
  std::string_view sha256;
};
constexpr std::array<Expected, 3> kFiles{{
    {"plugin.json", 64U, "34a628acb4a6d651bbceb33ffeb05b1167e070cb23f956d4b4068890014e8467"},
    {"plugin.sig", 4U, "0a017e570e0b4626683b04224173c91586bdcff8767c95ed58078dc0a693896c"},
    {"lookup", 10257U, "013b6d80908782684bfcd92896b14559c953e31a61e3ebde79ff870d2349267f"},
}};

// An entry's bytes: their size and SHA-256, or why not.
std::variant<std::pair<std::uint64_t, std::string>, std::string> extracted(const std::vector<std::uint8_t>& bytes,
                                                                           const secure_frontend::ZipEntry& entry) {
  secure_frontend::Sha256 hasher;
  std::uint64_t size = 0;
  const auto failed = secure_frontend::extract_zip_entry(bytes, entry, [&](const std::span<const std::uint8_t> chunk) {
    hasher.update(chunk);
    size += chunk.size();
    return true;
  });
  if (failed) return *failed;
  return std::pair{size, secure_frontend::to_hex(hasher.finish())};
}

// zipfile's archives, deflated at the root and stored in one top folder: the entries, then their bytes.
void zipfiles_archives_list_and_extract() {
  for (const auto& [name, prefix] : {std::pair<std::string_view, std::string_view>{"flat_deflated", ""}, {"folder_stored", "lookup/"}}) {
    const auto bytes = fixture(name);
    expect(!bytes.empty(), std::string{name} + ": its fixture is there");
    const auto read = secure_frontend::read_zip(bytes);
    const auto* archive = std::get_if<secure_frontend::ZipArchive>(&read);
    expect(archive != nullptr, std::string{name} + ": read" + (archive == nullptr ? ": " + std::get<std::string>(read) : ""));
    if (archive == nullptr) continue;
    expect(archive->entries.size() == kFiles.size(), std::string{name} + ": three entries");
    for (std::size_t i = 0; i < archive->entries.size() && i < kFiles.size(); ++i) {
      const auto& entry = archive->entries[i];
      expect(entry.name == std::string{prefix} + std::string{kFiles[i].file}, std::string{name} + ": entry " + entry.name);
      expect(entry.method == (name == "flat_deflated" ? 8U : 0U), std::string{name} + ": its method");
      const auto got = extracted(bytes, entry);
      const auto* done = std::get_if<std::pair<std::uint64_t, std::string>>(&got);
      expect(done != nullptr && done->first == kFiles[i].size && done->second == kFiles[i].sha256,
             std::string{name} + ": " + entry.name + "'s bytes" + (done == nullptr ? ": " + std::get<std::string>(got) : ""));
    }
  }
}

// Each hostile archive is refused when it is read, saying why (the words a person sees on the page).
void hostile_archives_are_refused_with_their_reason() {
  const std::array<std::pair<std::string_view, std::string_view>, 17> cases{{
      {"traversal", "name"},          {"absolute", "name"},     {"drive", "name"},
      {"backslash", "name"},          {"control", "name"},      {"empty_name", "name"},
      {"symlink", "link"},            {"encrypted", "encrypted"}, {"zip64", "ZIP64"},
      {"bad_method", "compression"},  {"truncated", "zip"},     {"no_end", "zip"},
      {"overlap", "overlap"},         {"local_disagrees", "local header"}, {"too_many", "entries"},
      {"bomb", "times"},              {"duplicate", "twice"},
  }};
  for (const auto& [name, words] : cases) {
    const auto read = secure_frontend::read_zip(fixture(name));
    const auto* refused = std::get_if<std::string>(&read);
    expect(refused != nullptr && refused->find(words) != std::string::npos,
           std::string{name} + ": " + (refused == nullptr ? std::string{"read"} : *refused));
  }
  // a CRC that does not match is found when the entry is extracted
  const auto bytes = fixture("bad_crc");
  const auto read = secure_frontend::read_zip(bytes);
  const auto* archive = std::get_if<secure_frontend::ZipArchive>(&read);
  expect(archive != nullptr && archive->entries.size() == 1U, "bad_crc: read (its directory is sound)");
  if (archive != nullptr && !archive->entries.empty()) {
    const auto got = extracted(bytes, archive->entries.front());
    expect(std::holds_alternative<std::string>(got) && std::get<std::string>(got).find("CRC") != std::string::npos, "bad_crc: refused when extracted");
  }
  expect(std::holds_alternative<std::string>(secure_frontend::read_zip({})), "no bytes: refused");
}

// CRC-32's check value (the "123456789" of every CRC catalogue), streamed in two parts.
void crc32_is_zips() {
  secure_frontend::Crc32 crc;
  const std::string_view text = "123456789";
  crc.update(std::span{reinterpret_cast<const std::uint8_t*>(text.data()), 4U});
  crc.update(std::span{reinterpret_cast<const std::uint8_t*>(text.data()) + 4, 5U});
  expect(crc.value() == 0xCBF43926U, "CRC-32 of 123456789 is CBF43926");
  expect(secure_frontend::Crc32{}.value() == 0U, "of nothing, 0");
}

// Every prefix of a good archive, and every one-byte change of it, is read (and its entries extracted) or refused -
// never past its bytes (the sanitizers' builds show an out-of-bounds read).
void damaged_archives_end_cleanly() {
  const auto good = fixture("flat_deflated");
  const auto attempt = [](const std::vector<std::uint8_t>& bytes) {
    const auto read = secure_frontend::read_zip(bytes);
    if (const auto* archive = std::get_if<secure_frontend::ZipArchive>(&read)) {
      for (const auto& entry : archive->entries) (void)extracted(bytes, entry);
    }
  };
  for (std::size_t cut = 0; cut < good.size(); ++cut) attempt(std::vector<std::uint8_t>(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(cut)));
  for (std::size_t i = 0; i < good.size(); ++i) {
    for (const std::uint8_t flip : {std::uint8_t{0x01}, std::uint8_t{0x80}, std::uint8_t{0xFF}}) {
      auto changed = good;
      changed[i] = static_cast<std::uint8_t>(changed[i] ^ flip);
      attempt(changed);
    }
  }
  expect(true, "");
}
}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"zipfiles_archives_list_and_extract", zipfiles_archives_list_and_extract},
      {"hostile_archives_are_refused_with_their_reason", hostile_archives_are_refused_with_their_reason},
      {"crc32_is_zips", crc32_is_zips},
      {"damaged_archives_end_cleanly", damaged_archives_end_cleanly},
  }, failures);
}
