#pragma once

#include "secure_frontend/config_defaults.hpp"
#include "secure_frontend/restart_policy.hpp"
#include "secure_frontend/supervised_process.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace secure_frontend {

// SP5 M1 Task 11 (D-9 vision split, Ruling T11-w): the supervised `maya-vision`
// encoder child. A separate role, not a second EngineSupervisor: the engine role
// is chat-protocol shaped (READY <ctx> <mode>, GEN/T/DONE, leases), while the
// pin's vision child speaks its own lifecycle — the first stdout line must start
// with "READY" (serve/server.py:2599-2612), "ENC <img> <out>" requests and
// "QUIT" to stop (:2755-2792; the request path is Task 12). The SP2 machinery
// it reuses: ProcessFactory/SupervisedProcess, RestartTracker, kill plumbing.
//
// Thread contract: the request path (Ruling T12-h) is guarded by the supervisor's own
// exchange lock — concurrent serving threads serialize exactly like the pin's
// vision.lock around encode_all (serve/server.py:4489): one ENC at a time.

// The child's argv tail, re-anchored by SP5-P2 Task 6 on the engine's own maya-vision
// at 451f9cb6 (tools/vision/strata_vision.cpp:61-79): --mmproj and --model first, then
// the optional --threads / --max-tokens; the grammar REJECTS --min-tokens (upstream
// v0.2.2's reader_args, serve/server.py:2566-2567, carried the #767 hand-edit; the
// engine's encoder dropped it), so engine.min_tokens never reaches this child. --gpu is
// not typed for the vision child in M1 (the GPU stays with the owner's box, cf. Rulings
// T7-c/T11-ee). no_flash_attn is the added flag from the spec §5.1 family — maya-vision
// reads it at the pin (strata_vision.cpp:74).
struct VisionChildSpec final {
  std::string mmproj;                       // the manifest's encoder artifact (absolute UTF-8)
  std::string model;                        // the weights the encoder reads (its --native)
  std::int64_t threads{0};                  // vision.threads (0: the pin's truthy skip, :2562)
  std::optional<std::int64_t> max_tokens;   // engine.max_tokens, when set (:2564-2565)
  bool no_flash_attn{false};                // vision.no_flash_attn (added; spec §5.1)
};

[[nodiscard]] std::vector<std::string> compose_vision_arguments(const VisionChildSpec& spec);

enum class VisionState { stopped, starting, up, circuit_open };

// The SP2 restart policy on this role (plan): restart once, then circuit
// (max_consecutive_failures 2 by default), and the pin's 300 s READY gate
// (VISION_READY_S default, serve/server.py:327 — not an env knob in M1).
struct VisionLimits final {
  RestartPolicy policy{RestartPolicy{.ready_timeout = std::chrono::milliseconds{std::chrono::seconds(
                                            config_defaults::kVisionReadyTimeoutS)}}};
  // The pin's VISION_ENCODE_S gate for an ENC reply (serve/server.py:328); 0 means
  // "wait for ever", the pin's rule for these gates (:291). Row: vision.encode_timeout_s.
  std::chrono::milliseconds encode_timeout{std::chrono::seconds(config_defaults::kVisionEncodeTimeoutS)};
  VisionLimits() { policy.max_consecutive_failures = 2U; }  // the plan: restart ONCE, then circuit
};

// An ENC reply of "OK <n>" (:2778): n is the image-token count the encoder
// produced; the embedding file itself is already on disk (the child wrote it).
struct VisionEncoded final {
  std::size_t image_tokens{0};
};

// The failure taxonomy (plan C-9 / Ruling T12-h). encoder_rejected keeps the role
// alive (the pin raises per-image and returns the encoder to its pool, :2774);
// no_reply means the generation was killed and reaped, and the NEXT request wakes
// it (the pin's _vision_up laziness, :2646); not_running refuses WITHOUT waking
// (circuit open, or an explicit stop() = shutdown).
struct VisionEncodeError final {
  enum class Kind { not_running, wake_failed, no_reply, encoder_rejected, bad_name };
  Kind kind;
};
using VisionEncodeResult = std::variant<VisionEncoded, VisionEncodeError>;

class VisionSupervisor final {
 public:
  VisionSupervisor(ProcessFactory factory, VisionLimits limits = {});
  ~VisionSupervisor();
  VisionSupervisor(const VisionSupervisor&) = delete;
  VisionSupervisor& operator=(const VisionSupervisor&) = delete;

  // Starts generations until the READY gate opens the role, or the policy is
  // exhausted (circuit_open: the boot caller decides what that means, Ruling
  // T11-z). Blocks on the owner thread; backoff sleeps between attempts.
  // After an explicit stop() the role stays down: stop() IS the shutdown signal.
  void start();

  // QUITs the child (pin :2792), reaps it within the policy's bound, and kills
  // the group if it lingers. Always leaves the role stopped, and marks the role
  // as shut down: later encode() calls refuse instead of waking it (T12-h).
  void stop();

  // The request path: "ENC <image_name> <out_name>" (:2776) with names RELATIVE to
  // the child's working directory (the pin's #480 rule — names carry no spaces), a
  // reply of "OK <n>"/"ERR ..." inside encode_timeout, and the pin's laziness: a
  // stopped-but-not-shut-down role wakes for the request (vision_up, :2646). One
  // exchange at a time; a silent or dead child takes the whole attempt down
  // (kill+reap) and leaves the role stopped for the NEXT request, never respawning
  // inside this call.
  [[nodiscard]] VisionEncodeResult encode(std::string_view image_name, std::string_view out_name);

  [[nodiscard]] VisionState state() const noexcept { return state_.load(); }
  [[nodiscard]] bool up() const noexcept { return state_.load() == VisionState::up; }
  // Spawns attempted by start() and by lazy wakes; diagnostics and test evidence.
  [[nodiscard]] std::uint64_t generations_started() const noexcept { return generation_.load(); }

 private:
  // start() with the exchange lock already held (encode's lazy wake).
  void start_locked();
  // One attempt at the pin's READY gate; consumes lines until the first one.
  [[nodiscard]] bool await_ready();
  // Kills and bounded-reaps the ended generation; false when it stayed unreaped.
  [[nodiscard]] bool end_generation();

  ProcessFactory factory_;
  VisionLimits limits_;
  std::unique_ptr<SupervisedProcess> process_;
  RestartTracker tracker_;
  std::mutex exchange_;
  bool shutdown_requested_{false};  // guarded by exchange_
  std::atomic<VisionState> state_{VisionState::stopped};
  std::atomic<std::uint64_t> generation_{0};
};

}  // namespace secure_frontend
