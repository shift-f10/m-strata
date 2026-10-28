#include "secure_frontend/connection_pool.hpp"

namespace secure_frontend {

AdmissionState::AdmissionState(const std::uint32_t workers, const std::uint32_t per_remote_cap)
    : slots_(workers), per_remote_cap_(per_remote_cap) {}

void AdmissionState::release_peer(const Slot& slot) {
  if (!slot.remote) return;
  const auto it = remote_counts_.find(slot.peer);
  if (it != remote_counts_.end() && --it->second == 0U) remote_counts_.erase(it);
}

AdmissionState::Decision AdmissionState::admit(const AddressScope scope, const PeerKey& peer, const Clock::time_point now) {
  const bool remote = scope != AddressScope::loopback;
  if (remote) {
    const auto it = remote_counts_.find(peer);
    if (it != remote_counts_.end() && it->second >= per_remote_cap_) return {Kind::refuse};
    if (per_remote_cap_ == 0U) return {Kind::refuse};
  }
  const std::size_t none = slots_.size();
  std::size_t chosen = none;
  Kind kind = Kind::assign;
  for (std::size_t i = 0; i < slots_.size(); ++i) {
    if (slots_[i].phase == WorkerPhase::idle) {
      chosen = i;
      break;
    }
  }
  if (chosen == none) {
    for (std::size_t i = 0; i < slots_.size(); ++i) {
      if (slots_[i].phase == WorkerPhase::header && (chosen == none || slots_[i].since < slots_[chosen].since)) chosen = i;
    }
    if (chosen == none) return {Kind::refuse};
    kind = Kind::evict;
    release_peer(slots_[chosen]);   // the evicted connection no longer holds its address's slot
  }
  Slot& slot = slots_[chosen];
  slot = Slot{WorkerPhase::header, now, ++next_connection_, remote, peer};
  if (remote) ++remote_counts_[peer];
  return {kind, chosen, slot.connection};
}

bool AdmissionState::header_complete(const std::size_t worker, const std::uint64_t connection) {
  Slot& slot = slots_.at(worker);
  if (slot.connection != connection || slot.phase != WorkerPhase::header) return false;
  slot.phase = WorkerPhase::busy;
  return true;
}

void AdmissionState::finished(const std::size_t worker, const std::uint64_t connection) {
  Slot& slot = slots_.at(worker);
  if (slot.connection != connection || slot.phase == WorkerPhase::idle) return;
  release_peer(slot);
  slot.phase = WorkerPhase::idle;
}

WorkerPhase AdmissionState::phase(const std::size_t worker) const { return slots_.at(worker).phase; }

std::size_t AdmissionState::remote_count(const PeerKey& peer) const {
  const auto it = remote_counts_.find(peer);
  return it == remote_counts_.end() ? 0U : it->second;
}

}  // namespace secure_frontend
