#pragma once

#include "secure_frontend/chat_execution.hpp"
#include "secure_frontend/chat_schema.hpp"
#include "secure_frontend/generation_gate.hpp"
#include "secure_frontend/image_payload.hpp"
#include "secure_frontend/qwen_prompt_renderer.hpp"
#include "secure_frontend/request_router.hpp"
#include "secure_frontend/serve_monitor.hpp"
#include "secure_frontend/tokenizer_pack.hpp"
#include "secure_frontend/tool_runner.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace secure_frontend {

struct ChatRouteConfig final {
  ChatSchemaLimits schema;
  std::string model_id;
  NativeChatProfile profile;
  std::size_t max_prompt_tokens;
  std::size_t max_completion_tokens;
  std::size_t default_max_tokens;   // 0 = auto (B1)
  SamplingDefaults sampling_default;  // SP5 M1 Task 4: defaults.sampling.* (request fields win)
  bool fit_max_tokens = true;         // SP5 M1 Task 5: engine.fit_max_tokens (false: over-room 400, pin :4536)
  // reasoning.effort for the qwen profile: false only for "none" (the template's closed
  // think block); unset, low, medium and high think (upstream frontend.py:134-148).
  bool qwen_thinking = true;
  // Session log step 16: the level a thinking request without its own reasoning_effort gets
  // (reasoning.effort; unset is the template's default, high), and reasoning.effort_position
  // "end" (the template's sentence for a non-default level right before the answer).
  QwenEffort qwen_effort = QwenEffort::high;
  bool effort_at_end = false;
  // Gemma 4 plan 1 Task 10 (spec §6.6, D10): whether a gemma4 request that names no
  // reasoning_effort thinks (reasoning.effort low/medium/high); unset or none: off.
  bool gemma4_thinking_default = false;
  // flint: whether a request that names no reasoning_effort thinks (flint_thinking_default: reasoning.effort none
  // no, unset or a level yes).
  bool flint_thinking_default = true;
  // minimax: the level a request that names no reasoning_effort gets (minimax_effort_default: reasoning.effort's word,
  // none for thinking off; nullopt, MiniMax's adaptive thinking with no effort named).
  std::optional<std::string> minimax_effort_default{};
  // SP5 M1 Task 8: engine.min_tokens / engine.max_tokens — added frontend bounds on the
  // request output budget (0: unset; Ruling T8-e) — and model.aliases: extra exact model
  // names the chat route answers to (set_aliases :3764-3777; §4: never empty or partial).
  std::size_t minimum_output_tokens{0};
  std::size_t maximum_output_tokens{0};
  std::vector<std::string> model_aliases{};
  std::chrono::milliseconds queue_timeout;
  std::chrono::milliseconds write_stall;
  std::chrono::milliseconds response_write_deadline;
  std::chrono::milliseconds stream_keepalive;
  // SP5 M1 Task 12 (C-9, Ruling T12-b): the encoder path for the managed model that
  // reads images; nullptr whenever the schema's vision_capable gate is closed (the two
  // are one decision made by secure_server_main), so a content array never reaches a
  // route without a service.
  VisionService* vision{nullptr};
  // listener.monitor (session log step 16): told about each generation; null when off.
  ServeMonitor* monitor{nullptr};
  // Web search (spec 2026-10-27-web-search-design §2): the server's tool for the qwen and glm53 profiles' text chats (the
  // search service: a build with SECURE_FRONTEND_WEB_SEARCH and search.enabled); null: every chat is served without it.
  ToolRunner* tools{nullptr};
  std::size_t tool_rounds{8};  // search.max_rounds
};

// One chat with the server's tool: what the search loop's rounds render again (the request, the tool, the thinking).
struct SearchTurn final {
  ChatCompletionRequest request;
  ToolOffer offer;
  bool thinking{true};
  QwenEffortPlacement effort{QwenEffort::high, false};
  ToolFormat format{ToolFormat::qwen};  // the profile's: qwen (upstream's template), glm53 (Maya's GLM-5.3 one)
  MinimaxThinking minimax{};            // the minimax profile's thinking (off, or its effort)
};

