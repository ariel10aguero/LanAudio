#include "lanaudio/core.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace lanaudio {
namespace {

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
void close_socket(SocketHandle socket) { closesocket(socket); }
std::string socket_error() { return "Winsock error " + std::to_string(WSAGetLastError()); }
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
void close_socket(SocketHandle socket) { close(socket); }
std::string socket_error() { return std::strerror(errno); }
#endif

SocketHandle as_socket(std::intptr_t handle) {
  return static_cast<SocketHandle>(handle);
}

void to_endpoint(const sockaddr_in6& address, Endpoint& endpoint) noexcept {
  std::memcpy(endpoint.address.data(), &address.sin6_addr, 16);
  endpoint.port = ntohs(address.sin6_port);
}

bool to_mapped_v6(const addrinfo& info, sockaddr_in6& address) noexcept {
  if (info.ai_family == AF_INET6) {
    std::memcpy(&address, info.ai_addr, sizeof(sockaddr_in6));
    return true;
  }
  if (info.ai_family != AF_INET) return false;
  const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(info.ai_addr);
  address = sockaddr_in6{};
  address.sin6_family = AF_INET6;
  address.sin6_port = ipv4->sin_port;
  address.sin6_addr.s6_addr[10] = 0xff;
  address.sin6_addr.s6_addr[11] = 0xff;
  std::memcpy(&address.sin6_addr.s6_addr[12], &ipv4->sin_addr, 4);
  return true;
}

}  // namespace

bool Endpoint::valid() const noexcept {
  return port != 0 ||
         std::any_of(address.begin(), address.end(),
                     [](std::uint8_t octet) { return octet != 0; });
}

std::string Endpoint::to_string() const {
  in6_addr raw{};
  std::memcpy(&raw, address.data(), 16);
  char text[INET6_ADDRSTRLEN] = {};
  const bool mapped =
      IN6_IS_ADDR_V4MAPPED(&raw) != 0;  // present on all supported platforms
  if (mapped) {
    in_addr ipv4{};
    std::memcpy(&ipv4, address.data() + 12, 4);
    if (inet_ntop(AF_INET, &ipv4, text, sizeof(text)) == nullptr) return {};
  } else if (inet_ntop(AF_INET6, &raw, text, sizeof(text)) == nullptr) {
    return {};
  }
  return std::string(text) + ":" + std::to_string(port);
}

bool resolve_endpoint(std::string_view host, std::uint16_t port,
                      Endpoint& endpoint, std::string& error) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* results = nullptr;
  const auto service = std::to_string(port);
  const std::string host_string(host);
  if (getaddrinfo(host_string.c_str(), service.c_str(), &hints, &results) != 0 ||
      results == nullptr) {
    error = "address resolution failed";
    return false;
  }
  sockaddr_in6 address{};
  const bool converted = to_mapped_v6(*results, address);
  freeaddrinfo(results);
  if (!converted) {
    error = "unsupported address family";
    return false;
  }
  to_endpoint(address, endpoint);
  return true;
}

UdpSocket::UdpSocket() {
#ifdef _WIN32
  WSADATA data{};
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return;
#endif
  handle_ = static_cast<std::intptr_t>(::socket(AF_INET6, SOCK_DGRAM, 0));
  if (as_socket(handle_) == kInvalidSocket) return;
  int disabled = 0;
  if (setsockopt(as_socket(handle_), IPPROTO_IPV6, IPV6_V6ONLY,
                 reinterpret_cast<const char*>(&disabled),
                 sizeof(disabled)) != 0) {
    close_socket(as_socket(handle_));
    handle_ = -1;
    return;
  }
  // Roomier kernel buffers absorb Wi-Fi stalls on both ends. Failure is
  // harmless: the defaults still work, just with less headroom.
  int buffer_bytes = 1 << 20;
  (void)setsockopt(as_socket(handle_), SOL_SOCKET, SO_SNDBUF,
                   reinterpret_cast<const char*>(&buffer_bytes),
                   sizeof(buffer_bytes));
  (void)setsockopt(as_socket(handle_), SOL_SOCKET, SO_RCVBUF,
                   reinterpret_cast<const char*>(&buffer_bytes),
                   sizeof(buffer_bytes));
}

UdpSocket::~UdpSocket() {
  if (handle_ != -1) close_socket(as_socket(handle_));
#ifdef _WIN32
  WSACleanup();
#endif
}

bool UdpSocket::bind(std::uint16_t port, std::string& error) {
  if (handle_ == -1) {
    error = "unable to create UDP socket";
    return false;
  }
  sockaddr_in6 address{};
  address.sin6_family = AF_INET6;
  address.sin6_addr = in6addr_any;
  address.sin6_port = htons(port);
  if (::bind(as_socket(handle_), reinterpret_cast<sockaddr*>(&address),
             sizeof(address)) != 0) {
    error = "UDP bind failed: " + socket_error();
    return false;
  }
  return true;
}

