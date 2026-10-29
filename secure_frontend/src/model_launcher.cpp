#include "secure_frontend/model_launcher.hpp"

#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/card_builder.hpp"
#include "secure_frontend/config_file.hpp"
#include "secure_frontend/engine_stdio_process.hpp"
#include "secure_frontend/host_readings.hpp"
#include "secure_frontend/launch_composition.hpp"
#include "secure_frontend/minimax_prompt_renderer.hpp"
#include "secure_frontend/subsystem_limits.hpp"
#include "secure_frontend/token_store.hpp"
#include "secure_frontend/tokenizer_pack.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <utility>
#include <variant>

namespace secure_frontend {
namespace {

// The fixed reasons a card's state may carry (the page shows them; the log has the detail).
constexpr std::string_view kStateDirFailed = "the state directory could not be prepared";
constexpr std::string_view kSupervisorFailed = "the engine supervisor could not start";
constexpr std::string_view kEndedBeforeReady = "the engine ended before it was ready (see the server log)";
constexpr std::string_view kVisionFailed = "the picture encoder did not start (see the server log)";
constexpr std::string_view kExhausted = "the engine stopped after repeated failures (see the server log)";
constexpr std::string_view kCalibrationFailed = "the engine did not start for the calibration (see the server log)";
constexpr std::string_view kCalibrationNotSaved = "the calibration could not be saved (see the server log)";

// GUI-first start-up (spec 2026-10-27 §5.1): the workload each candidate is measured on, upstream's shape - a warm-up,
// then three fixed prompts through the card's own template, thinking off (where it can be), greedy, 128 new tokens each.
constexpr std::array<std::string_view, 4> kCalibrationPrompts{
    "Say hello.",
    "Explain how a refrigerator keeps food cold, step by step.",
    "Write a short story about a lighthouse keeper who finds a message in a bottle.",
    "List ten practical tips for learning a new programming language, one sentence each."};

// Thinking off where the profile takes reasoning_effort per request; GLM-5.3 takes none (its prompt always opens its
// think block) and refuses it as unsupported_parameter (owner 2026-10-29: every GLM-5.3 calibration failed so).
std::string calibration_body(const EngineProfile profile, const std::string_view model, const std::string_view prompt,
                             const unsigned max_tokens) {
  std::string body = "{\"model\":";
  append_json_string(&body, model);
  body += ",\"messages\":[{\"role\":\"user\",\"content\":";
  append_json_string(&body, prompt);
  body += "}],\"max_tokens\":" + std::to_string(max_tokens) + ",\"temperature\":0,";
  if (per_request_reasoning_effort_for(profile)) body += "\"reasoning_effort\":\"none\",";
  body += "\"stream\":false}";
  return body;
}

// A card name may hold ':' (name:tag); its calibration file does not.
std::string calibration_file_name(const std::string_view card) {
  std::string name{card};
  std::replace(name.begin(), name.end(), ':', '_');
  return name + ".json";
}

// MiniMax's prefill record beside it (<card>.prefill.json).
std::string prefill_file_name(const std::string_view card) {
  std::string name{card};
  std::replace(name.begin(), name.end(), ':', '_');
  return name + ".prefill.json";
}

std::string_view calibration_state_name(const CalibrationState state) noexcept {
  switch (state) {
    case CalibrationState::missing: return "missing";
    case CalibrationState::fresh: return "fresh";
    case CalibrationState::stale: return "stale";
    case CalibrationState::settings_changed: return "settings_changed";
  }
  return "missing";
}

std::string decimal_text(const double value) {
  char buffer[32];
  const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), std::round(value * 100.0) / 100.0);
  return error == std::errc{} ? std::string{buffer, end} : std::string{"0"};
}

// A chat body already passed the chat route's own reader; this one only finds its "model".
constexpr JsonLimits kModelFieldLimits{.max_bytes = 16U * 1024U * 1024U, .max_depth = 64U, .max_container_entries = 1U << 20U,
                                       .max_string_bytes = 16U * 1024U * 1024U, .allow_fractions = true};

std::string value_text(const ModelOptionValue& value) {
  if (const auto* b = std::get_if<bool>(&value)) return *b ? "true" : "false";
  if (const auto* i = std::get_if<std::int64_t>(&value)) return std::to_string(*i);
  if (const auto* d = std::get_if<double>(&value)) {
    char buffer[32];
    const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), *d);
    return error == std::errc{} ? std::string{buffer, end} : std::string{"?"};
  }
  return std::get<std::string>(value);
}

std::string json_string(const std::string_view text) {
  std::string out;
  append_json_string(&out, text);
  return out;
}

// The option an issue names ("options.kv" -> "kv"), when it is an option name.
std::string option_of(const ConfigErrors& errors) {
  constexpr std::string_view kPrefix = "options.";
  for (const auto& issue : errors) {
    if (issue.key_path.rfind(kPrefix, 0) != 0) continue;
    std::string name = issue.key_path.substr(kPrefix.size());
    if (const auto dot = name.find('.'); dot != std::string::npos) name.resize(dot);
    if (is_model_option_name(name)) return name;
  }
  return {};
}

// Owner 2026-10-28 ("a toggle in the card that can enable search"): with the server's web search, a card whose
// profile has the tool offers the Web search checkbox, on unless its card file lists it otherwise. The option is the
// tables' last, so it is appended in display order.
void offer_web_search(ModelCard& card, const ToolRunner* tools) {
  const auto* spec = tools == nullptr ? nullptr : find_model_option(card.profile, "web_search");
  if (spec == nullptr) return;
  if (std::any_of(card.options.begin(), card.options.end(), [spec](const ModelOptionLimits& limit) { return limit.spec == spec; })) return;
  ModelOptionLimits limit;
  limit.spec = spec;
  limit.default_value = true;
  card.options.push_back(std::move(limit));
}

// Multi-GPU like upstream (owner 2026-10-28): every card offers its GPUs choice, auto unless its card file lists it
// otherwise (the page shows it where this PC has more than one GPU). The option is the tables' last: appended after Web
// search, in display order.
void offer_gpus(ModelCard& card) {
  const auto* spec = find_model_option(card.profile, "gpus");
  if (spec == nullptr) return;
  if (std::any_of(card.options.begin(), card.options.end(), [spec](const ModelOptionLimits& limit) { return limit.spec == spec; })) return;
  ModelOptionLimits limit;
  limit.spec = spec;
  limit.default_value = std::string{"auto"};
  limit.minimum = spec->minimum;
  limit.maximum = spec->maximum;
  card.options.push_back(std::move(limit));
}

// Owner 2026-10-29 ("make sure the card matches the chat settings when the engine is started as default so each model
// reset it to the recommended"): a sampling option the card's file does not list is offered with its model's recommended
// value (recommended_sampling), or the card's own defaults.sampling row where its config has one, in the profile's
// order. A card made before cards carried sampling (a Qwen3.8 pack's) then samples as its model's card says, not greedy.
void offer_sampling(ModelCard& card) {
  const auto order = model_options_for(card.profile);
  const auto place = [&order](const ModelOptionSpec* spec) { return static_cast<std::size_t>(spec - order.data()); };
  for (const auto& [name, recommended] : recommended_sampling(card.profile)) {
    const auto* spec = find_model_option(card.profile, name);
    if (spec == nullptr ||
        std::any_of(card.options.begin(), card.options.end(), [spec](const ModelOptionLimits& limit) { return limit.spec == spec; })) {
      continue;
    }
    ModelOptionLimits limit;
    limit.spec = spec;
    limit.default_value = recommended;
    limit.minimum = spec->minimum;
    limit.maximum = spec->maximum;
    limit.minimum_decimal = spec->minimum_decimal;
    limit.maximum_decimal = spec->maximum_decimal;
    if (const auto row = card.config.entries.find("defaults.sampling." + name); row != card.config.entries.end()) {
      if (const auto* decimal = std::get_if<double>(&row->second.value)) {
        limit.default_value = *decimal;
      } else if (const auto* integer = std::get_if<std::int64_t>(&row->second.value)) {
        limit.default_value = spec->kind == ModelOptionKind::decimal ? ModelOptionValue{static_cast<double>(*integer)} : ModelOptionValue{*integer};
      }
    }
    const auto later = std::find_if(card.options.begin(), card.options.end(), [&](const ModelOptionLimits& listed) {
      return listed.spec != nullptr && place(listed.spec) > place(spec);
    });
    card.options.insert(later, std::move(limit));
  }
}

