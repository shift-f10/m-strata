#pragma once

// Model downloads (plan docs/superpowers/plans/2026-10-23-model-downloads.md Task 4): the server
// side of the Models tab's download cards. The server holds no network code: every transfer is
// `strata --config <this config> pull --manifest <m> --trust-manifest sha256:<d> --yes
// --progress-lines`, and a pasted link's lookup `strata ... hf-info <link>`, each a supervised
// child (no shell, fixed argv, closed environment, the Job), read line by line. The manifests are
// made by hf_source (owner rule O4: Hugging Face and .gguf only). After a Qwen download the
// prepare step (upstream's pack tools through tools/secure/prepare_download.py, the owner's D7)
// builds the pack, and a model card is written for the launcher.

#include "secure_frontend/download_catalog.hpp"
#include "secure_frontend/engine_process.hpp"
#include "secure_frontend/hf_source.hpp"
#include "secure_frontend/http_request_handler.hpp"
#include "secure_frontend/request_router.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

namespace secure_frontend {

// One supervised child read line by line (production: EngineStdioProcess).
class LineChild {
 public:
  virtual ~LineChild() = default;
  // The next stdout line, or nullopt on timeout or when the child has ended (then ended() is true).
  [[nodiscard]] virtual std::optional<std::string> next_line(std::chrono::milliseconds timeout) = 0;
  [[nodiscard]] virtual bool ended() = 0;
  [[nodiscard]] virtual int exit_code() = 0;  // after ended()
  virtual void kill() = 0;
};

struct DiskSpace final {
  std::uint64_t free_bytes{0};
  std::uint64_t total_bytes{0};
};

struct DownloadSeams final {
  // Starts a child; nullptr when it could not start.
  std::function<std::unique_ptr<LineChild>(const EngineCommand&)> spawn{};
  std::function<std::optional<DiskSpace>(const std::filesystem::path&)> disk{};
  // The installed version folder of download `id` in the store at `store` (production: the store's own index).
  std::function<std::optional<std::filesystem::path>(const std::filesystem::path& store, std::string_view id)> locate{};
};

// strata-prepare run as `command` through `spawn` and read until it ends (two hours at most) or `stopping` says so:
// nullopt on its DONE line, else why not ("strata-prepare said <its ERROR reason>", or that it did not start or finish).
[[nodiscard]] std::optional<std::string> run_prepare_command(const std::function<std::unique_ptr<LineChild>(const EngineCommand&)>& spawn,
                                                             const EngineCommand& command, const std::function<bool()>& stopping);

// What a prepared Qwen download's card is made of: the engine settings of an existing qwen card.
struct DownloadCardTemplate final {
  std::string engine_executable;               // UTF-8, absolute
  std::vector<std::string> engine_lib_dirs;
  std::optional<std::string> engine_gpu;
};

// The card a prepared download becomes (the launcher adds it).
using CardWriter = std::function<bool(const std::string& name, const std::filesystem::path& card_file)>;

struct DownloadSettings final {
  DownloadCatalog catalog;
  std::filesystem::path strata_executable;     // `strata` beside the server
  std::filesystem::path config_file;           // the server's own config (strata reads it)
  std::filesystem::path downloads_root;        // models.downloads_root (the store)
  std::filesystem::path work_dir;              // <roots.state>/downloads: manifests, links, temp
  std::filesystem::path cards_dir;             // <roots.state>/cards: the cards prepared downloads become
  std::optional<std::filesystem::path> prepare_python;
  std::optional<std::filesystem::path> prepare_tools;   // upstream's tools folder
  std::optional<std::filesystem::path> prepare_script;  // tools/secure/prepare_download.py
  std::optional<DownloadCardTemplate> card_template;
  // GUI-first start-up (spec 2026-10-27 §4.3): models.default_engine. Set, a Gemma 4 or flint GGUF becomes a card the
  // moment it is verified (card_for_gguf: the tokenizer is read from the GGUF; source "download:<id>").
  std::optional<std::filesystem::path> gguf_engine;
  std::map<std::string, std::string, std::less<>> child_environment;  // the closed environment
  std::uint64_t margin_bytes{std::uint64_t{2} << 30U};                // free space a start leaves
  CardWriter on_card{};                        // a card was written: the launcher loads it
  // GUI-first start-up (spec 2026-10-27 §5a): an alert (download_finished, download_failed) with its subject and text.
  std::function<void(std::string_view event, const std::string& subject, const std::string& body)> on_event{};
  std::function<void(std::string_view)> log{};
  DownloadSeams seams{};
};

class DownloadManager final {
 public:
  explicit DownloadManager(DownloadSettings settings);
  ~DownloadManager();
  DownloadManager(const DownloadManager&) = delete;
  DownloadManager& operator=(const DownloadManager&) = delete;

