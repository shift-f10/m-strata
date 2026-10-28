#include "secure_frontend/generation_gate.hpp"

#include "secure_frontend/api_constants.hpp"

#include <algorithm>
#include <thread>

namespace secure_frontend {
namespace {

// Spec §2.5 fast fail: never wait for READY (SP2 V11). ready and generating
// are the only states a waiter may enter the queue for.
std::optional<GateRefusal> refusal_for(const SupervisorState state) {
  switch (state) {
    case SupervisorState::starting:
    case SupervisorState::backoff:
    case SupervisorState::circuit_open: return GateRefusal::engine_unavailable;
    case SupervisorState::stopping:
    case SupervisorState::shutting_down:
    case SupervisorState::stopped: return GateRefusal::shutting_down;
    case SupervisorState::ready:
    case SupervisorState::generating: return std::nullopt;
    // SP5 M1 Task 6 (pin :6252 "loads on use", StrataEngine.start on the request path):
    // an unloaded engine is queueable, not a refusal — the driver loop wakes it.
    case SupervisorState::unloaded: return std::nullopt;
  }
  return GateRefusal::engine_unavailable;   // unreachable: the enum is exhausted
}

}  // namespace

GenerationGate::GenerationGate(const GateLimits limits, std::function<Clock::time_point()> now)
    : limits_(limits), now_(std::move(now)) {}

std::variant<Lease, GateRefusal> GenerationGate::admit(SupervisorPort& port, const Clock::time_point deadline,
                                                       const std::function<bool()>& client_gone) {
  // One snapshot before queueing (spec §2.5): a non ready/generating state is
  // answered at once, never queued behind the generation.
  if (const auto fast = refusal_for(port.snapshot().state)) return *fast;
  std::unique_lock lock(mutex_);
  if (shutdown_) return GateRefusal::shutting_down;
  if (running_ || !queue_.empty()) {
    // max_queued bounds WAITERS; an arrival that finds slot and queue empty
    // never waits, so it passes even at max_queued = 0 (serial mode, DOS-02).
    if (queue_.size() >= limits_.max_queued) return GateRefusal::busy;
    const std::uint64_t ticket = next_ticket_++;
    queue_.push_back(ticket);
    // A waiter wakes on release()/shutdown() or every kStreamPollSlice to
    // re-check client_gone, its deadline, and whether it became head.
    for (;;) {
      changed_.wait_for(lock, api_constants::kStreamPollSlice);
      if (shutdown_) { leave(ticket); return GateRefusal::shutting_down; }
      if (client_gone()) { leave(ticket); return GateRefusal::cancelled; }
      if (now_() >= deadline) { leave(ticket); return GateRefusal::timeout; }
      if (!running_ && queue_.front() == ticket) break;
    }
    queue_.pop_front();
  }
  running_ = true;   // this thread now owns the single running slot
  lock.unlock();
  // The head drives SP2 to a lease (spec §2.5 "Granting the slot").
  bool woke_cold = false;  // SP5 M1 Task 6: this request woke (or is waiting on) a cold engine
  for (;;) {
    const auto snapshot = port.snapshot();
    if (snapshot.state == SupervisorState::ready) {
      if (auto lease = port.acquire()) return std::move(*lease);   // the holder calls release() after its Lease dies
      release();   // SP2 moved out of ready between the snapshot and acquire()
      return GateRefusal::engine_unavailable;
    }
    if (snapshot.state == SupervisorState::generating) {
      if (now_() >= deadline) { release(); return GateRefusal::timeout; }
      std::this_thread::sleep_for(api_constants::kStreamPollSlice);   // re-check; the slot stays ours
      continue;
    }
    // SP5 M1 Task 6 (parity rows lazy_load / idle_unload_s): the head of the queue wakes a
    // cold engine (pin: the request path restarts it, :6252) and stays queued through the
    // spawn — but only a spawn it woke itself; a crash-recovery `starting` still refuses
    // fast, as SP2 established.
    if (snapshot.state == SupervisorState::unloaded || (woke_cold && snapshot.state == SupervisorState::starting)) {
      port.wake_for_request();
      woke_cold = true;
      if (now_() >= deadline) { release(); return GateRefusal::timeout; }
      std::this_thread::sleep_for(api_constants::kStreamPollSlice);
      continue;
    }
    release();
    if (const auto refusal = refusal_for(snapshot.state)) return *refusal;
    return GateRefusal::engine_unavailable;   // unreachable: only ready/generating pass refusal_for
  }
}

void GenerationGate::release() noexcept {
  {
    std::lock_guard lock(mutex_);
    running_ = false;
  }
  changed_.notify_all();
}

void GenerationGate::shutdown() noexcept {
  {
    std::lock_guard lock(mutex_);
    shutdown_ = true;
  }
  changed_.notify_all();   // queued tickets answer shutting_down; a holder is untouched (its Lease path ends the work)
}

std::size_t GenerationGate::waiting() const {
  std::lock_guard lock(mutex_);
  return queue_.size();
}

void GenerationGate::leave(const std::uint64_t ticket) {
  queue_.erase(std::remove(queue_.begin(), queue_.end(), ticket), queue_.end());
  changed_.notify_all();   // a waiter behind the leaving one may now be head
}

}  // namespace secure_frontend
