#pragma once

#include "secure_frontend/acquire_url.hpp"
#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/config.hpp"
#include "secure_frontend/engine_environment.hpp"
#include "secure_frontend/sampling_params.hpp"
#include "secure_frontend/secret_source.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace secure_frontend {

enum class ConfigValueType { boolean, integer, string, string_array, decimal };

// One row per configuration key: the single source of truth for the JSON
// reader, the command line, --check output and the README key table.
struct ConfigKey final {
  std::string_view path;
  ConfigValueType type;
  bool required;
  std::int64_t minimum;          // integer keys
  std::int64_t maximum;
  std::int64_t default_integer;
  bool default_boolean;
  std::string_view default_string;  // empty = no default
  std::string_view flag;            // strata-unified-server option(s) for the README; empty = file only
  std::string_view description;
  // SP5 M1 Task 4 (decimal rows): bounds below; default_decimal NaN = "unset"
  // (the row stays absent); minimum_exclusive models the pin's open bounds.
  // Defaults keep every pre-Task-4 positional row valid under -Werror.
  double minimum_decimal = 0.0;
  double maximum_decimal = 0.0;
  double default_decimal = std::numeric_limits<double>::quiet_NaN();
  bool minimum_exclusive = false;
};

[[nodiscard]] std::span<const ConfigKey> config_schema() noexcept;
[[nodiscard]] const ConfigKey* find_config_key(std::string_view path) noexcept;
// SP3 addendum: per-key extras kept beside SP1's positional rows.
struct ConfigKeyExtension final {
  std::string_view path;
  std::size_t max_entries;                         // string_array: most entries (0 = unbounded)
  bool (*entry_valid)(std::string_view) noexcept;  // string_array: per-entry grammar (nullptr = any)
  std::string_view entry_reason;                   // fixed reason when an entry fails entry_valid
  std::string_view derived_default;                // non-empty: default derived from other keys (README / --check text)
};
[[nodiscard]] const ConfigKeyExtension* find_config_key_extension(std::string_view path) noexcept;
// True for an object path that prefixes at least one key ("limits", "limits.http").
[[nodiscard]] bool is_config_section(std::string_view path) noexcept;
[[nodiscard]] std::string_view type_reason(ConfigValueType type) noexcept;
// The README key table (secure_frontend/README.md, between the config-keys
// markers); a test fails when they differ.
[[nodiscard]] std::string render_config_schema_markdown();

enum class ConfigSource { file, command_line };
using ConfigDraftValue = std::variant<bool, std::int64_t, std::string, std::vector<std::string>, double>;
struct ConfigDraftEntry final {
  ConfigDraftValue value;
  ConfigSource source;
};

// Parsing produces this mutable draft. A key absent from `entries` is "not
// set"; values are stored exactly as written (no coercion) for validate().
struct FrontendConfigDraft final {
  std::map<std::string, ConfigDraftEntry, std::less<>> entries;
  std::optional<std::filesystem::path> config_directory;
  std::vector<std::string> overridden_by_command_line;
  bool card = false;   // a model card's config (card_draft): ModelsConfig::from_card
};

enum class ConfigIssueCode {
  malformed_document, unknown_key, duplicate_key, wrong_type, missing_required, out_of_range, invalid_value,
  conflicting_values, remote_not_enabled, wildcard_bind_not_enabled, file_unavailable, file_untrusted,
  excluded_key,
};

// SP5 M2: Maya's reasoning-effort vocabulary, spelled exactly as Maya spells it
// (spec §D-2; the vendored validator accepts only these — maya-ref
// serve/server.py:3798).
enum class GlmReasoningEffort { none, low, high, max };

// key_path names a key ("limits.http.max_request_bytes") or "config file";
// reason is fixed text. Neither ever contains a configured value.
struct ConfigIssue final {
  std::string key_path;
  ConfigIssueCode code;
  std::string reason;
};
using ConfigErrors = std::vector<ConfigIssue>;
[[nodiscard]] std::string format_issue(const ConfigIssue& issue);

