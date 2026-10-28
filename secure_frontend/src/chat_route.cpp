#include "secure_frontend/chat_route.hpp"

#include "secure_frontend/api_errors.hpp"
#include "secure_frontend/chat_generation_plan.hpp"
#include "secure_frontend/chat_stream.hpp"
#include "secure_frontend/lease_hold.hpp"
#include "secure_frontend/shutdown_signal.hpp"
#include "secure_frontend/token_generator.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <variant>
#include <vector>

namespace secure_frontend {
namespace {

// The Monitor's name for how a non-streamed generation ended (monitor_finish's literals).
std::string_view outcome_finish(const GenerationOutcome& outcome) {
  if (const auto* result = std::get_if<GenerationResult>(&outcome)) return result->finish == FinishReason::stop ? "stop" : "length";
  switch (std::get<GenerationFailure>(outcome)) {
    case GenerationFailure::cancelled: return "disconnect";
    case GenerationFailure::shutting_down: return "cancel";
    case GenerationFailure::engine_error:
    case GenerationFailure::engine_unavailable:
    case GenerationFailure::decode_failed: return "error";
  }
  return "error";
}

// A streamed generation tells the Monitor its end exactly once: from the loop, or, for a
// handler that never ran or never reached the loop, when the last copy of this goes.
struct MonitorTurn final {
  ServeMonitor* monitor{nullptr};
  bool ended{false};
  void end(const std::string_view finish) {
    if (monitor != nullptr && !ended) monitor->generation_ended(finish);
    ended = true;
  }
  MonitorTurn() = default;
  MonitorTurn(const MonitorTurn&) = delete;
  MonitorTurn& operator=(const MonitorTurn&) = delete;
  ~MonitorTurn() { end("disconnect"); }
};

}  // namespace

std::optional<std::string> make_completion_id() {
  std::array<unsigned char, 16> bytes{};
  if (!fill_random(bytes.data(), bytes.size())) return std::nullopt;
  static constexpr char kHex[] = "0123456789abcdef";
  std::string id{"chatcmpl-"};
  for (const unsigned char b : bytes) { id.push_back(kHex[b >> 4U]); id.push_back(kHex[b & 0x0fU]); }
  return id;
}

// Gemma 4 plan 1 Task 10 (spec §6.6, D10): none is off, low/medium/high are on, and a request
// that names no level takes the server's default (reasoning.effort; unset = off).
bool gemma4_request_thinks(const std::optional<std::string>& reasoning_effort, const bool thinking_default) {
  return reasoning_effort.has_value() ? *reasoning_effort != "none" : thinking_default;
}

std::optional<ToolFormat> search_tool_format(const NativeChatProfile profile) {
  switch (profile) {
    case NativeChatProfile::qwen: return ToolFormat::qwen;
    case NativeChatProfile::glm53: return ToolFormat::glm;
    case NativeChatProfile::raon1: return ToolFormat::raon1;  // raon-1-mini's template writes Qwen's calls
    case NativeChatProfile::flint: return ToolFormat::flint;  // flint's writes GLM's
    case NativeChatProfile::gemma4: return ToolFormat::gemma4;  // its own <|tool_call>call:NAME{...} (owner 2026-10-28)
    case NativeChatProfile::deepseek: return ToolFormat::deepseek;  // DSML, as Maya renders and reads it (owner 2026-10-28)
    case NativeChatProfile::minimax: break;  // (no tool form wired for it yet)
  }
  return std::nullopt;
}

MinimaxThinking minimax_request_thinking(const std::optional<std::string>& reasoning_effort,
                                         const std::optional<std::string>& effort_default) {
  const std::optional<std::string>& level = reasoning_effort.has_value() ? reasoning_effort : effort_default;
  if (!level.has_value()) return {};  // MiniMax's adaptive thinking, no effort named (Maya's "auto")
  if (*level == "none") return {.off = true};
  return {.effort = *level};
}

ChatGenerationPlanResult select_chat_generation_plan(const NativeChatProfile profile, const ChatCompletionRequest& request,
                                                     const TokenizerPack& tokenizer, const std::size_t maximum_prompt_tokens,
                                                     const SamplingDefaults& defaults,
                                                     const std::span<const std::size_t> image_token_counts, const bool thinking,
                                                     const QwenEffortPlacement qwen_effort, const MinimaxThinking& minimax) {
  // The route resolves the output budget itself (resolve_max_tokens), so the plans get 1.
  switch (profile) {
    case NativeChatProfile::qwen:
      return make_qwen_chat_generation_plan(request, tokenizer, maximum_prompt_tokens, 1U, defaults, image_token_counts, thinking,
                                            qwen_effort);
    case NativeChatProfile::glm53:
      return make_glm53_chat_generation_plan(request, tokenizer, maximum_prompt_tokens, 1U, defaults, image_token_counts);
    case NativeChatProfile::gemma4:
      // D2: text only. The schema's vision gate never opens for gemma4 (server_bootstrap), and
      // an image answer reaching here is a wiring bug, closed.
      if (!image_token_counts.empty()) {
        return ChatGenerationPlanError{ChatGenerationPlanErrorCode::invalid_request, "the Gemma 4 profile serves text only"};
      }
      return make_gemma4_chat_generation_plan(request, tokenizer, maximum_prompt_tokens, 1U, defaults, thinking);
    case NativeChatProfile::deepseek:
      // DeepSeek-V4.1 is text only (a GGUF pack has no vision tower); an image answer is a wiring bug. Thinking is the
      // effort line at the level's budget, chat mode without it.
      if (!image_token_counts.empty()) {
        return ChatGenerationPlanError{ChatGenerationPlanErrorCode::invalid_request, "the deepseek profile serves text only"};
      }
      return make_deepseek_chat_generation_plan(
          request, tokenizer, maximum_prompt_tokens, 1U, defaults,
          thinking ? deepseek_effort_budget(qwen_effort.effort == QwenEffort::low ? "low" : "high") : std::nullopt);
    case NativeChatProfile::raon1:
      if (!image_token_counts.empty()) {
        return ChatGenerationPlanError{ChatGenerationPlanErrorCode::invalid_request, "the raon1 profile serves text only"};
      }
      return make_raon1_chat_generation_plan(request, tokenizer, maximum_prompt_tokens, 1U, defaults, thinking);
    case NativeChatProfile::flint:
      if (!image_token_counts.empty()) {
        return ChatGenerationPlanError{ChatGenerationPlanErrorCode::invalid_request, "the flint profile serves text only"};
      }
      return make_flint_chat_generation_plan(request, tokenizer, maximum_prompt_tokens, 1U, defaults, thinking);
    case NativeChatProfile::minimax:
      // MiniMax-M3.1 from a GGUF pack is text only (Maya's text-only pack has no vision tower)
      if (!image_token_counts.empty()) {
        return ChatGenerationPlanError{ChatGenerationPlanErrorCode::invalid_request, "the minimax profile serves text only"};
      }
      return make_minimax_chat_generation_plan(request, tokenizer, maximum_prompt_tokens, 1U, defaults, minimax);
  }
  return ChatGenerationPlanError{ChatGenerationPlanErrorCode::invalid_request, "unknown chat profile"};
}

ChatRoute::ChatRoute(ChatRouteConfig config, SupervisorPort& port, GenerationGate& gate, const TokenizerPack& tokenizer)
    : config_(std::move(config)), port_(port), gate_(gate), tokenizer_(tokenizer) {}

HttpResponse ChatRoute::engine_unavailable() {
  const HealthSnapshot snapshot = port_.snapshot();
  if (snapshot.state == SupervisorState::shutting_down || snapshot.state == SupervisorState::stopped) {
    return make_error_response(ApiError::server_shutting_down);
  }
  return make_error_response(ApiError::engine_unavailable, {.retry_after = engine_retry_after(snapshot.recovery)});
}

HttpResponse ChatRoute::refusal(const GateRefusal refusal) {
  switch (refusal) {
    case GateRefusal::busy:
    case GateRefusal::timeout: return make_error_response(ApiError::server_busy);
    case GateRefusal::cancelled: return HttpResponse{.status = kNoResponseStatus, .headers = {}, .body = {}, .log_code = "client_gone"};
    case GateRefusal::engine_unavailable: return engine_unavailable();
    case GateRefusal::shutting_down: return make_error_response(ApiError::server_shutting_down);
  }
  return make_error_response(ApiError::internal_error);
}

RouteResult ChatRoute::handle(const std::string_view body, const RequestContext& context) {
  // SP5 M1 Task 6 (parity row idle_unload_s, pin :4100-4109 "without requests"): bracket
  // the request for the idle timer; both ends are no-ops when idle_unload is 0.
  port_.note_request_started();
  const struct RequestBracer {
    SupervisorPort& port;
    ~RequestBracer() { port.note_request_finished(); }
  } bracer{port_};
  // P4: schema, tokenization, budget (spec §5.4) - before any queue place is taken.
  const ChatRequestResult parsed = parse_chat_request(body, config_.schema);
  if (const auto* error = std::get_if<ChatRequestError>(&parsed)) return make_error_response(error->error, {.param = error->param});
  const ChatCompletionRequest& request = std::get<ChatCompletionRequest>(parsed);
  // SP5 M1 Task 8 (model.aliases; set_aliases :3764-3777): the served id plus the
  // configured aliases answer; matching is exact (§4: an empty or partial name never
  // matches — an empty request.model is refused by the schema before this line).
  if (request.model != config_.model_id &&
      std::find(config_.model_aliases.begin(), config_.model_aliases.end(), request.model) == config_.model_aliases.end())
    return make_error_response(ApiError::model_not_found);
  // Session log step 14: a request's own reasoning_effort (none closes the think block)
  // wins over the server's reasoning.effort; the schema admits it on the qwen and gemma4
  // profiles (Gemma 4 plan 1 Task 10: gemma4's default is off, D10; glm53 refuses it).
  const bool thinking = [&] {
    switch (config_.profile) {
      case NativeChatProfile::qwen:
        return request.reasoning_effort.has_value() ? *request.reasoning_effort != "none" : config_.qwen_thinking;
      case NativeChatProfile::gemma4: return gemma4_request_thinks(request.reasoning_effort, config_.gemma4_thinking_default);
      case NativeChatProfile::glm53: return false;  // the GLM profile has no thinking switch
      // raon1 (ChatML) honors reasoning_effort: none closes the think block (direct answer), any
      // level opens it; absent, KRAFTON's own default is thinking on.
      case NativeChatProfile::raon1:
        return request.reasoning_effort.has_value() ? *request.reasoning_effort != "none" : true;
      // flint: none closes the think block after the generation prompt, any level leaves it to the model; absent,
      // the server's reasoning.effort (unset: the template's own form, thinking).
      case NativeChatProfile::flint:
        return request.reasoning_effort.has_value() ? *request.reasoning_effort != "none" : config_.flint_thinking_default;
      // deepseek: none is chat mode, any level thinks (its effort below); absent, the server's reasoning.effort (the
      // card's thinking option, Maya's DS4_EFFORT high by default).
      case NativeChatProfile::deepseek:
        return request.reasoning_effort.has_value() ? *request.reasoning_effort != "none" : config_.qwen_thinking;
      // minimax: off only for none (the request's, else the card's); its level is minimax_thinking below
      case NativeChatProfile::minimax:
        return !minimax_request_thinking(request.reasoning_effort, config_.minimax_effort_default).off;
    }
    return false;
  }();
  const MinimaxThinking minimax_thinking = minimax_request_thinking(request.reasoning_effort, config_.minimax_effort_default);
  // Session log step 16: and its level writes the template's sentence (low, medium, high).
  QwenEffort level = config_.qwen_effort;
  if (request.reasoning_effort == "low") level = QwenEffort::low;
  else if (request.reasoning_effort == "medium") level = QwenEffort::medium;
  else if (request.reasoning_effort == "high") level = QwenEffort::high;
  const QwenEffortPlacement effort{level, config_.effort_at_end};
  // SP5 M1 Task 12 (C-9): the image half of P4/P6, in the pinned order.
  // image_urls is empty on every text request; then everything below it is inert and
  // the text path runs exactly as before.
  std::vector<std::string_view> image_urls;
  for (const auto& message : request.messages) {
    for (const auto& part : message.parts) {
      if (!part.image_data_url.empty()) image_urls.emplace_back(part.image_data_url);
    }
  }
  const bool vision_request = !image_urls.empty();
  // The schema gate (vision_capable) and this pointer are one boot-time decision
  // (Ruling T12-b); a mismatch is a wiring bug — closed, not served.
  if (vision_request && config_.vision == nullptr) return make_error_response(ApiError::invalid_request);
  // Every scratch file the request wrote is deleted before handle() returns: no LRU in
  // M1 (Ruling T12-d). The combined GENI file excepted — it follows the generation
  // itself, because the engine consumes it at generation start (streamed or not).
  std::vector<std::filesystem::path> scratch_files;
  const struct ScratchCleaner {
    VisionService* vision;
    const std::vector<std::filesystem::path>* files;
    ~ScratchCleaner() {
      if (vision == nullptr) return;
      for (const auto& file : *files) vision->discard(file);
    }
  } cleaner{config_.vision, &scratch_files};
  // T12-e: decoding, caps and staging happen BEFORE the queue place — a shape refusal
  // must not consume one. The ENC round trips wait for the lease, so encodes enter the
  // same FIFO as generations (the pin's single queue, serve/server.py:4489).
  std::vector<VisionService::Staged> staged_images;
  staged_images.reserve(image_urls.size());
  for (const std::string_view url : image_urls) {
    auto staged = config_.vision->stage(url);
    if (std::holds_alternative<ImagePayloadReason>(staged))
      return make_error_response(ApiError::unsupported_parameter, {.param = "image_url"});
    auto& entry = staged_images.emplace_back(std::get<VisionService::Staged>(std::move(staged)));
    scratch_files.push_back(entry.path);
  }

  // Web search (spec 2026-10-27-web-search-design §2): a qwen, GLM-5.3, raon1 or flint text chat gets the server's
  // tool, when there is one now, in its family's form - unless the request turned it off ("strata_search": false).
  std::shared_ptr<SearchTurn> search;
  const std::optional<ToolFormat> tool_format = search_tool_format(config_.profile);
  if (config_.tools != nullptr && tool_format && !vision_request && request.strata_search != false) {
    if (auto offer = config_.tools->offer()) {
      search = std::make_shared<SearchTurn>(SearchTurn{request, std::move(*offer), thinking, effort, *tool_format});
    }
  }

  // P4 again: a text plan is complete before the gate; an image plan needs the
  // encoder's per-image token counts, so only its budget pre-check waits (T12-e).
  std::optional<ChatGenerationPlan> plan;
  std::vector<std::uint32_t> prompt_ids;
  if (!vision_request) {
    // Gemma 4 plan 1 Tasks 3 and 10: an exhaustive switch, so a profile never takes another's plan.
    auto plan_result = search != nullptr ? search_first_plan(config_, *search, tokenizer_)
                                         : select_chat_generation_plan(config_.profile, request, tokenizer_, config_.max_prompt_tokens,
                                                                       config_.sampling_default, {}, thinking, effort, minimax_thinking);
    // A pack without the family's tool tags serves the chat without the tool, and the server log says so.
    if (const auto* refused = std::get_if<ChatGenerationPlanError>(&plan_result);
        search != nullptr && refused != nullptr && refused->code == ChatGenerationPlanErrorCode::tokenization_failed) {
      config_.tools->served_without(refused->message);
      search.reset();
      plan_result = select_chat_generation_plan(config_.profile, request, tokenizer_, config_.max_prompt_tokens, config_.sampling_default, {},
                                                thinking, effort, minimax_thinking);
    }
    if (const auto* plan_error = std::get_if<ChatGenerationPlanError>(&plan_result)) {
      return make_error_response(plan_error->code == ChatGenerationPlanErrorCode::prompt_too_large ? ApiError::context_length_exceeded
                                                                                                   : ApiError::invalid_request);
    }
    plan = std::get<ChatGenerationPlan>(std::move(plan_result));
    prompt_ids.reserve(plan->prompt_token_ids.size());
    for (const std::size_t id : plan->prompt_token_ids) {
      if (id > std::numeric_limits<std::uint32_t>::max()) return make_error_response(ApiError::invalid_request);
      prompt_ids.push_back(static_cast<std::uint32_t>(id));
    }
    if (!resolve_max_tokens(request.max_tokens, prompt_ids.size(), last_context_.load(), config_.max_completion_tokens,
                            config_.default_max_tokens, config_.fit_max_tokens)) {
      return make_error_response(ApiError::context_length_exceeded);
    }
  }

  // P5: the gate.
  auto admitted = gate_.admit(port_, std::chrono::steady_clock::now() + config_.queue_timeout, context.client_gone);
  if (const auto* refused = std::get_if<GateRefusal>(&admitted)) return refusal(*refused);
  std::optional<Lease> lease;
  lease.emplace(std::get<Lease>(std::move(admitted)));

  // The image plan, inside the lease and BEFORE the GENI write: encoder answers in part
  // order, the request's .sve files combine into one file (combined_embeddings_path
  // family, pin :158/:175), and the prompt gains one vision-marker run per image.
  std::string embeddings;
  std::optional<std::filesystem::path> combined_file;
  if (vision_request) {
    std::vector<VisionService::Encoded> encoded;
    encoded.reserve(staged_images.size());
    for (const auto& staged : staged_images) {
      auto answer = config_.vision->encode(staged);
      if (const auto* failure = std::get_if<VisionService::EncodeFailure>(&answer)) {
        lease.reset();
        gate_.release();
        // Ruling T12-h: a dead or un-wakeable encoder fails THIS request 503 (text
        // serving is untouched); an ERR answer is the image itself — the fixed 400.
        if (failure->kind == VisionEncodeError::Kind::not_running || failure->kind == VisionEncodeError::Kind::wake_failed ||
            failure->kind == VisionEncodeError::Kind::no_reply)
          return engine_unavailable();
        return make_error_response(ApiError::unsupported_parameter, {.param = "image_url"});
      }
      auto& entry = encoded.emplace_back(std::get<VisionService::Encoded>(std::move(answer)));
      scratch_files.push_back(entry.path);  // combine() removes these; the cleaner catches a combine failure
    }
    const auto combined = config_.vision->combine(encoded);
    if (!combined.has_value()) {
      lease.reset();
      gate_.release();
      return make_error_response(ApiError::internal_error);
    }
    combined_file = *combined;
    embeddings = combined->string();  // absolute, and the boot guard (T12-j) keeps it space-free (#480)
    std::vector<std::size_t> image_counts;
    image_counts.reserve(encoded.size());
    for (const auto& entry : encoded) image_counts.push_back(entry.image_tokens);
    // Gemma 4 plan 1 Task 10: the image path selects through the same switch (only qwen takes images).
    const auto plan_result = select_chat_generation_plan(config_.profile, request, tokenizer_, config_.max_prompt_tokens,
                                                         config_.sampling_default, image_counts, thinking, effort, minimax_thinking);
    if (const auto* plan_error = std::get_if<ChatGenerationPlanError>(&plan_result)) {
      config_.vision->discard(*combined_file);
      lease.reset();
      gate_.release();
      return make_error_response(plan_error->code == ChatGenerationPlanErrorCode::prompt_too_large ? ApiError::context_length_exceeded
                                                                                                   : ApiError::invalid_request);
    }
    plan = std::get<ChatGenerationPlan>(std::move(plan_result));
    prompt_ids.reserve(plan->prompt_token_ids.size());
    for (const std::size_t id : plan->prompt_token_ids) {
      if (id > std::numeric_limits<std::uint32_t>::max()) {
        config_.vision->discard(*combined_file);
        lease.reset();
        gate_.release();
        return make_error_response(ApiError::invalid_request);
      }
      prompt_ids.push_back(static_cast<std::uint32_t>(id));
    }
  }

  // P6 through the lease; the context rule is re-checked against this generation's READY.
  GenerationOutcome outcome = GenerationFailure::engine_unavailable;
  bool context_refused = false;
  const std::size_t context_tokens = lease->chat().context_tokens();
  last_context_ = context_tokens;
  const auto budget = resolve_max_tokens(request.max_tokens, prompt_ids.size(), context_tokens, config_.max_completion_tokens,
                                         config_.default_max_tokens, config_.fit_max_tokens, config_.minimum_output_tokens,
                                         config_.maximum_output_tokens);
  if (budget && request.stream)
    return stream_response(lease, prompt_ids, *budget, *plan, request.include_usage, context,
                           std::move(embeddings), combined_file, search);
  std::vector<std::string> search_events;  // web search: message.strata_search
  if (!budget) {
    context_refused = true;
  } else {
    if (config_.monitor != nullptr) config_.monitor->generation_started(prompt_ids.size(), *budget, context_tokens, config_.model_id);
    outcome = search != nullptr
                  ? run_search_rounds(*lease, *search, *plan, prompt_ids, *budget, context, search_events)
                  : lease->chat().run_generation(
                        prompt_ids,
                        {.max_tokens = *budget, .sampling = plan->sampling, .embeddings = embeddings, .think_tokens = plan->think_tokens,
                         .starts_in_reasoning = plan->starts_in_reasoning, .stop_tokens = plan->stop_tokens,
                         .channel_name = plan->channel_name, .server_stop_tokens = plan->server_stop_tokens,
                         .keeps_newlines_after_think = plan->keeps_newlines_after_think, .meter = config_.monitor},
                        {.cancelled = context.client_gone, .shutting_down = context.shutting_down,
                         .shutdown_requested = [] { return shutdown_signalled(); }},
                        tokenizer_);
    if (config_.monitor != nullptr) config_.monitor->generation_ended(outcome_finish(outcome));
    // Ruling R-H: never report_fatal once shutdown has begun (pool flag or SP2 signal).
    const bool shutting = (context.shutting_down != nullptr && context.shutting_down->load()) || shutdown_signalled();
    if (lease->protocol().closed() && !shutting) port_.report_fatal(*lease, kill_reason_for(lease->protocol()));
  }
  lease.reset();     // SP2: generating -> ready
  gate_.release();   // only now may the next head proceed
  if (combined_file.has_value()) config_.vision->discard(*combined_file);  // DONE returned: the engine has read it

  if (context_refused) return make_error_response(ApiError::context_length_exceeded);
  if (const auto* failure = std::get_if<GenerationFailure>(&outcome)) {
    switch (*failure) {
      case GenerationFailure::cancelled: return HttpResponse{.status = kNoResponseStatus, .headers = {}, .body = {}, .log_code = "client_gone"};
      case GenerationFailure::engine_error: return make_error_response(ApiError::engine_error);
      case GenerationFailure::engine_unavailable: return engine_unavailable();
      case GenerationFailure::decode_failed: {
        // Issue 20: the client sees the generic 500; the log says why (log-only code).
        HttpResponse refused = make_error_response(ApiError::internal_error);
        refused.log_code = "undecodable_token";
        return refused;
      }
      case GenerationFailure::shutting_down: return make_error_response(ApiError::server_shutting_down);
    }
  }
  const GenerationResult& result = std::get<GenerationResult>(outcome);
  const auto id = make_completion_id();
  if (!id) return make_error_response(ApiError::internal_error);
  const auto created = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  const auto rendered = render_chat_completion({.id = *id, .created = created, .model = config_.model_id, .text = result.text,
                                                .finish_reason = result.finish == FinishReason::length ? "length" : "stop",
                                                .prompt_tokens = result.prompt_tokens, .completion_tokens = result.completion_tokens,
                                                .reasoning = result.reasoning, .search_events = search_events},
                                               config_.schema);
  if (!rendered) return make_error_response(ApiError::internal_error);
  HttpResponse response{.status = 200, .headers = {{"Content-Type", "application/json; charset=utf-8"}}, .body = *rendered};
  if (result.timed_out) response.log_code = "generation_timeout";   // §8 log-only code; the client sees 200 length (B4)
  return response;
}

RouteResult ChatRoute::stream_response(std::optional<Lease>& lease, const std::vector<std::uint32_t>& prompt_ids, const std::size_t max_tokens,
                                       const ChatGenerationPlan& plan, const bool include_usage, const RequestContext& context,
                                       std::string embeddings, std::optional<std::filesystem::path> combined_file,
                                       std::shared_ptr<const SearchTurn> search) {
  const std::atomic<bool>* flag = context.shutting_down;
  std::function<bool()> shutting_down = [flag] { return (flag != nullptr && flag->load()) || shutdown_signalled(); };  // R-H
  auto hold = std::make_shared<LeaseHold>(std::move(*lease), gate_, port_, shutting_down);
  lease.reset();
  auto id = make_completion_id();
  if (!id) {
    hold->end_generation();
    return make_error_response(ApiError::internal_error);
  }
  auto turn = std::make_shared<MonitorTurn>();
  turn->monitor = config_.monitor;
  if (config_.monitor != nullptr) {
    config_.monitor->generation_started(prompt_ids.size(), max_tokens, hold->lease().chat().context_tokens(), config_.model_id);
  }
  if (const auto failure =
          hold->lease().chat().begin(prompt_ids, {.max_tokens = max_tokens, .sampling = plan.sampling, .embeddings = embeddings})) {
    turn->end("error");
    hold->end_generation();  // reports a closed protocol through the port
    if (combined_file.has_value()) config_.vision->discard(*combined_file);
    return *failure == GenerationFailure::engine_error ? make_error_response(ApiError::engine_error) : engine_unavailable();
  }
  const auto created = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  SseIdentity identity{std::move(*id), static_cast<std::int64_t>(created), config_.model_id};
  ChatStreamSettings settings;
  settings.run = hold->lease().chat().drive_limits(max_tokens);
  settings.run.think_tokens = plan.think_tokens;
  settings.run.starts_in_reasoning = plan.starts_in_reasoning;
  settings.run.stop_tokens = plan.stop_tokens;
  settings.run.channel_name = plan.channel_name;  // Gemma 4 plan 1 Task 9
  settings.run.server_stop_tokens = plan.server_stop_tokens;  // DeepSeek's turn ends (the frontend stops the engine)
  settings.run.keeps_newlines_after_think = plan.keeps_newlines_after_think;  // MiniMax's, as Maya's parser
  settings.run.literal_tokens = plan.literal_tokens;  // web search: the tool tags as text, for the call parser
  settings.run.meter = config_.monitor;
  settings.write_stall = config_.write_stall;
  settings.tail_deadline = config_.response_write_deadline;
  settings.keepalive = config_.stream_keepalive;
  settings.include_usage = include_usage;
  settings.prompt_tokens = prompt_ids.size();
  const TokenizerPack* tokenizer = &tokenizer_;
  std::optional<SearchLimits> rounds_limits;
  if (search != nullptr) rounds_limits = search_limits(config_);
  return StreamHandler{[hold, identity = std::move(identity), settings, tokenizer, shutting_down, vision = config_.vision,
                      combined_file, turn, search, rounds_limits](StreamSink& sink) {
    // The engine consumed the GENI file at generation start; if it never began, the
    // begin() failure above already removed it. Either way it must not outlive the stream.
    const struct FileGuard {
      VisionService* vision;
      std::optional<std::filesystem::path> file;
      ~FileGuard() { if (vision != nullptr && file.has_value()) vision->discard(*file); }
    } file_guard{vision, combined_file};
    const TokenByteDecoder decoder(*tokenizer);
    // Web search: the rounds share this stream and the lease (each later round begins on it).
    std::unique_ptr<StreamRounds> rounds;
    ChatStreamSettings stream_settings = settings;
    if (search != nullptr && rounds_limits.has_value()) {
      rounds = make_search_stream_rounds(*rounds_limits, search, *tokenizer, hold->lease().chat(), identity);
      stream_settings.rounds = rounds.get();
    }
    const ChatStreamOutcome outcome =
        run_chat_stream(sink, hold->lease().protocol(), decoder, identity, stream_settings, shutting_down,
                        [&hold, &turn](const GenerationRunResult& run) {
                          turn->end(monitor_finish(run.end, run.cancel));
                          hold->end_generation();
                        });
    sink.set_log_code(outcome.log_code);
  }};
}

}  // namespace secure_frontend