  struct Refusal final {
    int status;              // the HTTP status the route answers
    std::string_view code;   // an ApiError code name
  };
  // GET /v1/strata/downloads (the plan's JSON shape).
  [[nodiscard]] std::string list_json();
  // Starts (queues) a catalog or custom card. `folder` (spec 2026-10-27 §4.3): the drive or folder the model goes to,
  // an existing owner-only folder that becomes its store (strata pull --store); its own volume's free space is
  // checked; unset, the card's earlier folder or models.downloads_root. The choice is kept for a resume.
  // `vision` (spec 2026-10-28, the "Vision files" checkbox): the entry's picture encoder comes too, and the card reads
  // pictures with it; 400 invalid_request for an entry with none (vision_bytes 0). The choice is kept for a resume.
  [[nodiscard]] std::optional<Refusal> start(std::string_view id, const std::optional<std::filesystem::path>& folder = std::nullopt,
                                             bool vision = false);
  // A pasted link (O3): the lookup through `strata hf-info`; the confirm object, or a refusal and
  // a fixed reason for the page.
  struct LinkAnswer final {
    std::optional<std::string> confirm_json;
    std::optional<Refusal> refusal;
    std::string reason;
  };
  // url is the pasted link; profile matches it to a family this server runs (empty: unmatched, a
  // download-only custom card as before); extra_urls are the other shards of a multipart GGUF (at
  // most five), all pulled to the one location.
  // folder: as start's (the confirm's "fits" is for that folder's drive).
  [[nodiscard]] LinkAnswer check_link(std::string_view url, std::string_view profile = {},
                                      const std::vector<std::string>& extra_urls = {},
                                      const std::optional<std::filesystem::path>& folder = std::nullopt);
  // Starts the download a check_link confirmed (the same link, within ten minutes).
  [[nodiscard]] std::optional<Refusal> start_link(std::string_view url, const std::optional<std::filesystem::path>& folder = std::nullopt);
  // Stops a running, queued or paused download and discards its partial files (409 when none is).
  [[nodiscard]] std::optional<Refusal> cancel(std::string_view id);
  // Stops a running or queued download and keeps its partial files: a later start resumes it.
  [[nodiscard]] std::optional<Refusal> pause(std::string_view id);
  // The danger zone's delete of a downloaded model (spec 2026-10-27 §4.5): `strata rm <id>:download` in its own store
  // (the store's index row and version folder go), then it is downloadable again. 404 unknown, 409 while queued or
  // downloading, 500 when the store still lists it after.
  [[nodiscard]] std::optional<Refusal> remove_installed(std::string_view id);
  void shutdown();

  // Test seam: wait until nothing is queued or running.
  [[nodiscard]] bool wait_idle(std::chrono::milliseconds timeout);

