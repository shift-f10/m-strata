#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

#include "secure_frontend/calibration_thermal.hpp"
#include "secure_frontend/calibration_workload.hpp"
#include "secure_frontend/model_launcher.hpp"
#include "secure_frontend/sha256.hpp"
#include "secure_frontend/token_store.hpp"
#include "secure_frontend/tool_calls.hpp"

namespace secure_frontend {
namespace {
using J = JsonValue;
J string(std::string_view s) { return J::make_string(std::string{s}); }
J number(double n) { return std::isfinite(n) ? J::make_decimal(n) : J{}; }
J optional_number(std::optional<double> n) { return n ? number(*n) : J{}; }
J boolean(bool b) { return J::make_boolean(b); }
J strings(const std::vector<std::string>& values) {
  std::vector<J> a;
  for (const auto& s : values) a.push_back(string(s));
  return J::make_array(std::move(a));
}
std::vector<std::string> overlay_args(std::vector<std::string> base, const std::vector<std::string>& overrides) {
  for (std::size_t i = 0; i + 1U < overrides.size(); i += 2U) {
    bool replaced = false;
    for (std::size_t j = 0; j + 1U < base.size(); j += 2U)
      if (base[j] == overrides[i]) { base[j + 1U] = overrides[i + 1U]; replaced = true; break; }
    if (!replaced) { base.push_back(overrides[i]); base.push_back(overrides[i + 1U]); }
  }
  return base;
}
const J* get(const J& j, std::string_view key) {
  if (const auto* o = j.if_object())
    for (const auto& m : *o)
      if (m.key == key) return &m.value;
  return nullptr;
}
std::string text(const J& j, std::string_view key) {
  const auto* p = get(j, key);
  return p && p->if_string() ? *p->if_string() : std::string{};
}
double numeric(const J& j, std::string_view key, double fallback = 0) {
  const auto* p = get(j, key);
  if (p && p->if_decimal()) return *p->if_decimal();
  if (p && p->if_integer()) return static_cast<double>(*p->if_integer());
  return fallback;
}
bool flag(const J& j, std::string_view key) {
  const auto* p = get(j, key);
  return p && p->if_boolean() && *p->if_boolean();
}
void set(J& j, std::string key, J value) {
  auto fields = j.if_object() ? *j.if_object() : std::vector<JsonMember>{};
  for (auto& f : fields)
    if (f.key == key) {
      f.value = std::move(value);
      j = J::make_object(std::move(fields));
      return;
    }
  fields.push_back({std::move(key), std::move(value)});
  j = J::make_object(std::move(fields));
}
J parse(std::string_view s) {
  auto p = parse_json(s, {.max_bytes = 2U << 20U,
                          .max_depth = 16,
                          .max_container_entries = 8192,
                          .max_string_bytes = 32768,
                          .allow_fractions = true});
  if (auto* j = std::get_if<J>(&p)) return std::move(*j);
  return {};
}
std::string artifact(std::string_view card, std::string_view kind) {
  return "benchmark-" + to_hex(sha256(card)).substr(0, 24) + "." +
         std::string{kind} + ".json";
}
J read(const std::optional<std::filesystem::path>& directory,
       std::string_view name) {
  if (!directory) return {};
  auto root = open_root(*directory);
  auto path = parse_relative_path(name);
  if (!std::holds_alternative<CanonicalRoot>(root) ||
      !std::holds_alternative<RelativePath>(path))
    return {};
  auto f =
      open_beneath(std::get<CanonicalRoot>(root), std::get<RelativePath>(path),
                   {.trust = FileTrust::owner_only});
  if (auto* file = std::get_if<OpenedFile>(&f)) {
    auto contents = file->read_all(2U << 20U);
    if (auto* s = std::get_if<std::string>(&contents)) return parse(*s);
  }
  return {};
}
bool save(const std::optional<std::filesystem::path>& directory,
          std::string_view name, const J& value) {
  if (!directory) return false;
  auto root = open_root(*directory, RootPolicy::create_private);
  auto path = parse_relative_path(name);
  const auto data = python_json_text(value);
  if (data.size() > (2U << 20U)) return false;
  return std::holds_alternative<CanonicalRoot>(root) &&
         std::holds_alternative<RelativePath>(path) &&
         !write_private_file(std::get<CanonicalRoot>(root),
                             std::get<RelativePath>(path), data,
                             TokenStoreMode::replace);
}
std::string workload_name(BenchmarkWorkload w) {
  switch (w) {
    case BenchmarkWorkload::chat:
      return "chat";
    case BenchmarkWorkload::long_context:
      return "long_context";
    case BenchmarkWorkload::coding:
      return "coding";
    case BenchmarkWorkload::tools:
      return "tools";
    case BenchmarkWorkload::custom:
      return "custom";
  }
  return "chat";
}
double median(std::vector<double> v) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v.size() % 2 ? v[v.size() / 2]
                      : (v[v.size() / 2 - 1] + v[v.size() / 2]) / 2;
}
J median_or_null(const std::vector<double>& v) {
  return v.empty() ? J{} : number(median(v));
}
std::string trial_key(const ExperimentTrial& t) {
  return to_hex(sha256(python_json_text(strings(t.args)) + "|" +
                       std::to_string(t.max_tokens) + "|" + t.stage + "|" +
                       std::to_string(t.replicate) + "|" +
                       std::to_string(t.ordinal)));
}
struct Finally {
  std::function<void()> action;
  ~Finally() { action(); }
};
}  // namespace

