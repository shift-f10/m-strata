#include "secure_frontend/api_client.hpp"

#include "secure_frontend/socket_io.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>

#if defined(_WIN32)
#include <ws2tcpip.h>
using socklen_t = int;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace secure_frontend {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t kMaxHeadBytes = 16U * 1024U;
constexpr std::size_t kMaxCompleteBodyBytes = 16U * 1024U * 1024U;

bool equals_ascii_ci(const std::string_view a, const std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
         });
}

int hex_value(const char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Winsock stays started for the process (WSAStartup is reference-counted); a no-op on POSIX.
void ensure_winsock() {
  static const WinsockGuard guard;
  (void)guard;
}

bool wait_for(const NativeSocket socket, const bool read, const std::chrono::milliseconds timeout) {
  std::array<PollEntry, 1> entry{PollEntry{socket, read, !read}};
  return poll_sockets(entry, timeout) > 0 && (read ? (entry[0].readable || entry[0].failed) : entry[0].writable);
}

}  // namespace

// ---- ChunkedSseReader ----------------------------------------------------

ChunkedSseReader::ChunkedSseReader(const std::size_t max_event_bytes) : max_event_bytes_(max_event_bytes) {}

void ChunkedSseReader::feed(const std::string_view bytes) { raw_ += bytes; }

bool ChunkedSseReader::terminal_chunk_seen() const noexcept { return terminal_; }
bool ChunkedSseReader::done_seen() const noexcept { return done_; }

// Moves complete chunk payload bytes from raw_ to events_; false on a framing error.
bool ChunkedSseReader::decode_chunks() {
  for (;;) {
    if (terminal_) return raw_.empty();  // nothing may follow 0\r\n\r\n
    if (chunk_left_ > 0U) {
      const std::size_t take = std::min(chunk_left_, raw_.size());
      events_.append(raw_, 0, take);
      raw_.erase(0, take);
      chunk_left_ -= take;
      if (chunk_left_ > 0U) return true;
      need_crlf_ = true;
    }
    if (need_crlf_ || awaiting_terminal_crlf_) {
      if (raw_.size() < 2U) return true;
      if (raw_.compare(0, 2, "\r\n") != 0) return false;
      raw_.erase(0, 2);
      if (awaiting_terminal_crlf_) terminal_ = true;
      need_crlf_ = false;
      awaiting_terminal_crlf_ = false;
      continue;
    }
    const auto end = raw_.find("\r\n");
    if (end == std::string::npos) return raw_.size() <= 10U;
    if (end == 0U || end > 8U) return false;
    std::size_t size = 0U;
    for (std::size_t index = 0; index < end; ++index) {
      const int digit = hex_value(raw_[index]);
      if (digit < 0) return false;
      size = size * 16U + static_cast<std::size_t>(digit);
    }
    if (size > max_event_bytes_) return false;
    raw_.erase(0, end + 2U);
    if (size == 0U) {
      awaiting_terminal_crlf_ = true;  // no trailers
      continue;
    }
    chunk_left_ = size;
  }
}

SseReadState ChunkedSseReader::next(SseItem& item) {
  if (failed_) return SseReadState::error;
  if (!decode_chunks()) {
    failed_ = true;
    return SseReadState::error;
  }
  const auto end = events_.find("\n\n");
  if (end == std::string::npos) {
    if (events_.size() > max_event_bytes_ || (terminal_ && !events_.empty())) failed_ = true;
    return failed_ ? SseReadState::error : SseReadState::need_more;
  }
  if (end + 2U > max_event_bytes_) {
    failed_ = true;
    return SseReadState::error;
  }
  const std::string event = events_.substr(0, end);
  events_.erase(0, end + 2U);
  if (!event.empty() && event.front() == ':') {
    item = {SseItemKind::comment, event.substr(event.size() > 1U && event[1] == ' ' ? 2U : 1U)};
    return SseReadState::item;
  }
  if (done_ || event.rfind("data: ", 0) != 0 || event.find('\n') != std::string::npos) {
    failed_ = true;
    return SseReadState::error;
  }
  item = {SseItemKind::data, event.substr(6)};
  if (item.text == "[DONE]") done_ = true;
  return SseReadState::item;
}

// ---- head ------------------------------------------------------------------

std::optional<std::string_view> ResponseHead::header(const std::string_view name) const {
  for (const auto& [key, value] : headers) {
    if (equals_ascii_ci(key, name)) return std::string_view(value);
  }
  return std::nullopt;
}

