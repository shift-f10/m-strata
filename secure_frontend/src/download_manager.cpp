#include "secure_frontend/download_manager.hpp"

#include "secure_frontend/api_errors.hpp"
#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/card_builder.hpp"
#include "secure_frontend/config_file.hpp"
#include "secure_frontend/engine_stdio_process.hpp"
#include "secure_frontend/gguf_metadata.hpp"
#include "secure_frontend/json.hpp"
#include "secure_frontend/local_folders.hpp"
#include "secure_frontend/model_index_store.hpp"
#include "secure_frontend/sha256.hpp"
#include "secure_frontend/token_store.hpp"

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <utility>

namespace secure_frontend {
namespace {

// Every download lands in the store as <id>:download (one tag: a card is one model).
constexpr std::string_view kStoreTag = "download";
constexpr auto kLinkValidFor = std::chrono::minutes(10);
constexpr JsonLimits kChildJsonLimits{.max_bytes = 1U << 20U, .max_depth = 8U, .max_container_entries = 65536U,
                                      .max_string_bytes = 4096U, .allow_fractions = false};

const JsonValue* member(const JsonValue& object, const std::string_view key) {
  const auto* members = object.if_object();
  if (members == nullptr) return nullptr;
  for (const auto& m : *members) {
    if (m.key == key) return &m.value;
  }
  return nullptr;
}

// Decimal GB with one decimal, as the page writes sizes.
std::string decimal_gb(const std::uint64_t bytes) {
  char text[32];
  (void)std::snprintf(text, sizeof text, "%.1f", static_cast<double>(bytes) / 1e9);
  return text;
}

std::string json_text(const std::string_view text) {
  std::string out;
  append_json_string(&out, text);
  return out;
}

std::optional<std::uint64_t> number(const std::string_view text) {
  std::uint64_t value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
  return value;
}

std::vector<std::string_view> words(const std::string_view line) {
  std::vector<std::string_view> out;
  std::size_t start = 0;
  while (start < line.size()) {
    const auto end = line.find(' ', start);
    out.push_back(line.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
    if (end == std::string_view::npos) break;
    start = end + 1U;
  }
  return out;
}

// Owner 2026-10-23: "for custom download from huggingface, the downloads must be matched to a
// current settings we have available like qwen 3.8 flash next, swift, gemma4, qwen3.6 etc." A
// pasted GGUF is matched to one of these families. The qwen ones all run on the qwen engine by its
// own GGUF-direct path (--native, the tokenizer built from the GGUF): for a pasted single GGUF
// there is no iq_pack to download, so they share one recipe and differ only in the context they
// open with. Gemma 4 and flint are their own engines: GUI-first start-up (spec 2026-10-27 §4.3) makes
// their card from the GGUF itself (card_for_gguf; the tokenizer is read from the GGUF at each
// start), so they are runnable whenever the server names that engine (models.default_engine).
struct CustomProfile final {
  std::string_view key;      // the matched card's family and the page's value
  std::string_view display;  // the page's label
  std::string_view engine;   // "qwen" | "gemma4" | "flint"
  bool prepares;             // qwen: a runnable card is written after the download (gemma4, flint: gguf_family)
  std::uint64_t context_default;
  std::uint64_t context_min;
  std::uint64_t context_max;
};
constexpr std::array<CustomProfile, 8> kCustomProfiles{{
    {"qwen", "Qwen3.8-Flash-Next", "qwen", true, 131072U, 4096U, 1048576U},
    {"unsloth", "Qwen3.8-Flash-Next (Unsloth)", "qwen", true, 131072U, 4096U, 1048576U},
    {"coder", "Qwen3.8-Flash-Next-Coder", "qwen", true, 131072U, 4096U, 1048576U},
    {"gyro", "Qwen3.8-Flash-Next-Gyro", "qwen", true, 131072U, 4096U, 1048576U},
    {"swift", "Swift-1.5", "qwen", true, 131072U, 4096U, 1048576U},
    {"qwen36", "Qwen3.6-35B-A3B", "qwen", true, 32768U, 4096U, 262144U},
    {"gemma4", "Gemma 4", "gemma4", false, 262144U, 4096U, 262144U},
    {"flint", "flint", "flint", false, 262144U, 4096U, 262144U},
}};
// A runnable card follows the download: qwen's by its prepare step (Qwen3.6's from its GGUF where the server names an
// engine for new cards: gguf_family), Gemma 4's and flint's from the GGUF itself when the server names that engine
// (models.default_engine).
bool profile_runnable(const CustomProfile& profile, const bool gguf_engine) {
  return profile.engine == "qwen" ? profile.prepares : gguf_engine;
}

const CustomProfile* custom_profile(const std::string_view key) {
  for (const auto& profile : kCustomProfiles) {
    if (profile.key == key) return &profile;
  }
  return nullptr;
}

// A served name for a card: the display name with every character outside [A-Za-z0-9._-] as '-'.
std::string card_name_for(const DownloadEntry& entry) {
  std::string name;
  for (const char c : entry.name) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    name.push_back(ok ? c : '-');
  }
  while (!name.empty() && (name.front() == '-' || name.front() == '.' || name.front() == '_')) name.erase(name.begin());
  if (name.size() > 64U) name.resize(64U);
  return name.empty() ? entry.id : name;
}

// The model file's stem (a split model's first shard): what a card from a GGUF names as its model (engine.model_id).
std::string model_file_stem(const DownloadEntry& entry) {
  if (entry.files.empty()) return {};
  const std::string first = gguf_shard_set(entry.files.front().path).front();
  std::string name = first.substr(first.rfind('/') == std::string::npos ? 0 : first.rfind('/') + 1U);
  if (name.size() > 5U && name.substr(name.size() - 5U) == ".gguf") name.resize(name.size() - 5U);
  return name;
}

// The catalog's row for that first shard (its pinned size), or nullptr when the entry does not list it.
const CatalogFile* first_model_file(const DownloadEntry& entry) {
  if (entry.files.empty()) return nullptr;
  const std::string first = gguf_shard_set(entry.files.front().path).front();
  for (const auto& file : entry.files) {
    if (file.path == first) return &file;
  }
  return nullptr;
}

// A card the launcher holds, as the list matches it to the catalog (owner 2026-10-28): the download it came from (its
// source download:<id>), and the name and size of the model file it runs (card_model_file; the size when the file is there).
struct HeldModel final {
  std::string name;
  std::string download;
  std::string file;
  std::optional<std::uint64_t> size;
};

std::vector<HeldModel> held_models(const std::function<std::vector<ModelCard>()>& cards) {
  std::vector<HeldModel> held;
  if (!cards) return held;
  constexpr std::string_view kDownload = "download:";
  for (const auto& card : cards()) {
    HeldModel model{.name = card.name, .download = {}, .file = {}, .size = std::nullopt};
    if (card.source.rfind(kDownload, 0) == 0U) model.download = card.source.substr(kDownload.size());
    if (const auto file = card_model_file(card)) {
      model.file = path_to_utf8(file->filename());
      std::error_code ec;
      const auto size = std::filesystem::file_size(*file, ec);
      if (!ec) model.size = size;
    }
    held.push_back(std::move(model));
  }
  return held;
}

// <cards>/<id>.json is download <id>'s own card: one this manager wrote, its source that download's - or none, a card the
// prepare step wrote before cards named their source; a file that cannot be read counts as before. A card added from a
// folder whose file only shares the name (the page names a card's file after the card: on Windows "Gemma-Small.json" is
// "gemma-small.json") is not, so that download is not taken for done.
bool download_card_file(const std::filesystem::path& cards_dir, const std::string& id) {
  const auto file = cards_dir / path_from_utf8(id + ".json");
  std::error_code ec;
  if (!std::filesystem::is_regular_file(file, ec)) return false;
  if (const auto size = std::filesystem::file_size(file, ec); ec || size > (1U << 20U)) return true;   // not read: as before
  std::ifstream in(file, std::ios::binary);
  if (!in) return true;
  std::ostringstream text;
  text << in.rdbuf();
  // a card's options hold fractions (temperature 1.0, min_p 0.05)
  constexpr JsonLimits kCardLimits{.max_bytes = 1U << 20U, .max_depth = 8U, .max_container_entries = 65536U, .max_string_bytes = 4096U,
                                   .allow_fractions = true};
  const auto parsed = parse_json(text.str(), kCardLimits);
  const auto* root = std::get_if<JsonValue>(&parsed);
  const auto* source = root == nullptr ? nullptr : member(*root, "source");
  return source == nullptr || (source->if_string() != nullptr && *source->if_string() == "download:" + id);
}

// What this PC already has of a catalog entry (owner 2026-10-28, "Locally Provided vs Downloaded"): "downloaded" - a card
// from this download, or the download ready or installed (`downloaded`); "local" - another card runs the same model, its
// file there: the entry's first model file by name, and by size too where the catalog pins one (another quant of that
// name is not it; nothing is hashed); a card whose file is gone (deleted, a drive not plugged in) is not. "other" - no
// card has that file, but one has a file of that name and another size (another revision of it: the owner's
// raon-1-mini Q8_0 is 64 bytes larger than the catalog's); the download still brings the catalog's. Empty: none of these.
// With it the card's name (empty: none).
std::pair<std::string_view, std::string> have_of(const DownloadEntry& entry, const bool downloaded, const std::vector<HeldModel>& held) {
  for (const auto& model : held) {
    if (model.download == entry.id) return {"downloaded", model.name};
  }
  const std::string stem = model_file_stem(entry);
  const auto* first = first_model_file(entry);
  std::string local;
  std::string other;
  for (const auto& model : held) {
    if (stem.empty() || model.file != stem + ".gguf" || !model.size) continue;
    if (first != nullptr && first->size && *first->size != *model.size) {
      if (other.empty()) other = model.name;
      continue;
    }
    local = model.name;
    break;
  }
  if (downloaded) return {"downloaded", local};
  if (!local.empty()) return {"local", local};
  if (!other.empty()) return {"other", other};
  return {};
}

// strata's argv: --config <file>, then --store <folder> when the download goes elsewhere than models.downloads_root.
std::vector<std::string> store_arguments(const std::filesystem::path& config, const std::filesystem::path& downloads_root,
                                         const std::filesystem::path& store, std::vector<std::string> verb) {
  std::vector<std::string> arguments{"--config", path_to_utf8(config)};
  if (store != downloads_root) {
    arguments.emplace_back("--store");
    arguments.push_back(path_to_utf8(store));
  }
  arguments.insert(arguments.end(), std::make_move_iterator(verb.begin()), std::make_move_iterator(verb.end()));
  return arguments;
}

// A custom card's id from its repository and file: lowercase [a-z0-9._-], at most 64.
std::string custom_id(const std::string_view repo, const std::string_view path) {
  std::string base = "hf-" + std::string{repo.substr(repo.find('/') + 1U)} + "-" +
                     std::string{path.substr(path.rfind('/') == std::string_view::npos ? 0 : path.rfind('/') + 1U)};
  std::string id;
  for (char c : base) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    id.push_back(ok ? c : '-');
  }
  if (id.size() > 5U && id.substr(id.size() - 5U) == ".gguf") id.resize(id.size() - 5U);
  if (id.size() > 64U) id.resize(64U);
  return id;
}

class StdioLineChild final : public LineChild {
 public:
  explicit StdioLineChild(std::unique_ptr<EngineStdioProcess> process) : process_(std::move(process)) {}
  ~StdioLineChild() override {
    if (!ended()) (void)process_->terminate(std::chrono::milliseconds(250));
  }
  std::optional<std::string> next_line(const std::chrono::milliseconds timeout) override {
    if (closed_) return std::nullopt;
    auto read = process_->read_line(timeout);
    if (read.kind == EngineReadKind::line) return std::move(read.line);
    if (read.kind == EngineReadKind::closed || read.kind == EngineReadKind::failure) closed_ = true;
    return std::nullopt;
  }
  bool ended() override {
    if (!closed_) return false;
    if (!status_) status_ = process_->try_reap();
    if (!status_ && !process_->running()) status_ = process_->exit_status();
    return status_.has_value() || !process_->running();
  }
  int exit_code() override { return status_ && !status_->signaled ? static_cast<int>(status_->value) : -1; }
  void kill() override {
    (void)process_->terminate(std::chrono::milliseconds(250));
    closed_ = true;
  }

