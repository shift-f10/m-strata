#include "secure_frontend/plugin_registry.hpp"

#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/json.hpp"
#include "secure_frontend/sha256.hpp"
#include "secure_frontend/token_store.hpp"

#include <algorithm>
#include <fstream>
#include <system_error>
#include <utility>

namespace secure_frontend {
namespace {

constexpr std::string_view kChoicesSchema = "m-strata.plugin-choices/1";
constexpr JsonLimits kChoiceLimits{.max_bytes = 1U << 20U, .max_depth = 5U, .max_container_entries = 1024U,
                                   .max_string_bytes = 256U, .allow_fractions = false};

std::string hex(const Sha256Digest& digest) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(64U);
  for (const auto byte : digest) {
    out += kDigits[byte >> 4U];
    out += kDigits[byte & 0x0FU];
  }
  return out;
}

bool hex64(const std::string_view text) {
  return text.size() == 64U && std::all_of(text.begin(), text.end(), [](const char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// A regular file's bytes, or nullopt (missing, not a file, unreadable, larger than `limit`).
std::optional<std::string> read_bytes(const std::filesystem::path& file, const std::uintmax_t limit) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(file, ec)) || ec) return std::nullopt;
  const auto size = std::filesystem::file_size(file, ec);
  if (ec || size > limit) return std::nullopt;
  std::ifstream in(file, std::ios::binary);
  if (!in) return std::nullopt;
  std::string bytes(static_cast<std::size_t>(size), '\0');
  if (size > 0U && !in.read(bytes.data(), static_cast<std::streamsize>(size))) return std::nullopt;
  return bytes;
}

// A regular file's SHA-256, read in chunks (a plugin's library may be hundreds of MB); nullopt when it cannot be read.
std::optional<std::string> file_sha256(const std::filesystem::path& file) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(file, ec)) || ec) return std::nullopt;
  std::ifstream in(file, std::ios::binary);
  if (!in) return std::nullopt;
  Sha256 hash;
  std::string block(kSha256ReadChunkBytes, '\0');
  while (in) {
    in.read(block.data(), static_cast<std::streamsize>(block.size()));
    const auto got = in.gcount();
    if (got > 0) hash.update(std::string_view{block.data(), static_cast<std::size_t>(got)});
  }
  if (in.bad()) return std::nullopt;
  return hex(hash.finish());
}

const JsonValue* member(const JsonValue& object, const std::string_view key) {
  const auto* members = object.if_object();
  if (members == nullptr) return nullptr;
  for (const auto& entry : *members) {
    if (entry.key == key) return &entry.value;
  }
  return nullptr;
}

// The trust: what it is when the plugin can run, or the state and reason when it cannot.
struct Trust final {
  bool trusted{false};
  bool first_party{false};
  bool default_on{false};
  PluginState state{PluginState::waiting};
  std::string reason;
};

// The order of trust (spec section 11): make's build record, a signature under a trusted key that is this folder's, the
// owner's approval.
Trust trust_of(const PluginEntry& entry, const std::vector<FirstPartyPlugin>& first_party, const PluginChoices& choices) {
  const auto built = std::find_if(first_party.begin(), first_party.end(), [&entry](const FirstPartyPlugin& plugin) { return plugin.id == entry.id; });
  if (built != first_party.end() && built->pin == entry.pin) {
    return {.trusted = true, .first_party = true, .default_on = built->on, .state = PluginState::off, .reason = {}};
  }
  if (entry.signature && !entry.signature->signer.empty()) {
    return {.trusted = true, .first_party = false, .default_on = true, .state = PluginState::off, .reason = {}};
  }
  if (const auto approved = choices.approved.find(entry.id); approved != choices.approved.end()) {
    if (approved->second == entry.pin) return {.trusted = true, .first_party = false, .default_on = true, .state = PluginState::off, .reason = {}};
    return {.trusted = false, .first_party = false, .default_on = false, .state = PluginState::changed,
            .reason = "its manifest or a file changed since it was approved: approve it again to use it"};
  }
  if (entry.signature && !entry.signature->problem.empty()) {
    return {.trusted = false, .first_party = false, .default_on = false, .state = PluginState::waiting,
            .reason = entry.signature->problem + (entry.signature->problem.find("does not trust") != std::string::npos
                                                      ? ": trust its publisher's key in the Plugins tab, or approve it"
                                                      : ": approve it to use it as it is now")};
  }
  return {.trusted = false, .first_party = false, .default_on = false, .state = PluginState::waiting,
          .reason = built != first_party.end() ? "its files are not the ones this install was built with: approve it to use it"
                                               : "waiting for the owner's approval"};
}

}  // namespace