// deepseek and raon1 (unified-engine design §2.5/§3.3) are served by the one merged engine
// binary over the converged `unified` protocol flavor; qwen/glm53/gemma4 keep their own flavors.
// flint (multiengine_flint) speaks Gemma 4's strata_0143 serve loop with its own template and tokenizer.
enum class EngineProfile { qwen, glm53, gemma4, deepseek, raon1, flint, minimax };

enum class EngineArgumentsProfile;  // engine_arguments.hpp

// The engine CLI each profile's argv is built for (qwen: strata_0143, glm53:
// lighttransport_glm53, gemma4: strata_gemma4) — an exhaustive switch, so a new
// profile cannot silently take another profile's arguments.
[[nodiscard]] EngineArgumentsProfile arguments_profile_for(EngineProfile profile);

class ModelId final {
 public:
  [[nodiscard]] const std::string& value() const noexcept { return value_; }

 private:
  friend std::optional<ModelId> make_model_id(std::string_view text);
  explicit ModelId(std::string value) : value_(std::move(value)) {}
  std::string value_;
};
[[nodiscard]] std::optional<ModelId> make_model_id(std::string_view text);

struct ListenerConfig final {
  IpAddress host;
  std::uint16_t port;
  bool allow_remote;
  bool allow_wildcard_bind;
  std::uint32_t backlog;
  std::vector<std::string> cors_origins;  // SP3 §4.4: exact origins, each is_valid_cors_origin
  std::vector<std::string> host_aliases;  // SP3 §4.3: extra exact Host values, each is_valid_host_alias
  bool web_ui;                            // the built-in chat page at / (web_ui.hpp)
  bool loopback_without_key;              // the one keyless listener: loopback only, no token.source
  bool monitor;                           // GET /metrics, the page's Monitor tab (serve_monitor.hpp)
  bool login_link{false};                 // a one-time login link printed at every start (spec 2026-10-27 §3)
};
struct HttpLimits final {
  std::chrono::milliseconds request_deadline;
  std::uint32_t max_request_bytes;
  std::uint32_t max_header_bytes;
  std::uint32_t max_headers;
  std::uint32_t max_request_line_bytes;
  std::chrono::milliseconds header_deadline;          // SP3 §5.1
  std::chrono::milliseconds write_stall;
  std::chrono::milliseconds response_write_deadline;
  std::chrono::milliseconds stream_keepalive;
};
struct ChatLimits final {
  std::uint32_t max_body_bytes;
  std::uint32_t max_messages;
  std::uint32_t max_message_bytes;
  std::uint32_t max_prompt_tokens;
  std::uint32_t max_completion_tokens;
  std::uint32_t default_max_tokens;
  std::uint32_t max_generated_bytes;
};
struct EngineLimits final {
  std::chrono::seconds ready_timeout;
  std::chrono::seconds generation_timeout;
  std::chrono::seconds stop_drain_timeout;
};
// SP3 §5.1: an absent queue / per-remote key takes its value derived from the
// worker count (the *_derived flags record that for --check).
struct ConcurrencyLimits final {
  std::uint32_t max_concurrent_requests;
  std::uint32_t max_queued_generations;
  std::chrono::milliseconds queue_timeout;
  std::uint32_t max_connections_per_remote_address;
  bool queue_derived;
  bool remote_cap_derived;
};
[[nodiscard]] std::uint32_t derived_max_queued_generations(std::uint32_t workers) noexcept;
[[nodiscard]] std::uint32_t derived_max_connections_per_remote_address(std::uint32_t workers, bool web_ui) noexcept;
struct Limits final {
  HttpLimits http;
  ChatLimits chat;
  EngineLimits engine;
  ConcurrencyLimits concurrency;
};
// SP3 §8: logging.requests (one stderr line per request when true).
struct LoggingConfig final {
  bool requests;
};
struct Roots final {
  CanonicalRoot models;
  std::optional<CanonicalRoot> tokenizer_pack;  // --expert only (SP4 §4.9 d)
  std::optional<CanonicalRoot> secret;
  // Canonical paths that may not exist yet: validate() only checks them
  // (check_creatable_root); create_runtime_roots() creates them for serve/init.
  std::optional<std::filesystem::path> logs;
  std::optional<std::filesystem::path> cache;
  // roots.state: absolute only; validate() never touches the filesystem. serve
  // requires it and checks it is an existing owner-only directory (spec §3.9).
  std::optional<std::filesystem::path> state{};
  // Owner opt-in (roots.allow_untrusted_media): roots.models and roots.tokenizer_pack were opened with
  // RootPolicy::external (no owner/shared-writable check) for model data on exFAT/FAT media.
  bool allow_untrusted_media{false};
};
// SP5 M1 Task 5 (parity rows reasoning_*): the pin's config family. Kinds and defaults at
// frontend_config.cpp's rows; the pin keeps these as service-object values (serve/server.py:8015,
// :8102-8104, :8120-8136), except effort_position, which composes an engine argument (:2836-2856).
enum class ReasoningEffort { none, low, medium, high };      // clean_shared_defaults :7341-7343
enum class LoopRecovery { off, stop, recover };              // :8129-8136 (false | "stop" | "recover")
enum class EffortPosition { start, end };                    // :2841-2843
enum class AnthropicThinking { model, on_request };          // :8102-8104
// SP5 M1 Task 7 (parity rows gpu_order / gpu_policy). The pin reorders GPUs for any value
// but the string "as_given" (serve/server.py:3112); the typed row closes the vocabulary and
// names the reordering "auto" (Ruling T7-b: the reorder needs driver scores, owner-box only).
// GpuTurnPolicy is images.py's POLICIES tuple (images.py:45) re-scoped to the vision turn.
enum class GpuOrder { auto_order, as_given };
enum class GpuTurnPolicy { auto_policy, keep, shrink, unload };
struct ReasoningConfig final {
  std::optional<ReasoningEffort> effort;                     // unset: no config-level level (pin None)
  std::int64_t budget_tokens = 0;                            // 0 = no budget (pin default)
  bool close_retry = false;                                  // #1053 opt-in
  LoopRecovery loop_recovery = LoopRecovery::off;            // #728 opt-in
  EffortPosition effort_position = EffortPosition::start;
  AnthropicThinking anthropic_thinking = AnthropicThinking::model;
};

