#include "secure_frontend/card_routes.hpp"

#include "secure_frontend/api_errors.hpp"
#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/config_file.hpp"
#include "secure_frontend/config_writer.hpp"
#include "secure_frontend/gguf_metadata.hpp"
#include "secure_frontend/json.hpp"
#include "secure_frontend/launch_composition.hpp"
#include "secure_frontend/local_folders.hpp"
#include "secure_frontend/model_cards.hpp"
#include "secure_frontend/token_store.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace secure_frontend {
namespace {

constexpr JsonLimits kBodyLimits{.max_bytes = kModelControlBodyLimit, .max_depth = 2U, .max_container_entries = 8U,
                                 .max_string_bytes = 4096U};

// The body's members: strings and booleans only, each named once (parse_json refuses a duplicate). nullopt: not an
// object, or a member of another type.
struct Body final {
  std::map<std::string, std::string, std::less<>> text;
  std::map<std::string, bool, std::less<>> flags;
  [[nodiscard]] const std::string* get(const std::string_view key) const {
    const auto found = text.find(key);
    return found == text.end() ? nullptr : &found->second;
  }
};
std::optional<Body> read_body(const std::string_view body, const std::initializer_list<std::string_view> allowed) {
  const auto parsed = parse_json(body.empty() ? std::string_view{"{}"} : body, kBodyLimits);
  const auto* root = std::get_if<JsonValue>(&parsed);
  const auto* members = root == nullptr ? nullptr : root->if_object();
  if (members == nullptr) return std::nullopt;
  Body read;
  for (const auto& member : *members) {
    if (std::find(allowed.begin(), allowed.end(), member.key) == allowed.end()) return std::nullopt;
    if (const auto* value = member.value.if_string()) read.text.emplace(member.key, *value);
    else if (const auto* flag = member.value.if_boolean()) read.flags.emplace(member.key, *flag);
    else return std::nullopt;
  }
  return read;
}

std::string json_string(const std::string_view text) {
  std::string out;
  append_json_string(&out, text);
  return out;
}

HttpResponse json_answer(const int status, std::string body) {
  return {.status = status, .headers = {{"Content-Type", "application/json; charset=utf-8"}}, .body = std::move(body)};
}

HttpResponse refused(const ApiError error, const std::string_view reason = {}) { return make_error_response(error, {.reason = reason}); }

// The owner's choice to use model folders as they are, even where other users of this PC may change them: the config's
// roots.allow_untrusted_media (owner 2026-10-27).
bool allows_shared_folders(const FrontendConfigDraft& draft) {
  const auto found = draft.entries.find("roots.allow_untrusted_media");
  const bool* on = found == draft.entries.end() ? nullptr : std::get_if<bool>(&found->second.value);
  return on != nullptr && *on;
}

// A folder the page names, opened as the server opens a model folder; a folder other users may write is told apart
// (the page offers "Make this folder private"), unless `shared`: then it is used as it is (still local, never a link).
std::variant<CanonicalRoot, HttpResponse> trusted_folder(const std::string& text, const bool shared) {
  auto checked = local_folder_path(text);
  if (const auto* error = std::get_if<FolderError>(&checked)) return refused(ApiError::folder_refused, error->message);
  const auto& path = std::get<std::filesystem::path>(checked);
  std::error_code ec;
  if (!std::filesystem::is_directory(path, ec)) return refused(ApiError::folder_refused, "there is no such folder");
  auto opened = open_root(path, shared ? RootPolicy::external : RootPolicy::existing);
  if (auto* root = std::get_if<CanonicalRoot>(&opened)) return std::move(*root);
  const auto& error = std::get<PathError>(opened);
  switch (error.code) {
    case PathErrorCode::untrusted_owner:
    case PathErrorCode::shared_writable:
    case PathErrorCode::not_owned_by_user:
    case PathErrorCode::permissions_too_broad: return refused(ApiError::folder_not_private);
    default: return refused(ApiError::folder_refused, error.message);
  }
}

const std::string* text_row(const ModelCard& card, const std::string_view key) {
  const auto found = card.config.entries.find(std::string{key});
  return found == card.config.entries.end() ? nullptr : std::get_if<std::string>(&found->second.value);
}

// A card the page built from a GGUF (card_for_gguf): a GGUF family, its folder and model, no tokenizer pack.
bool gguf_card(const ModelCard& card) {
  return tokenizer_from_gguf(card.profile, true) && text_row(card, "roots.models") != nullptr && text_row(card, "engine.model_id") != nullptr &&
         !card.config.entries.contains("roots.tokenizer_pack");
}

}  // namespace

CardRoutes::CardRoutes(ModelLauncher& launcher, CardRouteSettings settings)
    : launcher_(launcher), settings_(std::move(settings)), shared_folders_(allows_shared_folders(settings_.main)) {}