std::string_view plugin_state_name(const PluginState state) noexcept {
  switch (state) {
    case PluginState::on: return "on";
    case PluginState::off: return "off";
    case PluginState::waiting: return "waiting";
    case PluginState::changed: return "changed";
    case PluginState::refused: return "refused";
  }
  return "refused";  // unreachable: every state is a case above
}

bool plugin_kind_runs(const PluginKind kind) noexcept {
  // an engine plugin since its family runs (plugin spec section 5: the server's families, its cards through the launcher)
  return kind == PluginKind::diagnostics || kind == PluginKind::tool || kind == PluginKind::notifier || kind == PluginKind::engine;
}

std::string render_plugin_choices(const PluginChoices& choices) {
  std::string out = "{\"schema\":";
  append_json_string(&out, kChoicesSchema);
  out += ",\"approved\":{";
  bool first = true;
  for (const auto& [id, pin] : choices.approved) {
    out += first ? "" : ",";
    first = false;
    append_json_string(&out, id);
    out += ":{\"manifest\":";
    append_json_string(&out, pin.manifest);
    out += ",\"files\":{";
    bool first_file = true;
    for (const auto& [path, sum] : pin.files) {
      out += first_file ? "" : ",";
      first_file = false;
      append_json_string(&out, path);
      out += ":";
      append_json_string(&out, sum);
    }
    out += "}}";
  }
  out += "},\"enabled\":{";
  first = true;
  for (const auto& [id, on] : choices.enabled) {
    out += first ? "" : ",";
    first = false;
    append_json_string(&out, id);
    out += on ? ":true" : ":false";
  }
  out += "},\"keys\":{";
  first = true;
  for (const auto& [key, name] : choices.keys) {
    out += first ? "" : ",";
    first = false;
    append_json_string(&out, key);
    out += ":";
    append_json_string(&out, name);
  }
  return out + "}}\n";
}

std::optional<PluginChoices> parse_plugin_choices(const std::string_view text) {
  const auto parsed = parse_json(text, kChoiceLimits);
  const auto* root = std::get_if<JsonValue>(&parsed);
  // (a file from before publisher keys has no "keys")
  if (root == nullptr || root->if_object() == nullptr || root->if_object()->size() < 3U || root->if_object()->size() > 4U) return std::nullopt;
  const JsonValue* schema = member(*root, "schema");
  const JsonValue* approved = member(*root, "approved");
  const JsonValue* enabled = member(*root, "enabled");
  const JsonValue* keys = member(*root, "keys");
  if (root->if_object()->size() == 4U && (keys == nullptr || keys->if_object() == nullptr || keys->if_object()->size() > kMaxPublisherKeys)) {
    return std::nullopt;
  }
  if (schema == nullptr || schema->if_string() == nullptr || *schema->if_string() != kChoicesSchema || approved == nullptr ||
      approved->if_object() == nullptr || enabled == nullptr || enabled->if_object() == nullptr) {
    return std::nullopt;
  }
  PluginChoices choices;
  for (const auto& [id, value] : *approved->if_object()) {
    const JsonValue* manifest = member(value, "manifest");
    const JsonValue* files = member(value, "files");
    if (!valid_plugin_id(id) || value.if_object() == nullptr || value.if_object()->size() != 2U || manifest == nullptr ||
        manifest->if_string() == nullptr || !hex64(*manifest->if_string()) || files == nullptr || files->if_object() == nullptr) {
      return std::nullopt;
    }
    PluginPin pin{.manifest = *manifest->if_string(), .files = {}};
    for (const auto& [path, sum] : *files->if_object()) {
      if (path.empty() || path.size() > 128U || sum.if_string() == nullptr || !hex64(*sum.if_string())) return std::nullopt;
      pin.files[path] = *sum.if_string();
    }
    choices.approved[id] = std::move(pin);
  }
  for (const auto& [id, value] : *enabled->if_object()) {
    if (!valid_plugin_id(id) || value.if_boolean() == nullptr) return std::nullopt;
    choices.enabled[id] = *value.if_boolean();
  }
  if (keys != nullptr) {
    for (const auto& [key, name] : *keys->if_object()) {
      if (!valid_signing_key(key) || name.if_string() == nullptr || !valid_key_name(*name.if_string())) return std::nullopt;
      choices.keys[key] = *name.if_string();
    }
  }
  return choices;
}