// SP5 M1 Task 9 (the session family; serve/server.py:8043-8110, :2999-3005, :8092-8100 +
// slot_save_dir :3469-3481, images.py:188). The four booleans are the pin's opt-in falses:
// their pin-side consumers (its OutputParser recovery, frontend._env_to_user, the Codex
// routes) have no counterpart in our stack, so they are stored typed for parity
// (config_only rows, Rulings T9-d/T9-e).
struct SessionPolicy final {
  std::string slot_save_path;                  // session.slot_save_path (absolute, T9-b; the /slots store route is not ours, T9-a)
  std::string expert_profile_save;             // session.expert_profile_save -> --expert-profile-save (:3002)
  std::int64_t expert_profile_save_every{10};  // minutes; 0 = at QUIT only (the pin's docstring, :2996)
  bool tool_call_recovery{false};              // session.tool_call_recovery (:8106, strict bool)
  bool env_to_user{false};                     // session.env_to_user (:8110, strict bool)
  bool codex_thread_titles{false};             // session.codex_thread_titles (:8043, `is True`)
  bool codex_compaction_cache{false};          // session.codex_compaction_cache (:8044, `is True`)
  std::int64_t keep_files{64};                 // session.keep_files (images.py:188; typed only, T9-e)
};

// SP5 M1 Task 10 (the C-4 narrowing, spec + README): N-child replica scheduling is not
// implemented — the pin's `replicas` dict and replica_* members are rejected at load by
// name. What stands is engine-INTERNAL parallelism: replicas.parallel decodes up to N
// requests together inside the one child (serve/server.py:2955-2975 renders --batch N).
struct ReplicasConfig final {
  std::int64_t parallel{1};  // replicas.parallel (1: one request at a time, the pin's default)
};

