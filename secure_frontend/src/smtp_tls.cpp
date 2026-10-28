#include "secure_frontend/smtp_tls.hpp"

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#include <winsock2.h>
#include <ws2tcpip.h>
#define SECURITY_WIN32
#include <security.h>
#define SCHANNEL_USE_BLACKLISTS
#include <subauth.h>
#include <schannel.h>
#else
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace secure_frontend {
namespace {

constexpr std::size_t kMaxLineBytes = 4096;  // an SMTP reply line is at most 512; anything far longer is refused

#if defined(_WIN32)
using Socket = SOCKET;
constexpr Socket kNoSocket = INVALID_SOCKET;
void close_socket(const Socket s) { closesocket(s); }
bool sockets_ready() {
  static const bool ready = [] {
    WSADATA data{};
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return ready;
}
#else
using Socket = int;
constexpr Socket kNoSocket = -1;
void close_socket(const Socket s) { ::close(s); }
bool sockets_ready() { return true; }
#endif

// TCP to host:port, each address the resolver gives in turn; send and receive bounded by `timeout`.
Socket connect_tcp(const std::string& host, const std::uint16_t port, const std::chrono::seconds timeout) {
  if (!sockets_ready()) return kNoSocket;
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* found = nullptr;
  const std::string service = std::to_string(port);
  if (getaddrinfo(host.c_str(), service.c_str(), &hints, &found) != 0 || found == nullptr) return kNoSocket;
  Socket connected = kNoSocket;
  for (const addrinfo* a = found; a != nullptr && connected == kNoSocket; a = a->ai_next) {
    const Socket s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (s == kNoSocket) continue;
#if defined(_WIN32)
    const DWORD ms = static_cast<DWORD>(timeout.count() * 1000);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
    const bool ok = ::connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen)) == 0;
#else
    timeval tv{};
    tv.tv_sec = static_cast<time_t>(timeout.count());
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    const bool ok = ::connect(s, a->ai_addr, a->ai_addrlen) == 0;
#endif
    if (ok) connected = s;
    else close_socket(s);
  }
  freeaddrinfo(found);
  return connected;
}

bool send_all(const Socket s, const char* data, std::size_t size) {
  while (size > 0U) {
    const auto chunk = static_cast<int>(std::min<std::size_t>(size, 1U << 20U));
    const auto sent = ::send(s, data, chunk, 0);
    if (sent <= 0) return false;
    data += sent;
    size -= static_cast<std::size_t>(sent);
  }
  return true;
}

// The line reader both transports share over their own read_some.
class LineTransport : public SmtpTransport {
 public:
  std::optional<std::string> read_line() override {
    for (;;) {
      const auto end = input_.find("\r\n");
      if (end != std::string::npos) {
        std::string line = input_.substr(0, end);
        input_.erase(0, end + 2U);
        return line;
      }
      if (input_.size() > kMaxLineBytes) return std::nullopt;
      char buffer[4096];
      const auto got = read_some(buffer, sizeof(buffer));
      if (got <= 0) return std::nullopt;
      input_.append(buffer, static_cast<std::size_t>(got));
    }
  }

  std::optional<std::string> read_chunk() override {
    if (!input_.empty()) return std::exchange(input_, std::string{});
    char buffer[16384];
    const auto got = read_some(buffer, sizeof(buffer));
    if (got <= 0) return std::nullopt;
    return std::string(buffer, static_cast<std::size_t>(got));
  }

 protected:
  virtual long read_some(char* buffer, std::size_t size) = 0;
  void clear_input() { input_.clear(); }

 private:
  std::string input_;
};

#if defined(_WIN32)
// ------------------------------------------------------------------ SChannel (SSPI)
class SchannelTransport final : public LineTransport {
 public:
  SchannelTransport(const Socket socket, std::wstring host) : socket_(socket), host_(std::move(host)) {}
  ~SchannelTransport() override {
    if (has_context_) DeleteSecurityContext(&context_);
    if (has_credentials_) FreeCredentialsHandle(&credentials_);
    close_socket(socket_);
  }
  SchannelTransport(const SchannelTransport&) = delete;
  SchannelTransport& operator=(const SchannelTransport&) = delete;

  bool write(const std::string_view bytes) override {
    if (!tls_) return send_all(socket_, bytes.data(), bytes.size());
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      const std::size_t chunk = std::min<std::size_t>(bytes.size() - offset, sizes_.cbMaximumMessage);
      std::vector<char> packet(sizes_.cbHeader + chunk + sizes_.cbTrailer);
      std::memcpy(packet.data() + sizes_.cbHeader, bytes.data() + offset, chunk);
      SecBuffer buffers[4]{{sizes_.cbHeader, SECBUFFER_STREAM_HEADER, packet.data()},
                           {static_cast<unsigned long>(chunk), SECBUFFER_DATA, packet.data() + sizes_.cbHeader},
                           {sizes_.cbTrailer, SECBUFFER_STREAM_TRAILER, packet.data() + sizes_.cbHeader + chunk},
                           {0, SECBUFFER_EMPTY, nullptr}};
      SecBufferDesc description{SECBUFFER_VERSION, 4, buffers};
      if (EncryptMessage(&context_, 0, &description, 0) != SEC_E_OK) return false;
      if (!send_all(socket_, packet.data(), buffers[0].cbBuffer + buffers[1].cbBuffer + buffers[2].cbBuffer)) return false;
      offset += chunk;
    }
    return true;
  }

  bool start_tls() override {
    // TLS 1.2 and later only (older ones disabled), the certificate chain and the host name checked by SChannel.
    TLS_PARAMETERS parameters{};
    parameters.grbitDisabledProtocols = SP_PROT_SSL2_CLIENT | SP_PROT_SSL3_CLIENT | SP_PROT_TLS1_0_CLIENT | SP_PROT_TLS1_1_CLIENT;
    SCH_CREDENTIALS credentials{};
    credentials.dwVersion = SCH_CREDENTIALS_VERSION;
    credentials.dwFlags = SCH_CRED_AUTO_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS | SCH_USE_STRONG_CRYPTO;
    credentials.cTlsParameters = 1;
    credentials.pTlsParameters = &parameters;
    TimeStamp expiry{};
    if (AcquireCredentialsHandleW(nullptr, const_cast<wchar_t*>(UNISP_NAME_W), SECPKG_CRED_OUTBOUND, nullptr, &credentials, nullptr,
                                  nullptr, &credentials_, &expiry) != SEC_E_OK) {
      return false;
    }
    has_credentials_ = true;
    clear_input();
    return handshake();
  }

 protected:
  long read_some(char* buffer, const std::size_t size) override {
    if (!tls_) return ::recv(socket_, buffer, static_cast<int>(std::min<std::size_t>(size, 1U << 20U)), 0);
    for (;;) {
      if (!plain_.empty()) {
        const std::size_t n = std::min(size, plain_.size());
        std::memcpy(buffer, plain_.data(), n);
        plain_.erase(0, n);
        return static_cast<long>(n);
      }
      if (!cipher_.empty()) {
        SecBuffer buffers[4]{{static_cast<unsigned long>(cipher_.size()), SECBUFFER_DATA, cipher_.data()},
                             {0, SECBUFFER_EMPTY, nullptr},
                             {0, SECBUFFER_EMPTY, nullptr},
                             {0, SECBUFFER_EMPTY, nullptr}};
        SecBufferDesc description{SECBUFFER_VERSION, 4, buffers};
        const SECURITY_STATUS status = DecryptMessage(&context_, &description, 0, nullptr);
        if (status == SEC_E_OK || status == SEC_I_RENEGOTIATE) {
          std::string extra;
          for (const auto& b : buffers) {
            if (b.BufferType == SECBUFFER_DATA) plain_.append(static_cast<const char*>(b.pvBuffer), b.cbBuffer);
            if (b.BufferType == SECBUFFER_EXTRA) extra.assign(static_cast<const char*>(b.pvBuffer), b.cbBuffer);
          }
          cipher_ = std::move(extra);
          // a post-handshake message (TLS 1.3 session tickets) arrives as SEC_I_RENEGOTIATE: it carries no data
          if (status == SEC_I_RENEGOTIATE && !continue_after_renegotiate()) return -1;
          continue;
        }
        if (status == SEC_I_CONTEXT_EXPIRED) return 0;
        if (status != SEC_E_INCOMPLETE_MESSAGE) return -1;
      }
      char chunk[16384];
      const int got = ::recv(socket_, chunk, sizeof(chunk), 0);
      if (got <= 0) return got;
      cipher_.append(chunk, static_cast<std::size_t>(got));
    }
  }

 private:
  bool handshake() {
    constexpr unsigned long kFlags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                                     ISC_REQ_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;
    std::string received;
    bool first = true;
    for (int round = 0; round < 32; ++round) {
      SecBuffer in_buffers[2]{{static_cast<unsigned long>(received.size()), SECBUFFER_TOKEN, received.data()},
                              {0, SECBUFFER_EMPTY, nullptr}};
      SecBufferDesc in{SECBUFFER_VERSION, 2, in_buffers};
      SecBuffer out_buffers[1]{{0, SECBUFFER_TOKEN, nullptr}};
      SecBufferDesc out{SECBUFFER_VERSION, 1, out_buffers};
      unsigned long attributes = 0;
      const SECURITY_STATUS status =
          InitializeSecurityContextW(&credentials_, first ? nullptr : &context_, host_.data(), kFlags, 0, 0, first ? nullptr : &in, 0,
                                     first ? &context_ : nullptr, &out, &attributes, nullptr);
      if (first) has_context_ = status == SEC_I_CONTINUE_NEEDED || status == SEC_E_OK || status == SEC_I_INCOMPLETE_CREDENTIALS;
      first = false;
      if (out_buffers[0].cbBuffer > 0 && out_buffers[0].pvBuffer != nullptr) {
        const bool sent = send_all(socket_, static_cast<const char*>(out_buffers[0].pvBuffer), out_buffers[0].cbBuffer);
        FreeContextBuffer(out_buffers[0].pvBuffer);
        if (!sent) return false;
      }
      if (status == SEC_E_INCOMPLETE_MESSAGE) {
        if (!receive_more(received)) return false;
        continue;
      }
      if (status == SEC_E_OK) {
        if (in_buffers[1].BufferType == SECBUFFER_EXTRA) cipher_.assign(received.data() + received.size() - in_buffers[1].cbBuffer, in_buffers[1].cbBuffer);
        if (QueryContextAttributesW(&context_, SECPKG_ATTR_STREAM_SIZES, &sizes_) != SEC_E_OK) return false;
        tls_ = true;
        return true;
      }
      if (status != SEC_I_CONTINUE_NEEDED) return false;  // the certificate, its name, or no common TLS 1.2+
      if (in_buffers[1].BufferType == SECBUFFER_EXTRA) {
        received = received.substr(received.size() - in_buffers[1].cbBuffer);
      } else {
        received.clear();
        if (!receive_more(received)) return false;
      }
    }
    return false;
  }

  // After SEC_I_RENEGOTIATE: the post-handshake message goes back to InitializeSecurityContext.
  bool continue_after_renegotiate() {
    constexpr unsigned long kFlags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                                     ISC_REQ_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;
    SecBuffer in_buffers[2]{{static_cast<unsigned long>(cipher_.size()), SECBUFFER_TOKEN, cipher_.data()}, {0, SECBUFFER_EMPTY, nullptr}};
    SecBufferDesc in{SECBUFFER_VERSION, 2, in_buffers};
    SecBuffer out_buffers[1]{{0, SECBUFFER_TOKEN, nullptr}};
    SecBufferDesc out{SECBUFFER_VERSION, 1, out_buffers};
    unsigned long attributes = 0;
    const SECURITY_STATUS status =
        InitializeSecurityContextW(&credentials_, &context_, host_.data(), kFlags, 0, 0, &in, 0, nullptr, &out, &attributes, nullptr);
    if (out_buffers[0].cbBuffer > 0 && out_buffers[0].pvBuffer != nullptr) {
      (void)send_all(socket_, static_cast<const char*>(out_buffers[0].pvBuffer), out_buffers[0].cbBuffer);
      FreeContextBuffer(out_buffers[0].pvBuffer);
    }
    if (in_buffers[1].BufferType == SECBUFFER_EXTRA) cipher_ = cipher_.substr(cipher_.size() - in_buffers[1].cbBuffer);
    else cipher_.clear();
    return status == SEC_E_OK || status == SEC_I_CONTINUE_NEEDED;
  }

  bool receive_more(std::string& into) {
    char chunk[16384];
    const int got = ::recv(socket_, chunk, sizeof(chunk), 0);
    if (got <= 0) return false;
    into.append(chunk, static_cast<std::size_t>(got));
    return into.size() <= (1U << 20U);
  }

  Socket socket_;
  std::wstring host_;
  CredHandle credentials_{};
  CtxtHandle context_{};
  bool has_credentials_{false};
  bool has_context_{false};
  bool tls_{false};
  SecPkgContext_StreamSizes sizes_{};
  std::string cipher_;  // received, not yet decrypted
  std::string plain_;   // decrypted, not yet read
};

