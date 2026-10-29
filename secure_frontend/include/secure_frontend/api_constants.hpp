#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

// Every SP3 internal constant (spec §5.3). Tunables are SP1 schema keys, never here.
namespace secure_frontend::api_constants {
inline constexpr std::chrono::milliseconds kAcceptPollSlice{100};
inline constexpr std::chrono::milliseconds kStreamPollSlice{20};
inline constexpr std::chrono::milliseconds kAcceptBackoff{50};
inline constexpr std::size_t kLingerListMax = 64U;
inline constexpr std::chrono::milliseconds kLingerBudget{100};
// SP5 M1 Task 12 (C-9, Ruling T12-k): the content parts grammar the plan opens for the
// vision model nests two more containers (parts form: request -> messages -> message ->
// content array -> part object -> image_url object = 6). 6 is exactly that depth; every
// entry/size bound is unchanged. The pre-C-9 value was 4 (spec §3.1); the C-9 carve-out
// supersedes it, and the server keeps its 400-before-anything property at the schema.
// 16 since 2026-10-29: an earlier answer's searches nest request -> messages -> message -> strata_rounds -> round ->
// calls -> call -> arguments -> the arguments' own values (6 before, since C-9's parts grammar; upstream sets none).
inline constexpr std::size_t kChatMaxJsonDepth = 16U;
inline constexpr std::size_t kMaxDeltaTextBytes = 16U * 1024U;     // M2
inline constexpr std::size_t kMaxSseEventBytes = 128U * 1024U;     // M2
inline constexpr std::size_t kMaxSendChunk = 64U * 1024U;
inline constexpr std::chrono::milliseconds kDrainBudget{100};
inline constexpr std::size_t kDrainBytes = 256U * 1024U;
inline constexpr std::size_t kRecvChunk = 4096U;
inline constexpr int kRetryAfterBusyS = 1;
inline constexpr int kRetryAfterEngineS = 5;
inline constexpr std::size_t kLogBurst = 100U;
inline constexpr std::size_t kLogRatePerSecond = 20U;
inline constexpr std::size_t kRefusalMaxBytes = 512U;
inline constexpr std::chrono::milliseconds kShutdownDrainBudget{2000};
inline constexpr std::size_t kChatModelMaxBytes = 256U;
inline constexpr std::size_t kChatUserMaxBytes = 256U;
inline constexpr double kTemperatureMax = 2.0;
inline constexpr std::uint64_t kSeedMax = 9223372036854775807ULL;
inline constexpr std::size_t kContextMarginTokens = 8U;
inline constexpr std::size_t kMaxModelFieldBytes = 1024U;   // /v1/models id and owned_by (pre-flight D10)
inline constexpr std::size_t kMaxPollEntries = 128U;        // one poll_sockets call (pre-flight D10)
}  // namespace secure_frontend::api_constants
