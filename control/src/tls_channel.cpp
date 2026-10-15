#include "lanaudio/control/tls_channel.hpp"

#include "lanaudio/protocol/control_messages.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <algorithm>
#include <csignal>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace lanaudio::control {
namespace {

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
void close_socket(SocketHandle socket) { closesocket(socket); }
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
void close_socket(SocketHandle socket) { ::close(socket); }
#endif

constexpr auto kAcceptedSocketTimeout = std::chrono::seconds(5);

struct IgnoreSigpipe {
  IgnoreSigpipe() { ::signal(SIGPIPE, SIG_IGN); }
} kIgnoreSigpipe;

SocketHandle as_socket(std::intptr_t handle) {
  return static_cast<SocketHandle>(handle);
}

std::string openssl_error(std::string_view context) {
  const auto code = ERR_get_error();
  char buffer[256] = {};
  if (code != 0) ERR_error_string_n(code, buffer, sizeof(buffer));
  std::string message(context);
  if (buffer[0] != '\0') {
    message += ": ";
    message += buffer;
  }
  return message;
}

// Self-signed device identities cannot chain to a certificate authority. The
// handshake therefore accepts the chain and the caller pins the fingerprint
// immediately afterwards; an unpinned channel is never used for media.
int accept_any_certificate(int /*preverify*/, X509_STORE_CTX* /*context*/) {
  return 1;
}

void apply_socket_timeout(SocketHandle socket,
                          std::chrono::milliseconds timeout) {
#ifdef _WIN32
  DWORD value = static_cast<DWORD>(timeout.count());
  setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
             reinterpret_cast<const char*>(&value), sizeof(value));
  setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
             reinterpret_cast<const char*>(&value), sizeof(value));
#else
  timeval value{};
  value.tv_sec = static_cast<decltype(value.tv_sec)>(timeout.count() / 1000);
  value.tv_usec =
      static_cast<decltype(value.tv_usec)>((timeout.count() % 1000) * 1000);
  setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
  setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value));
#endif
  int flag = 1;
  setsockopt(socket, IPPROTO_TCP, TCP_NODELAY,
             reinterpret_cast<const char*>(&flag), sizeof(flag));
#ifdef SO_NOSIGPIPE
  setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE,
             reinterpret_cast<const char*>(&flag), sizeof(flag));
#endif
}

void endpoint_from_sockaddr(const sockaddr_storage& storage,
                            Endpoint& endpoint) {
  if (storage.ss_family == AF_INET6) {
    const auto* address = reinterpret_cast<const sockaddr_in6*>(&storage);
    std::memcpy(endpoint.address.data(), &address->sin6_addr, 16);
    endpoint.port = ntohs(address->sin6_port);
  } else if (storage.ss_family == AF_INET) {
    const auto* address = reinterpret_cast<const sockaddr_in*>(&storage);
    endpoint.address = {};
    endpoint.address[10] = 0xff;
    endpoint.address[11] = 0xff;
    std::memcpy(endpoint.address.data() + 12, &address->sin_addr, 4);
    endpoint.port = ntohs(address->sin_port);
  }
}

SSL_CTX* make_context(security::DeviceIdentity& identity, bool server,
                      std::string& error) {
  SSL_CTX* context = SSL_CTX_new(server ? TLS_server_method()
                                        : TLS_client_method());
  if (context == nullptr) {
    error = openssl_error("unable to create the TLS context");
    return nullptr;
  }
  // TLS 1.3 only. Older versions are not negotiated at all.
  if (SSL_CTX_set_min_proto_version(context, TLS1_3_VERSION) != 1 ||
      SSL_CTX_set_max_proto_version(context, TLS1_3_VERSION) != 1) {
    error = openssl_error("unable to restrict the control channel to TLS 1.3");
    SSL_CTX_free(context);
    return nullptr;
  }
  if (SSL_CTX_use_certificate(
          context, static_cast<X509*>(identity.certificate_handle())) != 1 ||
      SSL_CTX_use_PrivateKey(
          context, static_cast<EVP_PKEY*>(identity.key_handle())) != 1) {
    error = openssl_error("unable to install the device identity");
    SSL_CTX_free(context);
    return nullptr;
  }
  // Both sides must present a certificate; the peer identity is pinned after
  // the handshake completes.
  SSL_CTX_set_verify(context,
                     SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                     accept_any_certificate);
  SSL_CTX_set_session_cache_mode(context, SSL_SESS_CACHE_OFF);
  return context;
}