void ModelLauncher::archive_calibration(const CalibrationResult& result,
                                        const ModelOptionValues& values,
                                        std::string_view label) {
  const auto file = artifact(result.card, "history");
  const auto old = read(settings_.calibration_dir, file);
  auto records = old.if_array() ? *old.if_array() : std::vector<J>{};
  const auto serialized = render_calibration(result);
  const auto id = to_hex(sha256(serialized)).substr(0, 24);
  if (std::any_of(records.begin(), records.end(),
                  [&](const auto& r) { return text(r, "id") == id; }))
    return;
  records.insert(records.begin(),
                 J::make_object({{"id", string(id)},
                                 {"label", string(label)},
                                 {"date", string(result.date)},
                                 {"before", number(result.baseline_tok_s)},
                                 {"after", number(result.best_tok_s)},
                                 {"result", parse(serialized)},
                                 {"values", parse(render_remembered_options(
                                                {{result.card, values}}))}}));
  if (records.size() > 8) records.resize(8);
  (void)save(settings_.calibration_dir, file,
             J::make_array(std::move(records)));
}

std::string ModelLauncher::benchmark_json(
    const ModelCard& card, const ModelOptionValues& values) const {
  J summary;
  if (const auto it = benchmark_status_.find(card.name);
      it != benchmark_status_.end())
    summary = parse(it->second);
  else
    summary = read(settings_.calibration_dir, artifact(card.name, "summary"));
  auto history =
      read(settings_.calibration_dir, artifact(card.name, "history"));
  std::vector<J> rows;
  if (const auto* records = history.if_array())
    for (const auto& record : *records) {
      const auto* saved = get(record, "result");
      const auto result =
          saved ? parse_calibration(python_json_text(*saved)) : std::nullopt;
      const auto* stored_values = get(record, "values");
      const auto remembered =
          stored_values
              ? parse_remembered_options(python_json_text(*stored_values))
              : RememberedOptions{};
      bool applicable = false;
      if (result && result->card == card.name)
        if (const auto it = remembered.find(card.name);
            it != remembered.end()) {
          const auto resolved =
              resolve_option_values(card.options, nullptr, it->second);
          if (const auto* selected = std::get_if<ModelOptionValues>(&resolved)) {
            const auto current=calibration_key(card,*selected);
            applicable = !current.runtime.empty() && !current.model.empty() &&
                compare_key(result->key,current)==CalibrationState::fresh;
          }
        }
      rows.push_back(J::make_object(
          {{"id", string(text(record, "id"))},
           {"label", string(text(record, "label"))},
           {"date", string(text(record, "date"))},
           {"before", number(numeric(record, "before"))},
           {"after", number(numeric(record, "after"))},
           {"applicable", boolean(applicable)},
           {"reason",
            string(applicable
                       ? "Restores these calibrated options for the next start"
                       : "Hardware, engine, model or saved options no longer "
                         "match")},
           {"settings", result ? strings(result->args) : J::make_array({})}}));
    }
  (void)values;
  if (!summary.if_object() && rows.empty()) return "null";
  set(summary, "history", J::make_array(std::move(rows)));
  return python_json_text(summary);
}

std::optional<ModelLauncher::Refusal> ModelLauncher::restore_calibration(
    std::string_view name, std::string_view id) {
  if (id.size() != 24 ||
      id.find_first_not_of("0123456789abcdef") != std::string_view::npos)
    return Refusal{ApiError::invalid_request};
  std::size_t index = 0;
  ModelCard card;
  {
    std::lock_guard lock(mutex_);
    if (shutting_down_) return Refusal{ApiError::server_shutting_down};
    if (busy_ || pending_) return Refusal{ApiError::switch_in_progress};
    const auto found = find_card(name);
    if (!found) return Refusal{ApiError::model_not_found};
    index = *found;
    card = settings_.cards[index];
    busy_ = true;
  }
  Finally release{[this] {
    std::lock_guard lock(mutex_);
    busy_ = false;
    changed_.notify_all();
  }};
  const auto history =
      read(settings_.calibration_dir, artifact(name, "history"));
  if (const auto* records = history.if_array())
    for (const auto& record : *records)
      if (text(record, "id") == id) {
        const auto* saved = get(record, "result");
        const auto result =
            saved ? parse_calibration(python_json_text(*saved)) : std::nullopt;
        const auto* stored = get(record, "values");
        const auto values =
            stored ? parse_remembered_options(python_json_text(*stored))
                   : RememberedOptions{};
        const auto it = values.find(std::string{name});
        if (!result || result->card != name || it == values.end())
          return Refusal{ApiError::invalid_request};
        const auto resolved =
            resolve_option_values(card.options, nullptr, it->second);
        const auto* selected = std::get_if<ModelOptionValues>(&resolved);
        if (!selected)return Refusal{ApiError::invalid_request};
        const auto current=calibration_key(card,*selected);
        if (current.runtime.empty() || current.model.empty() ||
            compare_key(result->key, current) !=
                CalibrationState::fresh ||
            !config_with(card, *selected, result->args))
          return Refusal{ApiError::invalid_request};
        if (!write_calibration(*result))
          return Refusal{ApiError::internal_error};
        remember(index, *selected);
        return std::nullopt;
      }
  return Refusal{ApiError::invalid_request};
}