// SP5 M1 Task 11 (D-9 vision split): image INPUT is typed and plumbed (supervised encoder
// child, mmproj manifest artifact); image GENERATION stays out. Defaults at the pin:
// max_side/max_n take the images-block values (serve/images.py:102-103 `or 2048`/`or 4`,
// caps added here, C-1); need_mib overrides the pin's VRAM arithmetic (images.py:156; the
// gate itself is driver-bound, cf. T7-c); resident is the pin's eager encoder
// (server.py:2511/:2552); threads 0 = the pin's unset (truthy -> --threads, :2562).
// no_flash_attn rides the spec §5.1 family: not read by this pin, proven by the kit.
struct VisionConfig final {
  bool enabled{false};                       // vision.enabled: gates the whole family
  std::int64_t max_side{2048};               // vision.max_side (pre-decode image side cap, Task 12)
  std::int64_t max_n{4};                     // vision.max_n (images per request cap, Task 12)
  std::optional<std::int64_t> need_mib;      // vision.need_mib (unset: the pin computes it)
  bool no_flash_attn{false};                 // vision.no_flash_attn (added knob, spec §5.1; child flag)
  bool resident{true};                       // vision.resident (false: woken by the first image, Task 12)
  std::int64_t threads{0};                   // vision.threads (0: unset — the pin's truthy skip, :2562)
  std::int64_t ready_timeout_s{300};         // vision.ready_timeout_s (0: wait for ever — the pin's :291 rule)
  std::int64_t encode_timeout_s{300};        // vision.encode_timeout_s (VISION_ENCODE_S gate, :328; 0: for ever)
  std::optional<std::filesystem::path> mmproj;  // vision.mmproj: a card's encoder file (spec 2026-10-28)
  bool gpu{false};                           // vision.gpu: the encoder's --gpu
};

// SP5 M2 Task 3 (spec §D-2): the engine-side GLM knobs, kept in Maya's own
// family so parity is provable by spelling. Kinds, defaults and ranges are
// Maya v1.1.21's (citations per row in the parity table); the closed
// vocabularies and caps marked C-1 are ours. They are accepted while
// engine.profile is absent (a managed model's manifest decides the profile;
// the launch composition, Task 7, refuses them for a qwen manifest) and
// refused by name when engine.profile is explicitly "qwen".
struct GlmSettings final {
  std::int64_t thinking_budget{32768};              // glm.thinking_budget (server.py:4090 default 32768; cap added, C-1)
  std::int64_t max_context{32768};                  // glm.max_context (Maya's DEFAULT_CONTEXT, maya.py:3148; bound added, C-1)
  std::string kv{"fp16"};                           // glm.kv: the engine's --kv (fp16 default, int8, q8_0, q4_0 - glm_driver.cpp)
  std::int64_t ram_headroom_gb{16};                 // glm.ram_headroom_gb (SP6 §4.9 M2: 6-1024, default 16; SETTINGS.md:38; the engine reads glm_fast_path.cu:1110/:1510)
  std::optional<double> pcie_share;                 // glm.pcie_share: STRATA_GLM_PCIE_SHARE 0-1 (calibration, Maya's calibrate_glm.py; unset: the engine's)
  std::optional<std::int64_t> cpu_lane;             // glm.cpu_lane: STRATA_GLM_CPU_LANE 1-64 (calibration; unset: the physical cores)
  std::int64_t idle_unload_s{0};                    // glm.idle_unload_s (v1.1.21 serve/server.py:8298, default 0 = never at :3753; under glm53 it wins over engine.idle_unload_s, R-T4-b)
  std::optional<GlmReasoningEffort> reasoning_effort;  // glm.reasoning_effort (unset: the request's; validator server.py:3798)
  bool fit_max_tokens{false};                       // glm.fit_max_tokens (Maya reads `is True` — server.py:4077; unlike engine's true)
  bool experimental_speed_projection{false};        // glm.experimental_speed_projection (server.py:3819/3877/789: truthy or)
  std::vector<unsigned> gpus{};                     // glm.gpu (parse_gpu_list, same grammar as engine.gpu; :1264/:1280-1288)
  std::string layer_split{"auto"};                  // glm.layer_split (Maya default "auto"; :1274-1277; closed grammar added, C-1)
  std::optional<std::string> tokenizer;             // glm.tokenizer (absolute store path, added rule C-1; :3989)
  std::optional<std::string> model_name;            // glm.model_name (served model id, added bound C-1; :4075)
  bool rows_present{false};                         // any glm.* row was present in the config (R-T3-a: the launch
                                                    // refuses the rows when a manifest names the qwen profile)
};