// The route's values a later round's prompt and budget need (chat_search_route.cpp); copied into a stream's closure.
struct SearchLimits final {
  std::size_t max_prompt_tokens;
  std::size_t max_completion_tokens;
  std::size_t default_max_tokens;
  bool fit_max_tokens;
  std::size_t minimum_output_tokens;
  std::size_t maximum_output_tokens;
  SamplingDefaults sampling;
  ToolRunner* tools;
  std::size_t rounds;
  ServeMonitor* monitor;
};
[[nodiscard]] SearchLimits search_limits(const ChatRouteConfig& config);
// The first round's plan: the conversation with the server's tool (make_qwen_tool_generation_plan).
[[nodiscard]] ChatGenerationPlanResult search_first_plan(const ChatRouteConfig& config, const SearchTurn& turn, const TokenizerPack& tokenizer);
// Which profiles' text chats get the server's tool, in their family's form: qwen, GLM-5.3, raon1 (Qwen's calls in its
// own prompt), flint (GLM's) and Gemma 4 (its own); nullopt for the others (no tool form wired yet).
[[nodiscard]] std::optional<ToolFormat> search_tool_format(NativeChatProfile profile);
class StreamRounds;  // chat_stream.hpp
struct SseIdentity;  // sse_encoder.hpp
// The stream's rounds for a chat with the server's tool: the first generation is already begun on `chat`.
[[nodiscard]] std::unique_ptr<StreamRounds> make_search_stream_rounds(SearchLimits limits, std::shared_ptr<const SearchTurn> turn,
                                                                      const TokenizerPack& tokenizer, ChatExecutionService& chat,
                                                                      const SseIdentity& identity);

[[nodiscard]] std::optional<std::string> make_completion_id();

// Gemma 4 plan 1 Task 10 (spec §6.6, D10): a gemma4 request thinks when its reasoning_effort
// is low, medium or high, not when it is none; a request that names none takes the server's
// default (gemma4_thinking_default: reasoning.effort, unset = off).
[[nodiscard]] bool gemma4_request_thinks(const std::optional<std::string>& reasoning_effort, bool thinking_default);

// MiniMax-M3.1 (owner 2026-10-28): a request's thinking in Maya's words - its reasoning_effort when it names one (none:
// thinking off; low, medium, high: that effort, the client's own word as Maya's effort_name keeps it), else the server's
// (`effort_default`: reasoning.effort; unset, MiniMax's adaptive thinking with no effort named).
[[nodiscard]] MinimaxThinking minimax_request_thinking(const std::optional<std::string>& reasoning_effort,
                                                       const std::optional<std::string>& effort_default);

// The route's plan for a request, chosen by an exhaustive switch on the profile (Gemma 4 plan 1
// Tasks 3 and 10): qwen renders ChatML (images allowed, thinking and its effort sentence);
// glm53 the GLM profile; gemma4 the Gemma 4 template (text only: any image answer is
// invalid_request). `thinking` is the profile's own decision, made by the caller; `minimax` is the
// minimax profile's (thinking off, or its effort).
[[nodiscard]] ChatGenerationPlanResult select_chat_generation_plan(NativeChatProfile profile, const ChatCompletionRequest& request,
                                                                   const TokenizerPack& tokenizer, std::size_t maximum_prompt_tokens,
                                                                   const SamplingDefaults& defaults,
                                                                   std::span<const std::size_t> image_token_counts, bool thinking,
                                                                   QwenEffortPlacement qwen_effort, const MinimaxThinking& minimax = {});

// POST /v1/chat/completions, non-streaming (spec §2.2 P4-P6, §2.5, §3.1, §3.5).
class ChatRoute final {
 public:
  ChatRoute(ChatRouteConfig config, SupervisorPort& port, GenerationGate& gate, const TokenizerPack& tokenizer);
  [[nodiscard]] RouteResult handle(std::string_view body, const RequestContext& context);

 private:
  [[nodiscard]] HttpResponse refusal(GateRefusal refusal);
  [[nodiscard]] HttpResponse engine_unavailable();
  // SP5 M1 Task 12 (T12-i): `embeddings` non-empty begins a GENI generation; the
  // combined file it names must outlive the stream until the engine has run, so its
  // removal belongs to this closure, not the handle() cleaner. The plan supplies the
  // sampling and the reasoning split (issue 19).
  // Web search: `search` set, the stream is the search loop's rounds (one stream, one lease).
  [[nodiscard]] RouteResult stream_response(std::optional<Lease>& lease, const std::vector<std::uint32_t>& prompt_ids,
                                            std::size_t max_tokens, const ChatGenerationPlan& plan, bool include_usage,
                                            const RequestContext& context, std::string embeddings = {},
                                            std::optional<std::filesystem::path> combined_file = std::nullopt,
                                            std::shared_ptr<const SearchTurn> search = nullptr);
  // Web search: the complete answer of a chat with the server's tool - the rounds inside the lease, each round's text
  // through the call parser; the reply's text and reasoning are every round's, its usage the last round's prompt and
  // every round's tokens. `events` gets the loop's events (message.strata_search).
  [[nodiscard]] GenerationOutcome run_search_rounds(Lease& lease, const SearchTurn& turn, ChatGenerationPlan plan,
                                                    std::vector<std::uint32_t> prompt_ids, std::size_t max_tokens,
                                                    const RequestContext& context, std::vector<std::string>& events);
  ChatRouteConfig config_;
  SupervisorPort& port_;
  GenerationGate& gate_;
  const TokenizerPack& tokenizer_;
  std::atomic<std::size_t> last_context_{0};   // last READY context seen through a lease; 0 = unknown
};

}  // namespace secure_frontend