std::optional<ExperimentSample> ModelLauncher::measure_benchmark(
    const ModelCard& card, const ModelOptionValues& values,
    const ExperimentTrial& trial, const BenchmarkOptions& options,
    const std::function<bool()>& stopped, J& detail,
    std::shared_ptr<ModelRuntime>& session, std::shared_ptr<ServeMonitor>& session_meter, std::string& session_signature) {
  detail =
      J::make_object({{"settings", strings(trial.args)},
                      {"stage", string(trial.stage)},
                      {"number", number(static_cast<double>(trial.ordinal))}});
  const auto fail =
      [&](std::string_view reason) -> std::optional<ExperimentSample> {
    set(detail, "status", string("failed"));
    set(detail, "reason", string(reason));
    return std::nullopt;
  };
  if (stopped()) return fail("Cancelled or time budget exhausted");
  auto selected = values;
  std::optional<double> tune_pcie, tune_min_p;
  std::vector<std::string> hardware_args;
  for (std::size_t i = 0; i + 1 < trial.args.size(); i += 2) {
    if (trial.args[i].starts_with("option:")) {
      const auto name = trial.args[i].substr(7);
      const auto* spec = find_model_option(card.profile, name);
      const auto value = parse(trial.args[i + 1]);
      if (!spec || !spec->accuracy_tradeoff)
        return fail("Unsupported comparison option");
      if (const auto* b = value.if_boolean())
        selected[name] = *b;
      else if (const auto* n = value.if_integer())
        selected[name] = *n;
      else if (const auto* d = value.if_decimal())
        selected[name] = *d;
      else
        return fail("Invalid comparison value");
    } else if (card.profile == EngineProfile::qwen &&
               (trial.args[i] == "--pcie-frac" || trial.args[i] == "--spec-min-p")) {
      const auto parsed = parse(trial.args[i + 1]);
      const auto* d = parsed.if_decimal();
      const auto* n = parsed.if_integer();
      if (!d && !n) return fail("Invalid live tuning value");
      const double value = d ? *d : static_cast<double>(*n);
      if (!std::isfinite(value) || value < 0.0 || value > 1.0) return fail("Invalid live tuning bound");
      (trial.args[i] == "--pcie-frac" ? tune_pcie : tune_min_p) = value;
    } else {
      hardware_args.push_back(trial.args[i]);
      hardware_args.push_back(trial.args[i + 1]);
    }
  }
  const auto selected_json =
      parse(render_remembered_options({{card.name, selected}}));
  const auto validated =
      resolve_option_values(card.options, get(selected_json, card.name), {});
  if (!std::holds_alternative<ModelOptionValues>(validated))
    return fail("Comparison value is outside this card's bounds");
  selected = std::get<ModelOptionValues>(validated);
  const auto signature = render_remembered_options({{card.name, selected}}) + python_json_text(strings(hardware_args));
  const bool reused = session && session_signature == signature;
  if (session && !reused) {
    session->supervisor().stop_engine();
    if (session->supervisor().engine_left_unreaped()) return fail("Previous benchmark engine could not be stopped");
    session.reset(); session_meter.reset();
  }
  const auto before = settings_.monitor
      ? await_calibration_baseline([&] { return settings_.monitor->latest_host_sample(); }, stopped)
      : std::nullopt;
  const auto started = std::chrono::steady_clock::now();
  auto config = config_with(card, selected, hardware_args);
  if (!config) return fail("Trial settings could not be validated");
  const auto& gpus =
      config->glm.gpus.empty() ? config->engine.gpus : config->glm.gpus;
  const auto gpu_before = settings_.benchmark_gpu
                              ? settings_.benchmark_gpu(gpus)
                              : sample_calibration_gpu(gpus);
  auto composed = compose_model(*config, true);
  auto* model = std::get_if<ComposedModel>(&composed);
  if (!model) return fail("Trial model could not be composed");
  if (!reused && !prepare_state_dir(*config->roots.state, *model->plan.environment))
    return fail("Trial state folder unavailable");
  std::size_t context = static_cast<std::size_t>(config->glm.max_context);
  if (const auto it = values.find("max_context"); it != values.end())
    if (const auto* n = std::get_if<std::int64_t>(&it->second))
      context = static_cast<std::size_t>(*n);
  // Replace measure_benchmark's counter lambda with this. Mirrors ChatRoute's
  // reasoning resolution and required/named client-tool prefill. The generated
  // workload is text-only, contains no history, and disables server search.
  std::string workload_error;
  const auto counter =
      [&](std::string_view body) -> std::optional<std::size_t> {
    const auto parsed = parse_chat_request(body, model->chat.schema);
    const auto* request = std::get_if<ChatCompletionRequest>(&parsed);
    if (!request) {
      const auto& rejected = std::get<ChatRequestError>(parsed);
      workload_error = "Benchmark request rejected";
      if (!rejected.param.empty()) workload_error += ": " + std::string{rejected.param};
      return std::nullopt;
    }
    const auto& chat = model->chat;
    const std::optional<std::string> effort_word =
        request->reasoning_effort || !request->json_object ||
                chat.profile == NativeChatProfile::glm53
            ? request->reasoning_effort
            : std::optional<std::string>{"none"};
    const auto minimax =
        minimax_request_thinking(effort_word, chat.minimax_effort_default);
    const bool thinking = [&] {
      switch (chat.profile) {
        case NativeChatProfile::qwen:
        case NativeChatProfile::deepseek:
          return effort_word ? *effort_word != "none" : chat.qwen_thinking;
        case NativeChatProfile::gemma4:
          return gemma4_request_thinks(effort_word,
                                       chat.gemma4_thinking_default);
        case NativeChatProfile::glm53:
          return false;
        case NativeChatProfile::raon1:
          return effort_word ? *effort_word != "none" : true;
        case NativeChatProfile::flint:
          return effort_word ? *effort_word != "none"
                             : chat.flint_thinking_default;
        case NativeChatProfile::minimax:
          return !minimax.off;
      }
      return false;
    }();
    auto level = chat.qwen_effort;
    if (effort_word == "low")
      level = QwenEffort::low;
    else if (effort_word == "medium")
      level = QwenEffort::medium;
    else if (effort_word == "high")
      level = QwenEffort::high;
    const QwenEffortPlacement effort{level, chat.effort_at_end};
    ChatGenerationPlanResult plan;
    if (request->client_tool_history ||
        (!request->client_tools.empty() && request->tool_choice != "none")) {
      const auto format = search_tool_format(chat.profile);
      if (!format) return std::nullopt;
      ClientToolTurn turn{.format = *format,
                          .declarations = {},
                          .parallel = request->parallel_tool_calls};
      if (request->tool_choice != "none")
        for (const auto& tool : request->client_tools)
          turn.declarations.push_back(tool.declaration);
      if (*format == ToolFormat::qwen || *format == ToolFormat::raon1 ||
          *format == ToolFormat::deepseek) {
        const auto named = std::find_if(
            turn.declarations.begin(), turn.declarations.end(),
            [&](const auto& tool) { return request->named_tool == tool.name; });
        const bool required =
            request->tool_choice == "required" && !turn.declarations.empty();
        if (named != turn.declarations.end() || required) {
          const bool dsml = *format == ToolFormat::deepseek;
          turn.prefix =
              dsml ? "<" + std::string(kDeepseekToolMarker) + " calls>\n<" +
                         std::string(kDeepseekToolMarker) + " invoke name=\""
                   : "<tool_call>\n<function=";
          const ToolDeclaration* fixed =
              named != turn.declarations.end() ? &*named
              : turn.declarations.size() == 1U ? &turn.declarations.front()
                                               : nullptr;
          if (fixed) turn.prefix += fixed->name + (dsml ? "\">\n" : ">\n");
        }
      }
      plan = client_tool_plan(chat, *request, model->tokenizer, turn, thinking,
                              effort, minimax);
    } else {
      plan = select_chat_generation_plan(
          chat.profile, *request, model->tokenizer, chat.max_prompt_tokens,
          chat.sampling_default, {}, thinking, effort, minimax,
          chat.embedded_template.get());
    }
    const auto* selected = std::get_if<ChatGenerationPlan>(&plan);
    if (!selected) workload_error = std::get<ChatGenerationPlanError>(plan).message;
    return selected
               ? std::optional<std::size_t>{selected->prompt_token_ids.size()}
               : std::nullopt;
  };

  CalibrationWorkloadRequest request{
      model->model_id,
      options.workload,
      context,
      model->chat.max_prompt_tokens,
      std::min<std::size_t>(trial.max_tokens,
                            model->chat.max_completion_tokens),
      options.custom_prompt,
      model->chat.schema.per_request_reasoning_effort};
  auto made = build_calibration_workload(request, counter);
  auto* cases = std::get_if<std::vector<CalibrationCase>>(&made);
  if (!cases)
    return fail(workload_error.empty() ? std::get<std::string>(made) : workload_error);
  if (options.memory_probe) {
    auto probe = build_calibration_memory_probe(request, counter);
    if (auto* c = std::get_if<CalibrationCase>(&probe))
      cases->push_back(std::move(*c));
    else
      return fail("The requested context probe could not be constructed");
  }
  if (!reused) {
    session_meter = std::make_shared<ServeMonitor>();
    auto plan = std::move(model->plan);
    session = std::make_shared<ModelRuntime>(std::move(*model), settings_.factory(std::move(plan)), settings_.gate,
                                             session_meter.get());
    session_signature = signature;
  }
  auto runtime = session;
  auto& meter = *session_meter;
  {
    std::lock_guard lock(mutex_);
    measuring_ = runtime;
  }
  if (!runtime->chat().set_calibration_tuning(tune_pcie, tune_min_p)) return fail("Live tuning is unsupported for this profile");
  if (!reused && !runtime->supervisor().start()) return fail("Engine did not start");
  for (;;) {
    if (stopped()) return fail("Cancelled or time budget exhausted");
    const auto state = runtime->supervisor().snapshot().state;
    if (state == SupervisorState::ready || state == SupervisorState::generating)
      break;
    if (state != SupervisorState::starting ||
        runtime->supervisor().generations_started() > 1)
      return fail("Engine failed before ready");
    (void)runtime->supervisor().wait_for_change(SupervisorState::starting,
                                                std::chrono::milliseconds(100));
  }
  if (runtime->supervisor().generations_started() != 1U) return fail("Engine restarted during the benchmark");
  const auto load_ms = std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - started)
                           .count();
  set(detail, "load_ms", number(reused ? 0.0 : load_ms));
  set(detail, "engine_reused", boolean(reused));
  std::vector<double> speeds, prompt_speeds, ttfts, elapsed, sustained_speeds;
  std::vector<std::string> warnings;
  bool accepted = true;
  std::string quality_reason;
  std::optional<double> cold_ttft, warm_ttft;
  double coverage = 1;
  const auto run_case = [&](const CalibrationCase& c, bool measured) -> bool {
    if (stopped()) return false;
    RequestContext ctx{.client_gone = stopped,
                       .shutting_down = runtime->stop_flag(),
                       .loopback_peer = true};
    auto answer = runtime->chat().handle(c.request_body, ctx);
    CalibrationQuality quality;
    if (const auto* response = std::get_if<HttpResponse>(&answer))
      quality =
          validate_calibration_response(c, response->status, response->body);
    else {
      CalibrationCapture capture{stopped};
      std::get<StreamHandler>(answer)(capture);
      quality = validate_calibration_response(
          c, capture.complete() ? capture.status() : 500, capture.body());
    }
    if (!quality.accepted) {
      accepted = false;
      quality_reason = quality.reason;
    }
    const auto m = meter.latest_generation_metrics();
    if (!m) return !quality.accepted;
    if (!reused && !cold_ttft)
      cold_ttft = m->ttft_ms;
    else if (!warm_ttft)
      warm_ttft = m->ttft_ms;
    if (measured && c.measured) {
      if (m->decode_tok_s) speeds.push_back(*m->decode_tok_s);
      if (m->prompt_tok_s) prompt_speeds.push_back(*m->prompt_tok_s);
      if (m->ttft_ms) ttfts.push_back(*m->ttft_ms);
      elapsed.push_back(m->elapsed_ms);
    }
    if (c.context_probe) {
      coverage = std::min(coverage, c.context_coverage);
      set(detail, "memory",
          J::make_object(
              {{"context_tokens", number(static_cast<double>(c.prompt_tokens))},
               {"target_context_tokens",
                number(static_cast<double>(c.target_prompt_tokens))},
               {"coverage", number(coverage)}}));
    }
    return true;
  };
  if (cases->empty() || (!reused && !run_case(cases->front(), false)))
    return fail("Warm-up did not complete");
  for (const auto& c : *cases)
    if (!run_case(c, true)) return fail("Workload generation did not complete");
  // Repeat the full workload in thorough mode to expose sustained and cache
  // behavior.
  const auto warm_speed = median_or_null(speeds);
  const auto warm_count = speeds.size();
  if (options.mode == BenchmarkMode::thorough)
    for (const auto& c : *cases)
      if (!run_case(c, true))
        return fail("Sustained workload did not complete");
  sustained_speeds.assign(
      speeds.begin() + static_cast<std::ptrdiff_t>(warm_count), speeds.end());
  const auto after = settings_.monitor ? settings_.monitor->latest_host_sample()
                                       : std::nullopt;
  auto diagnostics = calibration_host_diagnostics(
      before, after, std::chrono::steady_clock::now(), started, reused);
  warnings = diagnostics.flags;
  const auto gpu_after = settings_.benchmark_gpu ? settings_.benchmark_gpu(gpus)
                                                 : sample_calibration_gpu(gpus);
  warnings.insert(warnings.end(), gpu_before.notes.begin(),
                  gpu_before.notes.end());
  warnings.insert(warnings.end(), gpu_after.notes.begin(),
                  gpu_after.notes.end());
  if (gpu_before.compute_processes.value_or(0) > (reused ? 1U : 0U)) {
    diagnostics.contaminated = true;
    warnings.push_back(
        "Other GPU compute processes were present before this trial started");
  }
  if (gpu_before.thermal_throttled.value_or(false) ||
      gpu_after.thermal_throttled.value_or(false)) {
    diagnostics.contaminated = true;
    warnings.push_back(
        "GPU reported active thermal throttling; retry after cooling");
  }
  if (coverage < 0.95) {
    accepted = false;
    quality_reason =
        "Context probe did not cover the requested context; no memory-safety "
        "acceptance";
  }
  if (options.workload == BenchmarkWorkload::custom)
    warnings.push_back(
        "Custom workload received structural checks only; semantic accuracy is "
        "not established");
  if (!before || !after)
    warnings.push_back(
        "Host telemetry unavailable; interference cannot be ruled out");
  if (speeds.empty()) {
    if (!accepted || diagnostics.contaminated)
      return ExperimentSample{0, accepted, diagnostics.contaminated,
                              quality_reason, false};
    return fail("No valid generation timing was reported");
  }
  const double speed = median(speeds);
  double variance = 0;
  for (double s : speeds) variance += (s - speed) * (s - speed);
  variance /= static_cast<double>(speeds.size());
  set(detail, "decode_tok_s", number(speed));
  set(detail, "prompt_tok_s", median_or_null(prompt_speeds));
  set(detail, "ttft_ms", median_or_null(ttfts));
  set(detail, "elapsed_ms", median_or_null(elapsed));
  set(detail, "cold_ttft_ms", optional_number(cold_ttft));
  set(detail, "warm_ttft_ms", optional_number(warm_ttft));
  set(detail, "variation_pct",
      number(speed > 0 ? std::sqrt(variance) * 100 / speed : 0));
  set(detail, "warm_decode_tok_s", warm_speed);
  set(detail, "sustained_decode_tok_s", median_or_null(sustained_speeds));
  set(detail, "quality_pass", boolean(accepted));
  set(detail, "status",
      string(!accepted                  ? "quality_failed"
             : diagnostics.contaminated ? "contaminated"
                                        : "passed"));
  set(detail, "reason", string(quality_reason));
  set(detail, "warnings", strings(warnings));
  if (after && after->sample.memory) {
    auto mem = get(detail, "memory") ? *get(detail, "memory") : J{};
    if(after->sample.memory->used_bytes<=after->sample.memory->total_bytes)set(mem, "ram_free_bytes",
        number(static_cast<double>(after->sample.memory->total_bytes -
                                   after->sample.memory->used_bytes)));
    if (after->sample.gpu && after->sample.gpu->mem_total_bytes &&
        after->sample.gpu->mem_used_bytes &&
        *after->sample.gpu->mem_used_bytes<=*after->sample.gpu->mem_total_bytes)
      set(mem, "vram_free_bytes",
          number(static_cast<double>(*after->sample.gpu->mem_total_bytes -
                                     *after->sample.gpu->mem_used_bytes)));
    set(detail, "memory", std::move(mem));
  }
  return ExperimentSample{speed, accepted, diagnostics.contaminated,
                          quality_reason, false};
}

