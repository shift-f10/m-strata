#include "secure_frontend/stream_socket.hpp"

#include "secure_frontend/api_constants.hpp"

#include <array>

namespace secure_frontend {

AcceptedSocketStream::AcceptedSocketStream(const NativeSocket socket) : socket_(socket) {
  (void)set_non_blocking(socket_);  // M1 already makes accepted sockets non-blocking; idempotent
  set_no_delay(socket_);            // spec §3.2: TCP_NODELAY for streams
}

SendStep AcceptedSocketStream::send_some(const std::string_view bytes) {
  const IoResult result = secure_frontend::send_some(socket_, bytes);  // ≤ kMaxSendChunk, EINTR retried (M1)
  switch (result.status) {
    case IoStatus::done: return {result.bytes > 0U ? SendStatus::progressed : SendStatus::would_block, result.bytes};
    case IoStatus::would_block: return {SendStatus::would_block, 0U};
    case IoStatus::closed:
    case IoStatus::failed: break;
  }
  return {SendStatus::failed, 0U};
}

bool AcceptedSocketStream::wait_writable(const std::chrono::milliseconds timeout) {
  std::array<PollEntry, 1> entry{PollEntry{socket_, false, true}};
  return poll_sockets(entry, timeout) > 0 && entry[0].writable && !entry[0].failed;
}

bool AcceptedSocketStream::peer_gone() { return probe_peer(socket_) == PeerState::gone; }  // EOF, reset or any byte (spec §2.6)

}  // namespace secure_frontend