void CardRoutes::log(const std::string_view line) const {
  if (settings_.log) {
    settings_.log(line);
    return;
  }
  std::cerr << line << '\n' << std::flush;
}

HttpResponse CardRoutes::answer(const RouteId route, const std::string_view body, const RequestContext& context) {
  // D4: this PC's folders and files, and deletes, are for loopback clients, and since 2026-10-27 for the clients the
  // server's rule lets in (model_files_allowed: a listed host or subnet; never models.control_remote alone).
  const bool allowed = settings_.client_allowed ? settings_.client_allowed(context) : context.loopback_peer;
  if (!allowed) return make_error_response(ApiError::control_not_allowed);
  switch (route) {
    case RouteId::strata_folders: return folders(body);
    case RouteId::strata_folder_private: return make_private(body);
    case RouteId::strata_folder_new: return new_folder(body);
    // how this server trusts model folders: for the clients the file routes serve (the "make it private" dialog names it)
    case RouteId::strata_folder_sharing: return sharing(body);
    case RouteId::strata_card_add: return add(body);
    case RouteId::strata_card_folder: return repoint(body);
    case RouteId::strata_card_delete_begin: return delete_begin(body);
    case RouteId::strata_card_delete: return delete_confirm(body);
    case RouteId::strata_card_remove: return remove(body);
    default: return make_error_response(ApiError::not_found);
  }
}

HttpResponse CardRoutes::folders(const std::string_view body) {
  const auto read = read_body(body, {"path"});
  if (!read) return make_error_response(ApiError::invalid_request);
  const auto* path = read->get("path");
  if (path == nullptr || path->empty()) {
    std::string out = "{\"volumes\":[";
    bool first = true;
    for (const auto& volume : local_volumes()) {
      out += std::string{first ? "" : ","} + "{\"path\":" + json_string(volume.path) + ",\"label\":" + json_string(volume.label) +
             ",\"free_bytes\":" + std::to_string(volume.free_bytes) + ",\"total_bytes\":" + std::to_string(volume.total_bytes) + "}";
      first = false;
    }
    return json_answer(200, out + "]}\n");
  }
  auto listed = list_local_folder(*path);
  if (const auto* error = std::get_if<FolderError>(&listed)) return refused(ApiError::folder_refused, error->message);
  const auto& listing = std::get<FolderListing>(listed);
  std::string out = "{\"path\":" + json_string(listing.path) +
                    ",\"parent\":" + (listing.parent.empty() ? std::string{"null"} : json_string(listing.parent)) +
                    ",\"free_bytes\":" + std::to_string(listing.free_bytes) + ",\"truncated\":" + (listing.truncated ? "true" : "false") +
                    ",\"entries\":[";
  for (std::size_t i = 0; i < listing.entries.size(); ++i) {
    const auto& entry = listing.entries[i];
    out += std::string{i == 0U ? "" : ","} + "{\"name\":" + json_string(entry.name) + ",\"folder\":" + (entry.folder ? "true" : "false") +
           ",\"bytes\":" + std::to_string(entry.bytes) + "}";
  }
  return json_answer(200, out + "]}\n");
}

HttpResponse CardRoutes::make_private(const std::string_view body) {
  const auto read = read_body(body, {"path"});
  const auto* path = read ? read->get("path") : nullptr;
  if (path == nullptr) return make_error_response(ApiError::invalid_request);
  auto checked = local_folder_path(*path);
  if (const auto* error = std::get_if<FolderError>(&checked)) return refused(ApiError::folder_refused, error->message);
  if (auto failure = make_model_folder_private(std::get<std::filesystem::path>(checked))) {
    return refused(ApiError::folder_refused, failure->message);
  }
  log("[strata] models: a model folder was made private (owner-only)");
  return json_answer(200, "{\"private\":true}\n");
}

// §4.3: a new folder, owner-only from the start (make_private_folder), in a folder the page picked - a download's
// destination on a drive whose folders others may write.
HttpResponse CardRoutes::new_folder(const std::string_view body) {
  const auto read = read_body(body, {"parent", "name"});
  const auto* parent = read ? read->get("parent") : nullptr;
  const auto* name = read ? read->get("name") : nullptr;
  if (parent == nullptr || name == nullptr) return make_error_response(ApiError::invalid_request);
  auto checked = local_folder_path(*parent);
  if (const auto* error = std::get_if<FolderError>(&checked)) return refused(ApiError::folder_refused, error->message);
  auto made = make_private_folder(std::get<std::filesystem::path>(checked), *name);
  if (const auto* error = std::get_if<FolderError>(&made)) return refused(ApiError::folder_refused, error->message);
  log("[strata] models: a private folder was made for the page");
  return json_answer(201, "{\"path\":" + json_string(path_to_utf8(std::get<std::filesystem::path>(made))) + "}\n");
}

