// Adding and removing plugins from the page (plugin_install.hpp; plugin spec 2026-10-30 section 12). Only the paths
// library and the private-file writer the server already links (the server links no acquisition code: PRIV-04). The
// plugins folder is opened as an owner-only root, so no other user of this PC can change what is in it while it is
// staged, renamed or removed; each file is written owner-only (write_private_file). A folder added from is opened as an
// external root and read beneath it without following a link, each file a single-link regular file.
#include "secure_frontend/plugin_install.hpp"

#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/json.hpp"
#include "secure_frontend/plugin_manifest.hpp"
#include "secure_frontend/sha256.hpp"
#include "secure_frontend/token_store.hpp"
#include "secure_frontend/zip_reader.hpp"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <system_error>
#include <utility>

namespace secure_frontend {
namespace {

constexpr std::string_view kManifestFile = "plugin.json";
constexpr std::size_t kReadChunk = std::size_t{1} << 20U;
constexpr int kMaxSameSecond = 99;

const std::string kUnsigned =
    "it is not signed: the page adds only a plugin signed by a key this server trusts (an unsigned one can be put in the "
    "plugins folder by hand, then approved)";
const std::string kNotMovedAside = "that is not one that was moved aside";

// One of the plugin's files, its bytes as they are now (at most the size it had when it was checked), or why not.
using FileSource = std::function<std::variant<std::string, std::string>(const std::string& name, std::uint64_t size)>;

std::string sha256_hex(const std::string_view bytes) {
  Sha256 hash;
  hash.update(bytes);
  return to_hex(hash.finish());
}

// A relative path this code builds from names it checked, or one a manifest names: nullopt when it is not a P1 path.
std::optional<RelativePath> relative(const std::string_view text) {
  auto parsed = parse_relative_path(text);
  if (auto* path = std::get_if<RelativePath>(&parsed)) return std::move(*path);
  return std::nullopt;
}
std::optional<RelativePath> relative(const std::string_view folder, const std::string_view name) {
  return relative(std::string{folder} + "/" + std::string{name});
}

bool exists(const CanonicalRoot& root, const RelativePath& path) {
  std::error_code ec;
  return std::filesystem::symlink_status(root.join(path), ec).type() != std::filesystem::file_type::not_found;
}

OpenOptions single_link_file() { return OpenOptions{.kind = OpenKind::regular_file, .trust = FileTrust::any, .reject_hard_links = true}; }

// A single-link regular file below `root`, whole, at most `max` bytes; nullopt: not such a file.
std::optional<std::string> read_whole(const CanonicalRoot& root, const RelativePath& path, const std::size_t max) {
  auto opened = open_beneath(root, path, single_link_file());
  if (!std::holds_alternative<OpenedFile>(opened)) return std::nullopt;
  auto read = std::get<OpenedFile>(opened).read_all(max);
  if (auto* text = std::get_if<std::string>(&read)) return std::move(*text);
  return std::nullopt;
}

// Its size and SHA-256, read in chunks (at most `max` bytes), or why not.
struct Hashed final {
  std::uint64_t size{0};
  std::string sha256;
};
std::variant<Hashed, std::string> hash_file(const CanonicalRoot& root, const RelativePath& path, const std::uint64_t max) {
  const std::string missing = "a file its plugin.json lists is missing or is not a plain file: " + path.generic();
  auto opened = open_beneath(root, path, single_link_file());
  if (!std::holds_alternative<OpenedFile>(opened)) return missing;
  const auto& file = std::get<OpenedFile>(opened);
  if (file.size() > max) return std::string{"its files come to more than 1 GiB"};
  Sha256 hash;
  std::string buffer(kReadChunk, '\0');
  Hashed hashed;
  for (;;) {
    const auto read = file.read_some(buffer.data(), buffer.size());
    if (!std::holds_alternative<std::size_t>(read)) return "a file could not be read: " + path.generic();
    const auto n = std::get<std::size_t>(read);
    if (n == 0U) break;
    hashed.size += n;
    if (hashed.size > max) return std::string{"its files come to more than 1 GiB"};
    hash.update(std::string_view{buffer.data(), n});
  }
  hashed.sha256 = to_hex(hash.finish());
  return hashed;
}

std::variant<CanonicalRoot, std::string> open_plugins(const std::filesystem::path& plugins_dir) {
  auto root = open_root(plugins_dir, RootPolicy::existing);
  if (auto* error = std::get_if<PathError>(&root)) return "the plugins folder cannot be opened: " + error->message;
  return std::get<CanonicalRoot>(std::move(root));
}

// The id a manifest names, when it names a valid one (parse_plugin_manifest then checks the rest against it).
std::string manifest_id(const std::string_view text) {
  constexpr JsonLimits kLimits{.max_bytes = kMaxPluginManifestBytes, .max_depth = 8U, .max_container_entries = 256U,
                               .max_string_bytes = kMaxPluginManifestBytes, .allow_fractions = false};
  const auto parsed = parse_json(text, kLimits);
  const auto* root = std::get_if<JsonValue>(&parsed);
  if (root == nullptr || root->if_object() == nullptr) return {};
  for (const auto& member : *root->if_object()) {
    if (member.key == "id" && member.value.if_string() != nullptr && valid_plugin_id(*member.value.if_string())) {
      return *member.value.if_string();
    }
  }
  return {};
}

// A plugin as its manifest and signature say, both checked, before any of its files is looked at.
struct Candidate final {
  PluginManifest manifest;
  TrustedStatement statement;
};

std::variant<Candidate, std::string> check_candidate(const std::string_view manifest_text, const std::optional<std::string>& envelope,
                                                     const std::vector<TrustedKey>& keys) {
  auto parsed = parse_plugin_manifest(manifest_text, manifest_id(manifest_text));
  if (auto* reason = std::get_if<std::string>(&parsed)) return "its plugin.json: " + *reason;
  Candidate candidate{.manifest = std::get<PluginManifest>(std::move(parsed)), .statement = {}};
  if (!plugin_kind_runs(candidate.manifest.kind)) {
    return "this server does not run " + std::string{plugin_kind_name(candidate.manifest.kind)} + " plugins yet";
  }
  if (!envelope) return kUnsigned;
  std::string named;
  auto read = read_trusted_statement(*envelope, candidate.manifest.id, keys, &named);
  if (auto* problem = std::get_if<std::string>(&read)) {
    if (*problem == kUntrustedKeyProblem) return *problem + " (" + named + "): trust its publisher's key first";
    return *problem;
  }
  candidate.statement = std::get<TrustedStatement>(std::move(read));
  if (sha256_hex(manifest_text) != candidate.statement.manifest_sha256) return std::string{"its plugin.json is not the one that was signed"};
  std::set<std::string> listed(candidate.manifest.files.begin(), candidate.manifest.files.end());
  std::set<std::string> signed_files;
  for (const auto& [name, sum] : candidate.statement.files) signed_files.insert(name);
  if (listed != signed_files) return std::string{"its signature does not list the files its plugin.json does"};
  for (const auto& name : listed) {
    if (!relative(name)) return "a file's name is not one this server writes: " + name;
  }
  return candidate;
}

// <id>-<YYYYMMDD>T<HHMMSS>Z, then -2 ... -99: its id and when (YYYY-MM-DDTHH:MM:SSZ), or nullopt.
struct EntryName final {
  std::string id;
  std::string when;
};
std::optional<EntryName> parse_entry(const std::string_view entry) {
  const auto dash = entry.find('-');
  if (dash == std::string_view::npos || !valid_plugin_id(entry.substr(0, dash))) return std::nullopt;
  const auto rest = entry.substr(dash + 1U);
  if (rest.size() < 16U) return std::nullopt;
  for (std::size_t i = 0; i < 16U; ++i) {
    const char c = rest[i];
    if (i == 8U ? c != 'T' : i == 15U ? c != 'Z' : (c < '0' || c > '9')) return std::nullopt;
  }
  const auto tail = rest.substr(16U);
  if (!tail.empty()) {
    const auto number = tail.substr(1U);
    if (tail.front() != '-' || number.empty() || number.size() > 2U || number.front() == '0' ||
        !std::all_of(number.begin(), number.end(), [](const char c) { return c >= '0' && c <= '9'; }) || number == "1") {
      return std::nullopt;
    }
  }
  const std::string s{rest.substr(0, 16U)};
  return EntryName{.id = std::string{entry.substr(0, dash)},
                   .when = s.substr(0, 4) + "-" + s.substr(4, 2) + "-" + s.substr(6, 2) + "T" + s.substr(9, 2) + ":" + s.substr(11, 2) +
                           ":" + s.substr(13, 2) + "Z"};
}

std::string stamp(const std::chrono::system_clock::time_point now) {
  const auto day = std::chrono::floor<std::chrono::days>(now);
  const std::chrono::year_month_day date{day};
  const std::chrono::hh_mm_ss time{std::chrono::floor<std::chrono::seconds>(now - day)};
  char text[64];
  std::snprintf(text, sizeof text, "%04d%02u%02uT%02d%02d%02dZ", static_cast<int>(date.year()), static_cast<unsigned>(date.month()),
                static_cast<unsigned>(date.day()), static_cast<int>(time.hours().count()), static_cast<int>(time.minutes().count()),
                static_cast<int>(time.seconds().count()));
  return text;
}

// A folder directly in `below` (the plugins folder itself when nullopt), not a link.
bool is_folder(const CanonicalRoot& root, const std::optional<RelativePath>& below, const std::string_view name) {
  const auto listed = list_directory(root, below);
  if (!std::holds_alternative<std::vector<DirectoryEntry>>(listed)) return false;
  const auto& entries = std::get<std::vector<DirectoryEntry>>(listed);
  return std::any_of(entries.begin(), entries.end(),
                     [name](const DirectoryEntry& entry) { return entry.name == name && entry.type == EntryType::directory; });
}

// Its plugin.json's name and version, read from `folder` (empty when that cannot be read).
RemovedPlugin describe(const CanonicalRoot& root, const std::string& folder, std::string entry, const EntryName& named) {
  RemovedPlugin plugin{.entry = std::move(entry), .id = named.id, .name = {}, .version = {}, .when = named.when};
  const auto path = relative(folder, kManifestFile);
  const auto text = path ? read_whole(root, *path, kMaxPluginManifestBytes) : std::nullopt;
  if (text) {
    const auto parsed = parse_plugin_manifest(*text, named.id);
    if (const auto* manifest = std::get_if<PluginManifest>(&parsed)) {
      plugin.name = manifest->name;
      plugin.version = manifest->version;
    }
  }
  return plugin;
}

std::vector<RemovedPlugin> removed_in(const CanonicalRoot& root) {
  std::vector<RemovedPlugin> removed;
  const auto folder = relative(kRemovedFolder);
  const auto listed = list_directory(root, folder);
  if (!std::holds_alternative<std::vector<DirectoryEntry>>(listed)) return removed;
  for (const auto& entry : std::get<std::vector<DirectoryEntry>>(listed)) {
    const auto named = parse_entry(entry.name);
    if (entry.type != EntryType::directory || !named) continue;
    removed.push_back(describe(root, std::string{kRemovedFolder} + "/" + entry.name, entry.name, *named));
  }
  return removed;
}

// One of that id is there already: installed, or moved aside and not deleted.
std::optional<std::string> id_taken(const CanonicalRoot& root, const std::string& id) {
  if (exists(root, *relative(id))) return "a plugin with the id " + id + " is installed already: remove it first";
  const auto removed = removed_in(root);
  if (std::any_of(removed.begin(), removed.end(), [&id](const RemovedPlugin& plugin) { return plugin.id == id; })) {
    return "a plugin with the id " + id + " was moved aside: put it back or delete it for good first";
  }
  return std::nullopt;
}

// A folder made owner-only below `root`, each missing part of it too.
std::optional<std::string> make_folder(const CanonicalRoot& root, const std::string& folder) {
  const auto path = relative(folder);
  if (!path) return "a folder's name is not one this server writes: " + folder;
  if (const auto failure = create_directory_within(root, *path)) return "it could not be written into the plugins folder (" + failure->message + ")";
  return std::nullopt;
}

// The plugin written into <plugins>/.staging/<id> - each listed file's bytes from `source` (as they are now, at most the
// size they had when they were checked), the bytes written the bytes hashed and compared with the statement; then
// plugin.sig and plugin.json - and renamed to <plugins>/<id>. Staging is emptied on any failure. nullopt: added.
std::optional<std::string> install_staged(const CanonicalRoot& root, const Candidate& candidate, const std::string_view manifest_text,
                                          const std::string_view envelope, const std::map<std::string, std::uint64_t>& sizes,
                                          const FileSource& source) {
  const std::string& id = candidate.manifest.id;
  const std::string staging = std::string{kStagingFolder} + "/" + id;
  std::error_code ec;
  std::filesystem::remove_all(root.join(*relative(staging)), ec);  // an earlier add's leftovers (owner-only: the owner's)
  if (auto problem = make_folder(root, staging)) return problem;
  const auto fail = [&root, &staging](std::string why) -> std::optional<std::string> {
    std::error_code ignored;
    std::filesystem::remove_all(root.join(*relative(staging)), ignored);
    return why;
  };
  const auto write = [&root](const RelativePath& path, const std::string_view bytes) -> std::optional<std::string> {
    if (const auto failure = write_private_file(root, path, bytes, TokenStoreMode::create_new)) {
      return "it could not be written into the plugins folder (" + failure->message + ")";
    }
    return std::nullopt;
  };
  for (const auto& name : candidate.manifest.files) {
    const auto target = relative(staging, name);
    if (!target) return fail("a file's name is not one this server writes: " + name);
    const auto slash = name.rfind('/');
    if (slash != std::string::npos) {  // its folders, below staging/<id>
      if (auto problem = make_folder(root, staging + "/" + name.substr(0, slash))) return fail(*problem);
    }
    auto bytes = source(name, sizes.at(name));
    if (auto* problem = std::get_if<1>(&bytes)) return fail(*problem);
    const auto& content = std::get<0>(bytes);
    if (sha256_hex(content) != candidate.statement.files.at(name)) return fail("a file is not the one that was signed: " + name);
    if (auto problem = write(*target, content)) return fail(*problem);
#if !defined(_WIN32)
    // the owner's to run (a tool's or notifier's program, an engine's library); no one else's at all
    std::filesystem::permissions(root.join(*target), std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, ec);
    if (ec) return fail("it could not be written into the plugins folder (a file's mode)");
#endif
  }
  if (auto problem = write(*relative(staging, kPluginSignatureFile), envelope)) return fail(*problem);
  if (auto problem = write(*relative(staging, kManifestFile), manifest_text)) return fail(*problem);
  // into place: renamed in the plugins folder (one volume); the id was checked free under the routes' lock
  const auto target = *relative(id);
  if (exists(root, target)) return fail("a plugin with the id " + id + " is installed already: remove it first");
  std::filesystem::rename(root.join(*relative(staging)), root.join(target), ec);
  if (ec) return fail("it could not be moved into the plugins folder");
  return std::nullopt;
}

AddedPlugin added(const Candidate& candidate) {
  return AddedPlugin{.id = candidate.manifest.id, .name = candidate.manifest.name, .version = candidate.manifest.version,
                     .publisher = candidate.manifest.publisher, .signer = candidate.statement.signer, .key = candidate.statement.key};
}

}  // namespace

std::variant<AddedPlugin, std::string> add_plugin_from_folder(const std::filesystem::path& plugins_dir, const std::filesystem::path& folder,
                                                              const std::vector<TrustedKey>& keys) {
  if (!folder.is_absolute()) return std::string{"name the folder by an absolute path on this PC"};
  std::error_code ec;
  const auto status = std::filesystem::symlink_status(folder, ec);
  if (status.type() == std::filesystem::file_type::not_found) return std::string{"there is no folder at that path"};
  if (ec) return std::string{"that path cannot be read"};
  if (std::filesystem::is_regular_file(status)) return std::string{"that path is a file, not a folder"};
  if (!std::filesystem::is_directory(status)) return std::string{"the folder is a link (or not a plain folder): name the folder itself"};
  auto opened = open_root(folder, RootPolicy::external);
  if (auto* error = std::get_if<PathError>(&opened)) return "the folder cannot be opened: " + error->message;
  const auto& source = std::get<CanonicalRoot>(opened);

  const auto manifest_text = read_whole(source, *relative(kManifestFile), kMaxPluginManifestBytes);
  if (!manifest_text) return std::string{"it has no plugin.json (or not a plain file of at most 256 KiB)"};
  std::optional<std::string> envelope;
  if (exists(source, *relative(kPluginSignatureFile))) {
    envelope = read_whole(source, *relative(kPluginSignatureFile), kMaxPluginSignatureBytes);
    if (!envelope) return std::string{"its plugin.sig cannot be read (or is larger than 16 KiB)"};
  }
  auto checked = check_candidate(*manifest_text, envelope, keys);
  if (auto* problem = std::get_if<std::string>(&checked)) return *problem;
  const auto& candidate = std::get<Candidate>(checked);
  auto plugins = open_plugins(plugins_dir);
  if (auto* problem = std::get_if<std::string>(&plugins)) return *problem;
  const auto& root = std::get<CanonicalRoot>(plugins);
  if (auto taken = id_taken(root, candidate.manifest.id)) return *taken;

  // each file hashed where it is, before anything is written
  std::map<std::string, std::uint64_t> sizes;
  std::uint64_t budget = kMaxZipBytes;
  for (const auto& name : candidate.manifest.files) {
    const auto hashed = hash_file(source, *relative(name), budget);
    if (const auto* problem = std::get_if<std::string>(&hashed)) return *problem;
    if (std::get<Hashed>(hashed).sha256 != candidate.statement.files.at(name)) return "a file is not the one that was signed: " + name;
    sizes[name] = std::get<Hashed>(hashed).size;
    budget -= sizes[name];
  }
  const FileSource copy = [&source](const std::string& name, const std::uint64_t size) -> std::variant<std::string, std::string> {
    auto bytes = read_whole(source, *relative(name), static_cast<std::size_t>(size));
    if (!bytes) return std::variant<std::string, std::string>{std::in_place_index<1>, "a file changed while it was copied: " + name};
    return std::variant<std::string, std::string>{std::in_place_index<0>, std::move(*bytes)};
  };
  if (auto problem = install_staged(root, candidate, *manifest_text, envelope.value_or(""), sizes, copy)) return *problem;
  return added(candidate);
}

std::variant<AddedPlugin, std::string> add_plugin_from_zip(const std::filesystem::path& plugins_dir, const std::span<const std::uint8_t> zip,
                                                           const std::vector<TrustedKey>& keys) {
  auto read = read_zip(zip);
  if (auto* problem = std::get_if<std::string>(&read)) return "the archive: " + *problem;
  const auto& entries = std::get<ZipArchive>(read).entries;
  const auto entry_named = [&entries](const std::string_view name) -> const ZipEntry* {
    const auto found = std::find_if(entries.begin(), entries.end(), [name](const ZipEntry& entry) { return entry.name == name; });
    return found == entries.end() ? nullptr : &*found;
  };
  // its plugin.json: at the root, or in one top folder
  std::string top;
  if (entry_named(kManifestFile) == nullptr) {
    std::vector<std::string> tops;
    for (const auto& entry : entries) {
      const auto slash = entry.name.find('/');
      if (slash != std::string::npos && std::string_view{entry.name}.substr(slash + 1U) == kManifestFile) tops.push_back(entry.name.substr(0, slash + 1U));
    }
    if (tops.empty()) return std::string{"the archive has no plugin.json at its root or in one top folder"};
    if (tops.size() > 1U) return std::string{"the archive has a plugin.json in more than one folder"};
    top = tops.front();
  }
  // an entry's bytes, inflated or copied (at most `max`, and its own size): nullopt with `why` set when they are not
  const auto extract = [&zip](const ZipEntry& entry, const std::uint64_t max, std::string* why) -> std::optional<std::string> {
    if (entry.size > max) {
      *why = "too large";
      return std::nullopt;
    }
    std::string out;
    if (auto problem = extract_zip_entry(zip, entry, [&out](const std::span<const std::uint8_t> chunk) {
          out.append(reinterpret_cast<const char*>(chunk.data()), chunk.size());
          return true;
        })) {
      *why = *problem;
      return std::nullopt;
    }
    return out;
  };
  std::string why;
  const auto manifest_text = extract(*entry_named(top + std::string{kManifestFile}), kMaxPluginManifestBytes, &why);
  if (!manifest_text) return "its plugin.json cannot be read from the archive (" + why + ")";
  std::optional<std::string> envelope;
  if (const auto* signature = entry_named(top + std::string{kPluginSignatureFile})) {
    envelope = extract(*signature, kMaxPluginSignatureBytes, &why);
    if (!envelope) return "its plugin.sig cannot be read from the archive (" + why + ")";
  }
  auto checked = check_candidate(*manifest_text, envelope, keys);
  if (auto* problem = std::get_if<std::string>(&checked)) return *problem;
  const auto& candidate = std::get<Candidate>(checked);

  // exactly its files: each listed one there, nothing else (folders aside)
  std::map<std::string, const ZipEntry*> files;
  for (const auto& name : candidate.manifest.files) {
    const auto* entry = entry_named(top + name);
    if (entry == nullptr) return "the archive lacks a file its plugin.json lists: " + name;
    files[name] = entry;
  }
  for (const auto& entry : entries) {
    if (entry.name.back() == '/') continue;
    const bool ours = entry.name.starts_with(top) &&
                      (entry.name == top + std::string{kManifestFile} || entry.name == top + std::string{kPluginSignatureFile} ||
                       files.contains(entry.name.substr(top.size())));
    if (!ours) return "the archive has a file its plugin.json does not list: " + entry.name;
  }
  auto plugins = open_plugins(plugins_dir);
  if (auto* problem = std::get_if<std::string>(&plugins)) return *problem;
  const auto& root = std::get<CanonicalRoot>(plugins);
  if (auto taken = id_taken(root, candidate.manifest.id)) return *taken;

  // each file inflated and hashed, before anything is written
  std::map<std::string, std::uint64_t> sizes;
  for (const auto& [name, entry] : files) {
    Sha256 hash;
    if (auto problem = extract_zip_entry(zip, *entry, [&hash](const std::span<const std::uint8_t> chunk) {
          hash.update(chunk);
          return true;
        })) {
      return "the archive: " + name + ": " + *problem;
    }
    if (to_hex(hash.finish()) != candidate.statement.files.at(name)) return "a file is not the one that was signed: " + name;
    sizes[name] = entry->size;
  }
  const FileSource inflate = [&extract, &files](const std::string& name, const std::uint64_t size) -> std::variant<std::string, std::string> {
    std::string problem;
    auto bytes = extract(*files.at(name), size, &problem);
    if (!bytes) return std::variant<std::string, std::string>{std::in_place_index<1>, "the archive: " + name + ": " + problem};
    return std::variant<std::string, std::string>{std::in_place_index<0>, std::move(*bytes)};
  };
  if (auto problem = install_staged(root, candidate, *manifest_text, envelope.value_or(""), sizes, inflate)) return *problem;
  return added(candidate);
}

std::variant<RemovedPlugin, std::string> move_plugin_aside(const std::filesystem::path& plugins_dir, const std::string_view id,
                                                           const std::vector<FirstPartyPlugin>& first_party,
                                                           const std::chrono::system_clock::time_point now) {
  const std::string no_plugin = "no plugin with that id is installed";
  if (!valid_plugin_id(id)) return no_plugin;
  if (std::any_of(first_party.begin(), first_party.end(), [id](const FirstPartyPlugin& plugin) { return plugin.id == id; })) {
    return std::string{kBuiltInPluginProblem};
  }
  auto plugins = open_plugins(plugins_dir);
  if (auto* problem = std::get_if<std::string>(&plugins)) return *problem;
  const auto& root = std::get<CanonicalRoot>(plugins);
  if (!is_folder(root, std::nullopt, id)) return no_plugin;
  if (auto problem = make_folder(root, std::string{kRemovedFolder})) return "it could not be moved aside: " + *problem;
  const std::string base = std::string{id} + "-" + stamp(now);
  for (int n = 1; n <= kMaxSameSecond; ++n) {
    const std::string entry = n == 1 ? base : base + "-" + std::to_string(n);
    const auto target = relative(kRemovedFolder, entry);
    const auto named = parse_entry(entry);
    if (!target || !named) break;
    if (exists(root, *target)) continue;
    auto plugin = describe(root, std::string{id}, entry, *named);
    std::error_code ec;
    std::filesystem::rename(root.join(*relative(id)), root.join(*target), ec);
    if (ec) return std::string{"it could not be moved aside (its folder is in use, or cannot be renamed)"};
    return plugin;
  }
  return std::string{"it could not be moved aside (too many of its id moved aside this second)"};
}

std::vector<RemovedPlugin> removed_plugins(const std::filesystem::path& plugins_dir) {
  const auto plugins = open_plugins(plugins_dir);
  if (!std::holds_alternative<CanonicalRoot>(plugins)) return {};
  return removed_in(std::get<CanonicalRoot>(plugins));
}

std::variant<RemovedPlugin, std::string> restore_plugin(const std::filesystem::path& plugins_dir, const std::string_view entry) {
  const auto named = parse_entry(entry);
  if (!named) return kNotMovedAside;
  auto plugins = open_plugins(plugins_dir);
  if (auto* problem = std::get_if<std::string>(&plugins)) return *problem;
  const auto& root = std::get<CanonicalRoot>(plugins);
  if (!is_folder(root, relative(kRemovedFolder), entry)) return kNotMovedAside;
  if (exists(root, *relative(named->id))) return "a plugin with the id " + named->id + " is installed: remove it first";
  const std::string from = std::string{kRemovedFolder} + "/" + std::string{entry};
  auto plugin = describe(root, from, std::string{entry}, *named);
  std::error_code ec;
  std::filesystem::rename(root.join(*relative(from)), root.join(*relative(named->id)), ec);
  if (ec) return std::string{"it could not be put back (its folder cannot be renamed)"};
  return plugin;
}

std::optional<std::string> purge_plugin(const std::filesystem::path& plugins_dir, const std::string_view entry) {
  if (!parse_entry(entry)) return kNotMovedAside;
  auto plugins = open_plugins(plugins_dir);
  if (auto* problem = std::get_if<std::string>(&plugins)) return *problem;
  const auto& root = std::get<CanonicalRoot>(plugins);
  if (!is_folder(root, relative(kRemovedFolder), entry)) return kNotMovedAside;
  // owner-only, and a link in it is removed, never followed
  std::error_code ec;
  std::filesystem::remove_all(root.join(*relative(kRemovedFolder, entry)), ec);
  if (ec) return std::string{"it could not be deleted (a file in it is in use, or cannot be removed)"};
  return std::nullopt;
}

void forget_plugin_choices(PluginChoices& choices, const std::string_view id) {
  choices.approved.erase(std::string{id});
  choices.enabled.erase(std::string{id});
}

}  // namespace secure_frontend