 private:
  enum class State { available, queued, downloading, verifying, preparing, installed, ready, failed, cancelled, paused };
  struct Card final {
    DownloadEntry entry;
    State state{State::available};
    std::uint64_t bytes{0};
    std::uint64_t total{0};
    std::string error;
    std::string current_file;
    double rate_bps{0.0};
    std::filesystem::path store{};   // the folder it goes to (empty: models.downloads_root)
    bool vision{false};              // its picture encoder comes too (spec 2026-10-28)
  };
  struct PendingLink final {
    DownloadEntry entry;
    std::chrono::steady_clock::time_point at;
  };
  void run();
  void run_card(std::size_t index);
  [[nodiscard]] bool resolve_main(Card& card, std::string& error);
  [[nodiscard]] bool pull(std::size_t index, const HfDownload& download, std::string& error);
  [[nodiscard]] bool prepare(std::size_t index, const HfDownload& download, std::string& error);
  [[nodiscard]] bool gguf_card(std::size_t index, std::string& error);
  // strata-prepare (owner 2026-10-27, no Python): beside the engine for new cards (models.default_engine), with the
  // expert profiles in its profiles/ folder. A Qwen3.8-family download (iq_pack) is packed with it when it is there.
  [[nodiscard]] std::optional<std::filesystem::path> native_prepare_tool() const;
  [[nodiscard]] bool native_prepare_ready(const DownloadEntry& entry) const;
  [[nodiscard]] bool native_prepare(std::size_t index, std::string& error);
  // A GLM-5.3 download (prepare maya_pack) is packed with it as Maya's setup packs it (strata-prepare download --maya).
  [[nodiscard]] bool maya_prepare_ready(const DownloadEntry& entry) const;
  // MiniMax-M3.1 (owner 2026-10-28): Maya's chat template, which the install ships beside the engine
  // (engine/templates/minimax-m3.1/chat_template.jinja, with the MiniMax Community License); nullopt: not there.
  [[nodiscard]] std::optional<std::filesystem::path> minimax_template() const;
  [[nodiscard]] bool maya_prepare(std::size_t index, std::string& error);
  // strata-prepare with `arguments`, read until it ends: true on its DONE line (else `error` names its ERROR reason).
  [[nodiscard]] bool run_prepare_tool(const std::filesystem::path& tool, std::vector<std::string> arguments, std::string& error);
  // A download's card file (<cards>/<id>.json, owner-only) handed to the launcher.
  [[nodiscard]] bool write_download_card(const DownloadEntry& entry, const std::string& card_name, const std::string& card,
                                         std::string& error);
  [[nodiscard]] bool gguf_family(const DownloadEntry& entry) const;
  [[nodiscard]] std::filesystem::path store_of(const Card& card) const;
  [[nodiscard]] std::variant<std::filesystem::path, Refusal> chosen_store(const Card& card,
                                                                          const std::optional<std::filesystem::path>& folder) const;
  void load_destinations();
  void save_destinations();
  // vision.json: the downloads whose picture encoder comes too ({"ids": [...]}), for a resume after a restart.
  void load_vision();
  void save_vision();
  [[nodiscard]] std::optional<std::string> hf_info(std::string_view url, std::string& error);
  void discard_partials(std::string_view id, const std::filesystem::path& store);
  [[nodiscard]] std::unique_ptr<LineChild> spawn(std::vector<std::string> arguments, const std::filesystem::path& executable);
  [[nodiscard]] std::optional<DiskSpace> disk(const std::filesystem::path& store) const;
  [[nodiscard]] std::uint64_t total_of(const DownloadEntry& entry, bool vision = false) const;
  [[nodiscard]] std::optional<std::size_t> find(std::string_view id) const;
  void set_state(std::size_t index, State state, std::string error = {});
  void log(std::string_view line) const;
  [[nodiscard]] static std::string_view state_name(State state) noexcept;
  void load_custom();
  void save_custom();
  void refresh_installed();

  DownloadSettings settings_;
  std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<Card> cards_;                 // stable addresses: cards are only appended
  std::deque<std::size_t> queue_;
  std::optional<std::size_t> active_;
  std::unique_ptr<LineChild>* active_child_{nullptr};
  bool cancel_requested_{false};   // stop the active transfer now (pause or cancel)
  bool pause_requested_{false};    // ... and keep its partial files
  bool stopping_{false};
  std::map<std::string, PendingLink, std::less<>> links_;
  std::thread worker_;
};

// The download routes' answers (kModelDownloadRoutes) over one manager: the plan's JSON
// contract, every refusal an ApiError row.
[[nodiscard]] HttpResponse download_route_answer(DownloadManager& manager, RouteId route, std::string_view body);

// `strata` beside this process's own executable (the server's): the downloads' child. Empty
// when the executable's path cannot be read.
[[nodiscard]] std::filesystem::path strata_beside_this_executable();

// The production seams: EngineStdioProcess children (the Job / process group) and
// std::filesystem::space.
[[nodiscard]] DownloadSeams production_download_seams();

}  // namespace secure_frontend