// The page's Model folders setting: roots.allow_untrusted_media as the config file holds it (it applies at the next
// start), and as this server started with it; saved with {"shared": bool} (off: the row is taken out, its default).
HttpResponse CardRoutes::sharing(const std::string_view body) {
  const auto read = read_body(body, {"shared"});
  if (!read || !read->text.empty()) return make_error_response(ApiError::invalid_request);
  const bool editable = settings_.config_file.has_value();
  const auto flag = read->flags.find("shared");
  if (flag == read->flags.end()) {
    bool saved = shared_folders_;
    if (editable) {
      const auto draft = read_config_file(*settings_.config_file);
      if (!std::holds_alternative<FrontendConfigDraft>(draft)) {
        return make_error_response(ApiError::settings_refused, {.reason = config_update_reason(ConfigUpdateFailure::unreadable)});
      }
      saved = allows_shared_folders(std::get<FrontendConfigDraft>(draft));
    }
    return json_answer(200, std::string{"{\"shared\":"} + (saved ? "true" : "false") + ",\"running\":" +
                                (shared_folders_ ? "true" : "false") + ",\"editable\":" + (editable ? "true" : "false") + "}\n");
  }
  if (!editable) {
    return make_error_response(ApiError::settings_refused, {.reason = "this server was started without a config file to write"});
  }
  const bool on = flag->second;
  const std::scoped_lock lock(config_);
  const auto failure = update_config_file(*settings_.config_file, [on](FrontendConfigDraft& draft) {
    if (on) draft.entries["roots.allow_untrusted_media"] = ConfigDraftEntry{true, ConfigSource::file};
    else draft.entries.erase("roots.allow_untrusted_media");
  });
  if (failure) return make_error_response(ApiError::settings_refused, {.reason = config_update_reason(*failure)});
  log(on ? "[strata] models: model folders will be used as they are, other users' access included (from the next start)"
         : "[strata] models: model folders must be private again (from the next start)");
  return json_answer(200, "{\"saved\":true,\"restart_needed\":true}\n");
}

// A card's text, validated with the main config at its defaults (as at boot), as a held card.
namespace {
std::variant<ModelCard, HttpResponse> validated_card(const std::string& json, const FrontendConfigDraft& main,
                                                     const std::filesystem::path& cards_dir,
                                                     const std::function<void(std::string_view)>& log) {
  auto parsed = parse_model_card(json, cards_dir);
  if (const auto* issues = std::get_if<ConfigErrors>(&parsed)) {
    for (const auto& issue : *issues) log("[strata] models: a new card: " + format_issue(issue));
    return refused(ApiError::card_refused, "the card could not be written (see the server log)");
  }
  auto& card = std::get<ModelCard>(parsed);
  auto validated = card_config(main, card, default_option_values(card));
  std::vector<std::string> problems;
  if (const auto* issues = std::get_if<ConfigErrors>(&validated)) {
    for (const auto& issue : *issues) problems.push_back(format_issue(issue));
  } else {
    problems = launch_config_issues(*std::get<ValidatedConfig>(validated).shared(), true);
  }
  if (!problems.empty()) {
    for (const auto& problem : problems) log("[strata] models: " + card.name + ": " + problem);
    return refused(ApiError::card_refused, "the card does not fit this server's settings (see the server log)");
  }
  return std::move(card);
}

// The file in the cards folder holding the card named `name` (the page's cards are <name>.json; a download's is
// named by its catalog id), found by reading each.
std::optional<std::filesystem::path> card_file(const std::filesystem::path& cards_dir, const std::string_view name) {
  std::error_code ec;
  for (std::filesystem::directory_iterator it(cards_dir, ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec) || it->path().extension() != ".json") continue;
    const auto read = read_model_card(it->path());
    if (const auto* card = std::get_if<ModelCard>(&read); card != nullptr && card->name == name) return it->path();
  }
  return std::nullopt;
}

std::optional<std::string> write_card(const std::filesystem::path& cards_dir, const std::string& file_name, const std::string& text,
                                      const TokenStoreMode mode, bool* exists) {
  auto root = open_root(cards_dir, RootPolicy::create_private);
  const auto relative = parse_relative_path(file_name);
  if (!std::holds_alternative<CanonicalRoot>(root) || !std::holds_alternative<RelativePath>(relative)) {
    return "the server's cards folder could not be opened";
  }
  if (const auto failure = write_private_file(std::get<CanonicalRoot>(root), std::get<RelativePath>(relative), text, mode)) {
    if (exists != nullptr) *exists = failure->code == TokenStoreErrorCode::already_exists;
    return "the card could not be written in the server's cards folder";
  }
  return std::nullopt;
}
}  // namespace