 private:
  std::unique_ptr<EngineStdioProcess> process_;
  bool closed_{false};
  std::optional<ExitStatus> status_;
};

// A downloaded file where the store keeps it: <version>/<its catalog path>, the catalog's subfolder kept (Maya's
// repositories put each quant in one, Maya-XS/...; owner 2026-10-28: a DeepSeek pack failed with "missing_gguf"
// when only the name was kept), with this system's separators.
// Vision for cards (spec 2026-10-28): the entries whose picture encoder a card here reads pictures with - a qwen
// model's, pinned to a commit with its size (GLM-5.3's encoders stay unused: its cards are text only here).
bool vision_offered(const DownloadEntry& entry) {
  if (entry.engine != "qwen" || !entry.mmproj || !entry.mmproj->size) return false;
  return is_hf_commit(entry.mmproj->revision.empty() ? entry.revision : entry.mmproj->revision);
}

// The engines whose downloads Maya's step packs (prepare maya_pack; strata-prepare download --maya).
bool maya_pack_engine(const std::string_view engine) {
  return engine == "glm53" || engine == "raon1" || engine == "deepseek" || engine == "minimax";
}

std::filesystem::path stored_file(const std::filesystem::path& version, const std::string& relative) {
  return (version / path_from_utf8(relative)).make_preferred();
}

// Every file of the download in its store version, at its pinned size where it has one (sizes only: the downloader
// checked each SHA-256 before it published them).
bool stored_whole(const std::filesystem::path& version, const HfDownload& download) {
  for (const auto& file : download.files) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(stored_file(version, file.path), ec);
    if (ec || (file.size != 0U && size != file.size)) return false;
  }
  return !download.files.empty();
}

// Why a pull failed, in words (owner 2026-10-28: "did not even say why it failed"): the downloader's RESULT reason, or
// how it ended without one. Its own text goes nowhere (its stderr is NUL), so the page says these.
std::string pull_failure(const std::string_view result, const int exit_code) {
  struct Said final {
    std::string_view reason;
    std::string_view text;
  };
  static constexpr Said kSaid[] = {
      {"disk_space", "insufficient disk space on the data drive"},
      {"total_budget_exceeded", "the download is larger than the space downloads may use"},
      {"digest_mismatch", "a file did not match its SHA-256 (it was deleted)"},
      {"size_exceeded", "a file came larger than its pinned size (it was deleted)"},
      {"dns_failed", "the download host's name did not resolve: check this PC's network, then Download again"},
      {"connect_failed", "the download host could not be reached: check this PC's network, then Download again"},
      {"tls_failed", "the secure connection to the download host failed; Download again"},
      {"tls_untrusted", "the download host's certificate is not trusted"},
      {"stalled", "the transfer stalled (no data for too long); Download again resumes it"},
      {"size_short", "the connection ended before the file did; Download again resumes it"},
      {"transport_failed", "the transfer failed; Download again resumes it"},
      {"time_budget_exceeded", "the download took longer than allowed; Download again resumes it"},
      {"http_status", "the download host refused the file (an HTTP error): it may have moved"},
      {"range_mismatch", "the host sent another part of the file than asked; Download again starts that file over"},
      {"size_mismatch_announced", "the host announced another size than the pinned one: the file may have changed"},
      {"host_not_allowed", "a link led to a host downloads may not use"},
      {"redirect_not_allowed", "the host redirected to a place downloads may not use"},
      {"redirect_limit", "the host redirected too many times"},
      {"address_not_allowed", "the host's address is one downloads may not use"},
      {"response_malformed", "the host's answer was malformed; Download again"},
      {"models_busy", "the download folder is in use by another strata; Download again in a moment"},
      {"publish_failed", "the files came, but could not be put in place in the download folder"},
      {"index_write_failed", "the download folder's index could not be written"},
      {"index_inconsistent", "the download folder's index does not match its files"},
      {"interrupted", "the downloader was interrupted; Download again resumes it"},
      {"downloader_missing", "the downloader is missing beside the server"},
  };
  const std::string code = std::to_string(exit_code);
  if (result.empty()) {
    return exit_code < 0 ? std::string{"the downloader ended unexpectedly"}
                         : "the downloader ended with exit code " + code + " before it said how the download went";
  }
  if (result == "ok") {
    return "the downloader said the files came, then ended with exit code " + code +
           ", and the download folder does not hold every file at its size; Download again";
  }
  for (const auto& said : kSaid) {
    if (said.reason == result) return std::string{said.text};
  }
  return "the download failed (" + std::string{result} + ", exit code " + code + ")";
}

}  // namespace

namespace {

HttpResponse json_answer(const int status, std::string body) {
  return HttpResponse{.status = status, .headers = {{"Content-Type", "application/json; charset=utf-8"}}, .body = std::move(body)};
}

// A manager refusal as its ApiError row (the manager names only rows of the table).
ApiError error_for(const DownloadManager::Refusal& refusal) {
  for (const auto& row : api_error_table()) {
    if (row.code == refusal.code && row.status == refusal.status) return row.error;
  }
  return ApiError::internal_error;
}

HttpResponse refusal_answer(const DownloadManager::Refusal& refusal) { return make_error_response(error_for(refusal)); }

// {"state": "<state>", "id": "<id>"}: what the request did (the list shows how it goes).
HttpResponse accepted(const std::string_view state, const std::string_view id) {
  return json_answer(202, "{\"state\":" + json_text(state) + ",\"id\":" + json_text(id) + "}\n");
}

constexpr JsonLimits kRouteBodyLimits{.max_bytes = kModelControlBodyLimit, .max_depth = 2U, .max_container_entries = 8U,
                                      .max_string_bytes = 2048U, .allow_fractions = false};

}  // namespace

HttpResponse download_route_answer(DownloadManager& manager, const RouteId route, const std::string_view body) {
  if (route == RouteId::strata_downloads) return json_answer(200, manager.list_json());
  if (route != RouteId::strata_download_start && route != RouteId::strata_download_pause && route != RouteId::strata_download_cancel &&
      route != RouteId::strata_download_link) {
    return make_error_response(ApiError::internal_error);
  }
  // {"id": "<card>"} | {"url": "<link>"} | {"url": "<link>", "confirm": true}; the link check also
  // takes {"profile": "<family>"} and {"extra_urls": ["<part>", ...]} (the multipart shards).
  const auto parsed = parse_json(body, kRouteBodyLimits);
  const auto* root = std::get_if<JsonValue>(&parsed);
  if (root == nullptr) return make_error_response(ApiError::invalid_json);
  const auto* members = root->if_object();
  if (members == nullptr) return make_error_response(ApiError::invalid_request);
  const std::string* id = nullptr;
  const std::string* url = nullptr;
  const std::string* profile = nullptr;
  const std::string* folder_text = nullptr;   // spec 2026-10-27 §4.3: the drive or folder it goes to (start, link)
  std::vector<std::string> extra_urls;
  bool have_extra = false;
  bool confirm = false;
  std::optional<bool> vision;   // spec 2026-10-28: the "Vision files" checkbox (a catalog start)
  for (const auto& item : *members) {
    if (item.key == "id" && item.value.if_string() != nullptr) id = item.value.if_string();
    else if (item.key == "url" && item.value.if_string() != nullptr) url = item.value.if_string();
    else if (item.key == "folder" && item.value.if_string() != nullptr) folder_text = item.value.if_string();
    else if (item.key == "confirm" && item.value.if_boolean() != nullptr) confirm = *item.value.if_boolean();
    else if (item.key == "vision" && item.value.if_boolean() != nullptr) vision = *item.value.if_boolean();
    else if (item.key == "profile" && item.value.if_string() != nullptr) profile = item.value.if_string();
    else if (item.key == "extra_urls" && item.value.if_array() != nullptr) {
      have_extra = true;
      for (const auto& part : *item.value.if_array()) {
        if (part.if_string() == nullptr) return make_error_response(ApiError::invalid_request);
        extra_urls.push_back(*part.if_string());
      }
    } else {
      return make_error_response(ApiError::invalid_request);
    }
  }
  // profile and extra_urls belong to the link check only; a folder to a start or a link check.
  if (route != RouteId::strata_download_link && (profile != nullptr || have_extra)) {
    return make_error_response(ApiError::invalid_request);
  }
  if (folder_text != nullptr && route != RouteId::strata_download_start && route != RouteId::strata_download_link) {
    return make_error_response(ApiError::invalid_request);
  }
  if (vision.has_value() && (route != RouteId::strata_download_start || url != nullptr)) return make_error_response(ApiError::invalid_request);
  std::optional<std::filesystem::path> folder;
  if (folder_text != nullptr && !folder_text->empty()) {
    if (!is_valid_utf8(*folder_text)) return make_error_response(ApiError::invalid_request);
    folder = path_from_utf8(*folder_text);
  }
  if (route == RouteId::strata_download_link) {
    if (url == nullptr || id != nullptr || confirm) return make_error_response(ApiError::invalid_request);
    const auto checked =
        manager.check_link(*url, profile != nullptr ? std::string_view{*profile} : std::string_view{}, extra_urls, folder);
    if (checked.confirm_json) return json_answer(200, *checked.confirm_json);
    const auto refusal = checked.refusal.value_or(DownloadManager::Refusal{500, "internal_error"});
    // The fixed reason (O4's rule that refused the link, or what went wrong asking Hugging Face).
    std::string text = render_error_body(error_for(refusal));
    while (!text.empty() && (text.back() == '\n' || text.back() == '}')) text.pop_back();
    text += "}";
    if (!checked.reason.empty()) text += ",\"reason\":" + json_text(checked.reason);
    HttpResponse response = make_error_response(error_for(refusal));
    response.body = text + "}\n";
    return response;
  }
  if (route == RouteId::strata_download_start && url != nullptr) {
    if (id != nullptr || !confirm) return make_error_response(ApiError::invalid_request);  // a link starts once confirmed
    const auto link = parse_hf_link(*url);
    const auto* file = std::get_if<HfFileLink>(&link);
    if (file == nullptr) return make_error_response(ApiError::invalid_request);
    if (const auto refused = manager.start_link(*url, folder)) return refusal_answer(*refused);
    // The card's id is its first shard's (a multipart paste's shards share one card).
    return accepted("queued", custom_id(file->repo, gguf_shard_set(file->path).front()));
  }
  if (id == nullptr || url != nullptr || confirm) return make_error_response(ApiError::invalid_request);
  if (route == RouteId::strata_download_start) {
    if (const auto refused = manager.start(*id, folder, vision.value_or(false))) return refusal_answer(*refused);
    return accepted("queued", *id);
  }
  if (route == RouteId::strata_download_pause) {
    if (const auto refused = manager.pause(*id)) return refusal_answer(*refused);
    return accepted("paused", *id);
  }
  if (const auto refused = manager.cancel(*id)) return refusal_answer(*refused);
  return accepted("cancelled", *id);
}

std::filesystem::path strata_beside_this_executable() {
#if defined(_WIN32)
  std::wstring buffer(32768, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) return {};
  buffer.resize(length);
  return std::filesystem::path(buffer).parent_path() / L"strata.exe";
#else
  std::error_code ec;
  const auto self = std::filesystem::read_symlink("/proc/self/exe", ec);
  return ec ? std::filesystem::path{} : self.parent_path() / "strata";
#endif
}

