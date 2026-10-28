#include "secure_frontend/lease_hold.hpp"

#include "secure_frontend/api_constants.hpp"

#include <algorithm>
#include <chrono>
#include <variant>

namespace secure_frontend {

LeaseHold::LeaseHold(Lease lease, GenerationGate& gate, SupervisorPort& port, std::function<bool()> shutting_down)
    : gate_(gate), port_(port), shutting_down_(std::move(shutting_down)) {
  lease_.emplace(std::move(lease));
}

LeaseHold::~LeaseHold() {
  if (!lease_) return;
  EngineProtocolClient& protocol = lease_->protocol();
  if (protocol.generating()) {  // GEN was written but the handler never ran
    if (shutting_down_ && shutting_down_()) {
      protocol.abandon();  // R-H: no drain wait; the supervisor tears this generation down
    } else if (protocol.stop().has_value()) {
      protocol.abandon();
    } else {
      const auto started = std::chrono::steady_clock::now();
      auto until = started + lease_->chat().drive_limits(0U).stop_drain_timeout;
      while (protocol.generating() && std::chrono::steady_clock::now() < until) {
        if (shutting_down_ && shutting_down_()) {  // shutdown began during the drain: cap it (R-H)
          until = std::min(until, std::chrono::steady_clock::now() + api_constants::kShutdownDrainBudget);
        }
        if (std::holds_alternative<EngineProtocolError>(protocol.next_event(api_constants::kStreamPollSlice)) && protocol.closed()) break;
      }
      if (protocol.generating()) protocol.abandon();  // drain failed: SP2 drain_failed
    }
  }
  end_generation();
}

Lease& LeaseHold::lease() noexcept { return *lease_; }

void LeaseHold::end_generation() {
  if (!lease_) return;
  const bool shutting = shutting_down_ && shutting_down_();
  // A closed protocol leaves this engine generation unusable (SP2: kill, restart, count) — never reported during shutdown (R-H).
  if (!shutting && lease_->protocol().closed()) port_.report_fatal(*lease_, kill_reason_for(lease_->protocol()));
  lease_.reset();   // SP2: generating -> ready
  gate_.release();  // only now may the next head acquire (spec §2.5)
}

}  // namespace secure_frontend