HttpResponse CardRoutes::add(const std::string_view body) {
  const auto read = read_body(body, {"folder", "file"});
  const auto* folder = read ? read->get("folder") : nullptr;
  const auto* file = read ? read->get("file") : nullptr;
  if (folder == nullptr || file == nullptr) return make_error_response(ApiError::invalid_request);
  if (!settings_.engine.has_value()) return refused(ApiError::card_refused, "this server names no engine for new cards (models.default_engine)");
  auto trusted = trusted_folder(*folder, shared_folders_);
  if (auto* answer = std::get_if<HttpResponse>(&trusted)) return std::move(*answer);
  const auto& root = std::get<CanonicalRoot>(trusted);
  // a file name in that folder, never a path
  const auto relative = parse_relative_path(*file);
  if (file->find_first_of("/\\") != std::string::npos || !std::holds_alternative<RelativePath>(relative)) {
    return refused(ApiError::card_refused, "pick a .gguf file in that folder");
  }
  // a split model is added by its first file (the card names it; the engine finds the others beside it)
  if (file->size() > 5U && file->compare(file->size() - 5U, 5U, ".gguf") == 0 && !first_shard_or_whole(file->substr(0, file->size() - 5U))) {
    return refused(ApiError::card_refused, "pick the model's first file (its name ends -00001-of-...)");
  }
  auto opened = open_beneath(root, std::get<RelativePath>(relative));
  if (!std::holds_alternative<OpenedFile>(opened)) return refused(ApiError::card_refused, "the model file is not in that folder (or is a link)");
  const auto header = read_gguf_metadata(std::get<OpenedFile>(opened));
  if (!std::holds_alternative<GgufMetadata>(header)) return refused(ApiError::card_refused, "the file is not a GGUF model, or its header is damaged");
  const auto& metadata = std::get<GgufMetadata>(header);
  if (const auto family = packed_family(metadata)) return add_packed(*family, metadata, root, *file);
  // Gemma 4's MTP draft file (owner 2026-10-28): the model's mtp-*.gguf beside it or in MTP/ (find_mtp_gguf), inside
  // the model folder once resolved (no junction out of it) and held to the model folder's rule (private, unless model
  // folders are used as they are); else the card runs without it
  std::optional<std::filesystem::path> mtp;
  if (const auto* arch = metadata.string("general.architecture"); arch != nullptr && *arch == "gemma4") {
    mtp = find_mtp_gguf(root.path(), *file);
    std::error_code ec;
    const auto resolved = mtp ? std::filesystem::canonical(*mtp, ec) : std::filesystem::path{};
    if (mtp && (ec || !is_within(root.path(), resolved))) {
      log("[strata] models: the MTP draft file " + path_to_utf8(mtp->filename()) + " leads out of the model folder: the card runs without it");
      mtp.reset();
    } else if (mtp && !std::holds_alternative<CanonicalRoot>(open_root(mtp->parent_path(), shared_folders_ ? RootPolicy::external : RootPolicy::existing))) {
      log("[strata] models: the MTP draft file " + path_to_utf8(mtp->filename()) +
          " is in a folder other users of this PC can change: the card runs without it");
      mtp.reset();
    } else if (mtp) {
      log("[strata] models: the card drafts with the MTP file " + path_to_utf8(mtp->filename()));
    }
  }
  // vision for cards (spec 2026-10-28): the folder's one mmproj*.gguf is the model's picture encoder (a qwen card's)
  const auto made = card_for_gguf(metadata, {.folder = root.path(), .file = *file, .engine = *settings_.engine, .source = "folder",
                                             .mmproj = find_mmproj(root.path()), .mtp = std::move(mtp)});
  if (const auto* error = std::get_if<CardError>(&made)) return refused(ApiError::card_refused, error->message);
  return publish(std::get<CardText>(made));
}