DownloadSeams production_download_seams() {
  DownloadSeams seams;
  seams.spawn = [](const EngineCommand& command) -> std::unique_ptr<LineChild> {
    auto process = std::make_unique<EngineStdioProcess>();
    if (process->start(command)) return nullptr;
    process->close_stdin();
    return std::make_unique<StdioLineChild>(std::move(process));
  };
  seams.disk = [](const std::filesystem::path& path) -> std::optional<DiskSpace> {
    std::error_code ec;
    const auto space = std::filesystem::space(path, ec);
    if (ec) return std::nullopt;
    return DiskSpace{space.available, space.capacity};
  };
  seams.locate = [](const std::filesystem::path& store, const std::string_view id) -> std::optional<std::filesystem::path> {
    auto root = open_root(store);
    if (!std::holds_alternative<CanonicalRoot>(root)) return std::nullopt;
    auto read = read_model_index(std::get<CanonicalRoot>(root));
    const auto* index = std::get_if<ModelIndex>(&read);
    if (index == nullptr) return std::nullopt;
    const auto installed = find_installed(*index, ModelReference{std::string{id}, std::string{kStoreTag}});
    const auto* found = std::get_if<IndexEntry>(&installed);
    if (found == nullptr) return std::nullopt;
    return std::get<CanonicalRoot>(root).path() / path_from_utf8(id) / std::string{kStoreTag} / path_from_utf8(found->dir);
  };
  return seams;
}

DownloadManager::DownloadManager(DownloadSettings settings) : settings_(std::move(settings)) {
  if (!settings_.seams.spawn || !settings_.seams.disk || !settings_.seams.locate) {
    const auto production = production_download_seams();
    if (!settings_.seams.spawn) settings_.seams.spawn = production.spawn;
    if (!settings_.seams.disk) settings_.seams.disk = production.disk;
    if (!settings_.seams.locate) settings_.seams.locate = production.locate;
  }
  for (const auto& entry : settings_.catalog.entries) {
    Card card;
    card.entry = entry;
    card.total = total_of(entry);
    cards_.push_back(std::move(card));
  }
  load_custom();
  load_destinations();
  load_vision();
  refresh_installed();
  worker_ = std::thread([this] { run(); });
}

DownloadManager::~DownloadManager() { shutdown(); }

void DownloadManager::log(const std::string_view line) const {
  if (settings_.log) {
    settings_.log(line);
    return;
  }
  std::cerr << line << '\n' << std::flush;
}

std::string_view DownloadManager::state_name(const State state) noexcept {
  switch (state) {
    case State::available: return "available";
    case State::queued: return "queued";
    case State::downloading: return "downloading";
    case State::verifying: return "verifying";
    case State::preparing: return "preparing";
    case State::installed: return "installed";
    case State::ready: return "ready";
    case State::failed: return "failed";
    case State::cancelled: return "cancelled";
    case State::paused: return "paused";
  }
  return "available";
}

std::uint64_t DownloadManager::total_of(const DownloadEntry& entry, const bool vision) const {
  std::uint64_t total = 0;
  for (const auto& file : entry.files) total += file.size.value_or(0);
  if (entry.mtp) total += entry.mtp->size.value_or(0);   // Gemma 4's MTP draft file: always fetched with the model
  if (vision && entry.mmproj) total += entry.mmproj->size.value_or(0);
  return total;
}

std::optional<DiskSpace> DownloadManager::disk(const std::filesystem::path& store) const { return settings_.seams.disk(store); }

std::filesystem::path DownloadManager::store_of(const Card& card) const {
  return card.store.empty() ? settings_.downloads_root : card.store;
}

std::optional<std::filesystem::path> DownloadManager::native_prepare_tool() const {
  if (!settings_.gguf_engine) return std::nullopt;
#if defined(_WIN32)
  const auto tool = settings_.gguf_engine->parent_path() / "strata-prepare.exe";
#else
  const auto tool = settings_.gguf_engine->parent_path() / "strata-prepare";
#endif
  std::error_code ec;
  if (!std::filesystem::is_regular_file(tool, ec)) return std::nullopt;
  return tool;
}

bool DownloadManager::native_prepare_ready(const DownloadEntry& entry) const {
  if (entry.engine != "qwen" || entry.prepare != "iq_pack" || !native_prepare_tool()) return false;
  if (!entry.expert_profile) return true;
  std::error_code ec;
  return std::filesystem::is_regular_file(settings_.gguf_engine->parent_path() / "profiles" / path_from_utf8(*entry.expert_profile), ec);
}

// A Gemma 4, flint or Qwen3.6 GGUF (a catalog entry of that engine, or a paste matched to it): its card is made from
// the GGUF (owner 2026-10-27: Qwen3.6 too - upstream's engine reads it with --native, its tokenizer is in the header).
bool DownloadManager::gguf_family(const DownloadEntry& entry) const {
  return settings_.gguf_engine.has_value() &&
         (entry.engine == "gemma4" || entry.engine == "flint" || (entry.engine == "qwen" && entry.family == "qwen36"));
}

// The store a start names (spec 2026-10-27 §4.3): an existing folder the server trusts as a model folder (open_root);
// empty for models.downloads_root; unset, the card's own.
std::variant<std::filesystem::path, DownloadManager::Refusal> DownloadManager::chosen_store(
    const Card& card, const std::optional<std::filesystem::path>& folder) const {
  if (!folder.has_value()) return card.store;
  if (!folder->is_absolute()) return Refusal{422, "folder_refused"};
  const auto opened = open_root(*folder, RootPolicy::existing);
  if (const auto* error = std::get_if<PathError>(&opened)) {
    const bool shared = error->code == PathErrorCode::untrusted_owner || error->code == PathErrorCode::shared_writable ||
                        error->code == PathErrorCode::not_owned_by_user || error->code == PathErrorCode::permissions_too_broad;
    return shared ? Refusal{409, "folder_not_private"} : Refusal{422, "folder_refused"};
  }
  const auto path = std::get<CanonicalRoot>(opened).path();
  std::error_code ec;
  if (std::filesystem::equivalent(path, settings_.downloads_root, ec) && !ec) return std::filesystem::path{};
  return path;
}

// destinations.json: each download's folder ({"<id>": "<folder>"}), for a resume after a restart.
void DownloadManager::load_destinations() {
  std::ifstream in(settings_.work_dir / "destinations.json", std::ios::binary);
  if (!in) return;
  std::ostringstream text;
  text << in.rdbuf();
  const auto parsed = parse_json(text.str(), kChildJsonLimits);
  const auto* root = std::get_if<JsonValue>(&parsed);
  const auto* members = root == nullptr ? nullptr : root->if_object();
  if (members == nullptr) return;
  for (const auto& item : *members) {
    const auto index = find(item.key);
    if (index && item.value.if_string() != nullptr && !item.value.if_string()->empty()) {
      cards_[*index].store = path_from_utf8(*item.value.if_string());
    }
  }
}

void DownloadManager::save_destinations() {
  std::string text = "{";
  for (const auto& card : cards_) {
    if (card.store.empty()) continue;
    text += (text.size() > 1U ? "," : "") + json_text(card.entry.id) + ":" + json_text(path_to_utf8(card.store));
  }
  text += "}\n";
  auto root = open_root(settings_.work_dir);
  const auto name = parse_relative_path("destinations.json");
  if (std::holds_alternative<CanonicalRoot>(root) && std::holds_alternative<RelativePath>(name)) {
    (void)write_private_file(std::get<CanonicalRoot>(root), std::get<RelativePath>(name), text, TokenStoreMode::replace);
  }
}

void DownloadManager::load_vision() {
  std::ifstream in(settings_.work_dir / "vision.json", std::ios::binary);
  if (!in) return;
  std::ostringstream text;
  text << in.rdbuf();
  const auto parsed = parse_json(text.str(), kChildJsonLimits);
  const auto* root = std::get_if<JsonValue>(&parsed);
  const auto* ids = root == nullptr ? nullptr : member(*root, "ids");
  if (ids == nullptr || ids->if_array() == nullptr) return;
  for (const auto& id : *ids->if_array()) {
    const auto index = id.if_string() != nullptr ? find(*id.if_string()) : std::nullopt;
    if (!index || !vision_offered(cards_[*index].entry)) continue;
    cards_[*index].vision = true;
    cards_[*index].total = total_of(cards_[*index].entry, true);
  }
}

void DownloadManager::save_vision() {
  std::string text = "{\"ids\":[";
  bool first = true;
  for (const auto& card : cards_) {
    if (!card.vision) continue;
    text += (first ? "" : ",") + json_text(card.entry.id);
    first = false;
  }
  text += "]}\n";
  auto root = open_root(settings_.work_dir);
  const auto name = parse_relative_path("vision.json");
  if (std::holds_alternative<CanonicalRoot>(root) && std::holds_alternative<RelativePath>(name)) {
    (void)write_private_file(std::get<CanonicalRoot>(root), std::get<RelativePath>(name), text, TokenStoreMode::replace);
  }
}

std::optional<std::size_t> DownloadManager::find(const std::string_view id) const {
  for (std::size_t i = 0; i < cards_.size(); ++i) {
    if (cards_[i].entry.id == id) return i;
  }
  return std::nullopt;
}

void DownloadManager::set_state(const std::size_t index, const State state, std::string error) {
  std::string name;
  std::string reason;
  {
    std::lock_guard lock(mutex_);
    cards_[index].state = state;
    cards_[index].error = std::move(error);
    name = cards_[index].entry.name;
    reason = cards_[index].error;
  }
  changed_.notify_all();
  // GUI-first start-up (spec 2026-10-27 §5a): the alerts a download raises
  if (!settings_.on_event) return;
  if (state == State::ready) settings_.on_event("download_finished", name + " is downloaded", "Its card is in the Models tab.");
  else if (state == State::installed) settings_.on_event("download_finished", name + " is downloaded", "Its files are installed.");
  else if (state == State::failed) settings_.on_event("download_failed", name + " did not download", reason + ".");
}

// Each card's store says whether it is installed; the cards folder which became cards.
void DownloadManager::refresh_installed() {
  std::lock_guard lock(mutex_);
  for (auto& card : cards_) {
    if (download_card_file(settings_.cards_dir, card.entry.id)) {
      card.state = State::ready;
      card.bytes = card.total;
      continue;
    }
    if (settings_.seams.locate(store_of(card), card.entry.id).has_value()) {
      card.state = State::installed;
      card.bytes = card.total;
    }
  }
}

void DownloadManager::load_custom() {
  std::ifstream in(settings_.work_dir / "custom.json", std::ios::binary);
  if (!in) return;
  std::ostringstream text;
  text << in.rdbuf();
  auto catalog = parse_download_catalog(text.str());
  for (auto& entry : catalog.entries) {
    if (find(entry.id)) continue;
    entry.source = "custom";
    Card card;
    card.entry = std::move(entry);
    card.total = total_of(card.entry);
    cards_.push_back(std::move(card));
  }
}

// custom.json in the catalog's own format, so the same reader (and its rules) reads it back.
void DownloadManager::save_custom() {
  std::string text = "{\"schema_version\":1,\"models\":[";
  bool first = true;
  for (const auto& card : cards_) {
    if (card.entry.source != "custom") continue;
    const auto& e = card.entry;
    text += first ? "" : ",";
    first = false;
    text += "{\"id\":" + json_text(e.id) + ",\"name\":" + json_text(e.name) + ",\"source\":\"custom\",\"family\":" + json_text(e.family) +
            ",\"engine\":" + json_text(e.engine) + ",\"about\":" + json_text(e.about) + ",\"repo\":" + json_text(e.repo) + ",\"revision\":" + json_text(e.revision) +
            ",\"prepare\":" + json_text(e.prepare) + ",\"files\":[";
    for (std::size_t i = 0; i < e.files.size(); ++i) {
      text += (i ? "," : "") + std::string{"{\"path\":"} + json_text(e.files[i].path) + ",\"size\":" +
              std::to_string(e.files[i].size.value_or(0)) + ",\"sha256\":" + json_text(e.files[i].sha256) + "}";
    }
    text += "]}";
  }
  text += "]}\n";
  auto root = open_root(settings_.work_dir);
  const auto name = parse_relative_path("custom.json");
  if (std::holds_alternative<CanonicalRoot>(root) && std::holds_alternative<RelativePath>(name)) {
    (void)write_private_file(std::get<CanonicalRoot>(root), std::get<RelativePath>(name), text, TokenStoreMode::replace);
  }
}

