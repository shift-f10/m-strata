#include "secure_frontend/connection_pool.hpp"

#include <span>
#include <utility>

namespace secure_frontend {
namespace {
// Proof obligation (Task 12, spec §2.1/§2.4): the accept thread polls the
// listener together with the linger list in ONE poll_sockets call per 100 ms
// slice; it can never exceed api_constants::kMaxPollEntries.
static_assert(1U + api_constants::kLingerListMax <= api_constants::kMaxPollEntries,
              "the listener plus the linger list must fit one poll_sockets call");
}  // namespace

ConnectionPool::ConnectionPool(HttpSocketServer& listener, PoolLimits limits, RequestRouter& router, ConnectionObserver observer)
    : listener_(listener), limits_(limits), router_(router), observer_(std::move(observer)),
      admission_(limits.workers, limits.per_remote_cap) {
  for (std::uint32_t i = 0; i < limits.workers; ++i) workers_.push_back(std::make_unique<Worker>());
  for (std::size_t i = 0; i < workers_.size(); ++i) workers_[i]->thread = std::thread([this, i] { worker_loop(i); });
}

ConnectionPool::~ConnectionPool() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
    shutting_down_ = true;
  }
  for (auto& w : workers_) w->wake.notify_all();
  for (auto& w : workers_) if (w->thread.joinable()) w->thread.join();
}

const std::atomic<bool>& ConnectionPool::shutting_down() const noexcept { return shutting_down_; }

// One worker: wait for a hand-off under the pool mutex, serve one connection
// outside the lock, report the end back under the lock. Every AdmissionState
// call (this file's header_complete lambda and finished below, plus admit in
// run_until) runs with mutex_ held — the unit-level AdmissionState stays
// single-threaded and the pool mutex is its only synchronisation.
void ConnectionPool::worker_loop(const std::size_t index) {
  Worker& self = *workers_[index];
  std::unique_lock lock(mutex_);
  for (;;) {
    self.wake.wait(lock, [&] { return self.handoff.has_value() || stopping_; });
    if (!self.handoff.has_value()) return;
    HttpSocketServer::Accepted accepted = std::move(*self.handoff);
    self.handoff.reset();
    self.abort = false;
    const std::uint64_t connection = self.connection;
    lock.unlock();
    const WorkerSignals signals{&self.abort, &shutting_down_,
                                [this, index, connection] {
                                  std::lock_guard guard(mutex_);
                                  return admission_.header_complete(index, connection);
                                },
                                accepted.peer_scope == AddressScope::loopback, accepted.peer_key};
    const ConnectionResult result = serve_connection(accepted.socket.get(), router_, limits_.connection, signals, accepted.accepted_at);
    accepted.socket.reset();   // the pool owns the close: responded, evicted (closed unanswered) or gone
    if (observer_) {
      observer_(FinishedConnection{result, accepted.peer_scope,
                                   std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - accepted.accepted_at)});
    }
    lock.lock();
    admission_.finished(index, connection);   // ignored when an eviction already handed this worker a new connection
  }
}

PoolExit ConnectionPool::run_until(const std::function<bool()>& stop, const std::function<void()>& on_shutdown) {
  PoolExit exit = PoolExit::stopped;
  std::vector<PollEntry> entries;
  entries.reserve(1U + api_constants::kLingerListMax);   // static_assert above: <= kMaxPollEntries
  while (!stop()) {
    // Pre-flight P33: the supervisor's on-stopped hook closes the listener;
    // an invalid handle ends the loop even when stop() is a plain signal check.
    if (listener_.native_handle() == kInvalidNativeSocket) break;
    entries.clear();
    entries.push_back(PollEntry{listener_.native_handle(), true, false});
    linger_.append_poll_entries(entries);
    (void)poll_sockets(std::span<PollEntry>{entries}, api_constants::kAcceptPollSlice);
    linger_.service(std::chrono::steady_clock::now());
    if (!entries.front().readable) continue;
    HttpSocketServer::Accepted accepted;
    const auto status = listener_.accept_next(std::chrono::milliseconds(0), &accepted);
    if (status == HttpSocketServer::AcceptStatus::listener_failed) { exit = PoolExit::listener_failed; break; }
    if (status == HttpSocketServer::AcceptStatus::stopped) break;
    if (status != HttpSocketServer::AcceptStatus::accepted) continue;
    std::unique_lock lock(mutex_);
    const auto decision = admission_.admit(accepted.peer_scope, accepted.peer_key, accepted.accepted_at);
    if (decision.kind == AdmissionState::Kind::refuse) {
      lock.unlock();
      refuse_connection(std::move(accepted.socket), linger_, std::chrono::steady_clock::now());
      continue;
    }
    Worker& worker = *workers_[decision.worker];
    worker.connection = decision.connection;
    worker.peer = accepted.peer_scope;
    worker.handoff = std::move(accepted);
    if (decision.kind == AdmissionState::Kind::evict) worker.abort = true;   // seen within one 20 ms slice
    worker.wake.notify_one();
  }
  // Spec §2.8: stop accepting, tear the generation path down (on_shutdown),
  // close the listener and the lingering sockets, then cut and join workers.
  {
    std::lock_guard lock(mutex_);
    shutting_down_ = true;
  }
  if (on_shutdown) on_shutdown();
  listener_.shutdown_listener();
  linger_.clear();
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
    for (auto& w : workers_) {
      w->abort = true;
      w->handoff.reset();   // a connection handed over but not yet started is closed unanswered
    }
  }
  for (auto& w : workers_) w->wake.notify_all();
  for (auto& w : workers_) if (w->thread.joinable()) w->thread.join();
  return exit;
}

}  // namespace secure_frontend