// Owner 2026-10-29 ("make it an option in the card setting"): a GLM-engine card (glm53, raon1) whose file does not list
// it offers the RAM its engine leaves free, at the server's glm.ram_headroom_gb (a card's file holds no glm.* row), else
// the row's default (16), in the profile's order. Started without a choice, it runs as before.
void offer_ram_headroom(ModelCard& card, const FrontendConfigDraft& main) {
  const auto* spec = find_model_option(card.profile, "ram_headroom");
  if (spec == nullptr) return;
  if (std::any_of(card.options.begin(), card.options.end(), [spec](const ModelOptionLimits& limit) { return limit.spec == spec; })) return;
  constexpr std::string_view kRow = "glm.ram_headroom_gb";
  const auto schema = config_schema();
  const auto key = std::find_if(schema.begin(), schema.end(), [kRow](const ConfigKey& row) { return row.path == kRow; });
  std::int64_t value = key == schema.end() ? spec->minimum : key->default_integer;
  if (const auto row = main.entries.find(kRow); row != main.entries.end()) {
    if (const auto* integer = std::get_if<std::int64_t>(&row->second.value)) value = *integer;
  }
  ModelOptionLimits limit;
  limit.spec = spec;
  limit.default_value = value;
  limit.minimum = spec->minimum;
  limit.maximum = spec->maximum;
  const auto order = model_options_for(card.profile);
  const auto place = [&order](const ModelOptionSpec* listed) { return static_cast<std::size_t>(listed - order.data()); };
  const auto later = std::find_if(card.options.begin(), card.options.end(), [&](const ModelOptionLimits& listed) {
    return listed.spec != nullptr && place(listed.spec) > place(spec);
  });
  card.options.insert(later, std::move(limit));
}

// The options a card offers beyond its file's: its engine's RAM headroom, its model's recommended sampling, Web search
// with the server's, then its GPUs.
void offer_options(ModelCard& card, const FrontendConfigDraft& main, const ToolRunner* tools) {
  offer_ram_headroom(card, main);
  offer_sampling(card);
  offer_web_search(card, tools);
  offer_gpus(card);
}

// The card's options as the page sees them: without the server's web search, no Web search checkbox.
std::vector<ModelOptionLimits> shown_options(const ModelCard& card, const ToolRunner* tools) {
  std::vector<ModelOptionLimits> shown;
  for (const auto& limit : card.options) {
    if (tools != nullptr || limit.spec == nullptr || limit.spec->name != "web_search") shown.push_back(limit);
  }
  return shown;
}

}  // namespace

std::string_view card_state_name(const CardState state) noexcept {
  switch (state) {
    case CardState::stopped: return "stopped";
    case CardState::starting: return "starting";
    case CardState::running: return "running";
    case CardState::failed: return "failed";
    case CardState::calibrating: return "calibrating";
  }
  return "stopped";  // unreachable: every value is a case above
}

ModelLauncher::ModelLauncher(LauncherSettings settings) : settings_(std::move(settings)) {
  // add_card never reallocates: a start holds its card by reference outside the lock.
  if (settings_.cards.size() > kMaxLauncherCards) settings_.cards.resize(kMaxLauncherCards);
  settings_.cards.reserve(kMaxLauncherCards);
  for (auto& card : settings_.cards) offer_options(card, settings_.main, settings_.tools);
  status_.resize(settings_.cards.size());
  status_.reserve(kMaxLauncherCards);
  for (std::size_t i = 0; i < settings_.cards.size(); ++i) page_card_.push_back(i >= settings_.operator_cards);
  if (!settings_.factory) {
    settings_.factory = [](EngineLaunchPlan plan) { return make_stdio_process_factory(std::move(plan)); };
  }
  if (settings_.memory_file.has_value()) {
    auto text = read_trusted_config_text(*settings_.memory_file);
    if (const auto* read = std::get_if<TrustedConfigText>(&text)) remembered_ = parse_remembered_options(read->text);
  }
  if (settings_.calibration_dir.has_value()) {
    cpu_fact_ = cpu_fact();
    machine_ = SweepMachine{std::max(1U, std::thread::hardware_concurrency()), physical_cores()};
  }
  worker_ = std::thread([this] { run_jobs(); });
}

ModelLauncher::~ModelLauncher() { shutdown(); }

bool ModelLauncher::add_card(ModelCard card) {
  std::string name = card.name;
  {
    std::lock_guard lock(mutex_);
    if (shutting_down_ || find_card(card.name).has_value() || settings_.cards.size() >= kMaxLauncherCards) return false;
    offer_options(card, settings_.main, settings_.tools);
    settings_.cards.push_back(std::move(card));
    status_.emplace_back();
    page_card_.push_back(true);
  }
  log("[strata] models: added " + name);
  return true;
}

CardChange ModelLauncher::replace_card(ModelCard card) {
  const std::string name = card.name;
  {
    std::lock_guard lock(mutex_);
    if (shutting_down_) return CardChange::shutting_down;
    const auto found = find_card(card.name);
    if (!found.has_value()) return CardChange::not_found;
    if (!page_card_[*found]) return CardChange::not_page_card;
    if (busy_ || pending_.has_value()) return CardChange::busy;
    offer_options(card, settings_.main, settings_.tools);
    settings_.cards[*found] = std::move(card);
  }
  log("[strata] models: replaced " + name);
  return CardChange::done;
}

CardChange ModelLauncher::remove_card(const std::string_view name, const RunningCard running) {
  bool queued = false;
  {
    std::lock_guard lock(mutex_);
    if (shutting_down_) return CardChange::shutting_down;
    const auto found = find_card(name);
    if (!found.has_value()) return CardChange::not_found;
    if (!page_card_[*found]) return CardChange::not_page_card;
    if (busy_ || pending_.has_value()) return CardChange::busy;
    if (running_card_ == found) {
      if (running == RunningCard::refuse) return CardChange::running;
      // owner 2026-10-28: its model is stopped as stop() stops it; the worker removes the card after
      busy_ = true;
      switching_ = true;
      Job job;
      job.kind = Job::Kind::remove;
      job.card = *found;
      job.name = std::string{name};
      pending_ = std::move(job);
      queued = true;
    } else {
      erase_card(*found);
    }
  }
  if (queued) {
    log("[strata] models: stop, then remove the card " + std::string{name});
    changed_.notify_all();
    return CardChange::stopping;
  }
  forget_card(std::string{name});
  return CardChange::done;
}

// No start holds a card (none runs, or a remove stopped it), so the list may shift; the running card's index follows it.
void ModelLauncher::erase_card(const std::size_t index) {
  const std::string name = settings_.cards[index].name;
  const auto at = static_cast<std::ptrdiff_t>(index);
  settings_.cards.erase(settings_.cards.begin() + at);
  status_.erase(status_.begin() + at);
  page_card_.erase(page_card_.begin() + at);
  if (running_card_.has_value() && *running_card_ > index) --*running_card_;
  remembered_.erase(name);
  // its calibration goes with it (spec 2026-10-27 §4.5): a later card of that name starts uncalibrated
  auto_recalibrate_.erase(name);
  calibration_errors_.erase(name);
}

void ModelLauncher::forget_card(const std::string& name) {
  write_remembered();
  if (settings_.calibration_dir.has_value()) {
    std::error_code ec;
    std::filesystem::remove(*settings_.calibration_dir / path_from_utf8(calibration_file_name(name)), ec);
    std::filesystem::remove(*settings_.calibration_dir / path_from_utf8(prefill_file_name(name)), ec);
  }
  log("[strata] models: removed " + name);
}

std::optional<ModelLauncher::HeldCard> ModelLauncher::card(const std::string_view name) const {
  std::lock_guard lock(mutex_);
  const auto found = find_card(name);
  if (!found.has_value()) return std::nullopt;
  return HeldCard{settings_.cards[*found], page_card_[*found], status_[*found].state};
}

std::vector<ModelCard> ModelLauncher::cards() const {
  std::lock_guard lock(mutex_);
  return settings_.cards;
}

void ModelLauncher::alert(const std::string_view event, const std::string& subject, const std::string& body) const {
  if (settings_.on_event) settings_.on_event(event, subject, body);
}

void ModelLauncher::log(const std::string_view line) const {
  if (settings_.log) {
    settings_.log(line);
    return;
  }
  std::cerr << line << '\n' << std::flush;
}

std::optional<std::size_t> ModelLauncher::find_card(const std::string_view name) const {
  for (std::size_t i = 0; i < settings_.cards.size(); ++i) {
    if (settings_.cards[i].name == name) return i;
  }
  return std::nullopt;
}