std::string DownloadManager::list_json() {
  const auto space = disk(settings_.downloads_root);
  std::map<std::filesystem::path, std::optional<DiskSpace>> spaces;   // each chosen folder's drive, read once
  {
    std::lock_guard lock(mutex_);
    for (const auto& card : cards_) {
      if (!card.store.empty()) spaces.emplace(card.store, std::nullopt);
    }
  }
  for (auto& [store, free] : spaces) free = disk(store);
  // the launcher's cards and the model files they run, read outside this manager's lock (the launcher takes its own)
  const auto held = held_models(settings_.cards);
  std::lock_guard lock(mutex_);
  std::string out = "{\"object\":\"list\",\"disk\":{\"root\":";
  std::string root_label = settings_.downloads_root.root_name().string();
  if (root_label.empty()) root_label = settings_.downloads_root.root_path().string();
  out += json_text(root_label) + ",\"folder\":" + json_text(path_to_utf8(settings_.downloads_root)) +
         ",\"free_bytes\":" + std::to_string(space ? space->free_bytes : 0) +
         ",\"total_bytes\":" + std::to_string(space ? space->total_bytes : 0) + "},\"active\":";
  if (active_) {
    const auto& card = cards_[*active_];
    out += "{\"id\":" + json_text(card.entry.id) + ",\"file\":" + json_text(card.current_file) + ",\"bytes\":" + std::to_string(card.bytes) +
           ",\"total\":" + std::to_string(card.total) + ",\"rate_bps\":" + std::to_string(static_cast<std::uint64_t>(card.rate_bps)) +
           ",\"eta_s\":" +
           std::to_string(card.rate_bps > 1.0 && card.total > card.bytes
                              ? static_cast<std::uint64_t>(static_cast<double>(card.total - card.bytes) / card.rate_bps)
                              : 0U) +
           "}";
  } else {
    out += "null";
  }
  out += ",\"data\":[";
  for (std::size_t i = 0; i < cards_.size(); ++i) {
    const auto& card = cards_[i];
    const auto& e = card.entry;
    const std::uint64_t remaining = card.total > card.bytes ? card.total - card.bytes : 0U;
    const std::uint64_t needed = remaining + settings_.margin_bytes;
    const auto& card_space = card.store.empty() ? space : spaces[card.store];
    const bool fits = !card_space || card_space->free_bytes >= needed || card.state == State::ready || card.state == State::installed;
    // Qwen3.6-35B-A3B (OWN_PATH) and a profile-matched custom paste both read the GGUF itself, so no
    // iq_pack - only the tokenizer is prepared, then a card is written (prepare is "none" but it runs).
    // A matched custom paste carries its profile as its family; an unmatched one stays "custom".
    const bool matched_custom = e.source == "custom" && e.family != "custom";
    const bool gguf_direct = e.engine == "qwen" && (e.family == "qwen36" || matched_custom);
    const bool qwen_ready = (e.engine == "qwen" && (e.prepare == "iq_pack" || gguf_direct) && settings_.prepare_python &&
                             settings_.prepare_tools && settings_.prepare_script && settings_.card_template) ||
                            native_prepare_ready(e);
    const bool gguf_ready = gguf_family(e);
    const bool glm_ready = maya_prepare_ready(e);
    std::string note;
    if (glm_ready && e.engine == "deepseek") note = "after the download the DeepSeek pack is built (about a minute) and a card appears in Models";
    else if (glm_ready && e.engine == "minimax") {
      note = "MiniMax Community License (non-commercial use; engine/templates/minimax-m3.1 has it); after the download the "
             "MiniMax pack is built (about a minute) and a card appears in Models";
    } else if (glm_ready && e.engine == "raon1") note = "after the download the raon-1-mini pack is built (a few minutes) and a card appears in Models";
    else if (glm_ready) note = "after the download the GLM pack is built (a few minutes) and a card appears in Models";
    else if (e.engine == "minimax" && e.prepare == "maya_pack" && native_prepare_tool() && !minimax_template())
      note = "download only: MiniMax's chat template is not beside this server's engine (engine/templates/minimax-m3.1)";
    else if (maya_pack_engine(e.engine) && e.prepare == "maya_pack")
      note = "download only: this server names no engine for new cards (models.default_engine) with strata-prepare beside it";
    else if (e.engine == "glm53") note = "download only: the GLM-5.3 engine is not built here yet";
    else if (gguf_ready && e.mtp) note = "its MTP draft file comes too; after the download a card is made from the GGUF, drafting with it, and appears in Models";
    else if (gguf_ready) note = "after the download a card is made from the GGUF and appears in Models";
    else if (e.engine == "gemma4" || e.engine == "flint")
      note = "download only: this server names no engine for new cards (models.default_engine)";
    else if (e.source == "custom" && e.family == "custom")
      note = "download only: pick the model family it matches (its Download button) so a card can be made";
    else if (!qwen_ready) note = "download only: the prepare step is not configured (models.prepare_python, models.prepare_tools)";
    else if (gguf_direct) note = "after the download its tokenizer is prepared (a few seconds) and a card appears in Models";
    else note = "after the download the pack is built (a few minutes) and a card appears in Models";
    // Maya pins no sizes (read from Hugging Face at the start): until then the card's size is not known, not 0
    const bool size_known = std::all_of(e.files.begin(), e.files.end(), [](const CatalogFile& f) { return f.size.has_value(); });
    // owner 2026-10-28: what this PC already has of it - the page's and strata-tui's "Downloaded" / "Locally provided"
    const auto [have, have_card] = have_of(e, card.state == State::ready || card.state == State::installed, held);
    out += i != 0U ? "," : "";
    out += "{\"id\":" + json_text(e.id) + ",\"name\":" + json_text(e.name) + ",\"source\":" + json_text(e.source) + ",\"family\":" +
           json_text(e.family) + ",\"about\":" + json_text(e.about) + ",\"repo\":" + json_text(e.repo) + ",\"size_bytes\":" +
           std::to_string(card.total) + ",\"size_known\":" + (size_known ? "true" : "false") + ",\"files\":" +
           std::to_string(e.files.size() + (e.mtp ? 1U : 0U)) + ",\"state\":" + json_text(state_name(card.state)) +
           ",\"bytes\":" + std::to_string(card.bytes) + ",\"total\":" + std::to_string(card.total) + ",\"error\":" +
           (card.error.empty() ? std::string{"null"} : json_text(card.error)) + ",\"needed_bytes\":" + std::to_string(needed) +
           ",\"fits\":" + (fits ? "true" : "false") + ",\"runnable\":" + (qwen_ready || gguf_ready || glm_ready ? "true" : "false") +
           ",\"note\":" + json_text(note) + ",\"folder\":" + json_text(path_to_utf8(store_of(card))) +
           ",\"file\":" + json_text(model_file_stem(e)) +
           ",\"vision_bytes\":" + std::to_string(vision_offered(e) ? e.mmproj->size.value_or(0) : 0U) +
           ",\"vision\":" + (card.vision ? "true" : "false") +
           ",\"have\":" + (have.empty() ? std::string{"null"} : json_text(have)) +
           ",\"have_card\":" + (have_card.empty() ? std::string{"null"} : json_text(have_card)) + "}";
  }
  // The families a pasted GGUF can be matched to (the page's profile picker). "runnable" is whether
  // a card is written after the download; Gemma 4 is listed but stays download-only (no tokenizer).
  out += "],\"profiles\":[";
  for (std::size_t i = 0; i < kCustomProfiles.size(); ++i) {
    const auto& p = kCustomProfiles[i];
    out += (i != 0U ? "," : "") + std::string{"{\"key\":"} + json_text(p.key) + ",\"display\":" + json_text(p.display) +
           ",\"engine\":" + json_text(p.engine) + ",\"runnable\":" + (profile_runnable(p, settings_.gguf_engine.has_value()) ? "true" : "false") +
           "}";
  }
  out += "]}\n";
  return out;
}

std::optional<DownloadManager::Refusal> DownloadManager::start(const std::string_view id,
                                                              const std::optional<std::filesystem::path>& folder, const bool vision) {
  // §4.3: the folder it goes to (a paused download keeps its own unless the start names another), checked first
  std::filesystem::path store;
  {
    std::lock_guard lock(mutex_);
    if (stopping_) return Refusal{503, "server_shutting_down"};
    const auto index = find(id);
    if (!index) return Refusal{404, "model_not_found"};
    if (vision && !vision_offered(cards_[*index].entry)) return Refusal{400, "invalid_request"};   // no encoder to take
    auto chosen = chosen_store(cards_[*index], folder);
    if (const auto* refused = std::get_if<Refusal>(&chosen)) return *refused;
    store = std::get<std::filesystem::path>(std::move(chosen));
  }
  const auto space = disk(store.empty() ? settings_.downloads_root : store);
  {
    std::lock_guard lock(mutex_);
    if (stopping_) return Refusal{503, "server_shutting_down"};
    const auto index = find(id);
    if (!index) return Refusal{404, "model_not_found"};
    auto& card = cards_[*index];
    if (card.state == State::queued || card.state == State::downloading || card.state == State::verifying ||
        card.state == State::preparing) {
      return Refusal{409, "download_in_progress"};
    }
    if (card.state == State::ready) return Refusal{409, "download_in_progress"};
    // O2: the missing bytes and the margin must fit on the store's drive (Maya's sizes are known
    // only at start; the pull checks the space again while it writes). A download moved to another
    // folder starts over there (its partial files are in the old one).
    const bool moved = store != card.store;
    const std::uint64_t kept = moved ? 0U : card.bytes;
    const std::uint64_t wanted = total_of(card.entry, vision);   // with the picture encoder when it comes too
    const std::uint64_t remaining = wanted > kept ? wanted - kept : 0U;
    if (space && space->free_bytes < remaining + settings_.margin_bytes) return Refusal{507, "insufficient_storage"};
    if (moved) {
      card.store = store;
      card.bytes = 0;
    }
    card.state = State::queued;
    card.error.clear();
    card.vision = vision;
    card.total = wanted;
    queue_.push_back(*index);
    save_destinations();
    save_vision();
  }
  log("[strata] downloads: queued " + std::string{id});
  changed_.notify_all();
  return std::nullopt;
}

std::optional<DownloadManager::Refusal> DownloadManager::cancel(const std::string_view id) {
  bool discard = false;
  std::filesystem::path store;
  {
    std::lock_guard lock(mutex_);
    const auto index = find(id);
    if (!index) return Refusal{404, "model_not_found"};
    const auto queued = std::find(queue_.begin(), queue_.end(), *index);
    if (queued != queue_.end() || cards_[*index].state == State::paused) {
      if (queued != queue_.end()) queue_.erase(queued);
      cards_[*index].state = State::cancelled;
      cards_[*index].bytes = 0;
      discard = true;
      store = store_of(cards_[*index]);
    } else if (active_ == index && cards_[*index].state != State::preparing) {
      cancel_requested_ = true;
      pause_requested_ = false;
    } else {
      return Refusal{409, "download_not_active"};  // nothing running, queued or paused to cancel
    }
  }
  log("[strata] downloads: cancel " + std::string{id});
  if (discard) discard_partials(id, store);
  changed_.notify_all();
  return std::nullopt;
}