struct EngineLaunch final {
  std::optional<EngineProfile> profile;  // --expert only; a managed model's manifest sets it (SP4 §4.9 c)
  CanonicalFile executable;
  ModelId model_id;
  std::vector<std::string> args;
  std::vector<std::filesystem::path> lib_dirs{};  // engine.lib_dirs, each absolute
  std::vector<unsigned> gpus{};                   // engine.gpu, parsed by parse_gpu_list (empty: unset)
  bool allow_new_privileges{false};               // engine.allow_new_privileges (Linux: no PR_SET_NO_NEW_PRIVS)
  bool fit_max_tokens{true};                      // engine.fit_max_tokens (pin :3485: false answers 400 over room)
  // SP5 M1 Task 6 (parity rows lazy_load / idle_unload_s / mtp; runconfig.py:35-38): the
  // lifecycle family. mtp is on unless explicitly false (draft_layer.py is_off).
  bool lazy_load{false};                          // engine.lazy_load: the first request loads the engine
  std::chrono::seconds idle_unload{0};            // engine.idle_unload_s (0: never, the pin default)
  bool mtp{true};                                 // engine.mtp: false strips the draft layer at every start
  // SP5 M1 Task 7 (the VRAM / GPU / elastic family). Kinds and defaults verbatim from the
  // pin (runconfig.py:42; serve/server.py:2550-2552, 2870-2948, 3023, 3112, 8083;
  // serve/images.py:23/45/173); the caps and closed vocabularies are added (C-1).
  std::optional<std::int64_t> vram_reserve_mib;   // engine.vram_reserve_mib -> --vram-reserve-mib (unset: engine's 700)
  std::int64_t min_free_vram{0};                  // engine.min_free_vram_mib (svc-side pre-load gate; :8083, default 0)
  bool vram_elastic{false};                       // engine.vram_elastic -> --vram-elastic (#533, :2945)
  std::optional<std::int64_t> vram_segment_mib;   // engine.vram_segment_mib -> --vram-segment-mib (unset: engine's 512)
  bool elastic{false};                            // engine.elastic: the elastic vision turn (Task 11 consumer, :2550)
  std::chrono::seconds elastic_idle{120};         // engine.elastic_idle_s (pin float-or 120; integer seconds, T7-h)
  std::int64_t elastic_mib{2048};                 // engine.elastic_mib (pin int-or 2048; /v1/vram headroom, Task 11)
  GpuOrder gpu_order{GpuOrder::auto_order};       // engine.gpu_order (only "as_given" changes anything; :3112)
  GpuTurnPolicy gpu_policy{GpuTurnPolicy::auto_policy};  // engine.gpu_policy (POLICIES, images.py:45; vision turn)
  std::string layer_split{"auto"};                // engine.layer_split -> --layer-split when several GPUs (:2870-2894)
  bool split_skip_if_fits{false};                 // engine.split_skip_if_fits -> --split-skip-if-fits (:2941)
  std::optional<std::int64_t> hip_ordinal;        // engine.hip_ordinal: wins over one (or zero) GPUs (:3023-3029)
  bool uma{false};                                // engine.uma: an APU shares RAM either way (images.py:23/334)
  // SP5 M1 Task 8 (parity rows engine_silence_s / answer_room / repeat_stop_tokens /
  // max_tokens / min_tokens + model.aliases). Kinds and defaults verbatim from the pin
  // (runconfig.py:34/:39; serve/server.py:197-199/:8014/:8115/:4908); added bounds marked C-1.
  std::chrono::seconds silence{300};              // engine.silence_s (default 300, 0 = wait; :274-276/:2861-2868)
  bool answer_room{true};                         // engine.answer_room (pin `is not False`, :8014)
  std::size_t repeat_stop_tokens{256};            // engine.repeat_stop_tokens (REPEAT_STOP_TOKENS 256, :197/:8115; 0: off)
  std::optional<std::size_t> max_tokens;          // engine.max_tokens: ceiling on the request output budget (added, C-1)
  std::optional<std::size_t> min_tokens;          // engine.min_tokens: floor on the request output budget (added, C-1)
  std::vector<std::string> aliases{};             // model.aliases (set_aliases :3764-3777)
  // model.name (session log step 13; the pin's model_name, default_model_name :7855): the name
  // the API serves the model as. Unset: the model's own id (inventory file, or name:tag).
  std::optional<std::string> served_name{};
};

