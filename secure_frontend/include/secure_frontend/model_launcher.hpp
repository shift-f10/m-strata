#pragma once

// The model launcher (plan docs/superpowers/plans/2026-10-23-model-launcher.md Task 3; spec §5):
// with models.cards set, the server starts no fixed model. It holds the cards, starts the one
// models.start names (or none, D5), and lets the page start, switch and stop them (Task 4's
// routes). One model runs at a time; each start composes its own ModelRuntime (tokenizer,
// supervisor, port, gate, chat route) from the main config + the card + the option values, so a
// request validated against one model never runs on another's engine.
//
// A start (§5.2): the request is validated at once (card, option values, the card's config) and
// answered; the launcher's own thread then composes the model (the tokenizer loads) BEFORE it
// touches the running one, so a bad card never takes the running model down. Then the switch:
// new chat requests get 503 model_switching; queued ones are refused; the generation in flight
// may finish for up to 60 s (D3), then it is cut like a shutdown; the old engine is shut down;
// the state directory is prepared; the new engine starts and the launcher waits for READY. A
// start that fails stays stopped with a fixed reason (D4). Starts and stops are logged.

#include "secure_frontend/api_errors.hpp"
#include "secure_frontend/api_router.hpp"
#include "secure_frontend/calibration.hpp"
#include "secure_frontend/calibration_report.hpp"
#include "secure_frontend/engine_supervisor.hpp"
#include "secure_frontend/frontend_config.hpp"
#include "secure_frontend/generation_gate.hpp"
#include "secure_frontend/json.hpp"
#include "secure_frontend/model_cards.hpp"
#include "secure_frontend/model_options.hpp"
#include "secure_frontend/model_runtime.hpp"
#include "secure_frontend/request_router.hpp"
#include "secure_frontend/serve_monitor.hpp"
#include "secure_frontend/tool_runner.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace secure_frontend {

// The cards one launcher holds: the listed ones (kMaxModelCards) and the cards prepared
// downloads add (model downloads, plan 2026-10-23-model-downloads Task 4).
inline constexpr std::size_t kMaxLauncherCards = 64;

// calibrating: GUI-first start-up (spec 2026-10-27 §5), the card's engine flags being swept (the GPU to itself).
enum class CardState { stopped, starting, running, failed, calibrating };
[[nodiscard]] std::string_view card_state_name(CardState state) noexcept;

// GUI-first start-up (spec 2026-10-27 §4.1): the answer to a change of the cards the server wrote.
// stopping: the running card's model stops first, then the card is removed (remove_card with RunningCard::stop_first).
enum class CardChange { done, not_found, not_page_card, busy, running, shutting_down, stopping };
// What remove_card does with the card that runs: refuse (a delete: its engine holds the files), or stop it first.
enum class RunningCard { refuse, stop_first };

