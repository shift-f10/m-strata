// Adding a plugin from the page and removing one (plugin_install.hpp; plugin spec 2026-10-30 section 12): only a plugin
// signed by a key this server trusts is added, from a folder or a .zip, its signature and every SHA-256 checked before
// anything is written, and the next start's discovery takes it; a removed one is moved aside, from where it is put back
// or deleted for good; a built-in one is refused. The plugin is the signature test's fixture (signed with RFC 8032's
// TEST 1 key); the zips are written here, stored, as any archiver writes one (the zip reader's tests cover deflated ones).
#include "secure_frontend/plugin_install.hpp"
#include "secure_frontend/plugin_registry.hpp"
#include "secure_frontend/zip_reader.hpp"

#include "support/test_support.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace {

namespace fs = std::filesystem;
using secure_frontend::AddedPlugin;
using secure_frontend::RemovedPlugin;

int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

template <typename T>
std::string problem_of(const std::variant<T, std::string>& result) {
  return std::holds_alternative<std::string>(result) ? std::get<std::string>(result) : std::string{"(taken)"};
}

std::string fixture(const std::string& name) {
  std::ifstream in(std::string{SECURE_FRONTEND_PLUGIN_FIXTURES} + "/signedtool/" + name, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
std::string platform_signature() {
  return fixture(std::string{"plugin.sig."} + std::string{secure_frontend::plugin_signature_platform()});
}

#if defined(_WIN32)
const std::string kProgram = "signed-tool.exe";
#else
const std::string kProgram = "signed-tool";
#endif
const std::string kRfcPublic = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";
const std::string kOtherPublic = "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c";  // RFC 8032 TEST 2
const std::vector<secure_frontend::TrustedKey> kTrusted{{.key = kRfcPublic, .name = "Example Labs", .install = false}};
const std::chrono::system_clock::time_point kNoon = std::chrono::sys_days{std::chrono::year{2026} / 10 / 30} + std::chrono::hours{12};
const std::string kNoonEntry = "signedtool-20261030T120000Z";

// An install's plugins folder (owner-only, as make leaves it), and scratch folders beside it to add from.
struct Install final {
  fs::path root = secure_frontend_test::make_scratch_directory();
  fs::path plugins = root / "plugins";
  Install() { secure_frontend_test::make_owner_private_directory(plugins); }

  // The signed plugin as a folder: plugin.json, plugin.sig and this platform's program.
  [[nodiscard]] fs::path folder(const std::string& name) const {
    const auto at = root / name;
    fs::create_directories(at);
    secure_frontend_test::write_file(at / "plugin.json", fixture("plugin.json"));
    secure_frontend_test::write_file(at / "plugin.sig", platform_signature());
    secure_frontend_test::write_file(at / kProgram, fixture("program"), 0700);
    return at;
  }
  // The same, put in the plugins folder by hand.
  void by_hand() const {
    const auto at = plugins / "signedtool";
    secure_frontend_test::make_owner_private_directory(at);
    secure_frontend_test::write_file(at / "plugin.json", fixture("plugin.json"));
    secure_frontend_test::write_file(at / "plugin.sig", platform_signature());
    secure_frontend_test::write_file(at / kProgram, fixture("program"), 0700);
  }
  // No plugin folder, nothing left in staging.
  [[nodiscard]] bool nothing_added() const {
    std::error_code ec;
    if (fs::exists(plugins / "signedtool", ec)) return false;
    const auto staging = plugins / ".staging";
    return !fs::exists(staging, ec) || fs::is_empty(staging, ec);
  }
};

struct ZipItem final {
  std::string name;
  std::string bytes;
};
void put16(std::string& out, const std::size_t value) {
  out += static_cast<char>(value & 0xFFU);
  out += static_cast<char>((value >> 8U) & 0xFFU);
}
void put32(std::string& out, const std::size_t value) {
  put16(out, value & 0xFFFFU);
  put16(out, (value >> 16U) & 0xFFFFU);
}
// A .zip of stored entries: each local header and its bytes, the central directory, the end record (APPNOTE 4.3).
std::string make_zip(const std::vector<ZipItem>& items) {
  std::string out;
  std::string directory;
  for (const auto& item : items) {
    secure_frontend::Crc32 crc;
    crc.update({reinterpret_cast<const std::uint8_t*>(item.bytes.data()), item.bytes.size()});
    const std::size_t offset = out.size();
    put32(out, 0x04034B50U);
    put16(out, 20U);    // version needed
    put16(out, 0U);     // flags
    put16(out, 0U);     // stored
    put16(out, 0U);     // time
    put16(out, 0x21U);  // 1980-01-01
    put32(out, crc.value());
    put32(out, item.bytes.size());
    put32(out, item.bytes.size());
    put16(out, item.name.size());
    put16(out, 0U);
    out += item.name;
    out += item.bytes;
    put32(directory, 0x02014B50U);
    put16(directory, 20U);  // made by: MS-DOS, 2.0
    put16(directory, 20U);
    put16(directory, 0U);
    put16(directory, 0U);
    put16(directory, 0U);
    put16(directory, 0x21U);
    put32(directory, crc.value());
    put32(directory, item.bytes.size());
    put32(directory, item.bytes.size());
    put16(directory, item.name.size());
    put16(directory, 0U);  // extra
    put16(directory, 0U);  // comment
    put16(directory, 0U);  // disk
    put16(directory, 0U);  // internal attributes
    put32(directory, 0U);  // external attributes
    put32(directory, offset);
    directory += item.name;
  }
  const std::size_t at = out.size();
  out += directory;
  put32(out, 0x06054B50U);
  put16(out, 0U);
  put16(out, 0U);
  put16(out, items.size());
  put16(out, items.size());
  put32(out, directory.size());
  put32(out, at);
  put16(out, 0U);
  return out;
}
std::vector<ZipItem> signed_items(const std::string& top) {
  return {{top + "plugin.json", fixture("plugin.json")}, {top + "plugin.sig", platform_signature()}, {top + kProgram, fixture("program")}};
}
std::span<const std::uint8_t> bytes_of(const std::string& text) {
  return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

void taken_as_signed(const Install& install, const std::variant<AddedPlugin, std::string>& added, const std::string& what) {
  const auto* plugin = std::get_if<AddedPlugin>(&added);
  expect(plugin != nullptr, what + ": added: " + problem_of(added));
  if (plugin == nullptr) return;
  expect(plugin->id == "signedtool" && plugin->name == "Signed tool" && plugin->version == "1.0.0" && plugin->publisher == "Example Labs",
         what + ": what it is");
  expect(plugin->signer == "Example Labs" && plugin->key == kRfcPublic, what + ": who signed it");
  const auto folder = install.plugins / "signedtool";
  expect(secure_frontend_test::read_file(folder / "plugin.json") == fixture("plugin.json") &&
             secure_frontend_test::read_file(folder / "plugin.sig") == platform_signature() &&
             secure_frontend_test::read_file(folder / kProgram) == fixture("program"),
         what + ": its files, byte for byte");
  std::error_code ec;
  expect(std::distance(fs::directory_iterator(folder, ec), fs::directory_iterator{}) == 3, what + ": those files only");
  expect(!fs::exists(install.plugins / ".staging", ec) || fs::is_empty(install.plugins / ".staging", ec), what + ": nothing left in staging");
#if !defined(_WIN32)
  struct stat info {};
  expect(::stat((folder / kProgram).c_str(), &info) == 0 && (info.st_mode & 0777U) == 0700U, what + ": its program is the owner's to run");
#endif
  // the next start's discovery takes it, trusted through its signature
  secure_frontend::PluginChoices choices;
  choices.keys[kRfcPublic] = "Example Labs";
  const auto found = secure_frontend::discover_plugins(install.plugins, {}, choices);
  expect(found.size() == 1U && found[0].id == "signedtool" && found[0].state == secure_frontend::PluginState::on,
         what + ": discovered and on: " +
             (found.empty() ? std::string{"none"} : std::string{secure_frontend::plugin_state_name(found[0].state)} + " " + found[0].reason));
}

// From a folder: checked, copied, and taken at the next start; the folder it came from is left as it was.
void a_signed_folder_is_added() {
  const Install install;
  const auto source = install.folder("downloaded");
  taken_as_signed(install, secure_frontend::add_plugin_from_folder(install.plugins, source, kTrusted), "a folder");
  expect(secure_frontend_test::read_file(source / kProgram) == fixture("program") && fs::exists(source / "plugin.sig"),
         "the folder it came from is left as it was");
}

void a_folder_is_refused_before_anything_is_written() {
  const Install install;
  const auto refused = [&install](const fs::path& folder, const std::vector<secure_frontend::TrustedKey>& keys = kTrusted) {
    return problem_of(secure_frontend::add_plugin_from_folder(install.plugins, folder, keys));
  };
  const auto check = [&install](const std::string& problem, const std::string& part, const std::string& what) {
    expect(has(problem, part), what + ": " + problem);
    expect(install.nothing_added(), what + ": nothing written");
  };
  check(refused(fs::path{"relative"} / "folder"), "an absolute path", "a relative path");
  check(refused(install.root / "missing"), "no folder at that path", "a folder that is not there");
  {
    const auto file = install.root / "a-file";
    secure_frontend_test::write_file(file, "not a folder");
    check(refused(file), "a file, not a folder", "a file");
  }
  {
    const auto folder = install.folder("nomanifest");
    fs::remove(folder / "plugin.json");
    check(refused(folder), "no plugin.json", "no plugin.json");
  }
  {
    const auto folder = install.folder("badmanifest");
    secure_frontend_test::write_file(folder / "plugin.json", "{}");
    check(refused(folder), "its plugin.json: ", "a plugin.json that is not one");
  }
  {
    const auto folder = install.folder("unsigned");
    fs::remove(folder / "plugin.sig");
    check(refused(folder), "it is not signed", "an unsigned plugin");
  }
  {
    const auto folder = install.folder("untrusted");
    const auto problem = refused(folder, {{.key = kOtherPublic, .name = "Someone", .install = false}});
    check(problem, "does not trust", "a key not trusted");
    expect(has(problem, kRfcPublic) && has(problem, "trust its publisher's key first"), "it names the key to trust: " + problem);
  }
  {
    const auto folder = install.folder("changedmanifest");
    secure_frontend_test::write_file(folder / "plugin.json", fixture("plugin.json") + " ");
    check(refused(folder), "its plugin.json is not the one that was signed", "a changed plugin.json");
  }
  {
    const auto folder = install.folder("changedprogram");
    secure_frontend_test::write_file(folder / kProgram, "another program\n", 0700);
    check(refused(folder), "a file is not the one that was signed: " + kProgram, "a changed program");
  }
  {
    const auto folder = install.folder("missingprogram");
    fs::remove(folder / kProgram);
    check(refused(folder), "missing or is not a plain file: " + kProgram, "a missing program");
  }
  const auto good = install.folder("good");
  {
    const auto link = install.root / "link";
#if defined(_WIN32)
    expect(secure_frontend_test::create_junction(link, good), "a junction to the folder");
#else
    fs::create_directory_symlink(good, link);
#endif
    check(refused(link), "is a link", "a folder that is a link");
  }
#if !defined(_WIN32)
  {
    const auto folder = install.folder("linkedprogram");
    fs::remove(folder / kProgram);
    fs::create_symlink(good / kProgram, folder / kProgram);
    check(refused(folder), "missing or is not a plain file: " + kProgram, "a program that is a link");
  }
#endif
  // one of that id is there already: installed, or moved aside and not deleted
  expect(std::holds_alternative<AddedPlugin>(secure_frontend::add_plugin_from_folder(install.plugins, good, kTrusted)), "added once");
  expect(has(refused(good), "is installed already"), "added twice: " + refused(good));
  expect(std::holds_alternative<RemovedPlugin>(secure_frontend::move_plugin_aside(install.plugins, "signedtool", {}, kNoon)), "moved aside");
  expect(has(refused(good), "was moved aside"), "added while one is moved aside: " + refused(good));
}

// From a .zip, its entries at the root or in one top folder (with or without a folder entry).
void a_signed_zip_is_added() {
  {
    const Install install;
    const auto zip = make_zip(signed_items(""));
    taken_as_signed(install, secure_frontend::add_plugin_from_zip(install.plugins, bytes_of(zip), kTrusted), "a zip at the root");
  }
  {
    const Install install;
    auto items = signed_items("signedtool-1.0.0/");
    items.insert(items.begin(), ZipItem{"signedtool-1.0.0/", ""});
    const auto zip = make_zip(items);
    taken_as_signed(install, secure_frontend::add_plugin_from_zip(install.plugins, bytes_of(zip), kTrusted), "a zip in one folder");
  }
}

void a_zip_is_refused_before_anything_is_written() {
  const Install install;
  const auto refused = [&install](const std::vector<ZipItem>& items, const std::vector<secure_frontend::TrustedKey>& keys = kTrusted) {
    const auto zip = make_zip(items);
    return problem_of(secure_frontend::add_plugin_from_zip(install.plugins, bytes_of(zip), keys));
  };
  const auto check = [&install](const std::string& problem, const std::string& part, const std::string& what) {
    expect(has(problem, part), what + ": " + problem);
    expect(install.nothing_added(), what + ": nothing written");
  };
  const std::string not_zip = "this is not an archive";
  check(problem_of(secure_frontend::add_plugin_from_zip(install.plugins, bytes_of(not_zip), kTrusted)), "the archive: this is not a zip archive",
        "not a zip");
  auto items = signed_items("");
  check(refused({items[1], items[2]}), "no plugin.json at its root or in one top folder", "no plugin.json");
  check(refused({items[0], items[2]}), "it is not signed", "unsigned");
  check(refused(items, {{.key = kOtherPublic, .name = "Someone", .install = false}}), "does not trust", "a key not trusted");
  check(refused({items[0], items[1]}), "lacks a file its plugin.json lists: " + kProgram, "no program");
  check(refused({items[0], items[1], {kProgram, "another program\n"}}), "a file is not the one that was signed: " + kProgram, "a changed program");
  check(refused({items[0], items[1], items[2], {"notes.txt", "hello"}}), "a file its plugin.json does not list: notes.txt", "a file not listed");
  auto in_folder = signed_items("a/");
  in_folder.push_back({"b/extra", "x"});
  check(refused(in_folder), "a file its plugin.json does not list: b/extra", "a file outside its folder");
  auto twice = signed_items("");
  twice.push_back({"copy/plugin.json", fixture("plugin.json")});
  check(refused(twice), "a file its plugin.json does not list: copy/plugin.json", "a second plugin.json");
  check(refused({{"../plugin.json", fixture("plugin.json")}}), "the archive: an entry's name is not a plain relative path", "a name outside");
}

// Moved aside whole, listed, put back, or deleted for good; one moved aside in the same second gets its own entry.
void a_removed_plugin_can_be_put_back_or_deleted() {
  const Install install;
  const auto source = install.folder("downloaded");
  expect(std::holds_alternative<AddedPlugin>(secure_frontend::add_plugin_from_folder(install.plugins, source, kTrusted)), "added");
  const auto moved = secure_frontend::move_plugin_aside(install.plugins, "signedtool", {}, kNoon);
  const auto* removed = std::get_if<RemovedPlugin>(&moved);
  expect(removed != nullptr && removed->entry == kNoonEntry && removed->id == "signedtool" && removed->name == "Signed tool" &&
             removed->version == "1.0.0" && removed->when == "2026-10-30T12:00:00Z",
         "moved aside: " + problem_of(moved));
  std::error_code ec;
  expect(!fs::exists(install.plugins / "signedtool", ec), "gone from the plugins");
  expect(secure_frontend_test::read_file(install.plugins / ".removed" / kNoonEntry / kProgram) == fixture("program"), "moved aside whole");
  secure_frontend::PluginChoices choices;
  choices.keys[kRfcPublic] = "Example Labs";
  expect(secure_frontend::discover_plugins(install.plugins, {}, choices).empty(), "the next start does not take it");
  auto listed = secure_frontend::removed_plugins(install.plugins);
  expect(listed.size() == 1U && listed[0].entry == kNoonEntry && listed[0].name == "Signed tool" && listed[0].when == "2026-10-30T12:00:00Z",
         "listed");

  const auto back = secure_frontend::restore_plugin(install.plugins, kNoonEntry);
  expect(std::holds_alternative<RemovedPlugin>(back) && secure_frontend_test::read_file(install.plugins / "signedtool" / kProgram) == fixture("program"),
         "put back: " + problem_of(back));
  expect(secure_frontend::removed_plugins(install.plugins).empty(), "nothing moved aside once it is back");
  expect(secure_frontend::discover_plugins(install.plugins, {}, choices).size() == 1U, "the next start takes it again");

  expect(std::holds_alternative<RemovedPlugin>(secure_frontend::move_plugin_aside(install.plugins, "signedtool", {}, kNoon)), "moved aside again");
  install.by_hand();
  const auto second = secure_frontend::move_plugin_aside(install.plugins, "signedtool", {}, kNoon);
  expect(std::holds_alternative<RemovedPlugin>(second) && std::get<RemovedPlugin>(second).entry == kNoonEntry + "-2",
         "a second in the same second: " + problem_of(second));
  listed = secure_frontend::removed_plugins(install.plugins);
  expect(listed.size() == 2U && listed[0].entry == kNoonEntry && listed[1].entry == kNoonEntry + "-2", "both listed");

  const auto purged = secure_frontend::purge_plugin(install.plugins, kNoonEntry);
  expect(!purged.has_value(), "deleted for good: " + purged.value_or(""));
  expect(!fs::exists(install.plugins / ".removed" / kNoonEntry, ec), "its folder is gone");
  listed = secure_frontend::removed_plugins(install.plugins);
  expect(listed.size() == 1U && listed[0].entry == kNoonEntry + "-2", "the other is still listed");
}

void removing_is_refused_with_its_reason() {
  const Install install;
  install.by_hand();
  const auto remove = [&install](const std::string& id, const std::vector<secure_frontend::FirstPartyPlugin>& first_party = {}) {
    return problem_of(secure_frontend::move_plugin_aside(install.plugins, id, first_party, kNoon));
  };
  const std::string built_in = remove("signedtool", {{.id = "signedtool", .pin = {}, .on = true}});
  expect(has(built_in, "built in") && has(built_in, "turn it off instead"), "a built-in plugin: " + built_in);
  for (const std::string id : {"nothing", "../signedtool", ".staging", ".removed", ""}) {
    expect(has(remove(id), "no plugin"), "no such plugin (" + id + "): " + remove(id));
  }
  expect(fs::exists(install.plugins / "signedtool" / kProgram), "nothing was moved");
  for (const auto& entry :
       std::vector<std::string>{"signedtool", "../signedtool-20261030T120000Z", "signedtool-2026", kNoonEntry, kNoonEntry + "-x", ""}) {
    expect(has(problem_of(secure_frontend::restore_plugin(install.plugins, entry)), "not one that was moved aside"), "put back " + entry);
    expect(has(secure_frontend::purge_plugin(install.plugins, entry).value_or(""), "not one that was moved aside"), "delete " + entry);
  }
  expect(std::holds_alternative<RemovedPlugin>(secure_frontend::move_plugin_aside(install.plugins, "signedtool", {}, kNoon)), "moved aside");
  install.by_hand();
  const auto back = problem_of(secure_frontend::restore_plugin(install.plugins, kNoonEntry));
  expect(has(back, "is installed"), "put back while one of its id is installed: " + back);
  expect(secure_frontend::removed_plugins(install.plugins).size() == 1U, "it stays moved aside");
}

void forgetting_a_plugin_forgets_its_choices() {
  secure_frontend::PluginChoices choices;
  choices.approved["signedtool"] = secure_frontend::PluginPin{.manifest = "ab", .files = {}};
  choices.approved["other"] = secure_frontend::PluginPin{.manifest = "cd", .files = {}};
  choices.enabled["signedtool"] = true;
  choices.enabled["other"] = false;
  choices.keys[kRfcPublic] = "Example Labs";
  secure_frontend::forget_plugin_choices(choices, "signedtool");
  expect(!choices.approved.contains("signedtool") && !choices.enabled.contains("signedtool"), "its approval and on/off are forgotten");
  expect(choices.approved.contains("other") && choices.enabled.contains("other") && choices.keys.size() == 1U, "the rest stays");
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"a_signed_folder_is_added", a_signed_folder_is_added},
      {"a_folder_is_refused_before_anything_is_written", a_folder_is_refused_before_anything_is_written},
      {"a_signed_zip_is_added", a_signed_zip_is_added},
      {"a_zip_is_refused_before_anything_is_written", a_zip_is_refused_before_anything_is_written},
      {"a_removed_plugin_can_be_put_back_or_deleted", a_removed_plugin_can_be_put_back_or_deleted},
      {"removing_is_refused_with_its_reason", removing_is_refused_with_its_reason},
      {"forgetting_a_plugin_forgets_its_choices", forgetting_a_plugin_forgets_its_choices},
  }, failures);
}
