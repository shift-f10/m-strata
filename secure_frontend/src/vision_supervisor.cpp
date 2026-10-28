// SP5 M1 Task 11 (D-9, Ruling T11-w): the supervised maya-vision role.
// The READY gate mirrors the pin (serve/server.py:2599-2612): the first stdout
// line must start with "READY"; any other line, an oversize line (the shared
// 8192-byte bound, engine_stdio_process.hpp), a closed stdout or the 300 s
// timeout ends the generation. An ended generation is killed (kVisionKillExitCode
// on Windows, Ruling T11-x), reaped, and counted by the shared RestartTracker;
// the policy (default: restart ONCE, then circuit) decides the next move.
#include "secure_frontend/vision_supervisor.hpp"

#include "secure_frontend/deadline.hpp"  // MonotonicClock

#include <algorithm>
#include <thread>
#include <utility>

namespace secure_frontend {

std::vector<std::string> compose_vision_arguments(const VisionChildSpec& spec) {
  // The pin's reader_args order (serve/server.py:2556-2568): the files, --gpu (vision.gpu, upstream's reader_args),
  // --threads, --max-tokens; flash attention off is the added spec §5.1 family flag, last.
  std::vector<std::string> args{"--mmproj", spec.mmproj, "--model", spec.model};
  if (spec.gpu) args.emplace_back("--gpu");
  if (spec.threads > 0) {
    args.emplace_back("--threads");
    args.emplace_back(std::to_string(spec.threads));
  }
  // upstream's Vision.token_limit: a limit of its own, else the kind's default (setup.py's VISION: GPU 1024, CPU 300)
  std::optional<std::int64_t> max_tokens = spec.max_tokens;
  if (!max_tokens && spec.upstream_encoder) max_tokens = spec.gpu ? 1024 : 300;
  if (max_tokens.has_value()) {
    args.emplace_back("--max-tokens");
    args.emplace_back(std::to_string(*max_tokens));
  }
  // SP5-P2 Task 6: no --min-tokens — the maya-vision grammar at the pin (its argument
  // loop, tools/vision/strata_vision.cpp:61-79 at 451f9cb6) has no such slot and exits
  // 2 on an unknown argument.
  // upstream's strata-vision spells it --flash-attn off (tools/vision/strata_vision.cpp), maya-vision --no-flash-attn
  if (spec.no_flash_attn && spec.upstream_encoder) args.insert(args.end(), {"--flash-attn", "off"});
  else if (spec.no_flash_attn) args.emplace_back("--no-flash-attn");
  return args;
}

VisionSupervisor::VisionSupervisor(ProcessFactory factory, VisionLimits limits)
    : factory_(std::move(factory)), limits_(std::move(limits)), tracker_(limits_.policy) {}

VisionSupervisor::~VisionSupervisor() { stop(); }

void VisionSupervisor::start() {
  std::lock_guard lock(exchange_);
  shutdown_requested_ = false;  // an explicit start() is a fresh boot, not a post-shutdown wake
  start_locked();
}

// start() with exchange_ held (boot, or encode's lazy wake).
void VisionSupervisor::start_locked() {
  if (up() || state_.load() == VisionState::circuit_open) return;
  state_ = VisionState::starting;
  while (state_.load() == VisionState::starting) {
    ++generation_;
    auto spawned = factory_(generation_.load());
    if (const auto* failure = std::get_if<SpawnFailure>(&spawned)) {
      (void)failure;  // a spawn that produced no child is a generation failure (SP2 §3.3 family)
    } else {
      process_ = std::get<std::unique_ptr<SupervisedProcess>>(std::move(spawned));
      if (await_ready()) {
        state_ = VisionState::up;
        return;
      }
      if (!end_generation()) {
        state_ = VisionState::circuit_open;  // V10 family: never respawn onto an unreaped child
        return;
      }
    }
    // never became ready: healthy_for 0, so the healthy window cannot reset it
    const auto decision = tracker_.record_failure(std::chrono::milliseconds{0});
    if (decision.circuit_open) {
      state_ = VisionState::circuit_open;
      return;
    }
    std::this_thread::sleep_for(decision.delay);  // backoff under the exchange lock (one ENC at a time)
  }
}

bool VisionSupervisor::await_ready() {
  // Ruling T11-gg: vision.ready_timeout_s 0 means "wait for ever" — the pin's own rule
  // for STRATA_VISION_READY_S (serve/server.py:291), so the loop ends only on a line
  // or the child's end.
  const bool forever = limits_.policy.ready_timeout.count() == 0;
  const auto deadline = MonotonicClock::now() + limits_.policy.ready_timeout;
  while (forever || MonotonicClock::now() < deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - MonotonicClock::now());
    const auto slice = forever ? std::chrono::milliseconds{250} : std::min(left, std::chrono::milliseconds{250});
    const auto read = process_->transport().read_line(slice);
    if (read.kind == EngineReadKind::line) return read.line.rfind("READY", 0) == 0;  // the FIRST line decides
    if (read.kind == EngineReadKind::timeout) {
      if (process_->try_reap().has_value()) return false;  // died while we waited
      continue;
    }
    return false;  // closed stdout, or a failed read (oversize line included)
  }
  return false;  // the READY gate (VISION_READY_S) timed out
}