bool capture_peer_fingerprint(SSL* ssl, security::Fingerprint& fingerprint,
                              std::string& error) {
  X509* peer = SSL_get1_peer_certificate(ssl);
  if (peer == nullptr) {
    error = "the peer did not present a certificate";
    return false;
  }
  const bool ok = security::certificate_fingerprint(peer, fingerprint);
  X509_free(peer);
  if (!ok) error = "unable to compute the peer identity fingerprint";
  return ok;
}

}  // namespace

TlsChannel::TlsChannel(void* context, void* ssl, std::intptr_t socket,
                       bool is_client,
                       security::Fingerprint peer_fingerprint, Endpoint peer)
    : context_(context),
      ssl_(ssl),
      socket_(socket),
      is_client_(is_client),
      peer_fingerprint_(peer_fingerprint),
      peer_(peer) {}

TlsChannel::~TlsChannel() { close(); }

void TlsChannel::interrupt() noexcept {
  interrupted_.store(true, std::memory_order_release);
  const std::lock_guard<std::mutex> lock(io_mutex_);
  if (socket_ != -1) {
#ifdef _WIN32
    ::shutdown(as_socket(socket_), SD_BOTH);
#else
    ::shutdown(as_socket(socket_), SHUT_RDWR);
#endif
  }
}

void TlsChannel::close() noexcept {
  interrupted_.store(true, std::memory_order_release);
  const std::lock_guard<std::mutex> lock(io_mutex_);
  if (ssl_ != nullptr) {
    SSL_shutdown(static_cast<SSL*>(ssl_));
    SSL_free(static_cast<SSL*>(ssl_));
    ssl_ = nullptr;
  }
  if (context_ != nullptr) {
    SSL_CTX_free(static_cast<SSL_CTX*>(context_));
    context_ = nullptr;
  }
  if (socket_ != -1) {
    close_socket(as_socket(socket_));
    socket_ = -1;
  }
}

std::unique_ptr<TlsChannel> TlsChannel::connect(
    const std::string& host, std::uint16_t port,
    security::DeviceIdentity& identity, std::chrono::milliseconds timeout,
    std::string& error) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* results = nullptr;
  const auto service = std::to_string(port);
  if (getaddrinfo(host.c_str(), service.c_str(), &hints, &results) != 0 ||
      results == nullptr) {
    error = "unable to resolve the control address";
    return nullptr;
  }

  SocketHandle handle = kInvalidSocket;
  Endpoint peer;
  for (addrinfo* entry = results; entry != nullptr; entry = entry->ai_next) {
    handle = ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
    if (handle == kInvalidSocket) continue;
    apply_socket_timeout(handle, timeout);
    if (::connect(handle, entry->ai_addr,
                  static_cast<socklen_t>(entry->ai_addrlen)) == 0) {
      sockaddr_storage storage{};
      std::memcpy(&storage, entry->ai_addr,
                  std::min<std::size_t>(sizeof(storage), entry->ai_addrlen));
      endpoint_from_sockaddr(storage, peer);
      break;
    }
    close_socket(handle);
    handle = kInvalidSocket;
  }
  freeaddrinfo(results);
  if (handle == kInvalidSocket) {
    error = "unable to reach the control channel on " + host + ":" + service;
    return nullptr;
  }

  SSL_CTX* context = make_context(identity, false, error);
  if (context == nullptr) {
    close_socket(handle);
    return nullptr;
  }
  SSL* ssl = SSL_new(context);
  if (ssl == nullptr || SSL_set_fd(ssl, static_cast<int>(handle)) != 1) {
    if (ssl != nullptr) SSL_free(ssl);
    SSL_CTX_free(context);
    close_socket(handle);
    error = openssl_error("unable to start the TLS client session");
    return nullptr;
  }
  if (SSL_connect(ssl) != 1) {
    error = openssl_error("TLS handshake failed");
    SSL_free(ssl);
    SSL_CTX_free(context);
    close_socket(handle);
    return nullptr;
  }

  security::Fingerprint fingerprint{};
  if (!capture_peer_fingerprint(ssl, fingerprint, error)) {
    SSL_free(ssl);
    SSL_CTX_free(context);
    close_socket(handle);
    return nullptr;
  }
  return std::unique_ptr<TlsChannel>(new TlsChannel(
      context, ssl, static_cast<std::intptr_t>(handle), true, fingerprint,
      peer));
}