DownloadManager::LinkAnswer DownloadManager::check_link(const std::string_view url, const std::string_view profile,
                                                        const std::vector<std::string>& extra_urls,
                                                        const std::optional<std::filesystem::path>& folder) {
  // Owner 2026-10-23: a pasted GGUF is matched to a family (profile) so a runnable card can be made,
  // and a multipart GGUF's other shards are pasted too (at most five more), all to the one location.
  const CustomProfile* matched = profile.empty() ? nullptr : custom_profile(profile);
  if (!profile.empty() && matched == nullptr) {
    return LinkAnswer{std::nullopt, Refusal{400, "invalid_request"}, "That model family is not one this server runs."};
  }
  if (extra_urls.size() > 5U) {
    return LinkAnswer{std::nullopt, Refusal{400, "invalid_request"}, "At most five more part links can be added."};
  }
  // The primary link names the shard set (a single GGUF is a set of one).
  const auto parsed_primary = parse_hf_link(url);
  if (const auto* why = std::get_if<std::string_view>(&parsed_primary)) {
    return LinkAnswer{std::nullopt, Refusal{400, "invalid_request"}, std::string{*why}};
  }
  const auto& primary_link = std::get<HfFileLink>(parsed_primary);
  const auto shard_names = gguf_shard_set(primary_link.path);

  // Resolve one pasted link through `strata hf-info`: its repo, commit and .gguf file(s).
  struct Resolved final {
    std::string repo;
    std::string commit;
    std::vector<CatalogFile> files;
  };
  std::string reason;
  int fail_status = 400;
  const auto resolve = [this, &reason, &fail_status](const std::string_view link_url) -> std::optional<Resolved> {
    const auto parsed = parse_hf_link(link_url);
    if (const auto* why = std::get_if<std::string_view>(&parsed)) {
      reason = std::string{*why};
      fail_status = 400;
      return std::nullopt;
    }
    std::string error;
    const auto answer = hf_info(link_url, error);
    if (!answer) {
      reason = error;
      fail_status = 502;
      return std::nullopt;
    }
    const auto json = parse_json(*answer, kChildJsonLimits);
    const auto* root = std::get_if<JsonValue>(&json);
    const auto* commit = root ? member(*root, "commit") : nullptr;
    const auto* repo = root ? member(*root, "repo") : nullptr;
    const auto* files = root ? member(*root, "files") : nullptr;
    if (commit == nullptr || commit->if_string() == nullptr || !is_hf_commit(*commit->if_string()) || repo == nullptr ||
        repo->if_string() == nullptr || files == nullptr || files->if_array() == nullptr || files->if_array()->empty()) {
      reason = "Hugging Face answered in an unexpected shape.";
      fail_status = 502;
      return std::nullopt;
    }
    Resolved out;
    out.repo = *repo->if_string();
    out.commit = *commit->if_string();
    for (const auto& item : *files->if_array()) {
      const auto* path = member(item, "path");
      const auto* size = member(item, "size");
      const auto* sha = member(item, "sha256");
      if (path == nullptr || path->if_string() == nullptr || !is_hf_gguf_path(*path->if_string()) || size == nullptr ||
          size->if_integer() == nullptr || *size->if_integer() <= 0 || sha == nullptr || sha->if_string() == nullptr) {
        reason = "Hugging Face answered in an unexpected shape.";
        fail_status = 502;
        return std::nullopt;
      }
      out.files.push_back(CatalogFile{*path->if_string(), static_cast<std::uint64_t>(*size->if_integer()), *sha->if_string()});
    }
    return out;
  };
  const auto refuse = [&reason, &fail_status]() {
    return LinkAnswer{std::nullopt, Refusal{fail_status, fail_status == 400 ? "invalid_request" : "upstream_unavailable"}, reason};
  };

  const auto primary = resolve(url);
  if (!primary) return refuse();
  // Every provided file is placed at its position in the shard set; parts share repo and revision.
  std::map<std::size_t, CatalogFile> parts;
  const auto place = [&](const Resolved& r) -> std::optional<std::string> {
    if (r.repo != primary->repo || r.commit != primary->commit) {
      return std::string{"Every part must be from the same repository and revision."};
    }
    for (const auto& file : r.files) {
      const auto at = std::find(shard_names.begin(), shard_names.end(), file.path);
      if (at == shard_names.end()) return "That link is not a shard of this model (" + file.path + ").";
      const auto index = static_cast<std::size_t>(at - shard_names.begin());
      if (parts.count(index) != 0U) return "The same part was pasted twice (" + file.path + ").";
      parts.emplace(index, file);
    }
    return std::nullopt;
  };
  if (const auto err = place(*primary)) return LinkAnswer{std::nullopt, Refusal{400, "invalid_request"}, *err};
  for (const auto& extra : extra_urls) {
    const auto resolved = resolve(extra);
    if (!resolved) return refuse();
    if (const auto err = place(*resolved)) return LinkAnswer{std::nullopt, Refusal{400, "invalid_request"}, *err};
  }

  const std::string& first_shard = shard_names.front();
  DownloadEntry entry;
  entry.id = custom_id(primary->repo, first_shard);
  const auto slash = first_shard.rfind('/');
  entry.name = std::string{primary->repo.substr(primary->repo.find('/') + 1U)} + " " +
               first_shard.substr(slash == std::string::npos ? 0 : slash + 1U);
  if (entry.name.size() > 128U) entry.name.resize(128U);
  entry.source = "custom";
  entry.family = matched ? std::string{matched->key} : std::string{"custom"};
  entry.engine = matched ? std::string{matched->engine} : std::string{"qwen"};
  entry.prepare = "none";
  entry.about = "Pasted link: " + primary->repo + (matched ? " (matched to " + std::string{matched->display} + ")" : std::string{});
  entry.repo = primary->repo;
  entry.revision = primary->commit;
  for (const auto& [index, file] : parts) entry.files.push_back(file);  // shard order (map is sorted)

  const std::size_t parts_total = shard_names.size();
  const std::size_t parts_have = parts.size();
  const bool complete = parts_have == parts_total;
  const std::uint64_t total = total_of(entry);
  const auto space = disk(folder.value_or(settings_.downloads_root));
  const std::uint64_t needed = total + settings_.margin_bytes;
  std::string confirm = "{\"confirm\":{\"id\":" + json_text(entry.id) + ",\"name\":" + json_text(entry.name) + ",\"repo\":" +
                        json_text(entry.repo) + ",\"commit\":" + json_text(entry.revision) + ",\"family\":" + json_text(entry.family) +
                        ",\"engine\":" + json_text(entry.engine) + ",\"runnable\":" +
                        ((matched && profile_runnable(*matched, settings_.gguf_engine.has_value())) ? "true" : "false") +
                        ",\"multipart\":{\"parts\":" + std::to_string(parts_total) + ",\"have\":" + std::to_string(parts_have) +
                        ",\"complete\":" + (complete ? "true" : "false") + ",\"missing\":[";
  bool first_missing = true;
  for (std::size_t k = 0; k < shard_names.size(); ++k) {
    if (parts.count(k) != 0U) continue;
    confirm += (first_missing ? "" : ",") + json_text(shard_names[k]);
    first_missing = false;
  }
  confirm += "]},\"files\":[";
  for (std::size_t i = 0; i < entry.files.size(); ++i) {
    confirm += (i ? "," : "") + std::string{"{\"path\":"} + json_text(entry.files[i].path) + ",\"size\":" +
               std::to_string(entry.files[i].size.value_or(0)) + ",\"sha256\":" + json_text(entry.files[i].sha256) + "}";
  }
  confirm += "],\"size_bytes\":" + std::to_string(total) + ",\"needed_bytes\":" + std::to_string(needed) +
             ",\"fits\":" + (!space || space->free_bytes >= needed ? "true" : "false") + "}}\n";
  {
    std::lock_guard lock(mutex_);
    links_[std::string{url}] = PendingLink{std::move(entry), std::chrono::steady_clock::now()};
  }
  return LinkAnswer{confirm, std::nullopt, {}};
}

std::optional<DownloadManager::Refusal> DownloadManager::start_link(const std::string_view url, const std::optional<std::filesystem::path>& folder) {
  std::string id;
  {
    std::lock_guard lock(mutex_);
    const auto found = links_.find(url);
    if (found == links_.end() || std::chrono::steady_clock::now() - found->second.at > kLinkValidFor) {
      return Refusal{400, "invalid_request"};  // check the link first (its confirmation expired or never happened)
    }
    DownloadEntry entry = found->second.entry;
    // A multipart paste starts only once every shard has been pasted (the set derived from any one
    // shard names them all); otherwise the download could never complete.
    if (!entry.files.empty() && gguf_shard_set(entry.files.front().path).size() != entry.files.size()) {
      return Refusal{400, "invalid_request"};
    }
    links_.erase(found);
    id = entry.id;
    if (const auto existing = find(id)) {
      cards_[*existing].entry = std::move(entry);
      cards_[*existing].total = total_of(cards_[*existing].entry);
    } else {
      Card card;
      card.entry = std::move(entry);
      card.total = total_of(card.entry);
      cards_.push_back(std::move(card));
    }
    save_custom();
  }
  return start(id, folder);
}

// Owner: "Have a download pause and resume functionality". A pause keeps the partial files
// (SP4's pull resumes from them on the next start of the same manifest); a queued card simply
// leaves the queue.
std::optional<DownloadManager::Refusal> DownloadManager::pause(const std::string_view id) {
  {
    std::lock_guard lock(mutex_);
    const auto index = find(id);
    if (!index) return Refusal{404, "model_not_found"};
    const auto queued = std::find(queue_.begin(), queue_.end(), *index);
    if (queued != queue_.end()) {
      queue_.erase(queued);
      cards_[*index].state = State::paused;
    } else if (active_ == index && cards_[*index].state != State::preparing) {
      cancel_requested_ = true;
      pause_requested_ = true;
    } else {
      return Refusal{409, "download_not_active"};  // nothing running or queued to pause
    }
  }
  log("[strata] downloads: pause " + std::string{id});
  changed_.notify_all();
  return std::nullopt;
}

std::optional<DownloadManager::Refusal> DownloadManager::remove_installed(const std::string_view id) {
  std::filesystem::path store;
  {
    std::lock_guard lock(mutex_);
    const auto index = find(id);
    if (!index) return Refusal{404, "model_not_found"};
    if (active_ == index || std::find(queue_.begin(), queue_.end(), *index) != queue_.end()) {
      return Refusal{409, "download_in_progress"};
    }
    store = store_of(cards_[*index]);
  }
  // `strata rm <id>:download`: the store's own removal (its lock, its index, the version folder inside it).
  auto child = spawn(store_arguments(settings_.config_file, settings_.downloads_root, store,
                                     {"rm", std::string{id} + ":" + std::string{kStoreTag}}),
                     settings_.strata_executable);
  if (child) {
    bool ended = false;
    const auto until = std::chrono::steady_clock::now() + std::chrono::minutes(2);
    while (!ended && std::chrono::steady_clock::now() < until) {
      ended = !child->next_line(std::chrono::milliseconds(100)) && child->ended();
    }
    if (!ended) child->kill();
  }
  if (settings_.seams.locate(store, id).has_value()) {
    log("[strata] downloads: " + std::string{id} + " is still in its store after strata rm");
    return Refusal{500, "internal_error"};
  }
  {
    std::lock_guard lock(mutex_);
    if (const auto index = find(id)) {
      auto& card = cards_[*index];
      card.state = State::available;
      card.bytes = 0;
      card.error.clear();
    }
  }
  // and the pack strata-prepare built for it there (a folder this manager names: <store>/packs/<id>)
  std::error_code ec;
  std::filesystem::remove_all(store / "packs" / path_from_utf8(std::string{id}), ec);
  log("[strata] downloads: " + std::string{id} + " removed from its store");
  changed_.notify_all();
  return std::nullopt;
}

// `strata rm --partials <id>:download`: the staged partial files of a cancelled download (in its own store).
void DownloadManager::discard_partials(const std::string_view id, const std::filesystem::path& store) {
  auto child = spawn(store_arguments(settings_.config_file, settings_.downloads_root, store,
                                     {"rm", "--partials", std::string{id} + ":" + std::string{kStoreTag}}),
                     settings_.strata_executable);
  if (!child) return;
  const auto until = std::chrono::steady_clock::now() + std::chrono::minutes(1);
  while (std::chrono::steady_clock::now() < until) {
    if (!child->next_line(std::chrono::milliseconds(100)) && child->ended()) return;
  }
  child->kill();
}

void DownloadManager::shutdown() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
    cancel_requested_ = true;
  }
  changed_.notify_all();
  if (worker_.joinable()) worker_.join();
}

bool DownloadManager::wait_idle(const std::chrono::milliseconds timeout) {
  std::unique_lock lock(mutex_);
  return changed_.wait_for(lock, timeout, [this] { return queue_.empty() && !active_; });
}

void DownloadManager::run() {
  for (;;) {
    std::size_t index = 0;
    {
      std::unique_lock lock(mutex_);
      changed_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (stopping_) return;
      index = queue_.front();
      queue_.pop_front();
      active_ = index;
      cancel_requested_ = false;
      pause_requested_ = false;
    }
    run_card(index);
    {
      std::lock_guard lock(mutex_);
      active_.reset();
    }
    changed_.notify_all();
  }
}

