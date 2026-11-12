// Web search (docs/superpowers/specs/2026-10-27-web-search-design.md §2): the chat route's search loop - upstream's
// run_with_mcp (serve/server.py) over our engine lease: each round's reply through the call parser, the calls run by
// the server's tool, the round and its results rendered into the next prompt, at most search.max_rounds times (then
// one round to answer, SearchRounds::after).

#include "secure_frontend/chat_route.hpp"
#include "secure_frontend/embedded_template_plan.hpp"

#include "secure_frontend/chat_generation_plan.hpp"
#include "secure_frontend/chat_stream.hpp"
#include "secure_frontend/prompt_builder.hpp"
#include "secure_frontend/search_rounds.hpp"
#include "secure_frontend/shutdown_signal.hpp"

#include <algorithm>
#include <limits>
#include <utility>
#include <variant>

namespace secure_frontend {
namespace {

// A round's prompt: the conversation, the tool and the rounds so far, in the family's form.
ChatGenerationPlanResult tool_plan(const SearchLimits& limits, const SearchTurn& turn, const TokenizerPack& tokenizer,
                                   const std::span<const std::string> tools, const std::span<const ToolRound> rounds) {
  if (limits.embedded_template) {
    auto fixed = limits;
    fixed.embedded_template.reset();
    fixed.max_prompt_tokens = 1048576U;
    auto skeleton = turn;
    skeleton.request.messages = {{"user", "a"}};
    const std::vector<std::string> metadata_tools{R"({"name":"tool","parameters":{"type":"object"}})"};
    auto baseline = tool_plan(fixed, skeleton, tokenizer, metadata_tools, {});
    const auto effort = turn.minimax.effort.value_or(turn.effort.effort == QwenEffort::low ? "low" :
                                                   turn.effort.effort == QwenEffort::medium ? "medium" : "high");
    return apply_embedded_template(std::move(baseline), *limits.embedded_template, turn.request, tokenizer, tools, rounds,
                                  limits.max_prompt_tokens, turn.thinking, effort);
  }
  switch (turn.format) {
    case ToolFormat::glm:  // GLM-5.3 always thinks (its template opens <think>), as the glm53 profile does
      return make_glm53_tool_generation_plan(turn.request, tokenizer, tools, rounds, limits.max_prompt_tokens, 1U, limits.sampling);
    case ToolFormat::raon1:
      return make_raon1_tool_generation_plan(turn.request, tokenizer, tools, rounds, limits.max_prompt_tokens, 1U, limits.sampling,
                                             turn.thinking);
    case ToolFormat::flint:
      return make_flint_tool_generation_plan(turn.request, tokenizer, tools, rounds, limits.max_prompt_tokens, 1U, limits.sampling,
                                             turn.thinking);
    case ToolFormat::gemma4:
      return make_gemma4_tool_generation_plan(turn.request, tokenizer, tools, rounds, limits.max_prompt_tokens, 1U, limits.sampling,
                                              turn.thinking);
    case ToolFormat::deepseek:  // thinking is the effort line at the level's budget, as the text plan's (select_chat_generation_plan)
      return make_deepseek_tool_generation_plan(
          turn.request, tokenizer, tools, rounds, limits.max_prompt_tokens, 1U, limits.sampling,
          turn.thinking ? deepseek_effort_budget(turn.effort.effort == QwenEffort::low ? "low" : "high") : std::nullopt);
    case ToolFormat::minimax:  // its thinking as the text plan's (minimax_request_thinking)
      return make_minimax_tool_generation_plan(turn.request, tokenizer, tools, rounds, limits.max_prompt_tokens, 1U, limits.sampling,
                                               turn.minimax);
    case ToolFormat::qwen: break;
  }
  return make_qwen_tool_generation_plan(turn.request, tokenizer, tools, rounds, limits.max_prompt_tokens, 1U, limits.sampling,
                                        turn.thinking, turn.effort);
}

ChatGenerationPlanResult search_plan(const SearchLimits& limits, const SearchTurn& turn, const TokenizerPack& tokenizer,
                                     const std::span<const ToolRound> rounds) {
  const std::vector<std::string> tools{turn.offer.template_json};
  return tool_plan(limits, turn, tokenizer, tools, rounds);
}

std::optional<std::vector<std::uint32_t>> wire_ids(const std::vector<std::size_t>& ids) {
  std::vector<std::uint32_t> out;
  out.reserve(ids.size());
  for (const std::size_t id : ids) {
    if (id > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
    out.push_back(static_cast<std::uint32_t>(id));
  }
  return out;
}

// A later round's output budget, by the first round's rules (resolve_max_tokens), against this engine's context.
std::optional<std::size_t> round_budget(const SearchLimits& limits, const SearchTurn& turn, const std::size_t prompt_tokens,
                                        const std::size_t context_tokens) {
  return resolve_max_tokens(turn.request.max_tokens, prompt_tokens, context_tokens, limits.max_completion_tokens, limits.default_max_tokens,
                            limits.fit_max_tokens, limits.minimum_output_tokens, limits.maximum_output_tokens);
}

// The drive limits of a round: the service's, and the plan's reasoning split, turn ends and tool tags.
GenerationRunLimits round_limits(const ChatExecutionService& chat, const ChatGenerationPlan& plan, const std::size_t max_tokens,
                                 GenerationMeter* meter) {
  GenerationRunLimits limits = chat.drive_limits(max_tokens);
  limits.think_tokens = plan.think_tokens;
  limits.starts_in_reasoning = plan.starts_in_reasoning;
  limits.keeps_newlines_after_think = plan.keeps_newlines_after_think;
  limits.stop_tokens = plan.stop_tokens;
  limits.server_stop_tokens = plan.server_stop_tokens;
  limits.channel_name = plan.channel_name;
  limits.literal_tokens = plan.literal_tokens;
  limits.meter = meter;
  return limits;
}

bool shutting(const ExecutionControl& control) {
  return (control.shutting_down != nullptr && control.shutting_down->load()) || (control.shutdown_requested && control.shutdown_requested());
}

// The complete path's client: the answer's text and reasoning, and the request's cancel and the shutdown as
// ChatExecutionService::run_generation watches them.
struct Collector final : GenerationObserver {
  explicit Collector(const ExecutionControl& control) : control_(control) {}
  void on_text(const std::string_view piece) override { text += piece; }
  void on_reasoning(const std::string_view piece) override { reasoning += piece; }
  RunCancel on_tick(bool) override {
    if (shutting(control_)) return RunCancel::shutting_down;
    if (control_.cancelled && control_.cancelled()) return RunCancel::client_gone;
    return RunCancel::none;
  }
  std::string text;
  std::string reasoning;
  const ExecutionControl& control_;
};

// run_generation's mapping of a run that did not end as stop or length.
GenerationFailure failure_of(const GenerationRunResult& run, const ExecutionControl& control) {
  switch (run.end) {
    case RunEnd::cancelled: return run.cancel == RunCancel::shutting_down ? GenerationFailure::shutting_down : GenerationFailure::cancelled;
    case RunEnd::engine_error: return GenerationFailure::engine_error;
    case RunEnd::undecodable: return GenerationFailure::decode_failed;
    case RunEnd::stop:
    case RunEnd::length:
    case RunEnd::engine_unavailable: break;
  }
  return shutting(control) ? GenerationFailure::shutting_down : GenerationFailure::engine_unavailable;
}

// The stream's rounds: each round's observer is the call parser before the stream's own, and after a round the calls
// run, their events go out as strata_search chunks, and the next round begins on the same lease.
class SearchStreamRounds final : public StreamRounds {
 public:
  SearchStreamRounds(SearchLimits limits, std::shared_ptr<const SearchTurn> turn, const TokenizerPack& tokenizer, ChatExecutionService& chat,
                     SseIdentity identity)
      : limits_(std::move(limits)), turn_(std::move(turn)), tokenizer_(tokenizer), chat_(chat), identity_(std::move(identity)),
        rounds_(*limits_.tools, turn_->offer, limits_.rounds) {}

  GenerationObserver& observer(GenerationObserver& stream) override {
    round_.emplace(stream, std::vector<ToolDeclaration>{turn_->offer.declaration}, turn_->format);
    return *round_;
  }

  Next next(const GenerationRunResult& run, const std::function<bool()>& alive,
            const std::function<bool(std::string_view)>& event) override {
    if (!round_.has_value()) return {};
    round_->finish(run.end == RunEnd::stop);
    if (run.end != RunEnd::stop && run.end != RunEnd::length) return {};  // the run's own end stands
    bool sent = true;
    const auto step = rounds_.after(*round_, run.end, [&alive] { return !alive(); }, [&](std::string text) {
      sent = sent && event(render_search_event(identity_, text));
    });
    if (step == SearchRounds::Next::done) return {};
    if (step == SearchRounds::Next::cancelled || !sent) return ended(run, RunEnd::cancelled, RunCancel::client_gone);
    // The next prompt does not fit, or its room is gone: the answer ends here, cut.
    const auto planned = search_plan(limits_, *turn_, tokenizer_, rounds_.rounds());
    const auto* plan = std::get_if<ChatGenerationPlan>(&planned);
    const auto ids = plan != nullptr ? wire_ids(plan->prompt_token_ids) : std::nullopt;
    const auto budget = ids ? round_budget(limits_, *turn_, ids->size(), chat_.context_tokens()) : std::nullopt;
    if (!budget) return ended(run, RunEnd::length, RunCancel::none);
    if (const auto failure = chat_.begin(*ids, {.max_tokens = *budget, .sampling = plan->sampling})) {
      return ended(run, *failure == GenerationFailure::engine_error ? RunEnd::engine_error : RunEnd::engine_unavailable, RunCancel::none);
    }
    return Next{round_limits(chat_, *plan, *budget, limits_.monitor), ids->size(), std::nullopt};
  }

 private:
  static Next ended(const GenerationRunResult& run, const RunEnd end, const RunCancel cancel) {
    GenerationRunResult out = run;
    out.end = end;
    out.cancel = cancel;
    return Next{std::nullopt, 0U, out};
  }

  SearchLimits limits_;
  std::shared_ptr<const SearchTurn> turn_;
  const TokenizerPack& tokenizer_;
  ChatExecutionService& chat_;
  SseIdentity identity_;
  SearchRounds rounds_;
  std::optional<ToolCallObserver> round_;
};

class ClientToolStream final : public StreamRounds {
 public:
  ClientToolStream(ClientToolTurn turn, const SseIdentity& identity) : turn_(std::move(turn)), identity_(identity) {}

  void set_event_queue(std::function<void(std::string)> enqueue) override { enqueue_ = std::move(enqueue); }

  GenerationObserver& observer(GenerationObserver& stream) override {
    parser_.emplace(stream, turn_.declarations, turn_.format, [this](const ToolCall& call) { queue_call(call); });
    if (!turn_.prefix.empty()) parser_->on_text(turn_.prefix);
    return *parser_;
  }

  bool ended_with_tool_calls() const override { return count_ != 0U; }

  Next next(const GenerationRunResult& run, const std::function<bool()>&,
            const std::function<bool(std::string_view)>&) override {
    if (run.end == RunEnd::stop || run.end == RunEnd::length) parser_->finish(run.end == RunEnd::stop);
    return {};
  }

 private:
  void queue_call(const ToolCall& call) {
    if (count_ >= (turn_.parallel ? kMaxClientToolCalls : 1U)) return;
    const auto index = count_++;
    enqueue_(render_tool_call_event(identity_, index, make_tool_call_id(identity_.id, index), call.name, ""));
    const std::string arguments = tool_arguments_json(call);
    for (std::string_view remaining = arguments; !remaining.empty();) {
      const auto size = next_delta_size(remaining);
      enqueue_(render_tool_arguments_event(identity_, index, remaining.substr(0, size)));
      remaining.remove_prefix(size);
    }
  }

  ClientToolTurn turn_;
  SseIdentity identity_;
  std::optional<ToolCallObserver> parser_;
  std::function<void(std::string)> enqueue_;
  std::size_t count_{0};
};

}  // namespace

SearchLimits search_limits(const ChatRouteConfig& config) {
  return SearchLimits{.max_prompt_tokens = config.max_prompt_tokens, .max_completion_tokens = config.max_completion_tokens,
                      .default_max_tokens = config.default_max_tokens, .fit_max_tokens = config.fit_max_tokens,
                      .minimum_output_tokens = config.minimum_output_tokens, .maximum_output_tokens = config.maximum_output_tokens,
                      .sampling = config.sampling_default, .tools = config.tools, .rounds = config.tool_rounds, .monitor = config.monitor,
                      .embedded_template = config.embedded_template};
}

ChatGenerationPlanResult search_first_plan(const ChatRouteConfig& config, const SearchTurn& turn, const TokenizerPack& tokenizer) {
  return search_plan(search_limits(config), turn, tokenizer, {});
}

ChatGenerationPlanResult client_tool_plan(const ChatRouteConfig& config, const ChatCompletionRequest& request,
    const TokenizerPack& tokenizer, const ClientToolTurn& client, const bool thinking, const QwenEffortPlacement effort,
    const MinimaxThinking& minimax) {
  SearchTurn turn{.request = request, .offer = {}, .thinking = thinking && client.prefix.empty(), .effort = effort,
                  .format = client.format, .minimax = minimax};
  std::vector<std::string> tools;
  if (request.tool_choice != "none") {
    for (const auto& tool : request.client_tools) tools.push_back(tool.template_json);
  }
  std::vector<ToolRound> trailing;
  if (request.client_tool_history && turn.request.messages.back().role == "assistant" &&
      turn.request.messages.back().content.empty()) {
    trailing = std::move(turn.request.messages.back().rounds);
    turn.request.messages.pop_back();
  }
  auto result = tool_plan(search_limits(config), turn, tokenizer, tools, trailing);
  auto* plan = std::get_if<ChatGenerationPlan>(&result);
  if (plan == nullptr || client.prefix.empty()) return result;
  const auto opening = std::string_view(client.prefix);
  const auto marker = client.format == ToolFormat::deepseek ? kDeepseekToolMarker : std::string_view{"<tool_call>"};
  PromptBuilder prefix;
  if (std::any_of(plan->literal_tokens.begin(), plan->literal_tokens.end(),
                  [marker](const auto& literal) { return literal.second == marker; })) {
    prefix.templated(opening, std::span<const std::string_view>(&marker, 1U));
  } else {
    prefix.text(opening);
  }
  const auto pieces = prefix.take();
  const auto encoded = encode_prompt_pieces(tokenizer, pieces);
  const auto* ids = std::get_if<std::vector<std::size_t>>(&encoded);
  if (ids == nullptr) return ChatGenerationPlanError{ChatGenerationPlanErrorCode::tokenization_failed, "tool prefix"};
  plan->prompt_token_ids.insert(plan->prompt_token_ids.end(), ids->begin(), ids->end());
  if (plan->prompt_token_ids.size() > config.max_prompt_tokens)
    return ChatGenerationPlanError{ChatGenerationPlanErrorCode::prompt_too_large, "tool prefix exceeds context"};
  return result;
}

std::unique_ptr<StreamRounds> make_client_tool_stream(ClientToolTurn turn, const SseIdentity& identity) {
  return std::make_unique<ClientToolStream>(std::move(turn), identity);
}

GenerationOutcome ChatRoute::run_client_tools(Lease& lease, const ClientToolTurn& turn, const ChatGenerationPlan& plan,
    const std::vector<std::uint32_t>& prompt_ids, const std::size_t max_tokens, const RequestContext& context,
    std::vector<ToolCall>& calls) {
  if (const auto failure = lease.chat().begin(prompt_ids, {.max_tokens = max_tokens, .sampling = plan.sampling})) return *failure;
  const ExecutionControl control{.cancelled = context.client_gone, .shutting_down = context.shutting_down,
                                 .shutdown_requested = [] { return shutdown_signalled(); }};
  Collector collector(control);
  ToolCallObserver parser(collector, turn.declarations, turn.format);
  if (!turn.prefix.empty()) parser.on_text(turn.prefix);
  const TokenByteDecoder decoder(tokenizer_);
  const auto run = drive_generation(lease.protocol(), decoder, round_limits(lease.chat(), plan, max_tokens, config_.monitor), parser);
  parser.finish(run.end == RunEnd::stop);
  if (run.end != RunEnd::stop && run.end != RunEnd::length) return failure_of(run, control);
  const auto& parsed = parser.calls();
  const std::size_t count = std::min(parsed.size(), turn.parallel ? kMaxClientToolCalls : 1U);
  calls.assign(parsed.begin(), parsed.begin() + static_cast<std::ptrdiff_t>(count));
  return GenerationResult{std::move(collector.text), prompt_ids.size(), run.completion_tokens,
                          run.end == RunEnd::stop ? FinishReason::stop : FinishReason::length,
                          run.timed_out, std::move(collector.reasoning)};
}

std::unique_ptr<StreamRounds> make_search_stream_rounds(SearchLimits limits, std::shared_ptr<const SearchTurn> turn,
                                                        const TokenizerPack& tokenizer, ChatExecutionService& chat,
                                                        const SseIdentity& identity) {
  return std::make_unique<SearchStreamRounds>(std::move(limits), std::move(turn), tokenizer, chat, identity);
}

GenerationOutcome ChatRoute::run_search_rounds(Lease& lease, const SearchTurn& turn, ChatGenerationPlan plan,
                                               std::vector<std::uint32_t> prompt_ids, std::size_t max_tokens,
                                               const RequestContext& context, std::vector<std::string>& events) {
  const SearchLimits limits = search_limits(config_);
  const ExecutionControl control{.cancelled = context.client_gone, .shutting_down = context.shutting_down,
                                 .shutdown_requested = [] { return shutdown_signalled(); }};
  Collector collector(control);
  SearchRounds rounds(*config_.tools, turn.offer, config_.tool_rounds);
  const TokenByteDecoder decoder(tokenizer_);
  std::size_t completion_tokens = 0U;
  for (;;) {
    if (const auto failure = lease.chat().begin(prompt_ids, {.max_tokens = max_tokens, .sampling = plan.sampling})) return *failure;
    ToolCallObserver round(collector, {turn.offer.declaration}, turn.format);
    const GenerationRunResult run = drive_generation(lease.protocol(), decoder, round_limits(lease.chat(), plan, max_tokens, config_.monitor), round);
    round.finish(run.end == RunEnd::stop);
    completion_tokens += run.completion_tokens;
    if (run.end != RunEnd::stop && run.end != RunEnd::length) return failure_of(run, control);
    const auto answer = [&](const FinishReason finish) {
      return GenerationResult{collector.text, prompt_ids.size(), completion_tokens, finish, run.timed_out, collector.reasoning};
    };
    const auto step = rounds.after(round, run.end, [&control] { return shutting(control) || (control.cancelled && control.cancelled()); },
                                   [&events](std::string text) { events.push_back(std::move(text)); });
    if (step == SearchRounds::Next::done) return answer(run.end == RunEnd::stop ? FinishReason::stop : FinishReason::length);
    if (step == SearchRounds::Next::cancelled) return shutting(control) ? GenerationFailure::shutting_down : GenerationFailure::cancelled;
    // The next prompt does not fit, or its room is gone: the answer ends here, cut.
    auto planned = search_plan(limits, turn, tokenizer_, rounds.rounds());
    auto* next = std::get_if<ChatGenerationPlan>(&planned);
    auto ids = next != nullptr ? wire_ids(next->prompt_token_ids) : std::nullopt;
    const auto budget = ids ? round_budget(limits, turn, ids->size(), lease.chat().context_tokens()) : std::nullopt;
    if (!budget) return answer(FinishReason::length);
    plan = std::move(*next);
    prompt_ids = std::move(*ids);
    max_tokens = *budget;
  }
}

}  // namespace secure_frontend
