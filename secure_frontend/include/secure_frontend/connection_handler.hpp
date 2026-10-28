#pragma once

#include "secure_frontend/config_defaults.hpp"
#include "secure_frontend/http_request_head.hpp"
#include "secure_frontend/request_router.hpp"
#include "secure_frontend/socket_io.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <string_view>

namespace secure_frontend {

// Per-connection bounds (spec §2.2), derived from FrontendConfig by
// connection_limits() (subsystem_limits.hpp). The defaults are SP1's schema
// defaults, for callers without a typed config (the smoke servers).
struct ConnectionLimits final {
  HeadLimits head{static_cast<std::size_t>(config_defaults::kMaxRequestLineBytes),
                  static_cast<std::size_t>(config_defaults::kMaxHeaderBytes),
                  static_cast<std::size_t>(config_defaults::kMaxHeaders),
                  static_cast<std::size_t>(config_defaults::kMaxRequestBytes)};
  std::chrono::milliseconds header_deadline{config_defaults::kHeaderDeadlineMs};    // P1, from accept
  std::chrono::milliseconds request_deadline{config_defaults::kRequestDeadlineMs};  // P3, from accept
  std::chrono::milliseconds write_stall{config_defaults::kWriteStallMs};            // P7, on progress
  std::chrono::milliseconds response_write_deadline{config_defaults::kResponseWriteDeadlineMs};
};

// What a worker watches while it serves one connection. Each pointer may be null (never set).
struct WorkerSignals final {
  const std::atomic<bool>* abort{nullptr};          // eviction in P1 (spec §2.3)
  const std::atomic<bool>* shutting_down{nullptr};  // orderly shutdown (spec §2.8)
  std::function<bool()> on_header_complete;         // returns false when the connection was evicted meanwhile
  bool loopback_peer{false};                        // the accepted peer is a loopback address (RequestContext)
  PeerKey peer{};                                   // the accepted peer's address (RequestContext)
};

enum class ConnectionOutcome { responded, evicted, timed_out, client_gone, transport_error, shutdown };

struct ConnectionResult final {
  ConnectionOutcome outcome;
  int status{0};
  std::string_view code;   // a static literal (HttpResponse::log_code); empty when none
  std::size_t bytes_out{0};
  RouteId route{RouteId::other};
  bool streamed{false};    // the response was an SSE stream (M2)
};

// Serves one accepted, non-blocking socket through P1-P8 (spec §2.2): header
// (header_deadline), parse + authorize before any body byte, 100 Continue only
// when no body byte has arrived, body (request_deadline), handle, then
// deliver_route_result. Every wait is a poll of at most kStreamPollSlice, so
// `abort` and `shutting_down` are seen within one slice. Never closes `socket`.
[[nodiscard]] ConnectionResult serve_connection(NativeSocket socket, RequestRouter& router, const ConnectionLimits& limits,
                                                const WorkerSignals& signals, std::chrono::steady_clock::time_point accepted_at);

// Writes a RouteResult to the client (spec §2.2 P7-P8, §3.11): a complete response
// with Content-Length (kNoResponseStatus = close without writing), or a
// StreamHandler run against a socket-backed StreamSink (HTTP/1.1 chunks; the
// decision's CORS headers are added to the stream head). M2 produces the handlers.
[[nodiscard]] ConnectionResult deliver_route_result(NativeSocket socket, RouteResult result, const RouteDecision& decision,
                                                    const ConnectionLimits& limits, const WorkerSignals& signals);

}  // namespace secure_frontend