bool UdpSocket::set_destination(std::string_view host, std::uint16_t port,
                                std::string& error) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* results = nullptr;
  const auto service = std::to_string(port);
  const std::string host_string(host);
  const int result =
      getaddrinfo(host_string.c_str(), service.c_str(), &hints, &results);
  if (result != 0 || results == nullptr) {
    error = "destination resolution failed";
    return false;
  }

  sockaddr_in6 address{};
  const bool converted = to_mapped_v6(*results, address);
  freeaddrinfo(results);
  if (!converted) {
    error = "unsupported destination address family";
    return false;
  }
  static_assert(sizeof(address) <= 128);
  std::memcpy(destination_.data(), &address, sizeof(address));
  destination_size_ = sizeof(address);
  return true;
}

Endpoint UdpSocket::destination() const noexcept {
  Endpoint endpoint;
  if (destination_size_ != sizeof(sockaddr_in6)) return endpoint;
  sockaddr_in6 address{};
  std::memcpy(&address, destination_.data(), sizeof(address));
  to_endpoint(address, endpoint);
  return endpoint;
}

std::uint16_t UdpSocket::local_port(std::string& error) const {
  if (handle_ == -1) {
    error = "UDP socket is not available";
    return 0;
  }
  sockaddr_in6 address{};
  auto length = static_cast<socklen_t>(sizeof(address));
  if (getsockname(as_socket(handle_), reinterpret_cast<sockaddr*>(&address),
                  &length) != 0) {
    error = "unable to read the local UDP port: " + socket_error();
    return 0;
  }
  return ntohs(address.sin6_port);
}

bool UdpSocket::send(std::span<const std::byte> bytes, std::string& error) {
  if (handle_ == -1 || destination_size_ == 0) {
    error = "UDP destination is not configured";
    return false;
  }
#ifdef _WIN32
  const auto count = ::sendto(
      as_socket(handle_), reinterpret_cast<const char*>(bytes.data()),
      static_cast<int>(bytes.size()), 0,
      reinterpret_cast<const sockaddr*>(destination_.data()),
      static_cast<int>(destination_size_));
#else
  // macOS returns ENOBUFS when the Wi-Fi interface queue is momentarily
  // full (for example while the radio is off-channel). Dropping the datagram
  // then shows up as packet loss at the receiver, so retry briefly. This runs
  // on the sender's network thread, never in the audio callback.
  constexpr auto kRetryBudget = std::chrono::milliseconds(4);
  const auto deadline = std::chrono::steady_clock::now() + kRetryBudget;
  ssize_t count = -1;
  for (;;) {
    count = ::sendto(as_socket(handle_), bytes.data(), bytes.size(), 0,
                     reinterpret_cast<const sockaddr*>(destination_.data()),
                     static_cast<socklen_t>(destination_size_));
    if (count >= 0) break;
    const int code = errno;
    if ((code != ENOBUFS && code != EAGAIN && code != EWOULDBLOCK &&
         code != EINTR) ||
        std::chrono::steady_clock::now() >= deadline) {
      errno = code;
      break;
    }
    ++send_retries_;
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
#endif
  if (count != static_cast<decltype(count)>(bytes.size())) {
    error = "UDP send failed: " + socket_error();
    return false;
  }
  return true;
}

std::ptrdiff_t UdpSocket::receive(std::span<std::byte> bytes,
                                  std::chrono::milliseconds timeout,
                                  std::string& error) {
  Endpoint ignored;
  return receive_from(bytes, timeout, ignored, error);
}

std::ptrdiff_t UdpSocket::receive_from(std::span<std::byte> bytes,
                                       std::chrono::milliseconds timeout,
                                       Endpoint& source, std::string& error) {
  if (handle_ == -1) {
    error = "UDP socket is not available";
    return -1;
  }
#ifdef _WIN32
  WSAPOLLFD descriptor{as_socket(handle_), POLLRDNORM, 0};
  const int ready = WSAPoll(&descriptor, 1, static_cast<int>(timeout.count()));
#else
  pollfd descriptor{as_socket(handle_), POLLIN, 0};
  const int ready = poll(&descriptor, 1, static_cast<int>(timeout.count()));
#endif
  if (ready == 0) return 0;
  if (ready < 0) {
    error = "UDP poll failed: " + socket_error();
    return -1;
  }
  sockaddr_in6 from{};
#ifdef _WIN32
  int from_length = static_cast<int>(sizeof(from));
  const auto count = ::recvfrom(
      as_socket(handle_), reinterpret_cast<char*>(bytes.data()),
      static_cast<int>(bytes.size()), 0, reinterpret_cast<sockaddr*>(&from),
      &from_length);
#else
  auto from_length = static_cast<socklen_t>(sizeof(from));
  const auto count =
      ::recvfrom(as_socket(handle_), bytes.data(), bytes.size(), 0,
                 reinterpret_cast<sockaddr*>(&from), &from_length);
#endif
  if (count < 0) {
    error = "UDP receive failed: " + socket_error();
    return -1;
  }
  to_endpoint(from, source);
  return static_cast<std::ptrdiff_t>(count);
}

}  // namespace lanaudio