// Model launcher (plan 2026-10-23 Task 1; spec §3.1, D1/D2/D5): the card files the page may
// start, the card started at boot, and who may start and stop them. Launcher mode is on when
// `cards` is non-empty (model_cards.hpp holds the cards themselves).
struct ModelsConfig final {
  std::vector<std::filesystem::path> cards{};  // models.cards: absolute card files
  std::optional<std::string> start{};          // models.start: unset = boot with no engine (D5)
  bool control{false};                         // models.control: the page and API may start/stop
  bool control_remote{false};                  // models.control_remote: ... also from non-loopback clients (D2)
  // models.control_remote_networks (owner 2026-10-27): with control_remote, only clients in these hosts or subnets
  // (model_control_allowed); empty = every client
  std::vector<IpNetwork> control_remote_networks{};
  // set from FrontendConfigDraft::card (model_cards.cpp card_draft), never from a file: this config is a model card's,
  // which may read a Qwen tokenizer from its GGUF (tokenizer_from_gguf); the operator's single-model --expert keeps its pack
  bool from_card{false};
  // Model downloads (plan 2026-10-23-model-downloads Task 4).
  bool downloads{false};                                    // models.downloads: the Models tab's downloads
  std::optional<std::filesystem::path> downloads_root{};    // models.downloads_root: the downloads' store
  std::optional<std::filesystem::path> prepare_python{};    // models.prepare_python: runs upstream's pack tools
  std::optional<std::filesystem::path> prepare_tools{};     // models.prepare_tools: upstream's tools folder
  std::optional<std::string> download_engine_card{};        // models.download_engine_card: a prepared card's engine
  // GUI-first start-up (spec 2026-10-27 §3-§4): the engine of the cards the page writes (and of a card-less boot), and
  // the folder the page offers first for a model.
  std::optional<std::filesystem::path> default_engine{};    // models.default_engine
  std::optional<std::filesystem::path> default_root{};      // models.default_root
};

// GUI-first start-up (spec 2026-10-27 §5a): email alerts (alerts.email.*), read only in a build with
// SECURE_FRONTEND_EMAIL_ALERTS. The password is not a row: it is kept in roots.secret like the API key.
enum class SmtpSecurity { ssl, starttls };  // implicit TLS (465) or STARTTLS (587); never plain text
inline constexpr std::array<std::string_view, 6> kAlertEvents{"download_finished",    "download_failed", "calibration_finished",
                                                              "calibration_failed",   "engine_stopped",  "server_started"};
