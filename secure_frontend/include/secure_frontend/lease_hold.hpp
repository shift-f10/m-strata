#pragma once

#include "secure_frontend/engine_supervisor.hpp"
#include "secure_frontend/generation_gate.hpp"

#include <functional>
#include <optional>

namespace secure_frontend {

// Owns the granted Lease of one streamed chat (spec §2.5 order: report, destroy the Lease, then release the gate).
class LeaseHold final {
 public:
  LeaseHold(Lease lease, GenerationGate& gate, SupervisorPort& port, std::function<bool()> shutting_down);
  // If the handler never ran (the Lease is still held with GEN written): STOP and drain within
  // stop_drain_timeout — or, during shutdown, abandon at once (R-H) — then end_generation().
  ~LeaseHold();
  LeaseHold(const LeaseHold&) = delete;
  LeaseHold& operator=(const LeaseHold&) = delete;
  [[nodiscard]] Lease& lease() noexcept;
  // Idempotent: report a closed protocol through the port (never while shutting down), destroy the Lease, release the gate.
  void end_generation();

 private:
  std::optional<Lease> lease_;
  GenerationGate& gate_;
  SupervisorPort& port_;
  std::function<bool()> shutting_down_;
};

}  // namespace secure_frontend