struct LauncherSettings final {
  FrontendConfigDraft main;                         // the main config (main_config_card_issues is empty)
  std::vector<ModelCard> cards;                     // the loaded cards (load_model_cards)
  // The first `operator_cards` cards are the operator's (models.cards: read-only from the page); the rest, and every
  // card add_card adds, the server wrote (<roots.state>/cards) and the page may replace or remove.
  std::size_t operator_cards{kMaxLauncherCards};
  std::optional<std::filesystem::path> memory_file; // D7: the remembered options (nullopt: none kept)
  GateLimits gate;                                  // each runtime's generation gate
  ServeMonitor* monitor{nullptr};                   // listener.monitor; told about each generation
  std::chrono::milliseconds drain_wait{std::chrono::seconds(60)};  // D3: the generation in flight may finish
  std::chrono::milliseconds cut_wait{std::chrono::seconds(15)};    // ... then it is cut and given this long
  std::chrono::milliseconds stop_bound{config_defaults::kShutdownBound};  // one supervisor's orderly shutdown
  // How a start's engine processes are made (production: make_stdio_process_factory).
  std::function<ProcessFactory(EngineLaunchPlan)> factory{};
  // How a start's picture encoder is made when its card reads images (unset: vision_process_factory).
  std::function<ProcessFactory(const VisionPlan&)> vision_factory{};
  // One fixed line per start, stop and outcome (default: stderr).
  std::function<void(std::string_view)> log{};
  // GUI-first start-up (spec 2026-10-27 §5): <roots.state>/calibration, each card's calibration (<card>.json). Unset:
  // no calibration (a start uses the card's own flags only).
  std::optional<std::filesystem::path> calibration_dir{};
  // The GPU's name and memory for the calibration key (the Monitor's newest reading); empty: unknown.
  std::function<std::string()> gpu_fact{};
  // A calibration's report for a bug report (owner 2026-10-30): the folder a calibration with a failed trial writes it
  // to (<roots.logs>/calibration-reports; unset: none is written), and this PC's hardware for it, read once per report
  // on the calibration's thread (unset: the CPU, RAM and OS this process reads, with no GPU details).
  std::optional<std::filesystem::path> report_dir{};
  std::function<ReportMachine()> report_machine{};
  // GUI-first start-up (spec 2026-10-27 §5a): an alert (calibration_finished, calibration_failed, engine_stopped) with
  // its subject and text - the card's name and a fixed reason, never a path. Called outside the launcher's lock.
  std::function<void(std::string_view event, const std::string& subject, const std::string& body)> on_event{};
  // Web search (spec 2026-10-27-web-search-design §2): the server's tool for each started model's chat route (the
  // search service; null: none) and search.max_rounds. A calibration's engine never gets it.
  ToolRunner* tools{nullptr};
  std::size_t tool_rounds{8};
};

class ModelLauncher final {
 public:
  explicit ModelLauncher(LauncherSettings settings);
  ~ModelLauncher();
  ModelLauncher(const ModelLauncher&) = delete;
  ModelLauncher& operator=(const ModelLauncher&) = delete;

  // A refusal of start/stop: the API error row and, for invalid_option, the option's name.
  struct Refusal final {
    ApiError error;
    std::string param{};
  };
  // Validates and accepts a start of card `name` with `options` (the body's "options" object;
  // nullptr = the remembered values, else the card's defaults). nullopt: accepted (202). `may_calibrate` false (owner
  // 2026-10-29: the page's "Start without calibrating") starts it without the calibration a first start or a settings
  // change would run first.
  [[nodiscard]] std::optional<Refusal> start(std::string_view name, const JsonValue* options, bool remote_client,
                                             bool may_calibrate = true);
  // Stops the running model (or the one starting, once its start is done). nullopt: accepted.
  [[nodiscard]] std::optional<Refusal> stop(bool remote_client);
  // GUI-first start-up (spec 2026-10-27 §5): calibrates card `name` at `options` (as start's): the running model is
  // stopped first, the card's engine flags are swept (calibration.hpp), and the result is kept with its key; then, with
  // `then_start`, the card starts with it. `auto_recalibrate` (when given) is kept with the result: a later start at
  // other options calibrates first. A start of a card the page made that was never calibrated calibrates first too.
  [[nodiscard]] std::optional<Refusal> calibrate(std::string_view name, const JsonValue* options, bool then_start,
                                                 std::optional<bool> auto_recalibrate, bool remote_client);
  // Stops the running calibration: its engine is stopped and nothing is kept. nullopt: accepted.
  [[nodiscard]] std::optional<Refusal> cancel_calibration();
  // "Recalibrate when settings change" for card `name`: kept with its calibration (or with the next one).
  [[nodiscard]] std::optional<Refusal> set_auto_recalibrate(std::string_view name, bool on);
  // Card `name`'s last calibration report since this server started (owner 2026-10-30: made when a trial failed):
  // its file name in LauncherSettings::report_dir, when it was made, and its Markdown. nullopt: none.
  struct CalibrationReportFile final {
    std::string file;
    std::string date;
    std::string text;
  };
  [[nodiscard]] std::optional<CalibrationReportFile> calibration_report(std::string_view name) const;
  // A card made while the server runs (a prepared download's): listed at once, stopped. false:
  // a card of that name is already held, or kMaxLauncherCards are.
  [[nodiscard]] bool add_card(ModelCard card);
  // GUI-first start-up (spec 2026-10-27 §4.4): a card the server wrote, replaced by one of the same name (a moved
  // folder). Refused while a start or stop runs (a start reads its card outside the lock); a running model keeps
  // running and the new card applies at its next start.
  [[nodiscard]] CardChange replace_card(ModelCard card);
  // §4.5: a card the server wrote, removed with its remembered options and its calibration; refused while a start or
  // stop runs. The card that runs is refused, or with RunningCard::stop_first (owner 2026-10-28, "remove an active model
  // card but not delete files") its model is stopped as stop() stops it and the card removed after: `stopping`.
  [[nodiscard]] CardChange remove_card(std::string_view name, RunningCard running = RunningCard::refuse);
  // A copy of the card as held, whether the page may change it, and its state.
  struct HeldCard final {
    ModelCard card;
    bool page_card{false};
    CardState state{CardState::stopped};
  };
  [[nodiscard]] std::optional<HeldCard> card(std::string_view name) const;
  // Every card held, in the list's order (copies): the Download list says which catalog models are already on this PC
  // (owner 2026-10-28).
  [[nodiscard]] std::vector<ModelCard> cards() const;