std::optional<PluginChoices> read_plugin_choices(const std::filesystem::path& state_dir) {
  constexpr std::uintmax_t kMaxChoicesBytes = 1U << 20U;  // parse_plugin_choices' own bound
  const auto file = state_dir / path_from_utf8(kPluginChoicesFile);
  std::error_code ec;
  const auto status = std::filesystem::symlink_status(file, ec);
  if (status.type() == std::filesystem::file_type::not_found) return PluginChoices{};
  if (ec || !std::filesystem::is_regular_file(status)) return std::nullopt;
  const auto size = std::filesystem::file_size(file, ec);
  if (ec || size > kMaxChoicesBytes) return std::nullopt;
  std::ifstream in(file, std::ios::binary);
  std::string text(static_cast<std::size_t>(size), '\0');
  if (!in || (size > 0U && !in.read(text.data(), static_cast<std::streamsize>(size)))) return std::nullopt;
  return parse_plugin_choices(text);
}

bool write_plugin_choices(const std::filesystem::path& state_dir, const PluginChoices& choices) {
  const auto root = open_root(state_dir, RootPolicy::create_private);
  const auto name = parse_relative_path(kPluginChoicesFile);
  if (!std::holds_alternative<CanonicalRoot>(root) || !std::holds_alternative<RelativePath>(name)) return false;
  return !write_private_file(std::get<CanonicalRoot>(root), std::get<RelativePath>(name), render_plugin_choices(choices),
                             TokenStoreMode::replace)
              .has_value();
}

bool valid_key_name(const std::string_view name) noexcept {
  return !name.empty() && name.size() <= 64U && std::all_of(name.begin(), name.end(), [](const char c) { return c >= 0x20 && c < 0x7f; });
}

std::vector<TrustedKey> trusted_keys(const std::string_view install_key, const PluginChoices& choices) {
  std::vector<TrustedKey> keys;
  if (valid_signing_key(install_key)) keys.push_back({.key = std::string{install_key}, .name = "this install's key", .install = true});
  for (const auto& [key, name] : choices.keys) {
    if (key != install_key) keys.push_back({.key = key, .name = name, .install = false});
  }
  return keys;
}

