#pragma once

#include "secure_frontend/socket_io.hpp"

#include <chrono>
#include <cstddef>
#include <string_view>

namespace secure_frontend {

enum class SendStatus { progressed, would_block, failed };
struct SendStep final {
  SendStatus status;
  std::size_t sent;
};

// One connected socket, never blocking (spec §2.7): the seam the chunked sink
// writes through, so the sink's stall/flush logic is testable without a kernel.
class StreamSocket {
 public:
  virtual SendStep send_some(std::string_view bytes) = 0;             // one send of ≤ api_constants::kMaxSendChunk bytes
  virtual bool wait_writable(std::chrono::milliseconds timeout) = 0;  // poll for writability
  virtual bool peer_gone() = 0;                                       // peek: EOF, reset, or any stray byte
  virtual ~StreamSocket() = default;
};

class AcceptedSocketStream final : public StreamSocket {
 public:
  explicit AcceptedSocketStream(NativeSocket socket);  // non-blocking + TCP_NODELAY; does not own the socket
  SendStep send_some(std::string_view bytes) override;
  bool wait_writable(std::chrono::milliseconds timeout) override;
  bool peer_gone() override;

 private:
  NativeSocket socket_;
};

}  // namespace secure_frontend