struct EmailAlertsConfig final {
  bool enabled{false};
  std::string host;                  // the SMTP server (a DNS name)
  std::uint16_t port{465};
  SmtpSecurity security{SmtpSecurity::ssl};
  std::string username;              // IONOS: the full mailbox address
  std::string from;                  // IONOS: that mailbox
  std::vector<std::string> to;       // 1-5 addresses
  std::vector<std::string> events;   // which kAlertEvents are sent (all when the row is absent)
};
struct AlertsConfig final {
  EmailAlertsConfig email{};
};
// An address a header can carry: printable ASCII without spaces or <>()[],;:\", one @, a dotted domain, <= 254.
[[nodiscard]] bool is_valid_email_address(std::string_view text) noexcept;
// The alerts.email.* rows of a draft alone, by validate()'s own rules (the page's settings, before they are written).
[[nodiscard]] std::variant<AlertsConfig, ConfigErrors> alerts_from_draft(const FrontendConfigDraft& draft);
// Whether this build has email alerts (SECURE_FRONTEND_EMAIL_ALERTS).
[[nodiscard]] constexpr bool email_alerts_built() noexcept {
#if defined(SECURE_FRONTEND_EMAIL_ALERTS)
  return true;
#else
  return false;
#endif
}

// Web search (docs/superpowers/specs/2026-10-27-web-search-design.md §2): search.*, read only in a build with
// SECURE_FRONTEND_WEB_SEARCH (any other build refuses the rows by name).
struct SearchConfig final {
  bool enabled{false};
  std::string url;                   // the SearXNG instance (parse_search_url)
  std::size_t results{5};            // results per search, 1-10
  std::size_t max_rounds{8};         // searches one answer may make (upstream's mcp.max_rounds), 1-8
  std::chrono::seconds timeout{30};  // each network step of a search, 5-120 s
};
// The search.* rows of a draft alone, by validate()'s own rules (the page's settings, before they are written).
[[nodiscard]] std::variant<SearchConfig, ConfigErrors> search_from_draft(const FrontendConfigDraft& draft);
// Whether this build has web search (SECURE_FRONTEND_WEB_SEARCH).
[[nodiscard]] constexpr bool web_search_built() noexcept {
#if defined(SECURE_FRONTEND_WEB_SEARCH)
  return true;
#else
  return false;
#endif
}

// SP4 §3.9. pull.downloader is only a syntactically absolute path here; the
// file rules run lazily when a transferring verb or doctor --downloader runs
// (a recorded CFG-01 exception: a box without curl can still serve and import).
struct PullSettings final {
  std::filesystem::path downloader;
  std::optional<ProxySetting> proxy;
  bool allow_private_addresses;
  std::uint64_t max_total_bytes;
  std::chrono::seconds max_duration;
  std::chrono::seconds stall_timeout;
  std::chrono::seconds connect_timeout;
  std::uint32_t connections;  // parallel ranged downloads; 1 = one stream
};

// SP7 M2 Task 3 (spec D-10): release mirror URLs for `strata update`. The list
// is untrusted (trust comes from the pinned release key, D-2); each entry is a
// plain HTTPS source URL.
struct UpdateSettings final {
  std::vector<std::string> source;
};

// Linux /usr/bin/curl; Windows GetSystemDirectoryW() + "\curl.exe". Empty (so
// not absolute, and refused when a transfer runs) if Windows cannot report its
// system directory.
[[nodiscard]] std::filesystem::path default_downloader_path();