bool ModelLauncher::run_benchmark(const Job& job) {
  const auto options = *job.benchmark;
  ModelCard card;
  {
    std::unique_lock lock(mutex_);
    card = settings_.cards[job.card];
    switching_ = true;
    retire_running(lock);
    if (shutting_down_) return false;
    status_[job.card] = {CardState::calibrating, {}};
    calibrating_ = CalibrationRun{job.card, {}};
    calibration_errors_.erase(card.name);
  }
  const auto began = std::chrono::steady_clock::now();
  const auto elapsed = [&] {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                         began)
        .count();
  };
  const auto cancelled = [this] {
    std::lock_guard lock(mutex_);
    return shutting_down_ || unreaped_ || cancel_calibration_.load();
  };
  const auto stopped = [&] {
    return cancelled() || elapsed() >= options.budget_seconds;
  };
  auto base_values = job.values;
  if (options.compare_speed)
    for (const auto& spec : model_options_for(card.profile))
      if (spec.accuracy_tradeoff) {
        const auto v = parse(spec.accuracy_default);
        if (const auto* b = v.if_boolean())
          base_values[std::string{spec.name}] = *b;
        else if (const auto* n = v.if_integer())
          base_values[std::string{spec.name}] = *n;
        else if (const auto* d = v.if_decimal())
          base_values[std::string{spec.name}] = *d;
      }
  bool preparing_mtp = true;
  if (const auto it = base_values.find("mtp"); it != base_values.end())
    if (const auto* b = std::get_if<bool>(&it->second)) preparing_mtp = *b;
  auto preparing_plan = make_experiment_plan(card.profile, machine_, options,
      card_draft(settings_.main, card, base_values), preparing_mtp);
  if (options.compare_speed) {
    preparing_plan.steps.clear();
    for (const auto& spec : model_options_for(card.profile))
      if (spec.accuracy_tradeoff) preparing_plan.steps.push_back({std::string{spec.name}, {"candidate"}, {}, 128, 0});
  }
  {
    std::lock_guard lock(mutex_);
    if (calibrating_) calibrating_->progress = SweepProgress{
        "preparing", 1U, experiment_trial_upper_bound(preparing_plan), std::nullopt};
  }
  const auto base_key = calibration_key(card, base_values);
  std::vector<std::string> baseline_args;
  if (options.mode == BenchmarkMode::quick && !options.compare_speed)
    if (const auto previous = read_calibration(card.name);
        previous && compare_key(previous->key, base_key) == CalibrationState::fresh) baseline_args = previous->args;
  auto fast_values=base_values;
  fast_values["speed_fast_math"]=true;
  const auto comparison_runtime=options.compare_speed
      ? calibration_key(card,fast_values).runtime : std::string{};
  CalibrationResult identity_record{card.name, {}, 0, 0, "", false, base_key};
  const auto identity = to_hex(
      sha256("benchmark-workload-v3-warm-session\n" + render_calibration(identity_record) +
             render_remembered_options({{card.name, base_values}}) +
             python_json_text(strings(baseline_args)) +
             workload_name(options.workload) +
             (options.mode == BenchmarkMode::thorough ? "thorough" : "quick") +
             (options.memory_probe ? "memory" : "") +
             (options.compare_speed ? "comparison" : "") +
             comparison_runtime +
             to_hex(sha256(options.custom_prompt))));
  J status = J::make_object(
      {{"mode",
        string(options.mode == BenchmarkMode::thorough ? "thorough" : "quick")},
       {"workload", string(workload_name(options.workload))},
       {"paused", boolean(false)},
       {"notes",
        strings({"Quality gates check these tasks only; they do not certify "
                 "general answer accuracy.",
                 "Synthetic requests disable optional thinking when the selected model supports it, so the answer budget measures visible output.",
                 "Quick keeps one engine loaded and tunes only supported live controls. Thorough and explicit accuracy-tradeoff comparisons may restart the engine.",
                 "No conversation text or generated answers are saved."})}});
  const auto summary_file = artifact(card.name, "summary"),
             checkpoint_file = artifact(card.name, "checkpoint");
  const auto publish = [&] {
    std::lock_guard lock(mutex_);
    benchmark_status_[card.name] = python_json_text(status);
  };
  std::map<std::string, J> cache;
  std::vector<J> details;
  std::string error;
  if (base_key.runtime.empty() || base_key.model.empty())
    error =
        "Runtime or model identity could not be verified; previous calibration "
        "is "
        "unchanged";
  const double now = std::chrono::duration<double>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
  if (options.resume) {
    const auto saved = read(settings_.calibration_dir, checkpoint_file);
    const auto* entries = get(saved, "entries");
    if (text(saved, "identity") != identity || !entries ||
        !entries->if_array() || now - numeric(saved, "created", 0) > 86400 ||
        now < numeric(saved, "created", now))
      error =
          "Resume refused: no matching recent hardware, engine, model, options "
          "and workload checkpoint";
    else
      for (const auto& entry : *entries->if_array())
        cache.emplace(text(entry, "key"), entry);
  }
  const bool resume_refused = options.resume && !error.empty();
  const auto checkpoint = [&] {
    std::vector<J> entries;
    for (const auto& [key, value] : cache) {
      (void)key;
      entries.push_back(value);
    }
    return save(
        settings_.calibration_dir, checkpoint_file,
        J::make_object({{"identity", string(identity)},
                        {"created", number(now)},
                        {"entries", J::make_array(std::move(entries))}}));
  };
  std::shared_ptr<ModelRuntime> benchmark_session;
  std::shared_ptr<ServeMonitor> benchmark_meter;
  std::string benchmark_signature;
  Finally cleanup_session{[&] {
    if (benchmark_session) benchmark_session->supervisor().stop_engine();
    std::lock_guard lock(mutex_);
    if (benchmark_session && benchmark_session->supervisor().engine_left_unreaped()) unreaped_ = true;
    measuring_.reset();
  }};
  const auto measure =
      [&](const ExperimentTrial& trial) -> std::optional<ExperimentSample> {
    auto effective_trial = trial;
    effective_trial.args = overlay_args(baseline_args, trial.args);
    const auto key = trial_key(effective_trial);
    J detail;
    std::optional<ExperimentSample> sample;
    if (const auto it = cache.find(key);
        it != cache.end() && flag(it->second, "quality_ok") &&
        !flag(it->second, "contaminated") && numeric(it->second, "tok_s") > 0) {
      sample =
          ExperimentSample{numeric(it->second, "tok_s"), true, false, "", true};
      if (const auto* d = get(it->second, "detail")) detail = *d;
      set(detail, "reused", boolean(true));
    } else {
      sample =
          measure_benchmark(card, base_values, effective_trial, options, stopped, detail,
                            benchmark_session, benchmark_meter, benchmark_signature);
      if (sample)
        cache[key] =
            J::make_object({{"key", string(key)},
                            {"tok_s", number(sample->tok_s)},
                            {"quality_ok", boolean(sample->quality_ok)},
                            {"contaminated", boolean(sample->contaminated)},
                            {"detail", detail}});
      if (!checkpoint()) {
        error =
            "Checkpoint could not be saved; previous calibration is unchanged";
        return std::nullopt;
      }
    }
    set(detail, "number", number(static_cast<double>(details.size() + 1)));
    details.push_back(detail);
    set(status, "trials", J::make_array(details));
    set(status, "summary", detail);
    publish();
    return sample;
  };
  auto explicit_config = card_draft(settings_.main, card, base_values);
  bool mtp = true;
  if (const auto it = base_values.find("mtp"); it != base_values.end())
    if (const auto* b = std::get_if<bool>(&it->second)) mtp = *b;
  auto plan = make_experiment_plan(card.profile, machine_, options,
                                   explicit_config, mtp);
  if (options.compare_speed) {
    plan.steps.clear();
    plan.skipped.clear();
    for (const auto& spec : model_options_for(card.profile))
      if (spec.accuracy_tradeoff) {
        if(spec.name=="speed_fast_math" && comparison_runtime.empty()){
          plan.skipped.push_back({std::string{spec.name},"A verifiable fast-math runtime is unavailable"});continue;
        }
        if (spec.name == "speed_spec_prob" ||
            spec.name == "speed_ds4_dspark_sample") {
          plan.skipped.push_back({std::string{spec.name},
                                  "Inactive for these deterministic greedy "
                                  "workloads; requires sampled requests"});
          continue;
        }
        if (spec.name == "speed_route_resident_mtp" ||
            (!mtp && spec.name == "speed_glm_mtp_keep")) {
          plan.skipped.push_back(
              {std::string{spec.name},
               "Requires an active drafter and routing prerequisites not "
               "established by this comparison"});
          continue;
        }
        if (spec.name == "speed_ds4_yoco_keep") {
          plan.skipped.push_back(
              {std::string{spec.name},
               "A valid replay-window bound is model-specific; select it "
               "explicitly before benchmarking"});
          continue;
        }
        const auto baseline = parse(spec.accuracy_default);
        std::string candidate;
        if (const auto* b = baseline.if_boolean())
          candidate = *b ? "false" : "true";
        else if (spec.kind == ModelOptionKind::integer) {
          const auto* n = baseline.if_integer();
          if (!n) continue;
          const auto next = *n == spec.minimum ? std::min(spec.maximum, *n + 1)
                                               : std::max(spec.minimum, *n - 1);
          candidate = std::to_string(next);
        } else if (spec.kind == ModelOptionKind::decimal)
          candidate = "0.1";
        if (!candidate.empty())
          plan.steps.push_back(
              {"option:" + std::string{spec.name}, {candidate}, {}, 128, 0});
      }
    set(status, "notes",
        strings({"Comparison only: accuracy defaults are the baseline; "
                 "tradeoffs are explicitly tested, never applied to the card.",
                 "Passing these workload checks does not establish numerical "
                 "equivalence or general answer quality."}));
  }
  std::vector<J> skips;
  for (const auto& s : plan.skipped)
    skips.push_back(J::make_object(
        {{"setting", string(s.setting)}, {"reason", string(s.reason)}}));
  set(status, "skipped", J::make_array(std::move(skips)));
  const auto progress = [&](const ExperimentProgress& p) {
    set(status, "elapsed_s", number(p.elapsed_seconds));
    set(status, "remaining_s",
        p.completed ? number(p.estimated_remaining_seconds) : J{});
    set(status, "completed", number(static_cast<double>(p.completed)));
    set(status, "estimated_trials",
        number(static_cast<double>(p.trials_upper_bound)));
    publish();
    std::lock_guard lock(mutex_);
    if (calibrating_)
      calibrating_->progress = SweepProgress{
          p.stage, std::min(p.completed + 1, p.trials_upper_bound), p.trials_upper_bound, p.best_tok_s};
  };
  ExperimentOutcome outcome;
  if (error.empty())
    outcome = run_experiment(
        plan, options, measure, progress,
        [&] { return cancelled() || !error.empty(); }, elapsed);
  else
    outcome.end = ExperimentOutcome::End::failed;
  if (outcome.end == ExperimentOutcome::End::done) outcome.args = overlay_args(baseline_args, outcome.args);
  bool kept = false;
  if (compare_key(base_key, calibration_key(card, base_values)) !=
      CalibrationState::fresh ||
      (options.compare_speed && comparison_runtime != calibration_key(card,fast_values).runtime))
    error =
        "Hardware, runtime or model changed during the benchmark; previous "
        "calibration is retained";
  if (outcome.end == ExperimentOutcome::End::done && outcome.apply_allowed &&
      error.empty()) {
    if (const auto previous = read_calibration(card.name)) {
      ModelOptionValues old;
      {
        std::lock_guard lock(mutex_);
        if (const auto it = remembered_.find(card.name);
            it != remembered_.end())
          old = it->second;
      }
      archive_calibration(*previous, old, "Previous calibration");
    }
    const auto old = read_calibration(card.name);
    CalibrationResult result{
        card.name,
        outcome.args,
        outcome.baseline_tok_s,
        outcome.best_tok_s,
        utc_now(),
        job.auto_recalibrate.value_or(old ? old->auto_recalibrate : false),
        calibration_key(card, job.values)};
    kept = write_calibration(result);
    if (kept) {
      remember(job.card, job.values);
      archive_calibration(result, job.values,
                          options.label.empty() ? "Benchmark" : options.label);
    } else
      error =
          "The verified result could not be saved; previous calibration is "
          "retained";
  }
  const bool paused = outcome.end == ExperimentOutcome::End::cancelled ||
                      outcome.end == ExperimentOutcome::End::budget_exhausted;
  std::string end =
      kept     ? "applied"
      : paused ? "paused"
      : options.compare_speed && outcome.end == ExperimentOutcome::End::done && error.empty()
          ? "comparison_complete"
          : "not_applied";
  set(status, "paused", boolean(paused));
  set(status, "outcome", string(end));
  set(status, "elapsed_s", number(elapsed()));
  set(status, "remaining_s", J{});
  set(status, "before", number(outcome.baseline_tok_s));
  set(status, "after", number(outcome.best_tok_s));
  set(status, "settings", strings(outcome.args));
  set(status, "variation_pct", number(outcome.relative_variation * 100));
  set(status, "noisy", boolean(outcome.noisy));
  set(status, "retained_baseline", boolean(outcome.retained_baseline));
  if (outcome.retained_baseline)
    set(status, "rejected_candidate_variation_pct", number(outcome.rejected_candidate_variation * 100));
  if (outcome.end == ExperimentOutcome::End::done && error.empty()) {
    for (auto it = details.rbegin(); it != details.rend(); ++it) {
      const auto* args = get(*it, "settings");
      if (args &&
          python_json_text(*args) == python_json_text(strings(outcome.args)) &&
          text(*it, "status") == "passed") {
        auto summary = *it;
        set(summary, "decode_tok_s", number(outcome.best_tok_s));
        set(summary, "variation_pct", number(outcome.relative_variation * 100));
        set(status, "summary", std::move(summary));
        break;
      }
    }
  }
  std::vector<J> final_skips;
  for (const auto& s : outcome.skipped)
    final_skips.push_back(J::make_object(
        {{"setting", string(s.setting)}, {"reason", string(s.reason)}}));
  if (!final_skips.empty())
    set(status, "skipped", J::make_array(std::move(final_skips)));
  if (error.empty() && !kept && !options.compare_speed && !paused)
    error =
        "No stable, uncontaminated, quality-checked result was accepted; "
        "previous calibration retained";
  if (!error.empty()) set(status, "error", string(error));
  if (options.compare_speed) {
    std::vector<J> comparisons;
    for (const auto& d : details)
      comparisons.push_back(J::make_object(
          {{"label", string(text(d, "stage"))},
           {"summary", d},
           {"status", string(text(d, "status"))},
           {"reason", string(text(d, "reason"))},
           {"before", number(outcome.baseline_tok_s)},
           {"after", get(d, "decode_tok_s") ? *get(d, "decode_tok_s") : J{}}}));
    set(status, "comparison", J::make_array(std::move(comparisons)));
  }
  publish();
  (void)save(settings_.calibration_dir, summary_file, status);
  if (!paused && !resume_refused)
    (void)save(settings_.calibration_dir, checkpoint_file, J{});
  {
    std::lock_guard lock(mutex_);
    calibrating_.reset();
    cancel_calibration_.store(false);
    status_[job.card] = CardStatus{};
    if (!error.empty()) calibration_errors_[card.name] = error;
  }
  if (kept)
    alert("calibration_finished", card.name + " is calibrated",
          "Verified benchmark completed; metrics and history are on the model "
          "card.");
  return kept;
}

}  // namespace secure_frontend