std::optional<ResponseHead> parse_response_head(const std::string_view head) {
  if (head.rfind("HTTP/1.1 ", 0) != 0 || head.size() < 12U) return std::nullopt;
  ResponseHead parsed;
  for (std::size_t index = 9; index < 12; ++index) {
    if (!std::isdigit(static_cast<unsigned char>(head[index]))) return std::nullopt;
    parsed.status = parsed.status * 10 + (head[index] - '0');
  }
  std::size_t line = head.find("\r\n");
  while (line != std::string_view::npos) {
    const std::size_t start = line + 2U;
    const std::size_t end = head.find("\r\n", start);
    if (end == std::string_view::npos || end == start) break;
    const auto field = head.substr(start, end - start);
    const auto colon = field.find(':');
    if (colon == std::string_view::npos) return std::nullopt;
    auto value = field.substr(colon + 1U);
    while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
    parsed.headers.emplace_back(std::string(field.substr(0, colon)), std::string(value));
    line = end;
  }
  return parsed;
}

// ---- connection --------------------------------------------------------------

LoopbackStreamConnection::LoopbackStreamConnection(Socket socket) noexcept : socket_(std::move(socket)) {}
LoopbackStreamConnection::LoopbackStreamConnection(LoopbackStreamConnection&&) noexcept = default;
LoopbackStreamConnection& LoopbackStreamConnection::operator=(LoopbackStreamConnection&&) noexcept = default;
LoopbackStreamConnection::~LoopbackStreamConnection() = default;
void LoopbackStreamConnection::close() noexcept { socket_.reset(); }
bool LoopbackStreamConnection::eof() const noexcept { return eof_; }

std::optional<LoopbackStreamConnection> LoopbackStreamConnection::open(const LoopbackAddress& address, const std::uint16_t port,
                                                                       const std::chrono::milliseconds timeout, const int receive_buffer_bytes) {
  ensure_winsock();
  const IpAddress& ip = address.ip();
  sockaddr_storage storage{};
  socklen_t length = 0;
  if (ip.family() == IpFamily::v4) {
    auto* v4 = reinterpret_cast<sockaddr_in*>(&storage);
    v4->sin_family = AF_INET;
    v4->sin_port = htons(port);
    std::memcpy(&v4->sin_addr, ip.bytes().data(), 4);
    length = sizeof(sockaddr_in);
  } else {
    auto* v6 = reinterpret_cast<sockaddr_in6*>(&storage);
    v6->sin6_family = AF_INET6;
    v6->sin6_port = htons(port);
    std::memcpy(&v6->sin6_addr, ip.bytes().data(), 16);
    length = sizeof(sockaddr_in6);
  }
  Socket socket(::socket(storage.ss_family, SOCK_STREAM, IPPROTO_TCP));
  if (!socket.valid() || !set_non_blocking(socket.get())) return std::nullopt;
  if (receive_buffer_bytes > 0) {
    (void)setsockopt(socket.get(), SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&receive_buffer_bytes), sizeof(receive_buffer_bytes));
  }
  // The only connect() in the frontend sources (spec §6.1); the address is loopback by construction.
  (void)::connect(socket.get(), reinterpret_cast<const sockaddr*>(&storage), length);  // completes asynchronously
  int error = 0;
  socklen_t error_length = sizeof(error);
  if (!wait_for(socket.get(), false, timeout) ||
      getsockopt(socket.get(), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &error_length) != 0 || error != 0) {
    return std::nullopt;
  }
  return LoopbackStreamConnection(std::move(socket));
}

bool LoopbackStreamConnection::send_all(std::string_view bytes, const std::chrono::milliseconds timeout) {
  const auto until = Clock::now() + timeout;
  while (!bytes.empty()) {
    const IoResult sent = send_some(socket_.get(), bytes);  // M1: ≤ kMaxSendChunk, EINTR retried
    if (sent.status == IoStatus::done && sent.bytes > 0U) {
      bytes.remove_prefix(sent.bytes);
      continue;
    }
    if (sent.status != IoStatus::would_block && sent.status != IoStatus::done) return false;
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now());
    if (left.count() <= 0) return false;
    (void)wait_for(socket_.get(), false, std::min(left, std::chrono::milliseconds(50)));
  }
  return true;
}

std::optional<std::string> LoopbackStreamConnection::receive_some(const std::chrono::milliseconds timeout) {
  if (eof_) return std::string();
  if (!wait_for(socket_.get(), true, timeout)) return std::nullopt;
  char buffer[65536];
  const IoResult got = recv_some(socket_.get(), buffer, sizeof(buffer));
  if (got.status == IoStatus::closed) {
    eof_ = true;
    return std::string();
  }
  if (got.status != IoStatus::done) return std::nullopt;
  return std::string(buffer, got.bytes);
}

// ---- exchange ----------------------------------------------------------------