class ValidatedConfig;
class FrontendConfig final {
 public:
  const ListenerConfig listener;
  const Limits limits;
  const Roots roots;
  const SecretSource token;  // metadata only: env name or relative file path (CFG-06)
  const EngineLaunch engine;
  const PullSettings pull;
  const UpdateSettings update;  // SP7 M2 Task 3 (D-10): update.source mirrors
  const LoggingConfig logging;  // SP3 §8
  const SamplingDefaults sampling;  // SP5 M1 Task 4: defaults.sampling.*
  const ReasoningConfig reasoning;  // SP5 M1 Task 5: reasoning.*
  const SessionPolicy session;      // SP5 M1 Task 9: session.*
  const ReplicasConfig replicas;    // SP5 M1 Task 10: replicas.parallel (the C-4 narrowing)
  const VisionConfig vision;        // SP5 M1 Task 11: vision.* (the D-9 split)
  const GlmSettings glm;            // SP5 M2 Task 3: glm.* (Maya's names, §D-2)
  const ModelsConfig models;        // model launcher: models.* (plan 2026-10-23 Task 1)
  const AlertsConfig alerts;        // GUI-first start-up: alerts.email.* (spec 2026-10-27 §5a)
  const SearchConfig search;        // web search: search.* (spec 2026-10-27-web-search-design §2)

 private:
  friend std::variant<ValidatedConfig, std::vector<ConfigIssue>> validate(const FrontendConfigDraft& draft);
  FrontendConfig(ListenerConfig listener_value, Limits limits_value, Roots roots_value, SecretSource token_value,
                 EngineLaunch engine_value, PullSettings pull_value, LoggingConfig logging_value,
                 SamplingDefaults sampling_value, ReasoningConfig reasoning_value, SessionPolicy session_value,
                 ReplicasConfig replicas_value, VisionConfig vision_value, GlmSettings glm_value,
                 UpdateSettings update_value = {}, ModelsConfig models_value = {}, AlertsConfig alerts_value = {},
                 SearchConfig search_value = {})
      : listener(std::move(listener_value)), limits(limits_value), roots(std::move(roots_value)),
        token(std::move(token_value)), engine(std::move(engine_value)), pull(std::move(pull_value)),
        update(std::move(update_value)),
        logging(logging_value), sampling(std::move(sampling_value)), reasoning(reasoning_value),
        session(std::move(session_value)), replicas(replicas_value), vision(vision_value), glm(std::move(glm_value)),
        models(std::move(models_value)), alerts(std::move(alerts_value)), search(std::move(search_value)) {}
};

// Immutable after validation; subsystems derive their limits from it once.
class ValidatedConfig final {
 public:
  [[nodiscard]] const FrontendConfig& get() const noexcept { return *config_; }
  [[nodiscard]] std::shared_ptr<const FrontendConfig> shared() const noexcept { return config_; }

 private:
  friend std::variant<ValidatedConfig, std::vector<ConfigIssue>> validate(const FrontendConfigDraft& draft);
  explicit ValidatedConfig(std::shared_ptr<const FrontendConfig> config) : config_(std::move(config)) {}
  std::shared_ptr<const FrontendConfig> config_;
};

using ConfigValidationResult = std::variant<ValidatedConfig, ConfigErrors>;

// Collects every issue (all keys, all rules) before returning.
[[nodiscard]] ConfigValidationResult validate(const FrontendConfigDraft& draft);
// "key = value" per schema key in schema order. Holds no secret by construction.
[[nodiscard]] std::string describe(const FrontendConfig& config);
// The listener primitive HttpSocketServer::bind takes.
[[nodiscard]] Config listener_config(const FrontendConfig& config);

// SP5 M2 Task 4 (D-7): the typed glm.* config projected onto the glm family's
// closed typed child environment. Today the split row rides glm.layer_split
// (the config's layer_split sets the same knob, maya-ref docs/SETTINGS.md:44);
// the launch composition (Task 7) sets family and glm_gpus, and Task 8 adds
// the vision rows. Names are checked against the closed allowlist by the
// merge step; a name's typed row moves that §4.9 forced default.
[[nodiscard]] std::vector<ForcedEnvironmentValue> glm_environment_knobs(const FrontendConfig& config);
// The only filesystem side effect of a configuration: creates a missing
// roots.logs / roots.cache (0700 / private DACL) and checks an existing one.
// Called by serve and init, never by validate() or --check.
[[nodiscard]] ConfigErrors create_runtime_roots(const FrontendConfig& config);

}  // namespace secure_frontend