bool TlsChannel::write_frame(std::span<const std::byte> frame,
                             std::string& error) {
  if (ssl_ == nullptr || interrupted_.load(std::memory_order_acquire)) {
    error = "the control channel is closed";
    return false;
  }
  if (frame.empty() || frame.size() > protocol::kMaxControlFrameBytes) {
    error = "refusing to send an out-of-bounds control frame";
    return false;
  }
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  std::size_t written = 0;
  while (written < frame.size()) {
    if (interrupted_.load(std::memory_order_acquire)) {
      error = "the control channel was interrupted";
      return false;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      error = "control channel write timed out";
      return false;
    }
    const int count = SSL_write(static_cast<SSL*>(ssl_), frame.data() + written,
                                static_cast<int>(frame.size() - written));
    if (count > 0) {
      written += static_cast<std::size_t>(count);
      continue;
    }
    const int reason = SSL_get_error(static_cast<SSL*>(ssl_), count);
    if (reason == SSL_ERROR_WANT_READ || reason == SSL_ERROR_WANT_WRITE) {
      continue;
    }
    error = openssl_error("control channel write failed");
    return false;
  }
  return true;
}

bool TlsChannel::read_frame(std::vector<std::byte>& frame,
                            std::chrono::milliseconds timeout, bool& timed_out,
                            std::string& error) {
  timed_out = false;
  if (ssl_ == nullptr || interrupted_.load(std::memory_order_acquire)) {
    error = "the control channel is closed";
    return false;
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;

  for (;;) {
    if (interrupted_.load(std::memory_order_acquire)) {
      error = "the control channel was interrupted";
      return false;
    }
    protocol::CodecError codec_error = protocol::CodecError::none;
    const auto complete = protocol::peek_frame_size(buffer_, codec_error);
    if (codec_error != protocol::CodecError::none) {
      error = protocol::describe(codec_error);
      return false;
    }
    if (complete != 0) {
      frame.assign(buffer_.begin(),
                   buffer_.begin() + static_cast<std::ptrdiff_t>(complete));
      buffer_.erase(buffer_.begin(),
                    buffer_.begin() + static_cast<std::ptrdiff_t>(complete));
      return true;
    }
    if (buffer_.size() > protocol::kMaxControlFrameBytes) {
      error = "control peer exceeded the maximum frame size";
      return false;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      timed_out = true;
      return false;
    }

    std::array<std::byte, 1'024> chunk{};
    const int count = SSL_read(static_cast<SSL*>(ssl_), chunk.data(),
                               static_cast<int>(chunk.size()));
    if (count > 0) {
      buffer_.insert(buffer_.end(), chunk.begin(),
                     chunk.begin() + static_cast<std::ptrdiff_t>(count));
      continue;
    }
    const int reason = SSL_get_error(static_cast<SSL*>(ssl_), count);
    if (reason == SSL_ERROR_WANT_READ || reason == SSL_ERROR_WANT_WRITE) {
      continue;
    }
    if (reason == SSL_ERROR_ZERO_RETURN) {
      error = "the peer closed the control channel";
      return false;
    }
    if (reason == SSL_ERROR_SYSCALL && count < 0) {
      if (interrupted_.load(std::memory_order_acquire)) {
        error = "the control channel was interrupted";
        return false;
      }
      // A socket receive timeout surfaces here; retry until the deadline.
      if (std::chrono::steady_clock::now() < deadline) continue;
      timed_out = true;
      return false;
    }
    error = openssl_error("control channel read failed");
    return false;
  }
}

bool TlsChannel::export_keying_material(std::string_view label,
                                        std::span<std::uint8_t> out,
                                        std::string& error) const {
  if (ssl_ == nullptr) {
    error = "the control channel is closed";
    return false;
  }
  if (SSL_export_keying_material(static_cast<SSL*>(ssl_), out.data(),
                                 out.size(), label.data(), label.size(),
                                 nullptr, 0, 0) != 1) {
    error = openssl_error("TLS exporter failed");
    return false;
  }
  return true;
}

std::string TlsChannel::negotiated_version() const {
  if (ssl_ == nullptr) return "closed";
  const char* version = SSL_get_version(static_cast<SSL*>(ssl_));
  return version != nullptr ? version : "unknown";
}

TlsListener::TlsListener(std::intptr_t socket, std::uint16_t port)
    : socket_(socket), port_(port) {}

TlsListener::~TlsListener() { close(); }

void TlsListener::close() noexcept {
  const auto handle = socket_.exchange(-1, std::memory_order_acq_rel);
  if (handle != -1) {
#ifdef _WIN32
    ::shutdown(as_socket(handle), SD_BOTH);
#else
    ::shutdown(as_socket(handle), SHUT_RDWR);
#endif
    close_socket(as_socket(handle));
  }
}

std::unique_ptr<TlsListener> TlsListener::bind(std::uint16_t port,
                                               std::string& error) {
  SocketHandle handle = ::socket(AF_INET6, SOCK_STREAM, 0);
  if (handle == kInvalidSocket) {
    error = "unable to create the control listener socket";
    return nullptr;
  }
  int disabled = 0;
  setsockopt(handle, IPPROTO_IPV6, IPV6_V6ONLY,
             reinterpret_cast<const char*>(&disabled), sizeof(disabled));
  int enabled = 1;
  setsockopt(handle, SOL_SOCKET, SO_REUSEADDR,
             reinterpret_cast<const char*>(&enabled), sizeof(enabled));

  sockaddr_in6 address{};
  address.sin6_family = AF_INET6;
  address.sin6_addr = in6addr_any;
  address.sin6_port = htons(port);
  if (::bind(handle, reinterpret_cast<sockaddr*>(&address),
             sizeof(address)) != 0) {
    close_socket(handle);
    error = "unable to bind the control port " + std::to_string(port);
    return nullptr;
  }
  if (::listen(handle, 4) != 0) {
    close_socket(handle);
    error = "unable to listen on the control port";
    return nullptr;
  }

  sockaddr_in6 bound{};
  auto length = static_cast<socklen_t>(sizeof(bound));
  std::uint16_t actual = port;
  if (getsockname(handle, reinterpret_cast<sockaddr*>(&bound), &length) == 0) {
    actual = ntohs(bound.sin6_port);
  }
  return std::unique_ptr<TlsListener>(
      new TlsListener(static_cast<std::intptr_t>(handle), actual));
}

std::unique_ptr<TlsChannel> TlsListener::accept(
    security::DeviceIdentity& identity, std::chrono::milliseconds timeout,
    std::string& error) {
  const auto listen_handle = socket_.load(std::memory_order_acquire);
  if (listen_handle == -1) {
    error = "the control listener is closed";
    return nullptr;
  }
#ifdef _WIN32
  WSAPOLLFD descriptor{as_socket(listen_handle), POLLRDNORM, 0};
  const int ready = WSAPoll(&descriptor, 1, static_cast<int>(timeout.count()));
#else
  pollfd descriptor{as_socket(listen_handle), POLLIN, 0};
  const int ready = poll(&descriptor, 1, static_cast<int>(timeout.count()));
#endif
  if (ready == 0) return nullptr;
  if (ready < 0) {
    if (socket_.load(std::memory_order_acquire) == -1) return nullptr;
    error = "control listener poll failed";
    return nullptr;
  }

  sockaddr_storage storage{};
  auto length = static_cast<socklen_t>(sizeof(storage));
  SocketHandle handle = ::accept(
      as_socket(listen_handle), reinterpret_cast<sockaddr*>(&storage), &length);
  if (handle == kInvalidSocket) {
    if (socket_.load(std::memory_order_acquire) == -1) return nullptr;
    error = "unable to accept the control connection";
    return nullptr;
  }
  apply_socket_timeout(handle, kAcceptedSocketTimeout);
  Endpoint peer;
  endpoint_from_sockaddr(storage, peer);

  SSL_CTX* context = make_context(identity, true, error);
  if (context == nullptr) {
    close_socket(handle);
    return nullptr;
  }
  SSL* ssl = SSL_new(context);
  if (ssl == nullptr || SSL_set_fd(ssl, static_cast<int>(handle)) != 1) {
    if (ssl != nullptr) SSL_free(ssl);
    SSL_CTX_free(context);
    close_socket(handle);
    error = openssl_error("unable to start the TLS server session");
    return nullptr;
  }
  if (SSL_accept(ssl) != 1) {
    error = openssl_error("TLS handshake failed");
    SSL_free(ssl);
    SSL_CTX_free(context);
    close_socket(handle);
    return nullptr;
  }

  security::Fingerprint fingerprint{};
  if (!capture_peer_fingerprint(ssl, fingerprint, error)) {
    SSL_free(ssl);
    SSL_CTX_free(context);
    close_socket(handle);
    return nullptr;
  }
  return std::unique_ptr<TlsChannel>(new TlsChannel(
      context, ssl, static_cast<std::intptr_t>(handle), false, fingerprint,
      peer));
}

}  // namespace lanaudio::control