std::vector<PluginEntry> discover_plugins(const std::filesystem::path& plugins_dir, const std::vector<FirstPartyPlugin>& first_party,
                                          const PluginChoices& choices, const std::string_view install_key) {
  const auto keys = trusted_keys(install_key, choices);
  std::vector<std::filesystem::path> folders;
  std::error_code ec;
  for (std::filesystem::directory_iterator item(plugins_dir, ec), end; !ec && item != end; item.increment(ec)) {
    std::error_code kind;
    const auto status = item->symlink_status(kind);
    if (kind || !std::filesystem::is_directory(status)) continue;   // a file, or a link: not a plugin folder
    if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(item->path() / "plugin.json", kind)) || kind) continue;
    folders.push_back(item->path());
  }
  std::sort(folders.begin(), folders.end());
  std::vector<PluginEntry> entries;
  for (const auto& folder : folders) {
    PluginEntry entry;
    entry.id = path_to_utf8(folder.filename());
    const auto refuse = [&entries, &entry](std::string reason) {
      entry.state = PluginState::refused;
      entry.reason = std::move(reason);
      entries.push_back(std::move(entry));
    };
    if (!valid_plugin_id(entry.id)) {
      refuse("its folder's name is not a plugin id (a lowercase letter, then lowercase letters, digits or '_')");
      continue;
    }
    // owner-only, as the install folder it is in: no one else may change what a plugin runs
    if (!std::holds_alternative<CanonicalRoot>(open_root(folder, RootPolicy::existing))) {
      refuse("other users of this PC can change its folder: make it private (as the install folder is)");
      continue;
    }
    const auto manifest_bytes = read_bytes(folder / "plugin.json", kMaxPluginManifestBytes);
    if (!manifest_bytes.has_value()) {
      refuse("its plugin.json could not be read (or is larger than 256 KiB)");
      continue;
    }
    Sha256 hash;
    hash.update(*manifest_bytes);
    entry.pin.manifest = hex(hash.finish());
    auto parsed = parse_plugin_manifest(*manifest_bytes, entry.id);
    if (auto* reason = std::get_if<std::string>(&parsed)) {
      refuse("its plugin.json: " + *reason);
      continue;
    }
    entry.manifest = std::get<PluginManifest>(std::move(parsed));
    std::string missing;
    for (const auto& file : entry.manifest->files) {
      const auto sum = file_sha256(folder / path_from_utf8(file));
      if (!sum.has_value()) {
        missing = file;
        break;
      }
      entry.pin.files[file] = *sum;
    }
    if (!missing.empty()) {
      refuse("a file its plugin.json names is missing or cannot be read: " + missing);
      continue;
    }
    // its signature, if it has one (spec section 11), checked against the keys this server trusts
    std::error_code sig_kind;
    if (std::filesystem::symlink_status(folder / kPluginSignatureFile, sig_kind).type() != std::filesystem::file_type::not_found) {
      const auto envelope = read_bytes(folder / kPluginSignatureFile, kMaxPluginSignatureBytes);
      entry.signature = envelope ? check_plugin_signature(*envelope, entry.id, entry.pin.manifest, entry.pin.files, keys)
                                 : PluginSignature{.key = {}, .signer = {}, .problem = "its plugin.sig could not be read (or is larger than 16 KiB)"};
    }
    const Trust trust = trust_of(entry, first_party, choices);
    entry.first_party = trust.first_party;
    if (!trust.trusted) {
      entry.state = trust.state;
      entry.reason = trust.reason;
    } else if (!plugin_kind_runs(entry.manifest->kind)) {
      entry.state = PluginState::refused;
      entry.reason = "this server does not run " + std::string{plugin_kind_name(entry.manifest->kind)} + " plugins yet";
    } else {
      const auto chosen = choices.enabled.find(entry.id);
      entry.state = (chosen != choices.enabled.end() ? chosen->second : trust.default_on) ? PluginState::on : PluginState::off;
    }
    entries.push_back(std::move(entry));
  }
  return entries;
}

Diagnostics diagnostics_on(const std::vector<PluginEntry>& plugins) {
  Diagnostics on;
  for (const auto& plugin : plugins) {
    if (plugin.state != PluginState::on || !plugin.manifest || plugin.manifest->kind != PluginKind::diagnostics) continue;
    for (const auto value : plugin.manifest->switches) {
      on.log_requests = on.log_requests || value == DiagnosticSwitch::log_requests;
      on.log_engine_output = on.log_engine_output || value == DiagnosticSwitch::log_engine_output;
      on.troubleshooting = on.troubleshooting || value == DiagnosticSwitch::troubleshooting;
    }
  }
  return on;
}

}  // namespace secure_frontend
