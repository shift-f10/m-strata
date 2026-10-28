#include "secure_frontend/generation_gate.hpp"

namespace secure_frontend {

HealthSnapshot SupervisorPort::snapshot() {
  std::lock_guard lock(mutex_);
  return supervisor_.snapshot();
}

std::optional<Lease> SupervisorPort::acquire() {
  std::lock_guard lock(mutex_);
  return supervisor_.acquire();
}

void SupervisorPort::report_fatal(const Lease& lease, const KillReason reason) {
  std::lock_guard lock(mutex_);
  supervisor_.report_fatal(lease, reason);
}

// SP5 M1 Task 6 (idle_unload_s / lazy_load): the serving-path bracket and the wake.
void SupervisorPort::note_request_started() {
  supervisor_.note_request_started();
}

void SupervisorPort::note_request_finished() {
  supervisor_.note_request_finished();
}

void SupervisorPort::wake_for_request() {
  supervisor_.wake_for_request();
}

}  // namespace secure_frontend
