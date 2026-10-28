#pragma once

#include "secure_frontend/api_constants.hpp"
#include "secure_frontend/loopback_address.hpp"
#include "secure_frontend/socket_io.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace secure_frontend {

enum class SseItemKind { data, comment };
struct SseItem final {
  SseItemKind kind;
  std::string text;
};  // data payload after "data: ", or comment text after ':'
enum class SseReadState { need_more, item, error };

// Pure: fed the raw body after the head; decodes HTTP chunk framing and SSE events.
class ChunkedSseReader final {
 public:
  explicit ChunkedSseReader(std::size_t max_event_bytes = api_constants::kMaxSseEventBytes);
  void feed(std::string_view bytes);
  [[nodiscard]] SseReadState next(SseItem& item);
  [[nodiscard]] bool terminal_chunk_seen() const noexcept;
  [[nodiscard]] bool done_seen() const noexcept;

 private:
  bool decode_chunks();
  std::size_t max_event_bytes_;
  std::string raw_;
  std::string events_;
  std::size_t chunk_left_{0};
  bool need_crlf_{false};
  bool awaiting_terminal_crlf_{false};
  bool terminal_{false};
  bool done_{false};
  bool failed_{false};
};

struct ResponseHead final {
  int status{0};
  std::vector<std::pair<std::string, std::string>> headers;
  [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const;  // ASCII case-insensitive
};

[[nodiscard]] std::optional<ResponseHead> parse_response_head(std::string_view head);  // through the blank line

class LoopbackStreamConnection final {
 public:
  [[nodiscard]] static std::optional<LoopbackStreamConnection> open(const LoopbackAddress& address, std::uint16_t port,
                                                                    std::chrono::milliseconds timeout, int receive_buffer_bytes = 0);
  LoopbackStreamConnection(LoopbackStreamConnection&&) noexcept;
  LoopbackStreamConnection& operator=(LoopbackStreamConnection&&) noexcept;
  ~LoopbackStreamConnection();
  [[nodiscard]] bool send_all(std::string_view bytes, std::chrono::milliseconds timeout);
  [[nodiscard]] std::optional<std::string> receive_some(std::chrono::milliseconds timeout);  // nullopt: timeout or error; "" + eof(): EOF
  [[nodiscard]] bool eof() const noexcept;
  void close() noexcept;

 private:
  explicit LoopbackStreamConnection(Socket socket) noexcept;
  Socket socket_;
  bool eof_{false};
};

struct ChatRequestOptions final {
  std::chrono::milliseconds timeout{std::chrono::seconds(30)};
  std::size_t max_event_bytes{api_constants::kMaxSseEventBytes};
  std::function<bool(const SseItem&)> on_item{};  // false → close now (cancel)
  std::vector<std::pair<std::string, std::string>> extra_headers{};
  int receive_buffer_bytes{0};
};

enum class ChatExchangeEnd { complete, cut, cancelled, protocol_error, timeout, connect_failed };

struct ChatExchangeResult final {
  ChatExchangeEnd end{ChatExchangeEnd::connect_failed};
  ResponseHead head;
  bool streamed{false};  // 200 + text/event-stream + chunked
  std::vector<SseItem> items;
  bool done_seen{false};
  bool terminal_chunk_seen{false};
  std::string body;  // Content-Length body of a complete response
};

[[nodiscard]] std::string render_chat_request(const LoopbackAddress& address, std::uint16_t port, std::string_view bearer_token,
                                              std::string_view json_body,
                                              const std::vector<std::pair<std::string, std::string>>& extra_headers = {});
[[nodiscard]] ChatExchangeResult read_chat_exchange(LoopbackStreamConnection& connection, const ChatRequestOptions& options = {});
[[nodiscard]] ChatExchangeResult post_chat_completion(const LoopbackAddress& address, std::uint16_t port, std::string_view bearer_token,
                                                      std::string_view json_body, const ChatRequestOptions& options = {});

}  // namespace secure_frontend
