#pragma once

#include "secure_frontend/api_constants.hpp"
#include "secure_frontend/config.hpp"
#include "secure_frontend/http_socket_server.hpp"
#include "secure_frontend/socket_io.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// Accept-time admission (SP3 spec §2.3) and the lingering-close list for
// refused sockets (§2.4). The pool (Task 12) owns both: the accept thread calls
// admit() and workers call header_complete()/finished(), all under the pool
// mutex; the LingerList is touched by the accept thread only.
namespace secure_frontend {

// Family byte + 16 address bytes, as HttpSocketServer::Accepted::peer_key.
using PeerKey = std::array<unsigned char, 17>;

// idle: no connection; header: P1 (not yet authenticated, evictable); busy: past P1.
enum class WorkerPhase { idle, header, busy };

// Not thread-safe: the pool calls it under its own mutex.
class AdmissionState final {
 public:
  using Clock = std::chrono::steady_clock;
  AdmissionState(std::uint32_t workers, std::uint32_t per_remote_cap);
  enum class Kind { assign, evict, refuse };
  struct Decision final {
    Kind kind;
    std::size_t worker{0};
    std::uint64_t connection{0};
  };
  // Spec §2.3: remote over cap -> refuse; idle worker -> assign; else the worker longest in P1 -> evict; else refuse.
  // assign/evict put `worker` in P1 since `now` for the new peer and return the new connection id.
  [[nodiscard]] Decision admit(AddressScope scope, const PeerKey& peer, Clock::time_point now);
  // P1 -> busy for `connection`; false when that connection was evicted meanwhile.
  [[nodiscard]] bool header_complete(std::size_t worker, std::uint64_t connection);
  // busy/header -> idle and the peer count released; ignored for a stale (evicted) connection.
  void finished(std::size_t worker, std::uint64_t connection);
  [[nodiscard]] WorkerPhase phase(std::size_t worker) const;
  [[nodiscard]] std::size_t remote_count(const PeerKey& peer) const;

 private:
  struct Slot {
    WorkerPhase phase{WorkerPhase::idle};
    Clock::time_point since{};
    std::uint64_t connection{0};
    bool remote{false};
    PeerKey peer{};
  };
  void release_peer(const Slot& slot);
  std::vector<Slot> slots_;
  std::uint32_t per_remote_cap_;
  std::map<PeerKey, std::size_t> remote_counts_;
  std::uint64_t next_connection_{0};
};

// Refused sockets wait here (already half-closed) until peer EOF, an error,
// kDrainBytes drained or kLingerBudget elapsed. The accept thread polls them
// together with the listener in one poll_sockets call per slice.
class LingerList final {
 public:
  using Clock = std::chrono::steady_clock;
  explicit LingerList(std::size_t max = api_constants::kLingerListMax);
  void add(Socket socket, Clock::time_point now);        // full: the oldest is closed first
  void append_poll_entries(std::vector<PollEntry>& entries) const;
  void service(Clock::time_point now);                    // drain non-blocking; close on EOF, error, 256 KiB or 100 ms
  [[nodiscard]] std::size_t size() const noexcept;
  void clear() noexcept;

 private:
  struct Entry {
    Socket socket;
    Clock::time_point since;
    std::size_t drained{0};
  };
  std::deque<Entry> entries_;
  std::size_t max_;
};

[[nodiscard]] std::string refusal_response(std::chrono::system_clock::time_point now);   // fixed 503 server_busy, <= 512 bytes
void refuse_connection(Socket socket, LingerList& linger, LingerList::Clock::time_point now);  // one non-blocking send, half-close, linger

// Per-connection bounds plus the admission caps (spec §2.1, §2.3): workers =
// limits.concurrency.max_concurrent_requests, per_remote_cap =
// max_connections_per_remote_address.
struct PoolLimits final {
  std::uint32_t workers;
  std::uint32_t per_remote_cap;
  ConnectionLimits connection;
};

struct FinishedConnection final {
  ConnectionResult result;
  AddressScope peer;
  std::chrono::milliseconds elapsed;
};
using ConnectionObserver = std::function<void(const FinishedConnection&)>;  // Task 21 request logging

enum class PoolExit { stopped, listener_failed };

// The concurrent server (spec §2.1): one accept loop on the calling thread plus
// a fixed pool of workers created at construction and joined on exit. Every
// AdmissionState call (admit, header_complete, finished) runs under the single
// mutex_; the LingerList is touched by the accept thread only. The listener and
// the linger list share one poll_sockets call per 100 ms slice, at most
// 1 + kLingerListMax (65) entries, within api_constants::kMaxPollEntries (128).
class ConnectionPool final {
 public:
  ConnectionPool(HttpSocketServer& listener, PoolLimits limits, RequestRouter& router, ConnectionObserver observer = {});
  ~ConnectionPool();                                  // joins (run_until normally already did)
  // Accept loop on the calling thread until stop() is true or the listener fails; then spec §2.8:
  // shutting_down set, on_shutdown() (main: gate.shutdown()), listener closed, lingering sockets closed, workers joined.
  PoolExit run_until(const std::function<bool()>& stop, const std::function<void()>& on_shutdown = {});
  [[nodiscard]] const std::atomic<bool>& shutting_down() const noexcept;

 private:
  struct Worker final {
    std::thread thread;
    std::atomic<bool> abort{false};
    std::optional<HttpSocketServer::Accepted> handoff;
    std::uint64_t connection{0};
    AddressScope peer{AddressScope::remote};
    std::condition_variable wake;
  };
  void worker_loop(std::size_t index);
  HttpSocketServer& listener_;
  PoolLimits limits_;
  RequestRouter& router_;
  ConnectionObserver observer_;
  std::mutex mutex_;
  AdmissionState admission_;
  LingerList linger_;
  std::vector<std::unique_ptr<Worker>> workers_;
  std::atomic<bool> shutting_down_{false};
  bool stopping_{false};
};

}  // namespace secure_frontend