std::optional<ModelLauncher::Refusal> ModelLauncher::start(const std::string_view name, const JsonValue* options,
                                                           const bool remote_client) {
  std::optional<std::size_t> card;
  const ModelCard* chosen_card = nullptr;
  {
    std::lock_guard lock(mutex_);
    card = find_card(name);
    if (shutting_down_) return Refusal{ApiError::server_shutting_down};
    if (busy_ || pending_.has_value()) return Refusal{ApiError::switch_in_progress};
    if (!card.has_value()) return Refusal{ApiError::model_not_found};
    busy_ = true;  // reserved: one start or stop at a time
    chosen_card = &settings_.cards[*card];
  }
  const auto release = [this] {
    std::lock_guard lock(mutex_);
    busy_ = false;
    changed_.notify_all();
  };
  const ModelCard& chosen = *chosen_card;
  ModelOptionValues remembered;
  {
    std::lock_guard lock(mutex_);
    if (const auto found = remembered_.find(chosen.name); found != remembered_.end()) remembered = found->second;
  }
  auto values = resolve_option_values(chosen.options, options, remembered);
  if (const auto* issues = std::get_if<ConfigErrors>(&values)) {
    release();
    return Refusal{ApiError::invalid_option, option_of(*issues)};
  }
  auto validated = card_config(settings_.main, chosen, std::get<ModelOptionValues>(values));
  if (const auto* issues = std::get_if<ConfigErrors>(&validated)) {
    for (const auto& issue : *issues) log("[strata] models: " + chosen.name + ": " + format_issue(issue));
    release();
    return Refusal{ApiError::invalid_request};
  }
  auto config = std::get<ValidatedConfig>(validated).shared();
  if (const auto issues = launch_config_issues(*config, true); !issues.empty()) {
    for (const auto& issue : issues) log("[strata] models: " + chosen.name + ": " + issue);
    release();
    return Refusal{ApiError::invalid_request};
  }
  // GUI-first start-up (spec 2026-10-27 §5.2): a fresh calibration's flags join the start; a card the page made that
  // was never calibrated, or one whose options changed with "recalibrate when settings change" on, calibrates first.
  bool calibrate_first = false;
  if (calibration_supported(chosen)) {
    const auto stored = read_calibration(chosen.name);
    const auto& start_values = std::get<ModelOptionValues>(values);
    const auto calibration = stored ? compare_key(stored->key, calibration_key(chosen, start_values)) : CalibrationState::missing;
    bool page = false;
    {
      std::lock_guard lock(mutex_);
      page = page_card_[*card];
    }
    if (calibration == CalibrationState::fresh && !stored->args.empty()) {
      if (auto with = config_with(chosen, start_values, stored->args)) config = std::move(with);
    }
    // (only the engines whose sweep is a few short starts calibrate unasked; the others' run from Calibrate)
    calibrate_first = (calibration == CalibrationState::missing && page && calibrates_before_first_start(chosen.profile)) ||
                      (calibration == CalibrationState::settings_changed && stored->auto_recalibrate);
  }
  // MiniMax: its prefill calibration once, before the first start on this machine (Maya's needs_prefill_calibration)
  const bool prefill_first = !calibrate_first && needs_prefill_calibration(chosen, std::get<ModelOptionValues>(values));
  std::string line = std::string{"[strata] models: "} +
                     (calibrate_first ? "calibrate, then start " : prefill_first ? "measure how this PC reads prompts, then start " : "start ") +
                     chosen.name;
  for (const auto& [option, value] : std::get<ModelOptionValues>(values)) line += " " + option + "=" + value_text(value);
  line += remote_client ? " (remote client)" : " (loopback client)";
  log(line);
  {
    std::lock_guard lock(mutex_);
    status_[*card] = CardStatus{calibrate_first || prefill_first ? CardState::calibrating : CardState::starting, {}};
    starting_card_ = *card;
    if (calibrate_first || prefill_first) cancel_calibration_.store(false);
    pending_ = Job{calibrate_first ? Job::Kind::calibrate : Job::Kind::start, *card, std::move(std::get<ModelOptionValues>(values)),
                   std::move(config), true, std::nullopt, prefill_first};
  }
  changed_.notify_all();
  return std::nullopt;
}

std::optional<ModelLauncher::Refusal> ModelLauncher::stop(const bool remote_client) {
  {
    std::lock_guard lock(mutex_);
    if (shutting_down_) return Refusal{ApiError::server_shutting_down};
    if (busy_ || pending_.has_value()) return Refusal{ApiError::switch_in_progress};
    if (running_ == nullptr) return std::nullopt;  // nothing runs: already stopped
    busy_ = true;
    switching_ = true;
    pending_ = Job{Job::Kind::stop, running_card_.value_or(0U), {}, {}};
  }
  log(std::string{"[strata] models: stop"} + (remote_client ? " (remote client)" : " (loopback client)"));
  changed_.notify_all();
  return std::nullopt;
}

std::optional<ModelLauncher::Refusal> ModelLauncher::calibrate(const std::string_view name, const JsonValue* options,
                                                               const bool then_start, const std::optional<bool> auto_recalibrate,
                                                               const bool remote_client) {
  std::optional<std::size_t> card;
  const ModelCard* chosen_card = nullptr;
  {
    std::lock_guard lock(mutex_);
    card = find_card(name);
    if (shutting_down_) return Refusal{ApiError::server_shutting_down};
    if (busy_ || pending_.has_value()) return Refusal{ApiError::switch_in_progress};
    if (!card.has_value()) return Refusal{ApiError::model_not_found};
    busy_ = true;
    chosen_card = &settings_.cards[*card];
  }
  const auto release = [this] {
    std::lock_guard lock(mutex_);
    busy_ = false;
    changed_.notify_all();
  };
  const ModelCard& chosen = *chosen_card;
  if (!calibration_supported(chosen)) {
    release();
    return Refusal{ApiError::calibration_unsupported};
  }
  ModelOptionValues remembered;
  {
    std::lock_guard lock(mutex_);
    if (const auto found = remembered_.find(chosen.name); found != remembered_.end()) remembered = found->second;
  }
  auto values = resolve_option_values(chosen.options, options, remembered);
  if (const auto* issues = std::get_if<ConfigErrors>(&values)) {
    release();
    return Refusal{ApiError::invalid_option, option_of(*issues)};
  }
  auto config = config_with(chosen, std::get<ModelOptionValues>(values), {});
  if (config == nullptr) {
    release();
    return Refusal{ApiError::invalid_request};
  }
  log("[strata] models: calibrate " + chosen.name + (then_start ? ", then start" : "") +
      (remote_client ? " (remote client)" : " (loopback client)"));
  {
    std::lock_guard lock(mutex_);
    status_[*card] = CardStatus{CardState::calibrating, {}};
    starting_card_ = *card;
    cancel_calibration_.store(false);
    pending_ = Job{Job::Kind::calibrate, *card, std::move(std::get<ModelOptionValues>(values)), std::move(config), then_start,
                   auto_recalibrate};
  }
  changed_.notify_all();
  return std::nullopt;
}

std::optional<ModelLauncher::Refusal> ModelLauncher::cancel_calibration() {
  {
    std::lock_guard lock(mutex_);
    const bool queued = pending_.has_value() && pending_->kind == Job::Kind::calibrate;
    if (!calibrating_.has_value() && !queued) return Refusal{ApiError::not_calibrating};
    cancel_calibration_.store(true);
  }
  log("[strata] models: calibration cancelled");
  changed_.notify_all();
  return std::nullopt;
}

bool ModelLauncher::calibration_supported(const ModelCard& card) const {
  if (!settings_.calibration_dir.has_value() || sweep_plan(card.profile, machine_).empty()) return false;
  const auto text = [&card](const std::string_view key) {
    const auto found = card.config.entries.find(std::string{key});
    return found != card.config.entries.end() && std::holds_alternative<std::string>(found->second.value);
  };
  return text("roots.models") && text("engine.model_id") && text("engine.executable");
}

std::shared_ptr<const FrontendConfig> ModelLauncher::config_with(const ModelCard& card, const ModelOptionValues& values,
                                                                 const std::vector<std::string>& args) const {
  FrontendConfigDraft draft = card_draft(settings_.main, card, values);
  // the flags join engine.args and the rows (the GLM engine's environment) the draft; the card's own stay
  if (!args.empty() && !apply_calibration(draft, args)) return nullptr;
  auto validated = validate(draft);
  if (const auto* issues = std::get_if<ConfigErrors>(&validated)) {
    for (const auto& issue : *issues) log("[strata] models: " + card.name + ": " + format_issue(issue));
    return nullptr;
  }
  auto config = std::get<ValidatedConfig>(validated).shared();
  if (const auto issues = launch_config_issues(*config, true); !issues.empty()) {
    for (const auto& issue : issues) log("[strata] models: " + card.name + ": " + issue);
    return nullptr;
  }
  return config;
}