std::unique_ptr<LineChild> DownloadManager::spawn(std::vector<std::string> arguments, const std::filesystem::path& executable) {
  EngineCommand command;
  command.executable = executable;
  command.arguments = std::move(arguments);
  command.environment = settings_.child_environment;
  command.working_directory = settings_.work_dir;
  command.no_new_privileges = true;
  command.insert_serve = false;
  return settings_.seams.spawn(command);
}

std::optional<std::string> DownloadManager::hf_info(const std::string_view url, std::string& error) {
  auto child = spawn({"--config", path_to_utf8(settings_.config_file), "hf-info", std::string{url}}, settings_.strata_executable);
  if (!child) {
    error = "The downloader could not start.";
    return std::nullopt;
  }
  std::optional<std::string> answer;
  std::string code;
  const auto until = std::chrono::steady_clock::now() + std::chrono::minutes(2);
  while (std::chrono::steady_clock::now() < until) {
    auto line = child->next_line(std::chrono::milliseconds(200));
    if (line) {
      if (line->rfind("ERROR ", 0) == 0) code = line->substr(6);
      else if (!line->empty() && line->front() == '{') answer = std::move(*line);
      continue;
    }
    if (child->ended()) break;
  }
  if (!child->ended()) child->kill();
  if (answer) return answer;
  error = code == "link_refused"       ? "The link is not a Hugging Face .gguf file."
          : code == "file_not_found"   ? "The file (or one of its shards) is not at that revision."
          : code == "not_lfs"          ? "The file has no size and SHA-256 on Hugging Face."
          : code.empty()               ? "Hugging Face could not be reached."
                                       : "Hugging Face could not be reached or answered unexpectedly (" + code + ").";
  return std::nullopt;
}

// Maya's entries name "main" and pin each shard's SHA-256: ask Hugging Face for the commit and the
// sizes, and go on only when every reported SHA-256 is the pinned one.
bool DownloadManager::resolve_main(Card& card, std::string& error) {
  const std::string url = "https://huggingface.co/" + card.entry.repo + "/resolve/main/" + card.entry.files.front().path;
  const auto answer = hf_info(url, error);
  if (!answer) return false;
  const auto json = parse_json(*answer, kChildJsonLimits);
  const auto* root = std::get_if<JsonValue>(&json);
  const auto* commit = root ? member(*root, "commit") : nullptr;
  const auto* files = root ? member(*root, "files") : nullptr;
  if (commit == nullptr || commit->if_string() == nullptr || files == nullptr || files->if_array() == nullptr) {
    error = "Hugging Face answered in an unexpected shape.";
    return false;
  }
  for (auto& pinned : card.entry.files) {
    bool matched = false;
    for (const auto& item : *files->if_array()) {
      const auto* path = member(item, "path");
      const auto* size = member(item, "size");
      const auto* sha = member(item, "sha256");
      if (path == nullptr || path->if_string() == nullptr || *path->if_string() != pinned.path) continue;
      if (sha == nullptr || sha->if_string() == nullptr || *sha->if_string() != pinned.sha256 || size == nullptr ||
          size->if_integer() == nullptr || *size->if_integer() <= 0) {
        error = "The files on Hugging Face no longer match the pinned SHA-256; the download was not started.";
        return false;
      }
      pinned.size = static_cast<std::uint64_t>(*size->if_integer());
      matched = true;
    }
    if (!matched) {
      error = "A pinned file is not on Hugging Face's main revision.";
      return false;
    }
  }
  card.entry.revision = *commit->if_string();
  return true;
}

void DownloadManager::run_card(const std::size_t index) {
  Card card;
  {
    std::lock_guard lock(mutex_);
    card = cards_[index];
  }
  std::string error;
  if (card.entry.revision == "main") {
    if (!resolve_main(card, error)) {
      set_state(index, State::failed, error);
      log("[strata] downloads: " + card.entry.id + " failed: " + error);
      return;
    }
    std::uint64_t remaining = 0;
    {
      std::lock_guard lock(mutex_);
      cards_[index].entry.files = card.entry.files;
      cards_[index].entry.revision = card.entry.revision;  // a resume pulls this commit: the same partial files
      cards_[index].total = total_of(card.entry, card.vision);
      remaining = cards_[index].total > cards_[index].bytes ? cards_[index].total - cards_[index].bytes : 0U;
    }
    // O2: the sizes are known only now, so the free space is checked again before any pull.
    if (const auto space = disk(store_of(card)); space && space->free_bytes < remaining + settings_.margin_bytes) {
      error = "Insufficient disk space on the data drive for this download: " + decimal_gb(remaining + settings_.margin_bytes) +
              " GB needed, " + decimal_gb(space->free_bytes) + " GB free";
      set_state(index, State::failed, error);
      log("[strata] downloads: " + card.entry.id + " failed: " + error);
      return;
    }
  }
  HfDownload download;
  download.name = card.entry.id;
  download.tag = std::string{kStoreTag};
  download.profile = card.entry.engine == "glm53" ? "glm53" : "gguf";
  download.repo = card.entry.repo;
  download.commit = card.entry.revision;
  download.license_url = "https://huggingface.co/" + card.entry.repo;
  for (const auto& file : card.entry.files) download.files.push_back(HfFile{file.path, file.size.value_or(0), file.sha256, false});
  if (card.entry.mtp) {   // Gemma 4's MTP draft file (owner 2026-10-28): a weights file of its own pin, always with the model
    const auto& mtp = *card.entry.mtp;
    download.files.push_back(HfFile{mtp.path, mtp.size.value_or(0), mtp.sha256, false, mtp.repo, mtp.revision});
  }
  if (card.vision && vision_offered(card.entry)) {   // the picture encoder: role mmproj, from its own repository and revision
    const auto& encoder = *card.entry.mmproj;
    download.files.push_back(HfFile{encoder.path, encoder.size.value_or(0), encoder.sha256, true, encoder.repo, encoder.revision});
  }
  if (!pull(index, download, error)) {
    bool stopped = false;
    bool paused = false;
    {
      std::lock_guard lock(mutex_);
      stopped = cancel_requested_;
      paused = pause_requested_;
    }
    if (stopped && paused) {
      set_state(index, State::paused);
      log("[strata] downloads: " + card.entry.id + " paused (the partial files are kept)");
      return;
    }
    if (stopped) {
      discard_partials(card.entry.id, store_of(card));
      {
        std::lock_guard lock(mutex_);
        cards_[index].bytes = 0;
      }
      set_state(index, State::cancelled);
      log("[strata] downloads: " + card.entry.id + " cancelled (the partial files are discarded)");
      return;
    }
    set_state(index, State::failed, error);
    log("[strata] downloads: " + card.entry.id + " failed: " + error);
    return;
  }
  log("[strata] downloads: " + card.entry.id + " installed");
  // §4.3: a Gemma 4 or flint GGUF becomes a card at once (read from the GGUF; nothing to prepare).
  if (gguf_family(card.entry)) {
    if (!gguf_card(index, error)) {
      set_state(index, State::failed, "the download is installed, but its card could not be made: " + error);
      log("[strata] downloads: " + card.entry.id + " card failed: " + error);
      return;
    }
    set_state(index, State::ready);
    log("[strata] downloads: " + card.entry.id + " is ready (a model card was added)");
    return;
  }
  // owner 2026-10-27: a Qwen3.8-family pack built natively (strata-prepare, no Python) where the tool is installed, and a
  // GLM-5.3 one as Maya's setup builds it
  if (native_prepare_ready(card.entry) || maya_prepare_ready(card.entry)) {
    set_state(index, State::preparing);
    if (!(maya_prepare_ready(card.entry) ? maya_prepare(index, error) : native_prepare(index, error))) {
      set_state(index, State::failed, "the download is installed, but packing it failed: " + error);
      log("[strata] downloads: " + card.entry.id + " pack failed: " + error);
      return;
    }
    set_state(index, State::ready);
    log("[strata] downloads: " + card.entry.id + " is ready (packed natively; a model card was added)");
    return;
  }
  // A qwen card prepares when it has a pack to build (iq_pack), is Qwen3.6, or is a profile-matched
  // custom paste (all GGUF-direct); a custom paste never has an iq_pack.
  const bool can_prepare = card.entry.engine == "qwen" &&
                           (card.entry.prepare == "iq_pack" || card.entry.family == "qwen36" ||
                            (card.entry.source == "custom" && card.entry.family != "custom")) &&
                           settings_.prepare_python && settings_.prepare_tools && settings_.prepare_script && settings_.card_template;
  if (!can_prepare) {
    set_state(index, State::installed);
    return;
  }
  set_state(index, State::preparing);
  if (!prepare(index, download, error)) {
    set_state(index, State::failed, "the download is installed, but preparing it failed: " + error);
    log("[strata] downloads: " + card.entry.id + " prepare failed: " + error);
    return;
  }
  set_state(index, State::ready);
  log("[strata] downloads: " + card.entry.id + " is ready (a model card was added)");
}

bool DownloadManager::pull(const std::size_t index, const HfDownload& download, std::string& error) {
  const auto manifest = hf_manifest_json(download, "2026.10.23.1");
  if (const auto* reason = std::get_if<std::string_view>(&manifest)) {
    error = std::string{"the download could not be described: "} + std::string{*reason};
    return false;
  }
  const std::string& text = std::get<std::string>(manifest);
  const std::string digest = to_hex(sha256(text));
  const std::string file_name = download.name + ".manifest.json";
  auto root = open_root(settings_.work_dir);
  const auto relative = parse_relative_path(file_name);
  if (!std::holds_alternative<CanonicalRoot>(root) || !std::holds_alternative<RelativePath>(relative) ||
      write_private_file(std::get<CanonicalRoot>(root), std::get<RelativePath>(relative), text, TokenStoreMode::replace)) {
    error = "the download's manifest could not be written";
    return false;
  }
  set_state(index, State::downloading);
  std::filesystem::path store;
  {
    std::lock_guard lock(mutex_);
    store = store_of(cards_[index]);
  }
  auto child = spawn(store_arguments(settings_.config_file, settings_.downloads_root, store,
                                     {"pull", "--manifest", path_to_utf8(settings_.work_dir / file_name), "--trust-manifest",
                                      "sha256:" + digest, "--yes", "--progress-lines"}),
                     settings_.strata_executable);
  if (!child) {
    error = "the downloader could not start";
    return false;
  }
  std::map<std::string, std::uint64_t, std::less<>> sizes;
  std::uint64_t finished = 0;
  std::string result;
  auto last_at = std::chrono::steady_clock::now();
  std::uint64_t last_bytes = 0;  // a resume's rate counts from the bytes it kept
  {
    std::lock_guard lock(mutex_);
    last_bytes = cards_[index].bytes;
  }
  for (;;) {
    {
      std::lock_guard lock(mutex_);
      if (cancel_requested_) {
        child->kill();
        error = "cancelled";
        return false;
      }
    }
    auto line = child->next_line(std::chrono::milliseconds(200));
    if (!line) {
      if (child->ended()) break;
      continue;
    }
    const auto w = words(*line);
    if (w.empty()) continue;
    if (w[0] == "FILE" && w.size() == 3U) {
      if (const auto size = number(w[2])) sizes[std::string{w[1]}] = *size;
      std::lock_guard lock(mutex_);
      cards_[index].current_file = std::string{w[1]};
      cards_[index].state = State::downloading;
    } else if (w[0] == "BYTES" && w.size() == 4U) {
      const auto done = number(w[2]);
      const auto total = number(w[3]);
      if (!done || !total) continue;
      const auto now = std::chrono::steady_clock::now();
      std::lock_guard lock(mutex_);
      auto& c = cards_[index];
      c.bytes = finished + *done;
      const double seconds = std::chrono::duration<double>(now - last_at).count();
      if (seconds >= 1.0) {
        c.rate_bps = c.bytes > last_bytes ? static_cast<double>(c.bytes - last_bytes) / seconds : 0.0;
        last_at = now;
        last_bytes = c.bytes;
      }
      c.state = *done == *total ? State::verifying : State::downloading;
    } else if (w[0] == "FINISHED" && w.size() == 3U) {
      if (w[2] == "ok") {
        const auto size = sizes.find(w[1]);
        finished += size == sizes.end() ? 0U : size->second;
        std::lock_guard lock(mutex_);
        cards_[index].bytes = finished;
      }
    } else if (w[0] == "RESULT" && w.size() == 2U) {
      result = std::string{w[1]};
    }
  }
  const int code = child->exit_code();
  bool done = result == "ok" && code == 0;
  if (!done && result == "ok") {
    // Owner 2026-10-28: a Gemma 4 download said ok, was published, and the downloader ended with another code. The
    // download folder has the last word.
    std::string id;
    {
      std::lock_guard lock(mutex_);
      id = cards_[index].entry.id;
    }
    const auto version = settings_.seams.locate(store, id);
    done = version.has_value() && stored_whole(*version, download);
    if (done) {
      log("[strata] downloads: " + id + ": the downloader said ok and ended with exit code " + std::to_string(code) +
          "; every file is in the download folder, so the download is kept");
    }
  }
  if (done) {
    std::lock_guard lock(mutex_);
    cards_[index].bytes = cards_[index].total;
    cards_[index].rate_bps = 0.0;
    return true;
  }
  error = pull_failure(result, code);
  return false;
}