std::wstring widen(const std::string& host) {
  std::wstring out;
  for (const char c : host) out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
  return out;
}
#else
// ------------------------------------------------------------------ OpenSSL
class OpensslTransport final : public LineTransport {
 public:
  OpensslTransport(const Socket socket, std::string host) : socket_(socket), host_(std::move(host)) {}
  ~OpensslTransport() override {
    if (ssl_ != nullptr) {
      SSL_shutdown(ssl_);
      SSL_free(ssl_);
    }
    if (context_ != nullptr) SSL_CTX_free(context_);
    close_socket(socket_);
  }
  OpensslTransport(const OpensslTransport&) = delete;
  OpensslTransport& operator=(const OpensslTransport&) = delete;

  bool write(const std::string_view bytes) override {
    if (ssl_ == nullptr) return send_all(socket_, bytes.data(), bytes.size());
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      const int chunk = static_cast<int>(std::min<std::size_t>(bytes.size() - offset, 1U << 16U));
      const int sent = SSL_write(ssl_, bytes.data() + offset, chunk);
      if (sent <= 0) return false;
      offset += static_cast<std::size_t>(sent);
    }
    return true;
  }

  bool start_tls() override {
    // TLS 1.2 and later only, the system's trust store, the certificate's host name (SNI and checked).
    context_ = SSL_CTX_new(TLS_client_method());
    if (context_ == nullptr || SSL_CTX_set_min_proto_version(context_, TLS1_2_VERSION) != 1 ||
        SSL_CTX_set_default_verify_paths(context_) != 1) {
      return false;
    }
    SSL_CTX_set_verify(context_, SSL_VERIFY_PEER, nullptr);
    ssl_ = SSL_new(context_);
    if (ssl_ == nullptr || SSL_set_fd(ssl_, socket_) != 1 || SSL_set_tlsext_host_name(ssl_, host_.c_str()) != 1 ||
        SSL_set1_host(ssl_, host_.c_str()) != 1) {
      return false;
    }
    clear_input();
    if (SSL_connect(ssl_) != 1 || SSL_get_verify_result(ssl_) != X509_V_OK) {
      ERR_clear_error();
      return false;
    }
    return true;
  }

 protected:
  long read_some(char* buffer, const std::size_t size) override {
    if (ssl_ == nullptr) return ::recv(socket_, buffer, std::min<std::size_t>(size, 1U << 20U), 0);
    const int got = SSL_read(ssl_, buffer, static_cast<int>(std::min<std::size_t>(size, 1U << 20U)));
    return got <= 0 ? -1 : got;
  }

 private:
  Socket socket_;
  std::string host_;
  SSL_CTX* context_{nullptr};
  SSL* ssl_{nullptr};
};
#endif

}  // namespace

std::unique_ptr<SmtpTransport> connect_smtp(const std::string& host, const std::uint16_t port, const SmtpSecurity security,
                                            const std::chrono::seconds timeout, std::string* error) {
  const Socket socket = connect_tcp(host, port, timeout);
  if (socket == kNoSocket) {
    if (error != nullptr) *error = "the mail server could not be reached";
    return nullptr;
  }
#if defined(_WIN32)
  auto transport = std::make_unique<SchannelTransport>(socket, widen(host));
#else
  auto transport = std::make_unique<OpensslTransport>(socket, host);
#endif
  if (security == SmtpSecurity::ssl && !transport->start_tls()) {
    if (error != nullptr) *error = "the TLS handshake failed (the certificate, its host name or TLS 1.2)";
    return nullptr;
  }
  return transport;
}

}  // namespace secure_frontend