CalibrationKey ModelLauncher::calibration_key(const ModelCard& card, const ModelOptionValues& values) const {
  const auto text = [&card](const std::string_view key) -> std::string {
    const auto found = card.config.entries.find(std::string{key});
    const auto* value = found == card.config.entries.end() ? nullptr : std::get_if<std::string>(&found->second.value);
    return value == nullptr ? std::string{} : *value;
  };
  CalibrationKey key;
  key.gpu = settings_.gpu_fact ? settings_.gpu_fact() : std::string{};
  key.cpu = cpu_fact_;
  if (const auto memory = read_memory()) key.ram_bytes = memory->total_bytes;
  key.engine = file_fact(path_from_utf8(text("engine.executable")));
  key.model = file_fact(path_from_utf8(text("roots.models")) / path_from_utf8(text("engine.model_id") + ".gguf"));
  key.options = options_fact(values);
  return key;
}

std::optional<CalibrationResult> ModelLauncher::read_calibration(const std::string_view card) const {
  if (!settings_.calibration_dir.has_value()) return std::nullopt;
  std::ifstream in(*settings_.calibration_dir / path_from_utf8(calibration_file_name(card)), std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream text;
  text << in.rdbuf();
  auto result = parse_calibration(text.str());
  if (!result || result->card != card) return std::nullopt;
  return result;
}

bool ModelLauncher::write_calibration(const CalibrationResult& result) {
  auto root = open_root(*settings_.calibration_dir, RootPolicy::create_private);
  const auto file = parse_relative_path(calibration_file_name(result.card));
  return std::holds_alternative<CanonicalRoot>(root) && std::holds_alternative<RelativePath>(file) &&
         !write_private_file(std::get<CanonicalRoot>(root), std::get<RelativePath>(file), render_calibration(result),
                             TokenStoreMode::replace)
              .has_value();
}

bool ModelLauncher::needs_prefill_calibration(const ModelCard& card, const ModelOptionValues& values) const {
  if (card.profile != EngineProfile::minimax || !settings_.calibration_dir.has_value()) return false;
  const auto stored = read_prefill(card.name);
  return !stored || compare_key(stored->key, calibration_key(card, values)) == CalibrationState::stale;
}

std::optional<PrefillRecord> ModelLauncher::read_prefill(const std::string_view card) const {
  if (!settings_.calibration_dir.has_value()) return std::nullopt;
  std::ifstream in(*settings_.calibration_dir / path_from_utf8(prefill_file_name(card)), std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream text;
  text << in.rdbuf();
  auto record = parse_prefill(text.str());
  if (!record || record->card != card) return std::nullopt;
  return record;
}

// Maya's tools/calibrate_mm3_prefill.py on this server: the engine of the card's start, without the prompt path's own
// flags (Maya's KEYS), runs once with --calibrate-prefill OUT over about 4096 tokens of a real text - the out folder's
// LICENSE (Maya's is its CHANGELOG.md) as one user message through the card's tokenizer and MiniMax's template, thinking
// off - or, without that text, the engine's own token mix, as Maya's falls back. The engine keeps its choice in the
// pack's prefill-tune.txt, which every later start reads; the record here says the measurement ran.
bool ModelLauncher::run_prefill_calibration(const Job& job) {
  constexpr std::size_t kPrefillTokens = 4096U;            // Maya's TOKENS
  constexpr int kDefaultPrefillChunk = 1024;               // the engine's default chunk (mm3_main.cpp kDefChunk)
  constexpr auto kPrefillTimeout = std::chrono::hours(1);  // Maya's TIMEOUT_S, the model's load included
  constexpr std::array<std::string_view, 5> kPrefillFlags{"--chunk", "--prefill-gemm", "--prefill-tune", "--no-prefill-tune",
                                                          "--calibrate-prefill"};
  const ModelCard& card = [this, &job]() -> const ModelCard& {
    std::lock_guard lock(mutex_);
    return settings_.cards[job.card];
  }();
  const auto stop_asked = [this] {
    std::lock_guard lock(mutex_);
    return shutting_down_ || cancel_calibration_.load();
  };
  if (!settings_.calibration_dir.has_value() || job.config == nullptr) return false;
  log("[strata] models: " + card.name + ": measuring how this PC reads prompts (MiniMax's prefill calibration, once)");
  auto composed = compose_model(*job.config, true);
  if (auto* failure = std::get_if<ComposeFailure>(&composed)) {
    for (const auto& message : failure->messages) log("[strata] models: " + card.name + ": " + message);
    return false;
  }
  auto& model = std::get<ComposedModel>(composed);
  auto root = open_root(*settings_.calibration_dir, RootPolicy::create_private);
  if (!std::holds_alternative<CanonicalRoot>(root) || !prepare_state_dir(*job.config->roots.state, *model.plan.environment)) {
    log("[strata] models: " + card.name + ": the prefill calibration's folder could not be prepared");
    return false;
  }
  const auto& folder = std::get<CanonicalRoot>(root);
  const std::string base = prefill_file_name(card.name);
  const std::string ids_name = base.substr(0, base.size() - 5U) + "-ids.txt";
  const std::string out_name = base.substr(0, base.size() - 5U) + "-out.json";
  const auto ids_path = *settings_.calibration_dir / path_from_utf8(ids_name);
  const auto out_path = *settings_.calibration_dir / path_from_utf8(out_name);
  std::error_code ec;
  std::filesystem::remove(out_path, ec);
  // the text's tokens, at most kPrefillTokens (Maya's long_prompt_ids: about 6 characters a token, cut by the count)
  std::vector<std::size_t> ids;
  {
    // the out folder's LICENSE: <out>/engine/multiengine(.exe)
    std::ifstream in(job.config->engine.executable.path().parent_path().parent_path() / "LICENSE", std::ios::binary);
    std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (text.size() > kPrefillTokens * 6U) text.resize(kPrefillTokens * 6U);
    std::vector<std::string_view> special;
    for (const std::string_view literal : kMinimaxLiterals) {
      if (find_special_token(model.tokenizer, literal).has_value()) special.push_back(literal);
    }
    for (int round = 0; round < 4 && !text.empty(); ++round) {
      const auto pieces = render_minimax_text_prompt({ChatMessage{"user", "Summarize this text:\n\n" + text}}, MinimaxThinking{.off = true}, special);
      const auto encoded = pieces ? encode_prompt_pieces(model.tokenizer, *pieces) : TokenizerEncodeResult{};
      const auto* tokens = std::get_if<std::vector<std::size_t>>(&encoded);
      if (tokens == nullptr) {
        ids.clear();
        break;
      }
      ids = *tokens;
      if (ids.size() <= kPrefillTokens) break;
      text.resize(static_cast<std::size_t>(static_cast<double>(text.size()) * kPrefillTokens / static_cast<double>(ids.size()) * 0.98));
    }
  }
  std::string id_text;
  for (const std::size_t id : ids) id_text += (id_text.empty() ? "" : ",") + std::to_string(id);
  bool with_ids = false;
  if (!id_text.empty()) {
    const auto relative = parse_relative_path(ids_name);
    with_ids = std::holds_alternative<RelativePath>(relative) &&
               !write_private_file(folder, std::get<RelativePath>(relative), id_text, TokenStoreMode::replace).has_value();
  }
  EngineLaunchPlan plan = std::move(model.plan);
  std::vector<std::string> arguments;
  for (std::size_t i = 0; i < plan.command.arguments.size(); ++i) {
    const std::string& argument = plan.command.arguments[i];
    if (std::find(kPrefillFlags.begin(), kPrefillFlags.end(), argument) != kPrefillFlags.end()) {
      if (argument != "--no-prefill-tune") ++i;  // and its value
      continue;
    }
    arguments.push_back(argument);
  }
  arguments.insert(arguments.end(), {"--calibrate-prefill", path_to_utf8(out_path)});
  if (with_ids) arguments.insert(arguments.end(), {"--ids", "@" + path_to_utf8(ids_path)});
  plan.command.arguments = std::move(arguments);
  plan.command.insert_serve = false;
  PrefillRecord record{card.name, utc_now(), false, 0, {}, 0.0, 0.0, calibration_key(card, job.values)};
  bool cancelled = false;
  // How one run of the engine's measurement ended: with its JSON; ended by itself without it (a refusal such as "not
  // enough GPU memory ... a smaller --chunk", or a crash); stopped by us (cancelled, or past kPrefillTimeout); or never
  // started. Only a run that ended by itself is worth another try at a smaller chunk.
  enum class Run { finished, ended, stopped, not_started };
  const auto attempt = [&](const std::vector<std::string>& extra, const std::uint64_t generation) {
    EngineLaunchPlan run = plan;
    run.command.arguments.insert(run.command.arguments.end(), extra.begin(), extra.end());
    auto spawned = settings_.factory(std::move(run))(generation);
    std::optional<ExitStatus> ended;
    bool stopped = false;
    if (auto* process = std::get_if<std::unique_ptr<SupervisedProcess>>(&spawned)) {
      const auto until = std::chrono::steady_clock::now() + kPrefillTimeout;
      while (!(ended = (*process)->try_reap())) {
        if (stop_asked() || std::chrono::steady_clock::now() > until) {
          cancelled = stop_asked();
          stopped = true;
          (*process)->kill_group();
          for (int i = 0; i < 50 && !(ended = (*process)->try_reap()); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
          ended.reset();  // stopped, not finished
          break;
        }
        // its stdout is drained (an engine's stderr goes nowhere: the server logs no engine output)
        if ((*process)->transport().read_line(std::chrono::milliseconds(200)).kind != EngineReadKind::line) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
      }
      (*process)->release_resources(settings_.stop_bound);
    } else {
      log("[strata] models: " + card.name + ": the prefill calibration's engine did not start: " + std::get<SpawnFailure>(spawned).message);
      return Run::not_started;
    }
    std::ifstream out(out_path, std::ios::binary);
    const std::string measured{std::istreambuf_iterator<char>(out), std::istreambuf_iterator<char>()};
    out.close();
    std::filesystem::remove(out_path, ec);
    if (ended && !ended->signaled && ended->value == 0U && read_prefill_measurement(measured, record)) return Run::finished;
    return stopped ? Run::stopped : Run::ended;
  };
  // The measurement sizes its buffers for its largest chunk (4096 tokens); where the GPU lacks the memory for that (a
  // 16 GB card with MiniMax-M3.1: "not enough GPU memory for MiniMax-M3.1's dense part ... a smaller --chunk"), the
  // larger chunks could not run anyway (they need every expert in VRAM), so it measures again at the default chunk -
  // the engine's own advice - for the GEMMs and this PC's prompt speed. A run stopped (cancelled, timed out) or never
  // started is not tried again.
  Run outcome = attempt({}, 1U);
  if (outcome == Run::ended) {
    log("[strata] models: " + card.name + ": the measurement did not finish at its largest prompt chunk; measuring at the default one (" +
        std::to_string(kDefaultPrefillChunk) + ")");
    outcome = attempt({"--chunk", std::to_string(kDefaultPrefillChunk)}, 2U);
  }
  record.finished = outcome == Run::finished;
  std::filesystem::remove(ids_path, ec);
  if (cancelled) {
    log("[strata] models: " + card.name + ": the prefill calibration was cancelled; nothing was kept");
    return false;
  }
  if (record.finished) {
    log("[strata] models: " + card.name + ": prompts are read in chunks of " + std::to_string(record.chunk) + " tokens with " +
        (record.gemm == "hipblaslt" ? "hipBLASLt" : "BLAS") + " GEMMs, " + decimal_text(record.tok_s) + " tok/s (the defaults " +
        decimal_text(record.default_tok_s) + ") - kept for this PC in the pack");
  } else {
    log("[strata] models: " + card.name + ": the prefill calibration did not finish; the engine reads prompts with its defaults " +
        "(Calibrate measures again)");
  }
  const auto file = parse_relative_path(base);
  if (!std::holds_alternative<RelativePath>(file) ||
      write_private_file(folder, std::get<RelativePath>(file), render_prefill(record), TokenStoreMode::replace).has_value()) {
    log("[strata] models: " + card.name + ": " + std::string{kCalibrationNotSaved});
  }
  return record.finished;
}

std::optional<ModelLauncher::Refusal> ModelLauncher::set_auto_recalibrate(const std::string_view name, const bool on) {
  std::optional<ModelCard> held;
  {
    std::lock_guard lock(mutex_);
    if (const auto found = find_card(name)) held = settings_.cards[*found];
  }
  if (!held) return Refusal{ApiError::model_not_found};
  if (!calibration_supported(*held)) return Refusal{ApiError::calibration_unsupported};
  {
    std::lock_guard lock(mutex_);
    auto_recalibrate_[std::string{name}] = on;
  }
  if (auto stored = read_calibration(name); stored && stored->auto_recalibrate != on) {
    stored->auto_recalibrate = on;
    if (!write_calibration(*stored)) return Refusal{ApiError::internal_error};
  }
  log("[strata] models: " + std::string{name} + ": recalibrate when settings change " + (on ? "on" : "off"));
  return std::nullopt;
}

std::string ModelLauncher::calibration_json(const std::size_t index, const ModelCard& card, const ModelOptionValues& values) const {
  if (!calibration_supported(card)) return "null";
  // under mutex_ (list_json): the switch as set before a first calibration
  const auto switched_on = [this](const std::string& name) {
    const auto found = auto_recalibrate_.find(name);
    return found != auto_recalibrate_.end() && found->second;
  };
  const auto stored = read_calibration(card.name);
  const auto state = stored ? compare_key(stored->key, calibration_key(card, values)) : CalibrationState::missing;
  // first_start: the card calibrates before its first start (the engines whose sweep is a few short starts)
  std::string out = "{\"state\":" + json_string(calibration_state_name(state)) +
                    ",\"first_start\":" + (calibrates_before_first_start(card.profile) ? "true" : "false") + ",\"running\":";
  if (calibrating_.has_value() && calibrating_->card == index) {
    const auto& progress = calibrating_->progress;
    out += "{\"stage\":" + json_string(progress.stage) + ",\"trial\":" + std::to_string(progress.trial) +
           ",\"trials\":" + std::to_string(progress.trials) +
           ",\"best_tok_s\":" + (progress.best_tok_s ? decimal_text(*progress.best_tok_s) : std::string{"null"}) + "}";
  } else {
    out += "null";
  }
  std::string args;
  if (stored) {
    for (std::size_t i = 0; i < stored->args.size(); ++i) args += (i == 0U ? "" : ",") + json_string(stored->args[i]);
  }
  const auto error = calibration_errors_.find(card.name);
  out += ",\"before\":" + (stored ? decimal_text(stored->baseline_tok_s) : std::string{"null"}) +
         ",\"after\":" + (stored ? decimal_text(stored->best_tok_s) : std::string{"null"}) +
         ",\"date\":" + (stored ? json_string(stored->date) : std::string{"null"}) + ",\"args\":[" + args + "]" +
         ",\"auto\":" + ((stored ? stored->auto_recalibrate : switched_on(card.name)) ? "true" : "false") +
         ",\"error\":" + (error == calibration_errors_.end() ? std::string{"null"} : json_string(error->second));
  // MiniMax: its prefill calibration (null: never run here)
  if (card.profile == EngineProfile::minimax) {
    const auto prefill = read_prefill(card.name);
    out += ",\"prefill\":";
    if (!prefill) {
      out += "null";
    } else {
      const bool stale = compare_key(prefill->key, calibration_key(card, values)) == CalibrationState::stale;
      out += "{\"state\":" + json_string(stale ? "stale" : prefill->finished ? "done" : "failed") + ",\"chunk\":" +
             std::to_string(prefill->chunk) + ",\"gemm\":" + json_string(prefill->gemm) + ",\"tok_s\":" + decimal_text(prefill->tok_s) +
             ",\"default_tok_s\":" + decimal_text(prefill->default_tok_s) + ",\"date\":" + json_string(prefill->date) + "}";
    }
  }
  out += "}";
  return out;
}

// §5.1: every candidate is one start of the card with those flags; the workload's median decode tok/s is its score.
// The engine is stopped after each; a start that fails, a reply that is not one, or a cancel scores nothing.
std::optional<double> ModelLauncher::measure(const ModelCard& card, const ModelOptionValues& values,
                                             const std::vector<std::string>& args, const unsigned max_tokens) {
  const auto stop_asked = [this] {
    std::lock_guard lock(mutex_);
    return shutting_down_ || cancel_calibration_.load();
  };
  const auto config = config_with(card, values, args);
  if (config == nullptr) return std::nullopt;
  auto composed = compose_model(*config, true);
  if (auto* failure = std::get_if<ComposeFailure>(&composed)) {
    for (const auto& message : failure->messages) log("[strata] models: " + card.name + ": " + message);
    return std::nullopt;
  }
  auto& model = std::get<ComposedModel>(composed);
  if (!prepare_state_dir(*config->roots.state, *model.plan.environment)) return std::nullopt;
  const std::string model_id = model.model_id;
  EngineLaunchPlan plan = std::move(model.plan);
  ServeMonitor meter;
  auto runtime = std::make_shared<ModelRuntime>(std::move(model), settings_.factory(std::move(plan)), settings_.gate, &meter);
  {
    std::lock_guard lock(mutex_);
    measuring_ = runtime;
    if (shutting_down_) runtime->supervisor().hurry();
  }
  // the next step must not start beside this one's engine: its stop waits for it to end
  const auto finish = [this, &runtime] {
    runtime->supervisor().stop_engine();
    std::lock_guard lock(mutex_);
    measuring_.reset();
    if (runtime->supervisor().engine_left_unreaped()) unreaped_ = true;
  };
  if (!runtime->supervisor().start()) {
    std::lock_guard lock(mutex_);
    measuring_.reset();
    return std::nullopt;
  }
  bool ready = false;
  while (!stop_asked()) {
    const auto snapshot = runtime->supervisor().snapshot();
    if (snapshot.state == SupervisorState::ready || snapshot.state == SupervisorState::generating) {
      ready = true;
      break;
    }
    if (snapshot.state != SupervisorState::starting || runtime->supervisor().generations_started() > 1U) break;
    (void)runtime->supervisor().wait_for_change(SupervisorState::starting, std::chrono::milliseconds(200));
  }
  if (!ready) {
    finish();
    return std::nullopt;
  }
  std::vector<double> speeds;
  for (std::size_t i = 0; i < kCalibrationPrompts.size() && !stop_asked(); ++i) {
    const RequestContext context{.client_gone = {}, .shutting_down = runtime->stop_flag(), .loopback_peer = true};
    const auto result = runtime->chat().handle(calibration_body(card.profile, model_id, kCalibrationPrompts[i], i == 0U ? 16U : max_tokens), context);
    const auto* response = std::get_if<HttpResponse>(&result);
    if (response == nullptr || response->status != 200) {
      log("[strata] models: " + card.name + ": a calibration reply failed (" +
          (response != nullptr ? std::string{response->log_code} : std::string{"stream"}) + ")");
      break;
    }
    if (i == 0U) continue;  // the warm-up
    if (const auto speed = meter.last_decode_tok_s(); speed && std::isfinite(*speed) && *speed > 0.0) speeds.push_back(*speed);
  }
  finish();
  if (speeds.size() + 1U < kCalibrationPrompts.size()) return std::nullopt;
  std::sort(speeds.begin(), speeds.end());
  return speeds[speeds.size() / 2U];
}

// §5: the running model is stopped (the GPU to itself), the plan swept, the result kept with its key.
bool ModelLauncher::run_calibration(const Job& job) {
  const ModelCard& card = [this, &job]() -> const ModelCard& {
    std::lock_guard lock(mutex_);
    return settings_.cards[job.card];
  }();
  {
    std::unique_lock lock(mutex_);
    switching_ = true;
    retire_running(lock);
    if (shutting_down_) {
      status_[job.card] = CardStatus{};
      return false;
    }
    status_[job.card] = CardStatus{CardState::calibrating, {}};
    calibrating_ = CalibrationRun{job.card, {}};
    calibration_errors_.erase(card.name);
  }
  // MiniMax: Maya's --calibrate measures its prompt reading first (the engine keeps it in the pack; the sweep's starts read it)
  if (card.profile == EngineProfile::minimax) {
    {
      std::lock_guard lock(mutex_);
      calibrating_->progress = SweepProgress{"prompt reading", 0U, 0U, std::nullopt};
    }
    (void)run_prefill_calibration(job);
  }
  log("[strata] models: calibrating " + card.name + " (each candidate is one engine start)");
  const auto outcome = run_sweep(
      sweep_plan(card.profile, machine_),
      [this, &card, &job](const std::vector<std::string>& args, const unsigned max_tokens) {
        return measure(card, job.values, args, max_tokens);
      },
      [this, &card](const SweepProgress& progress) {
        {
          std::lock_guard lock(mutex_);
          if (calibrating_.has_value()) calibrating_->progress = progress;
        }
        log("[strata] models: " + card.name + ": calibration trial " + std::to_string(progress.trial) + "/" +
            std::to_string(progress.trials) + " (" + progress.stage + ")");
      },
      [this] {
        std::lock_guard lock(mutex_);
        return shutting_down_ || cancel_calibration_.load();
      },
      sweep_keep_margin(card.profile));
  std::string_view failure;
  bool kept = false;
  if (outcome.end == SweepOutcome::End::done) {
    const auto before = read_calibration(card.name);
    std::optional<bool> switched;
    {
      std::lock_guard lock(mutex_);
      if (const auto found = auto_recalibrate_.find(card.name); found != auto_recalibrate_.end()) switched = found->second;
    }
    CalibrationResult result{card.name, outcome.args, outcome.baseline_tok_s, outcome.best_tok_s, utc_now(),
                             job.auto_recalibrate.value_or(switched.value_or(before ? before->auto_recalibrate : false)),
                             calibration_key(card, job.values)};
    kept = write_calibration(result);
    std::string flags;
    for (const auto& arg : outcome.args) flags += " " + arg;
    log("[strata] models: " + card.name + " calibrated: " + decimal_text(outcome.baseline_tok_s) + " -> " +
        decimal_text(outcome.best_tok_s) + " tok/s" + (flags.empty() ? " (the engine's defaults)" : " with" + flags));
    if (!kept) failure = kCalibrationNotSaved;
    if (kept) {
      alert("calibration_finished", card.name + " is calibrated",
            "Decode speed: " + decimal_text(outcome.baseline_tok_s) + " tok/s with the engine's defaults, " +
                decimal_text(outcome.best_tok_s) + " tok/s" + (flags.empty() ? " (the defaults were fastest)." : " with" + flags + "."));
    }
  } else if (outcome.end == SweepOutcome::End::failed) {
    failure = kCalibrationFailed;
    log("[strata] models: " + card.name + ": " + std::string{kCalibrationFailed});
  } else {
    log("[strata] models: " + card.name + ": the calibration was cancelled; nothing was kept");
  }
  if (!failure.empty()) alert("calibration_failed", card.name + " was not calibrated", std::string{failure} + ".");
  std::lock_guard lock(mutex_);
  calibrating_.reset();
  cancel_calibration_.store(false);
  if (failure.empty()) {
    status_[job.card] = CardStatus{};
  } else {
    status_[job.card] = CardStatus{CardState::failed, failure};
    calibration_errors_[card.name] = std::string{failure};
  }
  return kept;
}

void ModelLauncher::run_jobs() {
  for (;;) {
    Job job;
    {
      std::unique_lock lock(mutex_);
      // idle: every 5 s, the running model's engine is checked - a circuit opened by repeated failures alerts once
      while (!changed_.wait_for(lock, std::chrono::seconds(5), [this] { return pending_.has_value() || shutting_down_; })) {
        if (running_ == nullptr || !running_card_.has_value() || stop_reported_) continue;
        if (running_->supervisor().snapshot().state != SupervisorState::circuit_open) continue;
        stop_reported_ = true;
        const std::string name = settings_.cards[*running_card_].name;
        lock.unlock();
        alert("engine_stopped", name + " stopped", "The model's engine stopped after repeated failures (see the server log).");
        lock.lock();
      }
      if (shutting_down_) return;
      job = std::move(*pending_);
      pending_.reset();
    }
    if (job.kind == Job::Kind::start) {
      if (job.prefill_first) {
        {
          // the GPU to itself: the running model is retired first, its card shown calibrating
          std::unique_lock lock(mutex_);
          switching_ = true;
          retire_running(lock);
          status_[job.card] = CardStatus{CardState::calibrating, {}};
          calibrating_ = CalibrationRun{job.card, SweepProgress{"prompt reading", 1U, 1U, std::nullopt}};
        }
        (void)run_prefill_calibration(job);
        bool cancelled = false;
        {
          std::lock_guard lock(mutex_);
          calibrating_.reset();
          cancelled = shutting_down_ || cancel_calibration_.load();
          cancel_calibration_.store(false);
          status_[job.card] = cancelled ? CardStatus{} : CardStatus{CardState::starting, {}};
        }
        if (!cancelled) run_start(job);
      } else {
        run_start(job);
      }
    } else if (job.kind == Job::Kind::calibrate) {
      const bool kept = run_calibration(job);
      if (job.then_start && kept) {
        // the start the calibration was for, with its result
        Job start = job;
        start.kind = Job::Kind::start;
        const ModelCard& card = [this, &job]() -> const ModelCard& {
          std::lock_guard lock(mutex_);
          return settings_.cards[job.card];
        }();
        if (const auto stored = read_calibration(card.name); stored && !stored->args.empty()) {
          if (auto with = config_with(card, job.values, stored->args)) start.config = std::move(with);
        }
        {
          std::lock_guard lock(mutex_);
          status_[job.card] = CardStatus{CardState::starting, {}};
        }
        run_start(start);
      }
    } else {
      std::unique_lock lock(mutex_);
      retire_running(lock);
      if (job.kind == Job::Kind::remove) {
        // the card goes once its model has stopped (owner 2026-10-28: its files stay)
        const auto found = find_card(job.name);
        const bool erased = found.has_value() && page_card_[*found];
        if (erased) erase_card(*found);
        lock.unlock();
        if (erased) forget_card(job.name);
      }
    }
    std::lock_guard lock(mutex_);
    busy_ = false;
    switching_ = false;
    starting_card_.reset();
    changed_.notify_all();
  }
}

// D3: queued requests are refused, the generation in flight may finish for drain_wait, then
// it is cut (its stop flag ends it like a shutdown) and given cut_wait; then the supervisor
// shuts the engine down. Requests still holding the runtime keep it alive until they end.
void ModelLauncher::retire_running(std::unique_lock<std::mutex>& lock) {
  if (running_ == nullptr) return;
  std::shared_ptr<ModelRuntime> old = running_;
  const std::size_t card = running_card_.value_or(0U);
  lock.unlock();
  old->mark_retiring();
  old->gate().shutdown();
  const auto wait_idle = [&old](const std::chrono::milliseconds bound) {
    const auto until = std::chrono::steady_clock::now() + bound;
    while (old->in_flight() > 0U && std::chrono::steady_clock::now() < until) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return old->in_flight() == 0U;
  };
  if (!wait_idle(settings_.drain_wait)) {
    log("[strata] models: the generation in flight is cut for the switch");
    old->raise_stop();
    (void)wait_idle(settings_.cut_wait);
  }
  old->stop_vision();  // the encoder role first: spawned last, stopped first
  // waits for an engine still ending (a big model frees its memory slowly), so the next one does not start beside it
  old->supervisor().stop_engine();
  const bool unreaped = old->supervisor().engine_left_unreaped();
  lock.lock();
  if (unreaped) unreaped_ = true;
  running_.reset();
  running_card_.reset();
  status_[card] = CardStatus{CardState::stopped, {}};
  // the Monitor's engine readings (expert tier, READY context) were the stopped engine's
  if (settings_.monitor != nullptr) settings_.monitor->engine_changed();
  const std::string name = settings_.cards[card].name;
  lock.unlock();
  log("[strata] models: stopped " + name);
  lock.lock();
}

void ModelLauncher::run_start(const Job& job) {
  const ModelCard& card = [this, &job]() -> const ModelCard& {
    std::lock_guard lock(mutex_);
    return settings_.cards[job.card];
  }();
  const auto fail = [this, &job, &card](const std::string_view reason) {
    {
      std::lock_guard lock(mutex_);
      status_[job.card] = CardStatus{CardState::failed, reason};
    }
    log("[strata] models: " + card.name + " failed: " + std::string{reason});
    alert("engine_stopped", card.name + " did not start", "The model did not start: " + std::string{reason} + ".");
  };
  // §5.2 step 2: compose (the tokenizer loads) before the running model is touched.
  auto composed = compose_model(*job.config, true);
  if (auto* failure = std::get_if<ComposeFailure>(&composed)) {
    for (const auto& message : failure->messages) log("[strata] models: " + card.name + ": " + message);
    fail(failure->reason);
    return;
  }
  auto& model = std::get<ComposedModel>(composed);
  {
    // §5.2 step 3: from here chat is refused until the new model is ready (or failed).
    std::unique_lock lock(mutex_);
    switching_ = true;
    retire_running(lock);
    if (shutting_down_) {
      status_[job.card] = CardStatus{};
      return;
    }
    // A restart of the running card: retiring it marked it stopped; it is starting again.
    status_[job.card] = CardStatus{CardState::starting, {}};
  }
  if (!prepare_state_dir(*job.config->roots.state, *model.plan.environment)) {
    fail(kStateDirFailed);
    return;
  }
  EngineLaunchPlan plan = std::move(model.plan);
  // web search: the server's tool for this model's chats, unless the card's Web search checkbox was off for this start
  const auto web_search = job.values.find("web_search");
  const bool* searches = web_search == job.values.end() ? nullptr : std::get_if<bool>(&web_search->second);
  model.chat.tools = searches == nullptr || *searches ? settings_.tools : nullptr;
  model.chat.tool_rounds = settings_.tool_rounds;
  // The Monitor's engine readings belong to the engine that ran before.
  if (settings_.monitor != nullptr) settings_.monitor->engine_changed();
  ProcessFactory vision_factory;  // a card that reads images: its encoder (unset: the production one)
  if (model.vision.has_value() && settings_.vision_factory) vision_factory = settings_.vision_factory(*model.vision);
  auto runtime = std::make_shared<ModelRuntime>(std::move(model), settings_.factory(std::move(plan)), settings_.gate, settings_.monitor,
                                                std::make_shared<SteadySupervisorClock>(), std::move(vision_factory));
  if (!runtime->supervisor().start()) {
    fail(kSupervisorFailed);
    return;
  }
  {
    std::lock_guard lock(mutex_);
    running_ = runtime;
    running_card_ = job.card;
    if (shutting_down_) runtime->supervisor().hurry();
  }
  // READY, or the first end before it (D4: no retry loop for a start that cannot load).
  bool ready = false;
  for (;;) {
    {
      std::lock_guard lock(mutex_);
      if (shutting_down_) break;
    }
    const auto snapshot = runtime->supervisor().snapshot();
    if (snapshot.state == SupervisorState::ready || snapshot.state == SupervisorState::generating) {
      ready = true;
      break;
    }
    if (snapshot.state != SupervisorState::starting || runtime->supervisor().generations_started() > 1U) break;
    (void)runtime->supervisor().wait_for_change(SupervisorState::starting, std::chrono::milliseconds(200));
  }
  if (!ready) {
    runtime->supervisor().stop_engine();
    std::lock_guard lock(mutex_);
    if (runtime->supervisor().engine_left_unreaped()) unreaped_ = true;
    running_.reset();
    running_card_.reset();
    if (shutting_down_) {
      status_[job.card] = CardStatus{};
      return;
    }
  }
  if (!ready) {
    fail(kEndedBeforeReady);
    return;
  }
  if (!runtime->start_vision()) {
    runtime->stop_vision();
    runtime->supervisor().stop_engine();
    {
      std::lock_guard lock(mutex_);
      if (runtime->supervisor().engine_left_unreaped()) unreaped_ = true;
      running_.reset();
      running_card_.reset();
    }
    fail(kVisionFailed);
    return;
  }
  if (runtime->vision() != nullptr) log("[strata] models: " + card.name + ": the picture encoder is running");
  {
    std::lock_guard lock(mutex_);
    status_[job.card] = CardStatus{CardState::running, {}};
    stop_reported_ = false;
  }
  // The Monitor shows "N experts loaded" on the VRAM and RAM cards; push the engine's startup
  // readings now so a just-started model shows them before any chat (owner 2026-10-23).
  if (settings_.monitor != nullptr) settings_.monitor->on_engine_stats(runtime->supervisor().ready_readings());
  log("[strata] models: " + card.name + " is running");
  remember(job.card, job.values);
}

void ModelLauncher::remember(const std::size_t card, const ModelOptionValues& values) {
  {
    std::lock_guard lock(mutex_);
    remembered_[settings_.cards[card].name] = values;
  }
  write_remembered();
}

void ModelLauncher::write_remembered() {
  std::string text;
  {
    std::lock_guard lock(mutex_);
    text = render_remembered_options(remembered_);
  }
  if (!settings_.memory_file.has_value()) return;
  const auto& file = *settings_.memory_file;
  auto root = open_root(file.parent_path());
  const auto name = parse_relative_path(path_to_utf8(file.filename()));
  if (!std::holds_alternative<CanonicalRoot>(root) || !std::holds_alternative<RelativePath>(name) ||
      write_private_file(std::get<CanonicalRoot>(root), std::get<RelativePath>(name), text, TokenStoreMode::replace).has_value()) {
    log("[strata] models: the page's options could not be remembered (the state directory refused the file)");
  }
}

std::shared_ptr<ModelRuntime> ModelLauncher::running_runtime() const {
  std::lock_guard lock(mutex_);
  return switching_ ? nullptr : running_;
}

std::string ModelLauncher::list_json() const {
  std::lock_guard lock(mutex_);
  const auto state_of_card = [this](const std::size_t i) -> CardStatus {
    CardStatus status = status_[i];
    // A running model whose engine kept failing: the supervisor's circuit is open.
    if (status.state == CardState::running && running_card_ == i && running_ != nullptr &&
        running_->supervisor().snapshot().state == SupervisorState::circuit_open) {
      status = CardStatus{CardState::failed, kExhausted};
    }
    return status;
  };
  std::string out = "{\"object\":\"list\",\"running\":";
  const bool runs = running_card_.has_value() && !switching_;
  out += runs ? json_string(settings_.cards[*running_card_].name) : std::string{"null"};
  std::string_view top = switching_ || busy_ ? "starting" : "stopped";
  std::string_view top_error;
  if (runs) {
    const CardStatus status = state_of_card(*running_card_);
    top = card_state_name(status.state);
    top_error = status.error;
  }
  out += ",\"state\":" + json_string(top) + ",\"error\":" + (top_error.empty() ? std::string{"null"} : json_string(top_error));
  out += ",\"data\":[";
  for (std::size_t i = 0; i < settings_.cards.size(); ++i) {
    const ModelCard& card = settings_.cards[i];
    const CardStatus status = state_of_card(i);
    ModelOptionValues values;
    if (const auto found = remembered_.find(card.name); found != remembered_.end()) values = found->second;
    if (i != 0U) out += ',';
    out += "{\"name\":" + json_string(card.name) + ",\"profile\":" + json_string(profile_name(card.profile)) +
           ",\"state\":" + json_string(card_state_name(status.state)) +
           ",\"error\":" + (status.error.empty() ? std::string{"null"} : json_string(status.error)) +
           ",\"page_card\":" + (page_card_[i] ? "true" : "false");
    if (page_card_[i]) {
      // The page's Folder field and the Download list's filter (spec 2026-10-27 §4.3a, §4.4).
      const auto text_row = [&card](const std::string_view key) {
        const auto found = card.config.entries.find(std::string{key});
        const auto* value = found == card.config.entries.end() ? nullptr : std::get_if<std::string>(&found->second.value);
        return value == nullptr ? std::string{"null"} : json_string(*value);
      };
      out += ",\"source\":" + (card.source.empty() ? std::string{"null"} : json_string(card.source)) +
             ",\"folder\":" + text_row("roots.models") + ",\"file\":" + text_row("engine.model_id");
      // its own draft layer, where the page can point it (owner 2026-10-29)
      if (const auto slot = draft_layer_slot(card)) out += ",\"draft\":" + draft_layer_json(*slot);
    }
    out += ",\"options\":" + describe_options_json(shown_options(card, settings_.tools), values);
    out += ",\"calibration\":" + calibration_json(i, card, values.empty() ? default_option_values(card) : values);
    // whether a request may name reasoning_effort for this card (the chat schema's own rule): the page omits it where
    // not (owner 2026-10-27, item 46: a GLM card's profile refuses it)
    out += std::string{",\"reasoning_effort\":"} + (per_request_reasoning_effort_for(card.profile) ? "true" : "false") + "}";
  }
  out += "]}\n";
  return out;
}

ServedModels ModelLauncher::served() const {
  std::lock_guard lock(mutex_);
  ServedModels served;
  std::string extension = "[";
  for (std::size_t i = 0; i < settings_.cards.size(); ++i) {
    if (i != 0U) extension += ',';
    extension += "{\"id\":" + json_string(settings_.cards[i].name) +
                 ",\"state\":" + json_string(card_state_name(status_[i].state)) + "}";
  }
  served.extension = extension + "]";
  if (running_ != nullptr && !switching_ && running_card_.has_value() && status_[*running_card_].state == CardState::running) {
    served.id = running_->model_id();
    served.aliases = running_->aliases();
  }
  return served;
}

RouteResult ModelLauncher::chat(const std::string_view body, const RequestContext& context) {
  std::shared_ptr<ModelRuntime> runtime;
  {
    std::lock_guard lock(mutex_);
    if (shutting_down_) return make_error_response(ApiError::server_shutting_down);
    // A start with nothing running yet counts as a switch: the model is on its way.
    if (switching_ || (running_ == nullptr && busy_)) return make_error_response(ApiError::model_switching);
    if (running_ == nullptr) return make_error_response(ApiError::no_model_running);
    runtime = running_;
  }
  // The request holds the runtime until its response or stream ends; the switch waits on it.
  struct InFlight final {
    std::shared_ptr<ModelRuntime> runtime;
    explicit InFlight(std::shared_ptr<ModelRuntime> held) : runtime(std::move(held)) { runtime->enter(); }
    ~InFlight() { runtime->leave(); }
    InFlight(const InFlight&) = delete;
    InFlight& operator=(const InFlight&) = delete;
  };
  auto held = std::make_shared<InFlight>(runtime);
  const RequestContext inner{.client_gone = context.client_gone, .shutting_down = runtime->stop_flag(),
                             .loopback_peer = context.loopback_peer, .peer = context.peer};
  RouteResult result = runtime->chat().handle(body, inner);
  if (auto* stream = std::get_if<StreamHandler>(&result)) {
    return StreamHandler{[handler = std::move(*stream), held](StreamSink& sink) { handler(sink); }};
  }
  auto& response = std::get<HttpResponse>(result);
  // A request the switch refused from the old model's queue, or cut (D3), was not refused by a
  // shutdown: it is told the server is switching models.
  if (response.log_code == "server_shutting_down" && runtime->retiring()) {
    std::lock_guard lock(mutex_);
    if (!shutting_down_) return make_error_response(ApiError::model_switching);
  }
  // D6: a request naming another card is told that card is not running.
  if (response.log_code == "model_not_found") {
    const auto parsed = parse_json(body, kModelFieldLimits);
    if (const auto* root = std::get_if<JsonValue>(&parsed); root != nullptr && root->if_object() != nullptr) {
      for (const auto& member : *root->if_object()) {
        if (member.key != "model" || member.value.if_string() == nullptr) continue;
        std::lock_guard lock(mutex_);
        const auto named = find_card(*member.value.if_string());
        if (!named.has_value()) continue;
        // The card an accepted start is bringing up is on its way, not stopped.
        if (starting_card_ == named) return make_error_response(ApiError::model_switching);
        return make_error_response(ApiError::model_not_running);
      }
    }
  }
  return result;
}

HealthView ModelLauncher::health() const {
  std::shared_ptr<ModelRuntime> runtime;
  {
    std::lock_guard lock(mutex_);
    if (switching_ || (running_ == nullptr && busy_)) return HealthView{false, "starting", ""};
    if (running_ == nullptr) return HealthView{true, "none", ""};
    runtime = running_;
  }
  const auto snapshot = runtime->port().snapshot();
  return HealthView{snapshot.healthy, snapshot.engine, snapshot.recovery};
}

std::string ModelLauncher::metrics_json() const {
  if (settings_.monitor == nullptr) return "{}\n";
  const auto runtime = running_runtime();
  if (runtime == nullptr) {
    std::lock_guard lock(mutex_);
    return settings_.monitor->render_json({.model = "", .engine = switching_ || busy_ ? "starting" : "none", .queued = 0U});
  }
  const auto snapshot = runtime->port().snapshot();
  return settings_.monitor->render_json({.model = runtime->model_id(), .engine = snapshot.engine, .queued = runtime->gate().waiting(),
                                         .unloaded = snapshot.state == SupervisorState::unloaded});
}

std::optional<EngineSample> ModelLauncher::engine_sample() const {
  // the same runtime metrics_json() reports (the one running still serves while another card's start composes)
  const auto runtime = running_runtime();
  if (runtime == nullptr) return std::nullopt;
  return runtime->supervisor().engine_sample();
}

void ModelLauncher::begin_shutdown() noexcept {
  {
    std::lock_guard lock(mutex_);
    shutting_down_ = true;
    if (running_ != nullptr) {
      running_->raise_stop();
      running_->gate().shutdown();
      running_->supervisor().hurry();  // a stop under way keeps the server's bound
    }
    if (measuring_ != nullptr) measuring_->supervisor().hurry();
  }
  changed_.notify_all();
}

void ModelLauncher::shutdown() {
  begin_shutdown();
  if (worker_.joinable()) worker_.join();
  std::shared_ptr<ModelRuntime> runtime;
  {
    std::lock_guard lock(mutex_);
    runtime = std::move(running_);
    running_card_.reset();
  }
  if (runtime == nullptr) return;
  runtime->stop_vision();  // the encoder role first: spawned last, stopped first
  runtime->supervisor().shutdown(settings_.stop_bound);
  if (runtime->supervisor().engine_left_unreaped()) {
    std::lock_guard lock(mutex_);
    unreaped_ = true;
  }
}

bool ModelLauncher::engine_left_unreaped() const noexcept {
  std::lock_guard lock(mutex_);
  return unreaped_;
}

bool ModelLauncher::wait_until_settled(const std::chrono::milliseconds timeout) const {
  std::unique_lock lock(mutex_);
  return changed_.wait_for(lock, timeout, [this] { return !busy_ && !pending_.has_value(); });
}

CardState ModelLauncher::state_of(const std::string_view name) const {
  std::lock_guard lock(mutex_);
  const auto card = find_card(name);
  return card.has_value() ? status_[*card].state : CardState::stopped;
}

}  // namespace secure_frontend