bool DownloadManager::prepare(const std::size_t index, const HfDownload& download, std::string& error) {
  DownloadEntry entry;
  bool vision = false;
  {
    std::lock_guard lock(mutex_);
    entry = cards_[index].entry;
    vision = cards_[index].vision && vision_offered(entry);
  }
  // The installed version directory (the store's own index).
  auto root = open_root(settings_.downloads_root);
  if (!std::holds_alternative<CanonicalRoot>(root)) {
    error = "the model store cannot be opened";
    return false;
  }
  auto read = read_model_index(std::get<CanonicalRoot>(root));
  const auto* index_file = std::get_if<ModelIndex>(&read);
  const auto installed = index_file ? find_installed(*index_file, ModelReference{download.name, download.tag})
                                    : std::variant<IndexEntry, AcquireError>{AcquireError{AcquireReason::model_unknown, ""}};
  const auto* found = std::get_if<IndexEntry>(&installed);
  if (found == nullptr) {
    error = "the installed download is not in the store's index";
    return false;
  }
  const std::filesystem::path version = settings_.downloads_root / download.name / download.tag / found->dir;
  const std::string first = gguf_shard_set(entry.files.front().path).front();
  const std::filesystem::path shard = version / path_from_utf8(first);
  const std::filesystem::path pack = settings_.downloads_root / "packs" / entry.id;
  // Qwen3.6 and a profile-matched custom paste are GGUF-direct: tokenizer only, the engine reads the
  // GGUF itself (--native). A custom paste never has an iq_pack, so any custom qwen card is direct.
  const bool gguf_direct = entry.engine == "qwen" && (entry.family == "qwen36" || entry.source == "custom");
  std::vector<std::string> args{"-I", path_to_utf8(*settings_.prepare_script), "--gguf", path_to_utf8(shard), "--out", path_to_utf8(pack),
                                "--tools", path_to_utf8(*settings_.prepare_tools)};
  if (gguf_direct) args.emplace_back("--tokenizer-only");
  else for (const auto& arg : entry.pack_args) args.push_back("--pack-arg=" + arg);
  auto child = spawn(std::move(args), *settings_.prepare_python);
  if (!child) {
    error = "the prepare step could not start";
    return false;
  }
  std::string last;
  const auto until = std::chrono::steady_clock::now() + std::chrono::hours(2);
  while (std::chrono::steady_clock::now() < until) {
    {
      std::lock_guard lock(mutex_);
      if (stopping_) {
        child->kill();
        error = "the server is stopping";
        return false;
      }
    }
    auto line = child->next_line(std::chrono::milliseconds(500));
    if (line) {
      last = *line;
      continue;
    }
    if (child->ended()) break;
  }
  if (last != "DONE" || child->exit_code() != 0) {
    error = last.rfind("ERROR ", 0) == 0 ? "the pack tools said " + last.substr(6) : "the pack tools ended without finishing";
    return false;
  }
  // prepared.json: the pack, its tokenizer and the PLE table's shard.
  std::ifstream in(pack / "prepared.json", std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  const auto parsed = parse_json(text.str(), kChildJsonLimits);
  const auto* prepared = std::get_if<JsonValue>(&parsed);
  const auto* ple = prepared ? member(*prepared, "ple") : nullptr;
  const auto& tmpl = *settings_.card_template;
  const std::filesystem::path models_root = shard.parent_path();
  std::string model_id = path_to_utf8(shard.filename());
  if (model_id.size() > 5U) model_id.resize(model_id.size() - 5U);  // the inventory id is the stem
  std::string args_json;
  if (gguf_direct) {
    args_json = "[\"--mtp\",\"2\"]";  // Unsloth's qwen35moe GGUFs carry the MTP block; 2 drafts (upstream OWN_PATH_FAMILIES)
  } else {
    args_json = "[\"--pack\"," + json_text(path_to_utf8(pack));
    if (ple != nullptr && ple->if_string() != nullptr && !ple->if_string()->empty()) args_json += ",\"--ple-gguf\"," + json_text(*ple->if_string());
    if (entry.expert_profile) {
      args_json += ",\"--expert-profile\"," + json_text(path_to_utf8(settings_.prepare_tools->parent_path() / "data" / *entry.expert_profile));
    }
    args_json += "]";
  }
  std::string lib_dirs = "[";
  for (std::size_t i = 0; i < tmpl.engine_lib_dirs.size(); ++i) lib_dirs += (i ? "," : "") + json_text(tmpl.engine_lib_dirs[i]);
  lib_dirs += "]";
  // The GGUF-direct options follow the matched family's context; MTP drafting is on for a catalog
  // Qwen3.6 (its GGUFs carry the MTP block) and off for a custom paste (most finetune GGUFs have no
  // MTP layer), with the card's toggle to turn it on.
  const CustomProfile* prof = custom_profile(entry.family);
  const std::uint64_t ctx_default = prof ? prof->context_default : 32768U;
  const std::uint64_t ctx_min = prof ? prof->context_min : 4096U;
  const std::uint64_t ctx_max = prof ? prof->context_max : 262144U;
  const bool mtp_default = entry.source != "custom";
  const std::string card_name = card_name_for(entry);
  // the picture encoder that came with it (spec 2026-10-28): vision.mmproj, and Pictures on the GPU by default
  const std::string vision_row =
      vision ? ",\"vision\":{\"mmproj\":" + json_text(path_to_utf8(stored_file(version, entry.mmproj->path))) + "}" : std::string{};
  const std::string vision_option = vision ? "\"vision\":{\"default\":\"gpu\"}," : "";
  const std::string card = "{\"schema_version\":1,\"name\":" + json_text(card_name) + ",\"profile\":\"qwen\",\"config\":{\"engine\":{" +
                           "\"executable\":" + json_text(tmpl.engine_executable) + ",\"model_id\":" + json_text(model_id) +
                           ",\"lib_dirs\":" + lib_dirs + (tmpl.engine_gpu ? ",\"gpu\":" + json_text(*tmpl.engine_gpu) : std::string{}) +
                           ",\"args\":" + args_json + "},\"roots\":{\"models\":" + json_text(path_to_utf8(models_root)) +
                           ",\"tokenizer_pack\":" + json_text(path_to_utf8(pack / "tokenizer")) + "}" + vision_row +
                           ",\"limits\":{\"chat\":{\"max_prompt_tokens\":1048576,\"max_completion_tokens\":1048576}}}," +
                           (gguf_direct
                                ? std::string{"\"options\":{\"max_context\":{\"default\":"} + std::to_string(ctx_default) +
                                      ",\"min\":" + std::to_string(ctx_min) + ",\"max\":" + std::to_string(ctx_max) + "},\"mtp\":{\"default\":" +
                                      (mtp_default ? "true" : "false") + "}," + vision_option +
                                      "\"prefill\":{\"default\":\"auto\"},\"thinking\":{\"default\":\"high\"}}}\n"
                                : std::string{"\"options\":{\"max_context\":{\"default\":131072,\"min\":4096,\"max\":1048576},\"kv\":{\"default\":\"int8\"},"} +
                                      "\"kv_resident\":{\"default\":32768,\"max\":262144},\"spec\":{\"default\":4},\"expert_cache\":{\"default\":\"auto\"}," +
                                      vision_option + "\"prefill\":{\"default\":\"auto\"},\"thinking\":{\"default\":\"high\"}}}\n");
  return write_download_card(entry, card_name, card, error);
}

// Owner 2026-10-27 (no Python): a Qwen3.8-family download packed by strata-prepare (upstream's iq_pack.py, native and
// byte for byte) into <its store>/packs/<id>, then its card: the pack, the first shard as the model (--native), the PLE
// shard where there are two shards at most (upstream setup's rule), the bundled expert profile; the tokenizer is read
// from the GGUF (no tokenizer pack).
bool DownloadManager::native_prepare(const std::size_t index, std::string& error) {
  DownloadEntry entry;
  std::filesystem::path store;
  bool vision = false;
  {
    std::lock_guard lock(mutex_);
    entry = cards_[index].entry;
    store = store_of(cards_[index]);
    vision = cards_[index].vision && vision_offered(entry);
  }
  const auto version = settings_.seams.locate(store, entry.id);
  const auto tool = native_prepare_tool();
  if (!version || !tool || entry.files.empty()) {
    error = !version ? "the installed download is not in its folder's index" : "strata-prepare is not beside the engine";
    return false;
  }
  const std::string first = gguf_shard_set(entry.files.front().path).front();
  const std::filesystem::path shard = stored_file(*version, first);
  const std::filesystem::path pack = store / "packs" / path_from_utf8(entry.id);
  std::vector<std::string> args{"download", "--gguf", path_to_utf8(shard), "--out", path_to_utf8(pack)};
  for (const auto& arg : entry.pack_args) args.push_back("--pack-arg=" + arg);
  if (!run_prepare_tool(*tool, std::move(args), error)) return false;
  std::ifstream in(pack / "prepared.json", std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  const auto parsed = parse_json(text.str(), kChildJsonLimits);
  const auto* prepared = std::get_if<JsonValue>(&parsed);
  const auto* ple = prepared ? member(*prepared, "ple") : nullptr;
  const auto* shards = prepared ? member(*prepared, "shards") : nullptr;
  const std::int64_t shard_count = shards != nullptr && shards->if_integer() != nullptr ? *shards->if_integer() : 0;
  std::string model_id = path_to_utf8(shard.filename());
  if (model_id.size() > 5U) model_id.resize(model_id.size() - 5U);   // the inventory id is the stem
  PackedCardRequest request{.family = PackedFamily::qwen38, .name = card_name_for(entry), .engine = *settings_.gguf_engine,
                            .source = "download:" + entry.id, .pack = pack, .folder = shard.parent_path(), .model_id = model_id};
  if (ple != nullptr && ple->if_string() != nullptr && !ple->if_string()->empty() && shard_count <= 2) request.ple_gguf = *ple->if_string();
  if (entry.expert_profile) request.expert_profile = settings_.gguf_engine->parent_path() / "profiles" / path_from_utf8(*entry.expert_profile);
  if (vision) request.mmproj = stored_file(*version, entry.mmproj->path);   // the picture encoder that came with it
  const auto card = card_for_pack(request);
  return write_download_card(entry, card.name, card.json, error);
}

bool DownloadManager::maya_prepare_ready(const DownloadEntry& entry) const {
  return maya_pack_engine(entry.engine) && entry.prepare == "maya_pack" &&
         native_prepare_tool().has_value() && (entry.engine != "minimax" || minimax_template().has_value());
}

std::optional<std::filesystem::path> DownloadManager::minimax_template() const {
  if (!settings_.gguf_engine) return std::nullopt;
  const auto file = settings_.gguf_engine->parent_path() / "templates" / "minimax-m3.1" / "chat_template.jinja";
  std::error_code ec;
  if (!std::filesystem::is_regular_file(file, ec)) return std::nullopt;
  return file;
}

// Owner 2026-10-27 ("Yes continue with the others": GLM after Qwen): a GLM-5.3 download packed as Maya's setup packs
// it (maya.py pack_step; strata-prepare download --maya, Maya's iq_pack.py --compat-bf16 byte for byte), the pack in a
// folder beside the shards because Maya's engine reads them at <pack>/... The store's version folder stays the store's:
// <store>/packs/<id> holds hard links to its shards (no copy; the same drive) and the pack in pack/. Then the card:
// glm53, the pack as roots.models with Maya's model name as its marker file (the inventory's model, whose folder the
// launch passes as --glm-pack), the pack's tokenizer, Maya's GLM menus (maya.py CONTEXTS, DEFAULT_CONTEXT, SAMPLING).
// raon-1-mini (owner 2026-10-28, our pin) is packed the same way, as its folder card is (strata-prepare's raon1 port of
// Maya's step): the GGUF linked beside pack/, the served model name its marker, a raon1 card.
bool DownloadManager::maya_prepare(const std::size_t index, std::string& error) {
  DownloadEntry entry;
  std::filesystem::path store;
  {
    std::lock_guard lock(mutex_);
    entry = cards_[index].entry;
    store = store_of(cards_[index]);
  }
  const auto version = settings_.seams.locate(store, entry.id);
  const auto tool = native_prepare_tool();
  if (!version || !tool || entry.files.empty()) {
    error = !version ? "the installed download is not in its folder's index" : "strata-prepare is not beside the engine";
    return false;
  }
  const std::filesystem::path folder = store / "packs" / path_from_utf8(entry.id);
  std::error_code ec;
  std::filesystem::create_directories(folder.parent_path(), ec);
  if (!std::holds_alternative<CanonicalRoot>(open_root(folder, RootPolicy::create_private))) {
    error = "the folder for its pack could not be made";
    return false;
  }
  if (entry.engine == "deepseek") {
    // DeepSeek-V4.1 (maya.py ds4_pack_step): ds4.txt names the model's folder, so the pack is <store>/packs/<id> and reads
    // the store's version folder in place; Maya's shipped DeepSeek profile beside the engine seeded into it. The store keeps
    // the catalog's subfolder (<version>/Maya-XS/...-00001-of-00005.gguf; owner 2026-10-28: "missing_gguf" without it)
    const auto first = stored_file(*version, gguf_shard_set(entry.files.front().path).front());
    std::vector<std::string> arguments{"download", "--maya", "--gguf", path_to_utf8(first), "--out", path_to_utf8(folder)};
    const auto profile = settings_.gguf_engine->parent_path() / "profiles" / "expert-profile-ds4.bin";
    if (std::filesystem::is_regular_file(profile, ec)) arguments.insert(arguments.end(), {"--expert-profile", path_to_utf8(profile)});
    if (!run_prepare_tool(*tool, arguments, error)) return false;
    if (!std::holds_alternative<CanonicalRoot>(open_root(folder)) && !make_tree_owner_only(folder)) {
      error = "its pack could not be made private";
      return false;
    }
    // the inventory's model: the DeepSeek launch passes its folder, the pack, as --pack
    if (!write_pack_marker(folder, kDeepseekModelName)) {
      error = "the pack's model file could not be written";
      return false;
    }
    const auto card = card_for_pack({.family = PackedFamily::deepseek, .name = card_name_for(entry), .engine = *settings_.gguf_engine,
                                     .source = "download:" + entry.id, .pack = folder});
    return write_download_card(entry, card.name, card.json, error);
  }
  if (entry.engine == "minimax") {
    // MiniMax-M3.1 (maya.py mm3_pack_step, owner 2026-10-28): as DeepSeek's - mm3.txt names the model's folder, so the
    // pack is <store>/packs/<id> and reads the store's version folder in place (the indexers file beside the shards);
    // Maya's chat template from the install
    const auto chat_template = minimax_template();
    if (!chat_template) {
      error = "MiniMax's chat template is not beside the engine";
      return false;
    }
    const auto first = stored_file(*version, gguf_shard_set(entry.files.front().path).front());
    if (!run_prepare_tool(*tool, {"download", "--maya", "--gguf", path_to_utf8(first), "--out", path_to_utf8(folder), "--template",
                                  path_to_utf8(*chat_template)},
                          error)) {
      return false;
    }
    if (!std::holds_alternative<CanonicalRoot>(open_root(folder)) && !make_tree_owner_only(folder)) {
      error = "its pack could not be made private";
      return false;
    }
    if (!write_pack_marker(folder, kMinimaxModelName)) {
      error = "the pack's model file could not be written";
      return false;
    }
    const auto card = card_for_pack({.family = PackedFamily::minimax, .name = card_name_for(entry), .engine = *settings_.gguf_engine,
                                     .source = "download:" + entry.id, .pack = folder});
    return write_download_card(entry, card.name, card.json, error);
  }
  // each shard as the store keeps it (under the catalog's subfolder), linked by its name beside the pack
  std::vector<std::filesystem::path> stored;
  std::vector<std::filesystem::path> shards;
  for (const auto& name : gguf_shard_set(entry.files.front().path)) {
    stored.push_back(stored_file(*version, name));
    shards.push_back(path_from_utf8(name).filename());
  }
  for (std::size_t i = 0; i < shards.size(); ++i) {
    const auto& file = shards[i];
    const auto& target = stored[i];
    const auto link = folder / file;
    std::error_code e;
    if (std::filesystem::exists(link, e)) {
      if (std::filesystem::equivalent(link, target, e) && !e) continue;
      std::filesystem::remove(link, e);
    }
    std::filesystem::create_hard_link(target, link, e);
    if (e) {
      error = "its files could not be linked beside the pack (the drive must support hard links)";
      return false;
    }
  }
  const std::filesystem::path pack = folder / "pack";
  if (!run_prepare_tool(*tool, {"download", "--maya", "--gguf", path_to_utf8(folder / shards.front()), "--out", path_to_utf8(pack)},
                        error)) {
    return false;
  }
  // the pack is the card's model folder: owner-only as the server checks one (strata-prepare's folders inherit)
  if (!std::holds_alternative<CanonicalRoot>(open_root(pack)) && !make_tree_owner_only(pack)) {
    error = "its pack could not be made private";
    return false;
  }
  // the inventory's model: the GLM launch passes its folder, the pack, as --glm-pack (raon1: the served model name)
  const bool raon1 = entry.engine == "raon1";
  if (!write_pack_marker(pack, raon1 ? kRaon1ModelName : kGlmModelName)) {
    error = "the pack's model file could not be written";
    return false;
  }
  const auto card = card_for_pack({.family = raon1 ? PackedFamily::raon1 : PackedFamily::glm53, .name = card_name_for(entry),
                                   .engine = *settings_.gguf_engine, .source = "download:" + entry.id, .pack = pack});
  return write_download_card(entry, card.name, card.json, error);
}

std::optional<std::string> run_prepare_command(const std::function<std::unique_ptr<LineChild>(const EngineCommand&)>& spawn,
                                               const EngineCommand& command, const std::function<bool()>& stopping) {
  auto child = spawn ? spawn(command) : nullptr;
  if (!child) return std::string{"strata-prepare could not start"};
  std::string last;
  const auto until = std::chrono::steady_clock::now() + std::chrono::hours(2);
  while (std::chrono::steady_clock::now() < until) {
    if (stopping && stopping()) {
      child->kill();
      return std::string{"the server is stopping"};
    }
    if (auto line = child->next_line(std::chrono::milliseconds(500))) {
      last = *line;
      continue;
    }
    if (child->ended()) break;
  }
  if (last != "DONE" || child->exit_code() != 0) {
    return last.rfind("ERROR ", 0) == 0 ? "strata-prepare said " + last.substr(6) : std::string{"strata-prepare ended without finishing"};
  }
  return std::nullopt;
}

bool DownloadManager::run_prepare_tool(const std::filesystem::path& tool, std::vector<std::string> arguments, std::string& error) {
  EngineCommand command;
  command.executable = tool;
  command.arguments = std::move(arguments);
  command.environment = settings_.child_environment;
  command.working_directory = settings_.work_dir;
  command.no_new_privileges = true;
  command.insert_serve = false;
  const auto failure = run_prepare_command(settings_.seams.spawn, command, [this] {
    std::lock_guard lock(mutex_);
    return stopping_;
  });
  if (failure) error = *failure;
  return !failure;
}

bool DownloadManager::write_download_card(const DownloadEntry& entry, const std::string& card_name, const std::string& card,
                                          std::string& error) {
  // never over a card added from a folder whose file shares the name (on Windows "Gemma-Small.json" is "gemma-small.json"):
  // that card already runs the model, and replacing it would lose its folder and its options
  std::error_code ec;
  if (std::filesystem::exists(settings_.cards_dir / path_from_utf8(entry.id + ".json"), ec) &&
      !download_card_file(settings_.cards_dir, entry.id)) {
    error = "this PC already has a card in " + entry.id + ".json (added from a model folder), so the download's card was not written "
            "over it; its files are installed";
    return false;
  }
  auto cards_root = open_root(settings_.cards_dir);
  const auto card_file = parse_relative_path(entry.id + ".json");
  if (!std::holds_alternative<CanonicalRoot>(cards_root) || !std::holds_alternative<RelativePath>(card_file) ||
      write_private_file(std::get<CanonicalRoot>(cards_root), std::get<RelativePath>(card_file), card, TokenStoreMode::replace)) {
    error = "the model card could not be written";
    return false;
  }
  if (settings_.on_card && !settings_.on_card(card_name, settings_.cards_dir / (entry.id + ".json"))) {
    error = "the model card was written but the launcher refused it (see the server log)";
    return false;
  }
  return true;
}

// §4.3: the card of a downloaded Gemma 4 or flint GGUF, from its header (card_for_gguf), its folder the installed
// version, its source the download.
bool DownloadManager::gguf_card(const std::size_t index, std::string& error) {
  DownloadEntry entry;
  std::filesystem::path store;
  bool vision = false;
  {
    std::lock_guard lock(mutex_);
    entry = cards_[index].entry;
    store = store_of(cards_[index]);
    vision = cards_[index].vision && vision_offered(entry);
  }
  const auto version = settings_.seams.locate(store, entry.id);
  if (!version) {
    error = "the installed download is not in its folder's index";
    return false;
  }
  const std::string first = gguf_shard_set(entry.files.front().path).front();
  const auto stored = stored_file(*version, first);  // its own folder in the store: the card's model folder
  const std::string file = path_to_utf8(stored.filename());
  auto root = open_root(stored.parent_path());
  const auto relative = parse_relative_path(file);
  if (!std::holds_alternative<CanonicalRoot>(root) || !std::holds_alternative<RelativePath>(relative)) {
    error = "the installed model folder cannot be opened";
    return false;
  }
  auto opened = open_beneath(std::get<CanonicalRoot>(root), std::get<RelativePath>(relative));
  if (!std::holds_alternative<OpenedFile>(opened)) {
    error = "the model file is not in its folder";
    return false;
  }
  const auto header = read_gguf_metadata(std::get<OpenedFile>(opened));
  if (!std::holds_alternative<GgufMetadata>(header)) {
    error = "the model file's GGUF header could not be read";
    return false;
  }
  std::optional<std::filesystem::path> encoder;   // the picture encoder that came with it
  if (vision) encoder = stored_file(*version, entry.mmproj->path);
  std::optional<std::filesystem::path> mtp;       // Gemma 4's MTP draft file, which always comes with it
  if (entry.mtp) mtp = stored_file(*version, entry.mtp->path);
  const auto made = card_for_gguf(std::get<GgufMetadata>(header), {.folder = std::get<CanonicalRoot>(root).path(), .file = file,
                                                                   .engine = *settings_.gguf_engine, .source = "download:" + entry.id,
                                                                   .mmproj = std::move(encoder), .mtp = std::move(mtp)});
  if (const auto* refused = std::get_if<CardError>(&made)) {
    error = refused->message;
    return false;
  }
  const auto& card = std::get<CardText>(made);
  return write_download_card(entry, card.name, card.json, error);
}

}  // namespace secure_frontend