std::string render_chat_request(const LoopbackAddress& address, const std::uint16_t port, const std::string_view bearer_token,
                                const std::string_view json_body, const std::vector<std::pair<std::string, std::string>>& extra_headers) {
  const IpAddress& ip = address.ip();
  const std::string host = ip.family() == IpFamily::v6 ? "[" + ip.canonical_text() + "]" : ip.canonical_text();
  std::string request = "POST /v1/chat/completions HTTP/1.1\r\nHost: " + host + ":" + std::to_string(port) + "\r\n";
  request += "Authorization: Bearer ";
  request += bearer_token;
  request += "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(json_body.size()) + "\r\n";
  for (const auto& [name, value] : extra_headers) request += name + ": " + value + "\r\n";
  request += "\r\n";
  request += json_body;
  return request;
}

ChatExchangeResult read_chat_exchange(LoopbackStreamConnection& connection, const ChatRequestOptions& options) {
  ChatExchangeResult result;
  result.end = ChatExchangeEnd::timeout;
  const auto until = Clock::now() + options.timeout;
  std::string head;
  std::string body;
  std::optional<ChunkedSseReader> reader;
  std::size_t content_length = 0U;
  for (;;) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now());
    if (left.count() <= 0) return result;
    const auto bytes = connection.receive_some(std::min(left, std::chrono::milliseconds(50)));
    if (!bytes) continue;
    if (bytes->empty() && connection.eof()) {
      if (reader) {
        result.end = reader->done_seen() && reader->terminal_chunk_seen() ? ChatExchangeEnd::complete : ChatExchangeEnd::cut;
      } else {
        result.end = !head.empty() && head.find("\r\n\r\n") != std::string::npos && body.size() == content_length ? ChatExchangeEnd::complete
                                                                                                                 : ChatExchangeEnd::cut;
        result.body = std::move(body);
      }
      return result;
    }
    std::string_view incoming = *bytes;
    if (result.head.status == 0) {
      head += incoming;
      const auto end = head.find("\r\n\r\n");
      if (end == std::string::npos) {
        if (head.size() > kMaxHeadBytes) { result.end = ChatExchangeEnd::protocol_error; return result; }
        continue;
      }
      const auto parsed = parse_response_head(std::string_view(head).substr(0, end + 4U));
      if (!parsed) { result.end = ChatExchangeEnd::protocol_error; return result; }
      result.head = *parsed;
      const auto type = result.head.header("Content-Type");
      const auto coding = result.head.header("Transfer-Encoding");
      result.streamed = result.head.status == 200 && type && type->rfind("text/event-stream", 0) == 0 && coding && equals_ascii_ci(*coding, "chunked");
      if (result.streamed) {
        reader.emplace(options.max_event_bytes);
      } else {
        const auto length = result.head.header("Content-Length");
        if (!length || length->empty() || length->size() > 9U || length->find_first_not_of("0123456789") != std::string_view::npos) {
          result.end = ChatExchangeEnd::protocol_error;
          return result;
        }
        content_length = static_cast<std::size_t>(std::stoul(std::string(*length)));
        if (content_length > kMaxCompleteBodyBytes) { result.end = ChatExchangeEnd::protocol_error; return result; }
      }
      const std::string rest = head.substr(end + 4U);
      head.resize(end + 4U);
      if (rest.empty()) continue;
      body.clear();
      incoming = std::string_view();
      if (reader) reader->feed(rest); else body += rest;
    }
    if (reader) {
      reader->feed(incoming);
      SseItem item{};
      SseReadState state;
      while ((state = reader->next(item)) == SseReadState::item) {
        if (item.kind == SseItemKind::data && item.text == "[DONE]") result.done_seen = true;
        result.items.push_back(item);
        if (options.on_item && !options.on_item(item)) {
          connection.close();
          result.end = ChatExchangeEnd::cancelled;
          return result;
        }
      }
      result.terminal_chunk_seen = reader->terminal_chunk_seen();
      if (state == SseReadState::error) { result.end = ChatExchangeEnd::protocol_error; return result; }
    } else {
      body += incoming;
      if (body.size() > content_length) { result.end = ChatExchangeEnd::protocol_error; return result; }
    }
  }
}

ChatExchangeResult post_chat_completion(const LoopbackAddress& address, const std::uint16_t port, const std::string_view bearer_token,
                                        const std::string_view json_body, const ChatRequestOptions& options) {
  auto connection = LoopbackStreamConnection::open(address, port, std::min(options.timeout, std::chrono::milliseconds(5000)),
                                                   options.receive_buffer_bytes);
  if (!connection) return {};
  if (!connection->send_all(render_chat_request(address, port, bearer_token, json_body, options.extra_headers), options.timeout)) {
    ChatExchangeResult failed;
    failed.end = ChatExchangeEnd::cut;
    return failed;
  }
  return read_chat_exchange(*connection, options);
}

}  // namespace secure_frontend
