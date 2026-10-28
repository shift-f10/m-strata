#include "secure_frontend/connection_pool.hpp"

#include "secure_frontend/api_errors.hpp"
#include "secure_frontend/http_wire.hpp"

#include <array>
#include <iterator>
#include <utility>

#if !defined(_WIN32)
#include <sys/socket.h>
#endif

namespace secure_frontend {

LingerList::LingerList(const std::size_t max) : max_(max) {}

void LingerList::add(Socket socket, const Clock::time_point now) {
  if (max_ == 0U) return;   // no room at all: `socket` closes here
  while (entries_.size() >= max_) entries_.pop_front();   // Socket's destructor closes the oldest (spec §2.4)
  entries_.push_back(Entry{std::move(socket), now, 0U});
}

void LingerList::append_poll_entries(std::vector<PollEntry>& entries) const {
  for (const auto& entry : entries_) entries.push_back(PollEntry{entry.socket.get(), true, false});
}

void LingerList::service(const Clock::time_point now) {
  std::array<char, api_constants::kRecvChunk> sink{};
  for (auto it = entries_.begin(); it != entries_.end();) {
    bool close = now - it->since >= api_constants::kLingerBudget;
    while (!close) {
      const IoResult r = recv_some(it->socket.get(), sink.data(), sink.size());
      if (r.status == IoStatus::would_block) break;
      if (r.status != IoStatus::done || r.bytes == 0U) {   // EOF, reset or error
        close = true;
        break;
      }
      it->drained += r.bytes;
      close = it->drained >= api_constants::kDrainBytes;
    }
    it = close ? entries_.erase(it) : std::next(it);
  }
}

std::size_t LingerList::size() const noexcept { return entries_.size(); }
void LingerList::clear() noexcept { entries_.clear(); }

std::string refusal_response(const std::chrono::system_clock::time_point now) {
  return serialize_response(make_error_response(ApiError::server_busy), now);
}

void refuse_connection(Socket socket, LingerList& linger, const LingerList::Clock::time_point now) {
  const std::string bytes = refusal_response(std::chrono::system_clock::now());
  (void)send_some(socket.get(), bytes);   // one non-blocking send; the accept thread never waits on a client
#if defined(_WIN32)
  (void)::shutdown(socket.get(), SD_SEND);
#else
  (void)::shutdown(socket.get(), SHUT_WR);
#endif
  linger.add(std::move(socket), now);
}

}  // namespace secure_frontend