  // GET /v1/strata/models (the plan's JSON shape).
  [[nodiscard]] std::string list_json() const;
  // /v1/models: the running model's id and aliases (none while nothing runs or a switch is on).
  [[nodiscard]] ServedModels served() const;
  // POST /v1/chat/completions in launcher mode.
  [[nodiscard]] RouteResult chat(std::string_view body, const RequestContext& context);
  // GET /health: the running model's engine, "starting" while a start or switch runs, "none"
  // when nothing runs.
  [[nodiscard]] HealthView health() const;
  // GET /metrics: the Monitor's JSON for the running model.
  [[nodiscard]] std::string metrics_json() const;
  // The Monitor's sampler, once a second: the running model's engine while it is up (EngineSupervisor::engine_sample);
  // nullopt while nothing runs, a switch is retiring it, or its engine is not up.
  [[nodiscard]] std::optional<EngineSample> engine_sample() const;

  // The pool's shutdown hook: no more starts; every runtime's generation ends like a shutdown.
  void begin_shutdown() noexcept;
  // Joins the launcher's thread and shuts the running model's supervisor down.
  void shutdown();
  // After shutdown(): a supervisor left an engine it could not reap (spec C4: exit 1).
  [[nodiscard]] bool engine_left_unreaped() const noexcept;

  // Test seams: wait until no start or stop is running; the card's state.
  [[nodiscard]] bool wait_until_settled(std::chrono::milliseconds timeout) const;
  [[nodiscard]] CardState state_of(std::string_view name) const;