bool VisionSupervisor::end_generation() {
  process_->kill_group();
  const auto deadline = MonotonicClock::now() + limits_.policy.reap_timeout;
  while (!process_->try_reap().has_value()) {
    if (MonotonicClock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  process_->release_resources(std::chrono::milliseconds{0});
  process_.reset();
  return true;
}

void VisionSupervisor::stop() {
  std::lock_guard lock(exchange_);
  shutdown_requested_ = true;  // stop() IS shutdown: encode() refuses instead of waking (T12-h)
  if (process_ != nullptr) {
    // The pin's stop: QUIT, then the drain-else-kill shape (serve/server.py:2792).
    std::string error;
    (void)process_->transport().write_line("QUIT", MonotonicClock::now() + std::chrono::milliseconds{500}, error);
    const auto drain = MonotonicClock::now() + std::chrono::seconds{2};
    while (!process_->try_reap().has_value()) {
      if (MonotonicClock::now() >= drain) {
        process_->kill_group();
        const auto kill_deadline = MonotonicClock::now() + limits_.policy.reap_timeout;
        while (!process_->try_reap().has_value() && MonotonicClock::now() < kill_deadline) {
          std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    process_->release_resources(std::chrono::milliseconds{0});
    process_.reset();
  }
  state_ = VisionState::stopped;
}

VisionEncodeResult VisionSupervisor::encode(const std::string_view image_name, const std::string_view out_name) {
  // Names ride one protocol line, space-separated, RELATIVE to the child's cwd
  // (the pin's #480): anything containing a space, a control byte, or nothing at all
  // never reaches the child. The route only ever hands hex keys with fixed suffixes,
  // so this is the fail-closed backstop, not the ordinary path.
  const auto bad_name = [](const std::string_view name) {
    return name.empty() || name.size() > 128U || name.find_first_of(" \t\r\n") != std::string_view::npos ||
           name.front() == '.';
  };
  if (bad_name(image_name) || bad_name(out_name)) return VisionEncodeError{VisionEncodeError::Kind::bad_name};
  std::lock_guard lock(exchange_);
  if (shutdown_requested_ || state_.load() == VisionState::circuit_open)
    return VisionEncodeError{VisionEncodeError::Kind::not_running};
  if (state_.load() == VisionState::stopped) {
    // The pin's laziness (:2646): the request itself wakes the encoder. A wake that
    // exhausts the policy is not retried inside this call — the role reports and the
    // NEXT request tries again (the tracker keeps counting READY failures).
    start_locked();
    if (!up()) return VisionEncodeError{VisionEncodeError::Kind::wake_failed};
  }
  if (!up()) return VisionEncodeError{VisionEncodeError::Kind::not_running};

  // One exchange: write, then read until the answer, the child's end, or the
  // VISION_ENCODE_S bound (:328; 0 = wait for ever, :291).
  std::string io_error;
  if (!process_->transport().write_line("ENC " + std::string{image_name} + " " + std::string{out_name},
                                        MonotonicClock::now() + std::chrono::milliseconds{500}, io_error)) {
    // stdin closed: the child is gone (or never took commands). Same path as a silent one.
    if (!end_generation()) state_ = VisionState::circuit_open;
    else state_ = VisionState::stopped;
    return VisionEncodeError{VisionEncodeError::Kind::no_reply};
  }
  const bool forever = limits_.encode_timeout.count() == 0;
  const auto deadline = MonotonicClock::now() + limits_.encode_timeout;
  while (forever || MonotonicClock::now() < deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - MonotonicClock::now());
    const auto slice = forever ? std::chrono::milliseconds{250} : std::min(left, std::chrono::milliseconds{250});
    const auto read = process_->transport().read_line(slice);
    if (read.kind == EngineReadKind::line) {
      if (read.line.rfind("OK ", 0) == 0) {
        // The pin parses `OK (\d+)` (:2777-2778): digits only, positive.
        const std::string count{read.line.substr(3)};
        if (!count.empty() && count.find_first_not_of("0123456789") == std::string::npos && count.size() <= 9U &&
            count != "0") {
          return VisionEncoded{static_cast<std::size_t>(std::stoul(count))};
        }
        continue;  // malformed OK: not an answer; keep waiting (a later line may answer)
      }
      if (read.line.rfind("ERR", 0) == 0) {
        // The image failed, not the role (:2774): the child goes back to the pool.
        return VisionEncodeError{VisionEncodeError::Kind::encoder_rejected};
      }
      continue;  // the pin's reply loop ignores noise (log lines) until the gate ends
    }
    if (read.kind == EngineReadKind::timeout) {
      if (process_->try_reap().has_value()) {
        if (!end_generation()) state_ = VisionState::circuit_open;
        else state_ = VisionState::stopped;
        return VisionEncodeError{VisionEncodeError::Kind::no_reply};
      }
      continue;
    }
    // closed stdout / failed read: the child is gone (dieenc family).
    if (!end_generation()) state_ = VisionState::circuit_open;
    else state_ = VisionState::stopped;
    return VisionEncodeError{VisionEncodeError::Kind::no_reply};
  }
  // The encode gate closed (VISION_ENCODE_S): kill, reap, stopped — the next request wakes.
  if (!end_generation()) state_ = VisionState::circuit_open;
  else state_ = VisionState::stopped;
  return VisionEncodeError{VisionEncodeError::Kind::no_reply};
}

}  // namespace secure_frontend
