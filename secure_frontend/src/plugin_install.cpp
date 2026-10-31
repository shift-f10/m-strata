// Adding and removing plugins from the page (plugin_install.hpp; plugin spec 2026-10-30 section 12). The plugins folder
// is opened as an owner-only root and every write, rename and removal in it goes through the models store's primitives
// (models_store.hpp: beneath the root, never following a link, never replacing); a folder added from is opened as an
// external root and read the same way, each file a single-link regular file.
#include "secure_frontend/plugin_install.hpp"

#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/json.hpp"
#include "secure_frontend/models_store.hpp"
#include "secure_frontend/plugin_manifest.hpp"
#include "secure_frontend/sha256.hpp"
#include "secure_frontend/zip_reader.hpp"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <utility>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace secure_frontend {
namespace {

constexpr std::string_view kManifestFile = "plugin.json";
constexpr std::size_t kCopyChunk = std::size_t{1} << 20U;
constexpr int kMaxSameSecond = 99;

const std::string kUnsigned =
    "it is not signed: the page adds only a plugin signed by a key this server trusts (an unsigned one can be put in the "
    "plugins folder by hand, then approved)";
const std::string kNotMovedAside = "that is not one that was moved aside";

using Sink = std::function<bool(std::span<const std::uint8_t>)>;
// Streams one of the plugin's files into the sink; nullopt: all of it, else why not.
using FileSource = std::function<std::optional<std::string>(const std::string& name, const Sink& sink)>;

std::string sha256_hex(const std::string_view bytes) {
  Sha256 hash;
  hash.update(bytes);
  return to_hex(hash.finish());
}

std::string not_written(const AcquireError& error) {
  return "it could not be written into the plugins folder (" + error.detail + ")";
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
    if (!std::holds_alternative<RelativePath>(parse_relative_path(name))) return "a file's name is not one this server writes: " + name;
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

// A folder directly in `below` (the plugins folder's root when nullopt), not a link.
bool is_folder(const CanonicalRoot& root, const std::optional<RelativePath>& below, const std::string_view name) {
  const auto listed = list_directory(root, below);
  if (!std::holds_alternative<std::vector<DirectoryEntry>>(listed)) return false;
  const auto& entries = std::get<std::vector<DirectoryEntry>>(listed);
  return std::any_of(entries.begin(), entries.end(),
                     [name](const DirectoryEntry& entry) { return entry.name == name && entry.type == EntryType::directory; });
}

// Its plugin.json's name and version, read from `folder` (empty when that cannot be read).
RemovedPlugin describe(const CanonicalRoot& root, const RelativePath& folder, std::string entry, const EntryName& named) {
  RemovedPlugin plugin{.entry = std::move(entry), .id = named.id, .name = {}, .version = {}, .when = named.when};
  const auto text = read_small_file(root, join_rel(folder, kManifestFile), kMaxPluginManifestBytes);
  if (const auto* manifest_text = std::get_if<std::string>(&text)) {
    const auto parsed = parse_plugin_manifest(*manifest_text, named.id);
    if (const auto* manifest = std::get_if<PluginManifest>(&parsed)) {
      plugin.name = manifest->name;
      plugin.version = manifest->version;
    }
  }
  return plugin;
}

std::vector<RemovedPlugin> removed_in(const CanonicalRoot& root) {
  std::vector<RemovedPlugin> removed;
  const auto folder = must_rel(kRemovedFolder);
  const auto listed = list_directory(root, folder);
  if (!std::holds_alternative<std::vector<DirectoryEntry>>(listed)) return removed;
  for (const auto& entry : std::get<std::vector<DirectoryEntry>>(listed)) {
    const auto named = parse_entry(entry.name);
    if (entry.type != EntryType::directory || !named) continue;
    removed.push_back(describe(root, join_rel(folder, entry.name), entry.name, *named));
  }
  return removed;
}

// One of that id is there already: installed, or moved aside and not deleted.
std::optional<std::string> id_taken(const CanonicalRoot& root, const std::string& id) {
  if (exists_within(root, must_rel(id))) return "a plugin with the id " + id + " is installed already: remove it first";
  const auto removed = removed_in(root);
  if (std::any_of(removed.begin(), removed.end(), [&id](const RemovedPlugin& plugin) { return plugin.id == id; })) {
    return "a plugin with the id " + id + " was moved aside: put it back or delete it for good first";
  }
  return std::nullopt;
}

#if !defined(_WIN32)
std::optional<AcquireError> make_owner_executable(const CanonicalRoot& root, const RelativePath& path) {
  auto opened = open_beneath(root, path, OpenOptions{OpenKind::regular_file, FileTrust::any, true});
  if (std::holds_alternative<PathError>(opened) || ::fchmod(std::get<OpenedFile>(opened).native(), 0700) != 0) {
    return AcquireError{AcquireReason::publish_failed, path.generic() + ": mode 0700"};
  }
  return std::nullopt;
}
#endif

// The plugin written into <plugins>/.staging/<id> - each listed file from `source`, at most the size it had when it was
// checked and its SHA-256 as written compared with the statement again; then plugin.sig and plugin.json - and renamed to
// <plugins>/<id>. Staging is emptied on any failure. nullopt: added.
std::optional<std::string> install_staged(const CanonicalRoot& root, const Candidate& candidate, const std::string_view manifest_text,
                                          const std::string_view envelope, const std::map<std::string, std::uint64_t>& sizes,
                                          const FileSource& source) {
  const StoreEnvironment env;
  const auto staging = join_rel(must_rel(kStagingFolder), candidate.manifest.id);
  if (auto failure = ensure_directory(root, must_rel(kStagingFolder))) return not_written(*failure);
  if (auto failure = remove_tree_within(root, staging)) return not_written(*failure);  // an earlier add's leftovers
  if (auto failure = ensure_directory(root, staging)) return not_written(*failure);
  const auto fail = [&root, &staging](std::string why) -> std::optional<std::string> {
    (void)remove_tree_within(root, staging);
    return why;
  };
  for (const auto& name : candidate.manifest.files) {
    const auto target = try_join_rel(staging, name);
    if (!target) return fail("a file's name is not one this server writes: " + name);
    for (std::size_t depth = 3U; depth < target->components().size() + 1U; ++depth) {  // its folders, below staging/<id>
      std::string folder;
      for (std::size_t i = 0; i + 1U < depth; ++i) folder += (i == 0U ? "" : "/") + target->components()[i];
      if (auto failure = ensure_directory(root, must_rel(folder))) return fail(not_written(*failure));
    }
    auto part = open_part_file(root, *target, PartOpen::create_new, env);
    if (auto* failure = std::get_if<AcquireError>(&part)) return fail(not_written(*failure));
    auto& file = std::get<PartFile>(part);
    Sha256 hash;
    std::uint64_t written = 0;
    std::optional<std::string> problem;
    const auto copied = source(name, [&](const std::span<const std::uint8_t> chunk) {
      written += chunk.size();
      if (written > sizes.at(name)) {
        problem = "a file changed while it was copied: " + name;
        return false;
      }
      hash.update(chunk);
      if (auto failure = file.append(std::string_view{reinterpret_cast<const char*>(chunk.data()), chunk.size()})) {
        problem = not_written(*failure);
        return false;
      }
      return true;
    });
    if (problem) return fail(*problem);
    if (copied) return fail(*copied);
    if (auto failure = file.sync()) return fail(not_written(*failure));
    file.close();
    if (to_hex(hash.finish()) != candidate.statement.files.at(name)) return fail("a file is not the one that was signed: " + name);
#if !defined(_WIN32)
    if (auto failure = make_owner_executable(root, *target)) return fail(not_written(*failure));
#endif
  }
  if (auto failure = write_new_file(root, join_rel(staging, kPluginSignatureFile), envelope)) return fail(not_written(*failure));
  if (auto failure = write_new_file(root, join_rel(staging, kManifestFile), manifest_text)) return fail(not_written(*failure));
  if (auto failure = sync_directory(root, staging)) return fail(not_written(*failure));
  if (auto failure = rename_within(root, staging, must_rel(candidate.manifest.id), env)) return fail(not_written(*failure));
  (void)sync_directory(root, std::nullopt);
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

  auto manifest_text = read_small_file(source, must_rel(kManifestFile), kMaxPluginManifestBytes);
  if (!std::holds_alternative<std::string>(manifest_text)) {
    return std::string{"it has no plugin.json (or not a plain file of at most 256 KiB)"};
  }
  std::optional<std::string> envelope;
  if (exists_within(source, must_rel(kPluginSignatureFile))) {
    auto read = read_small_file(source, must_rel(kPluginSignatureFile), kMaxPluginSignatureBytes);
    if (!std::holds_alternative<std::string>(read)) return std::string{"its plugin.sig cannot be read (or is larger than 16 KiB)"};
    envelope = std::get<std::string>(std::move(read));
  }
  auto checked = check_candidate(std::get<std::string>(manifest_text), envelope, keys);
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
    Sha256 hash;
    const auto hashed = hash_into(source, must_rel(name), hash, budget);
    if (const auto* failure = std::get_if<AcquireError>(&hashed)) {
      if (failure->reason == AcquireReason::size_exceeded) return std::string{"its files come to more than 1 GiB"};
      return "a file its plugin.json lists is missing or is not a plain file: " + name;
    }
    if (to_hex(hash.finish()) != candidate.statement.files.at(name)) return "a file is not the one that was signed: " + name;
    sizes[name] = std::get<std::uint64_t>(hashed);
    budget -= sizes[name];
  }
  const FileSource copy = [&source](const std::string& name, const Sink& sink) -> std::optional<std::string> {
    auto file = open_beneath(source, must_rel(name), OpenOptions{OpenKind::regular_file, FileTrust::any, true});
    if (std::holds_alternative<PathError>(file)) return "a file its plugin.json lists is missing or is not a plain file: " + name;
    std::string buffer(kCopyChunk, '\0');
    while (true) {
      const auto read = std::get<OpenedFile>(file).read_some(buffer.data(), buffer.size());
      if (std::holds_alternative<PathError>(read)) return "a file could not be read: " + name;
      const auto n = std::get<std::size_t>(read);
      if (n == 0U) return std::nullopt;
      if (!sink({reinterpret_cast<const std::uint8_t*>(buffer.data()), n})) return "a file could not be copied: " + name;
    }
  };
  if (auto problem = install_staged(root, candidate, std::get<std::string>(manifest_text), envelope.value_or(""), sizes, copy)) return *problem;
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
  const auto extract_small = [&zip](const ZipEntry& entry, const std::size_t max, std::string* out) -> std::optional<std::string> {
    if (entry.size > max) return std::string{"too large"};
    return extract_zip_entry(zip, entry, [out](const std::span<const std::uint8_t> chunk) {
      out->append(reinterpret_cast<const char*>(chunk.data()), chunk.size());
      return true;
    });
  };
  std::string manifest_text;
  if (auto problem = extract_small(*entry_named(top + std::string{kManifestFile}), kMaxPluginManifestBytes, &manifest_text)) {
    return "its plugin.json cannot be read from the archive (" + *problem + ")";
  }
  std::optional<std::string> envelope;
  if (const auto* signature = entry_named(top + std::string{kPluginSignatureFile})) {
    envelope.emplace();
    if (auto problem = extract_small(*signature, kMaxPluginSignatureBytes, &*envelope)) {
      return "its plugin.sig cannot be read from the archive (" + *problem + ")";
    }
  }
  auto checked = check_candidate(manifest_text, envelope, keys);
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
  const FileSource inflate = [&zip, &files](const std::string& name, const Sink& sink) -> std::optional<std::string> {
    if (auto problem = extract_zip_entry(zip, *files.at(name), sink)) return "the archive: " + name + ": " + *problem;
    return std::nullopt;
  };
  if (auto problem = install_staged(root, candidate, manifest_text, envelope.value_or(""), sizes, inflate)) return *problem;
  return added(candidate);
}

std::variant<RemovedPlugin, std::string> move_plugin_aside(const std::filesystem::path& plugins_dir, const std::string_view id,
                                                           const std::vector<FirstPartyPlugin>& first_party,
                                                           const std::chrono::system_clock::time_point now) {
  const std::string no_plugin = "no plugin with that id is installed";
  if (!valid_plugin_id(id)) return no_plugin;
  if (std::any_of(first_party.begin(), first_party.end(), [id](const FirstPartyPlugin& plugin) { return plugin.id == id; })) {
    return std::string{"it is built in (one of make's): turn it off instead"};
  }
  auto plugins = open_plugins(plugins_dir);
  if (auto* problem = std::get_if<std::string>(&plugins)) return *problem;
  const auto& root = std::get<CanonicalRoot>(plugins);
  if (!is_folder(root, std::nullopt, id)) return no_plugin;
  const StoreEnvironment env;
  if (auto failure = ensure_directory(root, must_rel(kRemovedFolder))) return "it could not be moved aside (" + failure->detail + ")";
  const std::string base = std::string{id} + "-" + stamp(now);
  for (int n = 1; n <= kMaxSameSecond; ++n) {
    const std::string entry = n == 1 ? base : base + "-" + std::to_string(n);
    const auto target = join_rel(must_rel(kRemovedFolder), entry);
    if (exists_within(root, target)) continue;
    const auto named = parse_entry(entry);
    if (!named) break;
    auto plugin = describe(root, must_rel(id), entry, *named);
    if (auto failure = rename_within(root, must_rel(id), target, env)) return "it could not be moved aside (" + failure->detail + ")";
    (void)sync_directory(root, std::nullopt);
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
  if (!is_folder(root, must_rel(kRemovedFolder), entry)) return kNotMovedAside;
  if (exists_within(root, must_rel(named->id))) return "a plugin with the id " + named->id + " is installed: remove it first";
  const auto from = join_rel(must_rel(kRemovedFolder), entry);
  auto plugin = describe(root, from, std::string{entry}, *named);
  const StoreEnvironment env;
  if (auto failure = rename_within(root, from, must_rel(named->id), env)) return "it could not be put back (" + failure->detail + ")";
  (void)sync_directory(root, std::nullopt);
  return plugin;
}

std::optional<std::string> purge_plugin(const std::filesystem::path& plugins_dir, const std::string_view entry) {
  if (!parse_entry(entry)) return kNotMovedAside;
  auto plugins = open_plugins(plugins_dir);
  if (auto* problem = std::get_if<std::string>(&plugins)) return *problem;
  const auto& root = std::get<CanonicalRoot>(plugins);
  if (!is_folder(root, must_rel(kRemovedFolder), entry)) return kNotMovedAside;
  if (auto failure = remove_tree_within(root, join_rel(must_rel(kRemovedFolder), entry))) {
    return "it could not be deleted (" + failure->detail + ")";
  }
  return std::nullopt;
}

void forget_plugin_choices(PluginChoices& choices, const std::string_view id) {
  choices.approved.erase(std::string{id});
  choices.enabled.erase(std::string{id});
}

}  // namespace secure_frontend
