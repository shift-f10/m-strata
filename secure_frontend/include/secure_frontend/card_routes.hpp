#pragma once

// GUI-first start-up (docs/superpowers/specs/2026-10-27-gui-first-install-design.md §4): the page's card and folder
// routes (kModelCardRoutes). Every route answers loopback clients (D4) and the clients CardRouteSettings::client_allowed
// lets in (the server: a host or subnet models.control_remote_networks lists), each body a small JSON object:
//
//   POST /v1/strata/folders             {}                  -> {"volumes": [{"path","label","free_bytes","total_bytes"}]}
//                                       {"path"}            -> {"path","parent","free_bytes","truncated","entries":[...]}
//   POST /v1/strata/folders/private     {"path"}            -> {"private": true}   (local_folders.hpp's guards)
//   POST /v1/strata/folders/new         {"parent","name"}   -> 201 {"path"}   (a new owner-only folder: a download's place)
//   POST /v1/strata/folders/sharing     {}                  -> {"shared","running","editable"}
//                                       {"shared": bool}    -> {"saved": true, "restart_needed": true}
//                                                              (roots.allow_untrusted_media in the config file: the next
//                                                              start uses model folders as they are; owner 2026-10-27)
//   POST /v1/strata/cards/add           {"folder","file"}   -> 201 {"name","profile"}   (card_for_gguf; source "folder")
//   POST /v1/strata/cards/folder        {"name","folder"}   -> {"name"}   (the card's model file must be there)
//   POST /v1/strata/cards/delete/begin  {"name"}            -> {"code"}   (DeleteCodes; Cache-Control: no-store)
//   POST /v1/strata/cards/delete        {"name","code","key","permanent": true}
//                                                           -> {"name","files","bytes","folder_removed"}
//                                                              (+ "download_removed" for a downloaded model)
//   POST /v1/strata/cards/remove        {"name"}            -> {"name","stopping"}   (the card goes, its model files stay;
//                                                              a running card's model stops first: "stopping": true)
//
// A model folder the page names must be private (others may not change it; the page offers to make it so), unless the
// main config sets roots.allow_untrusted_media: then it is used as it is, and a pack built beside its files is left as
// strata-prepare made it (owner 2026-10-27: "not change the permissions when browsing and using existing models").
// A card the page writes is <cards_dir>/<name>.json (owner-only, the card format), validated with the main config as
// at boot before it is written or joins the launcher. Only cards the server wrote may be changed or deleted, never
// while they run or a start or stop runs; only a GGUF card's model files are deleted (delete_gguf_model_files), and
// the delete needs all three of the code (spent by any try), the server's API key and "permanent": true.

#include "secure_frontend/api_router.hpp"
#include "secure_frontend/card_builder.hpp"
#include "secure_frontend/frontend_config.hpp"
#include "secure_frontend/model_launcher.hpp"
#include "secure_frontend/request_router.hpp"

#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace secure_frontend {

struct CardRouteSettings final {
  std::filesystem::path cards_dir;              // <roots.state>/cards (owner-only): the cards the server writes
  std::optional<std::filesystem::path> engine;  // models.default_engine: a page card's engine (unset: adding is refused)
  FrontendConfigDraft main;                     // the main config each new card is validated with
  // The server's own API key check (the delete's second step). Empty (listener.loopback_without_key): deletes refused.
  std::function<bool(std::string_view key)> key_matches{};
  // Who may use these routes (the server passes model_files_allowed over models.control_remote_networks; owner
  // 2026-10-27). Empty: loopback clients only (D4).
  std::function<bool(const RequestContext& context)> client_allowed{};
  // A deleted card that came from a download ("download:<id>"): take <id> out of its download store
  // (DownloadManager::remove_installed); nullopt when it is gone, else why not. Empty: no downloads on this server.
  std::function<std::optional<std::string>(std::string_view download_id)> remove_download{};
  // A Qwen3.8 or GLM-5.3 model added from its folder has its pack built first (owner 2026-10-27): the folder a Qwen3.8
  // pack goes in (<roots.state>/packs; a GLM pack goes beside the shards), and strata-prepare run with `arguments`
  // (nullopt on its DONE line, else why not). Empty: such a model can't be added (strata-prepare not beside the engine).
  std::optional<std::filesystem::path> packs_dir{};
  std::function<std::optional<std::string>(const std::vector<std::string>& arguments)> prepare{};
  std::function<void(std::string_view line)> log{};  // default: stderr
  // The config file the server started from: the Model folders setting is saved there. Unset: shown, never saved.
  std::optional<std::filesystem::path> config_file{};
};

class CardRoutes final {
 public:
  CardRoutes(ModelLauncher& launcher, CardRouteSettings settings);
  [[nodiscard]] HttpResponse answer(RouteId route, std::string_view body, const RequestContext& context);

 private:
  HttpResponse folders(std::string_view body);
  HttpResponse make_private(std::string_view body);
  HttpResponse new_folder(std::string_view body);
  HttpResponse sharing(std::string_view body);
  HttpResponse add(std::string_view body);
  HttpResponse add_packed(PackedFamily family, const GgufMetadata& metadata, const CanonicalRoot& root, const std::string& file);
  HttpResponse publish(const CardText& text);
  HttpResponse repoint(std::string_view body);
  HttpResponse delete_begin(std::string_view body);
  HttpResponse delete_confirm(std::string_view body);
  HttpResponse remove(std::string_view body);
  void log(std::string_view line) const;

  ModelLauncher& launcher_;
  CardRouteSettings settings_;
  bool shared_folders_{false};  // the main config's roots.allow_untrusted_media: model folders used as they are
  DeleteCodes codes_;
  std::mutex files_;  // one card file change at a time
  std::mutex config_;  // one config file write at a time
};

}  // namespace secure_frontend