 private:
  struct CardStatus final {
    CardState state{CardState::stopped};
    std::string_view error{};  // a fixed reason; empty unless failed
  };
  void write_remembered();
  // A card out of the list (mutex_ held), then its remembered options and calibration files (mutex_ not held).
  void erase_card(std::size_t index);
  void forget_card(const std::string& name);
  struct Job final {
    // remove: stop (the card runs), then remove the card `name`
    enum class Kind { start, stop, calibrate, remove } kind{Kind::start};
    std::size_t card{0};
    ModelOptionValues values{};
    std::shared_ptr<const FrontendConfig> config{};
    bool then_start{false};                   // calibrate: start the card with the result afterwards
    std::optional<bool> auto_recalibrate{};   // calibrate: kept with the result
    bool prefill_first{false};                // start: MiniMax's prefill calibration runs first (run_prefill_calibration)
    std::string name{};                       // remove: the card
  };
  void run_jobs();
  void run_start(const Job& job);
  // §5: the sweep, each candidate one engine start of the card with those flags; true when a result was kept.
  [[nodiscard]] bool run_calibration(const Job& job);
  // `trial` (when given) is filled with how the start went: its speed, or why it failed, the server's lines for its
  // engine and the engine's last lines of output (a report's, owner 2026-10-30).
  [[nodiscard]] std::optional<double> measure(const ModelCard& card, const ModelOptionValues& values,
                                              const std::vector<std::string>& args, unsigned max_tokens,
                                              CalibrationTrialRecord* trial = nullptr);
  // A calibration with a failed trial: its report written to settings_.report_dir and kept for calibration_report;
  // the file's name (empty: none written).
  [[nodiscard]] std::string write_report(const ModelCard& card, const ModelOptionValues& values, std::string started,
                                         std::string outcome, std::vector<CalibrationTrialRecord> trials);
  // MiniMax (Maya's tools/calibrate_mm3_prefill.py): one run of the engine's own --calibrate-prefill with the card's
  // start, over a real text's tokens; the engine keeps the result in the pack (prefill-tune.txt) and this server a record
  // of it (<card>.prefill.json beside the calibrations). True when the engine finished.
  [[nodiscard]] bool run_prefill_calibration(const Job& job);
  // A MiniMax card at these options has no prefill record yet, or one measured on another machine, engine or model.
  [[nodiscard]] bool needs_prefill_calibration(const ModelCard& card, const ModelOptionValues& values) const;
  [[nodiscard]] std::optional<PrefillRecord> read_prefill(std::string_view card) const;
  // The card's validated config at `values` with `args` added to its engine flags (a flag the card sets itself
  // is left as the card has it).
  [[nodiscard]] std::shared_ptr<const FrontendConfig> config_with(const ModelCard& card, const ModelOptionValues& values,
                                                                  const std::vector<std::string>& args) const;
  [[nodiscard]] CalibrationKey calibration_key(const ModelCard& card, const ModelOptionValues& values) const;
  [[nodiscard]] std::optional<CalibrationResult> read_calibration(std::string_view card) const;
  [[nodiscard]] bool calibration_supported(const ModelCard& card) const;
  // Under mutex_: GET /v1/strata/models' "calibration" of card `index` at `values` (null when it has none).
  [[nodiscard]] std::string calibration_json(std::size_t index, const ModelCard& card, const ModelOptionValues& values) const;
  void retire_running(std::unique_lock<std::mutex>& lock);
  void remember(std::size_t card, const ModelOptionValues& values);
  // Under mutex_: add_card appends to settings_.cards (reserved to kMaxLauncherCards, so a held
  // card reference stays valid), and every lookup or walk of the list holds the lock.
  [[nodiscard]] std::optional<std::size_t> find_card(std::string_view name) const;
  [[nodiscard]] std::shared_ptr<ModelRuntime> running_runtime() const;
  void log(std::string_view line) const;

  LauncherSettings settings_;
  RememberedOptions remembered_;
  mutable std::mutex mutex_;
  mutable std::condition_variable changed_;
  std::vector<CardStatus> status_;
  std::vector<bool> page_card_;               // beside settings_.cards: the server wrote it
  std::shared_ptr<ModelRuntime> running_;     // the started (or starting) model's runtime
  std::shared_ptr<ModelRuntime> measuring_;   // a calibration step's runtime (the shutdown hurries its stop)
  std::optional<std::size_t> running_card_;   // which card it is
  std::optional<std::size_t> starting_card_;  // the card an accepted start is bringing up
  bool busy_{false};                          // a start or stop is accepted and not finished
  bool switching_{false};                     // chat is refused: the old model is being retired or the new one loads
  bool shutting_down_{false};
  bool unreaped_{false};
  std::optional<Job> pending_;
  // §5: the calibration running (its card and progress), the cancel request, the last failure per card.
  struct CalibrationRun final {
    std::size_t card{0};
    SweepProgress progress{};
  };
  std::optional<CalibrationRun> calibrating_;
  std::atomic<bool> cancel_calibration_{false};
  std::map<std::string, std::string, std::less<>> calibration_errors_;
  std::map<std::string, CalibrationReportFile, std::less<>> calibration_reports_;  // the last report per card
  std::map<std::string, bool, std::less<>> auto_recalibrate_;  // a switch set before the card's first calibration
  [[nodiscard]] bool write_calibration(const CalibrationResult& result);
  std::string cpu_fact_;
  SweepMachine machine_{};  // the CPU the sweeps are planned for
  bool stop_reported_{false};  // the running model's circuit opening was alerted once
  void alert(std::string_view event, const std::string& subject, const std::string& body) const;
  std::thread worker_;
};

}  // namespace secure_frontend