// Owner 2026-10-27 ("Then work on the add model"): a Qwen3.8 or GLM-5.3 model's card needs its pack, which
// strata-prepare builds first, as for a download - Qwen3.8's (upstream's iq_pack, with --compat-bf16 so any quant packs)
// in the server's packs folder, the model folder left as it is; GLM-5.3's beside the shards, where Maya's engine reads
// them: pack/ as Maya's setup puts it (a pack of this model already there and still matching its files is used as it
// is), or pack-<name>/ when pack/ holds another model's; DeepSeek-V4.1's in maya-ds4/ the same way (maya-ds4-<name>/),
// Maya's shipped DeepSeek profile seeded into it; MiniMax-M3.1's in maya-mm3/ (maya-mm3-<name>/) with Maya's chat template
// (owner 2026-10-28); raon-1-mini's as GLM-5.3's (strata-prepare's raon1 port of the pack
// our raon1 setup built), its marker the served model name (owner 2026-10-27).
HttpResponse CardRoutes::add_packed(const PackedFamily family, const GgufMetadata& metadata, const CanonicalRoot& root, const std::string& file) {
  if (!settings_.prepare || (family == PackedFamily::qwen38 && !settings_.packs_dir)) {
    return refused(ApiError::card_refused, "this model needs a pack built first, and strata-prepare is not beside this server's engine");
  }
  if (file.size() <= 5U || file.compare(file.size() - 5U, 5U, ".gguf") != 0) return refused(ApiError::card_refused, "the model file must be a .gguf file");
  const std::string stem = file.substr(0, file.size() - 5U);
  const std::string name = card_name_for_file(stem);
  if (name.empty()) return refused(ApiError::card_refused, "the model file's name gives no usable card name");
  if (launcher_.card(name)) return make_error_response(ApiError::card_exists);
  const auto model = root.path() / path_from_utf8(file);
  PackedCardRequest request{.family = family, .name = name, .engine = *settings_.engine, .source = "folder"};
  const auto built = [&](std::vector<std::string> arguments) -> std::optional<HttpResponse> {
    log("[strata] models: " + name + ": building its pack (strata-prepare " + arguments.front() + ")");
    if (const auto failure = settings_.prepare(arguments)) {
      log("[strata] models: " + name + ": its pack was not built: " + *failure);
      return refused(ApiError::card_refused, "its pack could not be built (" + *failure + ")");
    }
    return std::nullopt;
  };
  if (family == PackedFamily::qwen38) {
    auto packs = open_root(*settings_.packs_dir, RootPolicy::create_private);
    if (!std::holds_alternative<CanonicalRoot>(packs)) return refused(ApiError::card_refused, "the server's packs folder could not be made");
    request.pack = std::get<CanonicalRoot>(packs).path() / path_from_utf8(name);
    if (auto failure = built({"download", "--gguf", path_to_utf8(model), "--out", path_to_utf8(request.pack), "--pack-arg=--compat-bf16"})) {
      return std::move(*failure);
    }
    // prepared.json: the PLE table's shard (the card names it for two shards at most, upstream setup's rule)
    std::ifstream in(request.pack / "prepared.json", std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    const auto parsed = parse_json(text.str(), JsonLimits{.max_bytes = 1U << 16U, .max_depth = 4U, .max_container_entries = 16U,
                                                          .max_string_bytes = 4096U, .allow_fractions = false});
    const auto* prepared = std::get_if<JsonValue>(&parsed);
    const auto* members = prepared != nullptr ? prepared->if_object() : nullptr;
    const JsonValue* ple = nullptr;
    const JsonValue* shards = nullptr;
    if (members != nullptr) {
      for (const auto& member : *members) {
        if (member.key == "ple") ple = &member.value;
        if (member.key == "shards") shards = &member.value;
      }
    }
    const std::int64_t shard_count = shards != nullptr && shards->if_integer() != nullptr ? *shards->if_integer() : 0;
    if (ple != nullptr && ple->if_string() != nullptr && !ple->if_string()->empty() && shard_count <= 2) request.ple_gguf = *ple->if_string();
    request.folder = root.path();
    request.model_id = stem;
    request.mmproj = find_mmproj(root.path());   // the model's picture encoder beside it (spec 2026-10-28)
    if (request.mmproj) log("[strata] models: " + name + ": reads pictures with " + path_to_utf8(request.mmproj->filename()));
    // the bundled expert profile for the model's expert set: the original's 512 experts, the Coder's 256 (upstream's
    // setup gives the Coder expert-profile-coder.bin, every other Qwen3.8 model expert-profile.bin)
    const auto experts = metadata.integer("qwen4exp.expert_count");
    const char* profile = experts == std::optional<std::int64_t>{512} ? "expert-profile.bin"
                          : experts == std::optional<std::int64_t>{256} ? "expert-profile-coder.bin" : nullptr;
    std::error_code ec;
    if (profile != nullptr && std::filesystem::is_regular_file(settings_.engine->parent_path() / "profiles" / profile, ec)) {
      request.expert_profile = settings_.engine->parent_path() / "profiles" / profile;
    }
    // the MTP draft layer upstream's setup made (owner 2026-10-28: MTP settings on the cards), held to the model
    // folder's rule: private, unless model folders are used as they are
    if (const auto rt = find_mtp_runtime(root.path())) {
      if (std::holds_alternative<CanonicalRoot>(open_root(*rt, shared_folders_ ? RootPolicy::external : RootPolicy::existing))) {
        request.mtp = *rt;
        log("[strata] models: " + name + ": the MTP draft layer in " + path_to_utf8(*rt) + " is used");
      } else {
        log("[strata] models: " + name + ": the MTP draft layer in " + path_to_utf8(*rt) +
            " is not private (other users of this PC can change it): the card runs without it");
      }
    }
  } else if (family == PackedFamily::deepseek || family == PackedFamily::minimax) {
    // Maya's DeepSeek layout: <folder>/maya-ds4 (maya.py ds4_pack_step), or maya-ds4-<name>/ when that one indexes
    // another model's shards; the shipped profile beside the engine for strata-prepare to seed (ds4_seed_profile).
    // MiniMax-M3.1's the same in maya-mm3 (mm3_pack_step), with Maya's chat template the install ships beside the engine.
    const bool deepseek = family == PackedFamily::deepseek;
    const std::string folder_name = deepseek ? "maya-ds4" : "maya-mm3";
    std::vector<std::string> extra;
    std::error_code ec;
    if (deepseek) {
      const auto profile = settings_.engine->parent_path() / "profiles" / "expert-profile-ds4.bin";
      if (std::filesystem::is_regular_file(profile, ec)) extra = {"--expert-profile", path_to_utf8(profile)};
    } else {
      const auto chat_template = settings_.engine->parent_path() / "templates" / "minimax-m3.1" / "chat_template.jinja";
      if (!std::filesystem::is_regular_file(chat_template, ec)) {
        return refused(ApiError::card_refused, "MiniMax's chat template is not beside this server's engine (engine/templates/minimax-m3.1)");
      }
      extra = {"--template", path_to_utf8(chat_template)};
    }
    request.pack = root.path() / path_from_utf8(folder_name);
    if (const auto sources = pack_recorded_files(request.pack); sources && std::find(sources->begin(), sources->end(), file) == sources->end()) {
      std::string lower_name = name;
      for (auto& c : lower_name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      request.pack = root.path() / path_from_utf8(folder_name + "-" + lower_name);
    }
    std::vector<std::string> arguments{"download", "--maya", "--gguf", path_to_utf8(model), "--out", path_to_utf8(request.pack)};
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    if (auto failure = built(std::move(arguments))) return std::move(*failure);
    if (!shared_folders_ && !std::holds_alternative<CanonicalRoot>(open_root(request.pack)) && !make_tree_owner_only(request.pack)) {
      return refused(ApiError::card_refused, "its pack could not be made private");
    }
    // the inventory's model, whose folder the launch passes as --pack: Maya's model name
    if (!write_pack_marker(request.pack, deepseek ? kDeepseekModelName : kMinimaxModelName)) {
      return refused(ApiError::card_refused, "the pack's model file could not be written");
    }
  } else {
    request.pack = root.path() / "pack";
    if (const auto source = pack_indexed_gguf(request.pack); source && *source != file) {
      std::string lower_name = name;
      for (auto& c : lower_name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      request.pack = root.path() / path_from_utf8("pack-" + lower_name);
    }
    if (auto failure = built({"download", "--maya", "--gguf", path_to_utf8(model), "--out", path_to_utf8(request.pack)})) return std::move(*failure);
    // the card's model folder: owner-only as the server checks one (a pack strata-prepare just made inherits its folder's),
    // or as it is where the owner has the server use model folders as they are
    if (!shared_folders_ && !std::holds_alternative<CanonicalRoot>(open_root(request.pack)) && !make_tree_owner_only(request.pack)) {
      return refused(ApiError::card_refused, "its pack could not be made private");
    }
    // the inventory's model, whose folder the launch passes as --glm-pack: Maya's model name (raon1: the served one)
    if (!write_pack_marker(request.pack, family == PackedFamily::raon1 ? kRaon1ModelName : kGlmModelName)) {
      return refused(ApiError::card_refused, "the pack's model file could not be written");
    }
  }
  return publish(card_for_pack(request));
}

// A card made from a model folder: validated as at boot, written owner-only in the server's cards folder, held.
HttpResponse CardRoutes::publish(const CardText& text) {
  const std::scoped_lock lock(files_);
  if (launcher_.card(text.name)) return make_error_response(ApiError::card_exists);
  auto card = validated_card(text.json, settings_.main, settings_.cards_dir, [this](const std::string_view line) { log(line); });
  if (auto* answer = std::get_if<HttpResponse>(&card)) return std::move(*answer);
  const std::string file_name = text.name + ".json";
  bool exists = false;
  if (const auto failure = write_card(settings_.cards_dir, file_name, text.json, TokenStoreMode::create_new, &exists)) {
    return exists ? make_error_response(ApiError::card_exists) : refused(ApiError::card_refused, *failure);
  }
  if (!launcher_.add_card(std::get<ModelCard>(std::move(card)))) {
    std::error_code ec;
    std::filesystem::remove(settings_.cards_dir / file_name, ec);
    return make_error_response(ApiError::card_exists);
  }
  log("[strata] models: card " + text.name + " added from a model folder");
  return json_answer(201, "{\"name\":" + json_string(text.name) + ",\"profile\":" + json_string(profile_name(text.profile)) + "}\n");
}

HttpResponse CardRoutes::repoint(const std::string_view body) {
  const auto read = read_body(body, {"name", "folder"});
  const auto* name = read ? read->get("name") : nullptr;
  const auto* folder = read ? read->get("folder") : nullptr;
  if (name == nullptr || folder == nullptr) return make_error_response(ApiError::invalid_request);
  const std::scoped_lock lock(files_);
  const auto held = launcher_.card(*name);
  if (!held) return make_error_response(ApiError::model_not_found);
  if (!held->page_card) return make_error_response(ApiError::card_read_only);
  if (held->state == CardState::starting) return make_error_response(ApiError::card_in_use);
  auto trusted = trusted_folder(*folder, shared_folders_);
  if (auto* answer = std::get_if<HttpResponse>(&trusted)) return std::move(*answer);
  const auto& root = std::get<CanonicalRoot>(trusted);
  const auto* model_id = text_row(held->card, "engine.model_id");
  const auto model_file = model_id == nullptr ? std::variant<RelativePath, PathError>{PathError{PathErrorCode::empty, ""}}
                                              : parse_relative_path(*model_id + ".gguf");
  if (!std::holds_alternative<RelativePath>(model_file) ||
      !std::holds_alternative<OpenedFile>(open_beneath(root, std::get<RelativePath>(model_file)))) {
    return refused(ApiError::card_refused, "the card's model file is not in that folder");
  }
  const auto file = card_file(settings_.cards_dir, *name);
  if (!file) return refused(ApiError::card_refused, "the card's file is not in the server's cards folder");
  const auto old_text = read_trusted_config_text(*file);
  if (!std::holds_alternative<TrustedConfigText>(old_text)) return refused(ApiError::card_refused, "the card's file could not be read");
  const std::string& before = std::get<TrustedConfigText>(old_text).text;
  const auto rewritten = card_with_folder(before, root.path());
  if (const auto* error = std::get_if<CardError>(&rewritten)) return refused(ApiError::card_refused, error->message);
  auto card = validated_card(std::get<std::string>(rewritten), settings_.main, settings_.cards_dir,
                             [this](const std::string_view line) { log(line); });
  if (auto* answer = std::get_if<HttpResponse>(&card)) return std::move(*answer);
  const std::string file_name = path_to_utf8(file->filename());
  if (const auto failure = write_card(settings_.cards_dir, file_name, std::get<std::string>(rewritten), TokenStoreMode::replace, nullptr)) {
    return refused(ApiError::card_refused, *failure);
  }
  const auto changed = launcher_.replace_card(std::get<ModelCard>(std::move(card)));
  if (changed != CardChange::done) {
    (void)write_card(settings_.cards_dir, file_name, before, TokenStoreMode::replace, nullptr);   // the file as it was
    return make_error_response(changed == CardChange::not_found ? ApiError::model_not_found : ApiError::card_in_use);
  }
  log("[strata] models: card " + *name + " now reads its model from another folder");
  return json_answer(200, "{\"name\":" + json_string(*name) + "}\n");
}

HttpResponse CardRoutes::delete_begin(const std::string_view body) {
  const auto read = read_body(body, {"name"});
  const auto* name = read ? read->get("name") : nullptr;
  if (name == nullptr) return make_error_response(ApiError::invalid_request);
  if (!settings_.key_matches) {
    return refused(ApiError::card_refused, "this server runs without an API key, so the page can't confirm a delete; delete the files yourself");
  }
  const auto held = launcher_.card(*name);
  if (!held) return make_error_response(ApiError::model_not_found);
  if (!held->page_card) return make_error_response(ApiError::card_read_only);
  if (held->state == CardState::running || held->state == CardState::starting) return make_error_response(ApiError::card_in_use);
  if (!gguf_card(held->card)) return refused(ApiError::card_refused, "only a model added from its GGUF files can be deleted from the page");
  const auto code = codes_.issue(*name, DeleteCodes::Clock::now());
  if (!code) return make_error_response(ApiError::internal_error);
  auto answer = json_answer(200, "{\"code\":" + json_string(*code) + "}\n");
  answer.headers.emplace_back("Cache-Control", "no-store");
  return answer;
}

HttpResponse CardRoutes::delete_confirm(const std::string_view body) {
  const auto read = read_body(body, {"name", "code", "key", "permanent"});
  const auto* name = read ? read->get("name") : nullptr;
  const auto* code = read ? read->get("code") : nullptr;
  if (name == nullptr || code == nullptr) return make_error_response(ApiError::invalid_request);
  // All three, each checked: the code (spent by this try, right or wrong), the key, the permanent flag.
  const bool code_ok = codes_.claim(*name, *code, DeleteCodes::Clock::now());
  const auto* key = read->get("key");
  const bool key_ok = key != nullptr && settings_.key_matches && settings_.key_matches(*key);
  const auto permanent = read->flags.find("permanent");
  const bool confirmed = permanent != read->flags.end() && permanent->second;
  if (!code_ok || !key_ok || !confirmed) {
    log("[strata] models: a delete of " + *name + " was refused (the code, the key or the confirmation)");
    return make_error_response(ApiError::delete_refused);
  }
  const std::scoped_lock lock(files_);
  const auto held = launcher_.card(*name);
  if (!held) return make_error_response(ApiError::model_not_found);
  if (!held->page_card) return make_error_response(ApiError::card_read_only);
  if (!gguf_card(held->card)) return refused(ApiError::card_refused, "only a model added from its GGUF files can be deleted from the page");
  const std::string folder = *text_row(held->card, "roots.models");
  const std::string model_id = *text_row(held->card, "engine.model_id");
  // the folder is still the trusted folder the card names (not swapped for a link since)
  auto trusted = trusted_folder(folder, shared_folders_);
  if (auto* answer = std::get_if<HttpResponse>(&trusted)) return std::move(*answer);
  const auto file = card_file(settings_.cards_dir, *name);
  switch (launcher_.remove_card(*name)) {
    case CardChange::done: break;
    case CardChange::not_found: return make_error_response(ApiError::model_not_found);
    case CardChange::not_page_card: return make_error_response(ApiError::card_read_only);
    case CardChange::shutting_down: return make_error_response(ApiError::server_shutting_down);
    case CardChange::busy:
    case CardChange::running:
    case CardChange::stopping: return make_error_response(ApiError::card_in_use);
  }
  const auto deleted = delete_gguf_model_files(std::get<CanonicalRoot>(trusted).path(), model_id);
  if (const auto* error = std::get_if<CardError>(&deleted)) {
    (void)launcher_.add_card(held->card);   // nothing was removed: the card stays
    return refused(ApiError::card_refused, error->message);
  }
  if (file) {
    std::error_code ec;
    std::filesystem::remove(*file, ec);
  }
  const auto& done = std::get<DeletedFiles>(deleted);
  log("[strata] models: deleted " + *name + " and its model files (" + std::to_string(done.files) + " files, " +
      std::to_string(done.bytes) + " bytes)");
  // a downloaded model also leaves its download store, so the Download list offers it again
  std::string download;
  constexpr std::string_view kDownloadSource = "download:";
  if (held->card.source.rfind(kDownloadSource, 0) == 0U && settings_.remove_download) {
    const std::string id = held->card.source.substr(kDownloadSource.size());
    const auto failure = settings_.remove_download(id);
    if (failure) log("[strata] models: " + *name + "'s download " + id + " is still in its store (" + *failure + ")");
    download = std::string{",\"download_removed\":"} + (failure ? "false" : "true");
  }
  return json_answer(200, "{\"name\":" + json_string(*name) + ",\"files\":" + std::to_string(done.files) +
                              ",\"bytes\":" + std::to_string(done.bytes) +
                              ",\"folder_removed\":" + (done.folder_removed ? "true" : "false") + download + "}\n");
}

// Owner 2026-10-28 ("an ability to remove an active model card but not delete files"): a card the page made goes off
// the list - its card file, remembered options and calibration with it - and its model files stay where they are (a
// download's store keeps it too); the running card's model is stopped first. Nothing permanent, so no code or key.
HttpResponse CardRoutes::remove(const std::string_view body) {
  const auto read = read_body(body, {"name"});
  const auto* name = read ? read->get("name") : nullptr;
  if (name == nullptr) return make_error_response(ApiError::invalid_request);
  const std::scoped_lock lock(files_);
  const auto held = launcher_.card(*name);
  if (!held) return make_error_response(ApiError::model_not_found);
  if (!held->page_card) return make_error_response(ApiError::card_read_only);
  const auto file = card_file(settings_.cards_dir, *name);
  const auto changed = launcher_.remove_card(*name, RunningCard::stop_first);
  switch (changed) {
    case CardChange::done:
    case CardChange::stopping: break;
    case CardChange::not_found: return make_error_response(ApiError::model_not_found);
    case CardChange::not_page_card: return make_error_response(ApiError::card_read_only);
    case CardChange::shutting_down: return make_error_response(ApiError::server_shutting_down);
    case CardChange::busy:
    case CardChange::running: return make_error_response(ApiError::card_in_use);
  }
  if (file) {
    std::error_code ec;
    std::filesystem::remove(*file, ec);
  }
  const bool stopping = changed == CardChange::stopping;
  log("[strata] models: card " + *name + (stopping ? " is removed once its model has stopped" : " removed") + "; its model files stay");
  return json_answer(200, "{\"name\":" + json_string(*name) + ",\"stopping\":" + (stopping ? "true" : "false") + "}\n");
}

}  // namespace secure_frontend
