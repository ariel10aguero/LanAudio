#pragma once

#include "lanaudio/core.hpp"
#include "lanaudio/security/identity.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lanaudio::control {

// Mutually authenticated TLS 1.3 channel carrying length-prefixed control
// frames. Certificates are self-signed device identities, so trust comes from
// pinned fingerprints rather than from a certificate authority.
class TlsChannel {
 public:
  ~TlsChannel();
  TlsChannel(const TlsChannel&) = delete;
  TlsChannel& operator=(const TlsChannel&) = delete;

  [[nodiscard]] static std::unique_ptr<TlsChannel> connect(
      const std::string& host, std::uint16_t port,
      security::DeviceIdentity& identity, std::chrono::milliseconds timeout,
      std::string& error);

  // Reads one complete control frame. Returns false on timeout, protocol
  // violation, or closure; `timed_out` distinguishes an idle read.
  [[nodiscard]] bool read_frame(std::vector<std::byte>& frame,
                                std::chrono::milliseconds timeout,
                                bool& timed_out, std::string& error);
  [[nodiscard]] bool write_frame(std::span<const std::byte> frame,
                                 std::string& error);

  // RFC 8446 exporter. Used for the pairing transcript and SRTP keys; the
  // application never derives key material by any other means.
  [[nodiscard]] bool export_keying_material(std::string_view label,
                                            std::span<std::uint8_t> out,
                                            std::string& error) const;

  [[nodiscard]] security::Fingerprint peer_fingerprint() const noexcept {
    return peer_fingerprint_;
  }
  [[nodiscard]] bool is_client() const noexcept { return is_client_; }
  [[nodiscard]] Endpoint peer_endpoint() const noexcept { return peer_; }
  [[nodiscard]] std::string negotiated_version() const;
  // Unblocks a read/write on another thread so Disconnect cannot hang.
  void interrupt() noexcept;
  void close() noexcept;

 private:
  friend class TlsListener;
  TlsChannel(void* context, void* ssl, std::intptr_t socket, bool is_client,
             security::Fingerprint peer_fingerprint, Endpoint peer);

  void* context_{};
  void* ssl_{};
  std::intptr_t socket_{-1};
  bool is_client_{};
  security::Fingerprint peer_fingerprint_{};
  Endpoint peer_{};
  std::vector<std::byte> buffer_;
  std::atomic<bool> interrupted_{false};
  std::mutex io_mutex_;
};

// Accepts inbound control connections. The listener may run before pairing,
// but media never starts until the peer is trusted and a session is agreed.
class TlsListener {
 public:
  ~TlsListener();
  TlsListener(const TlsListener&) = delete;
  TlsListener& operator=(const TlsListener&) = delete;

  [[nodiscard]] static std::unique_ptr<TlsListener> bind(
      std::uint16_t port, std::string& error);

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

  // Returns nullptr with an empty error when no connection arrived in time.
  [[nodiscard]] std::unique_ptr<TlsChannel> accept(
      security::DeviceIdentity& identity, std::chrono::milliseconds timeout,
      std::string& error);
  void close() noexcept;

 private:
  explicit TlsListener(std::intptr_t socket, std::uint16_t port);

  std::atomic<std::intptr_t> socket_{-1};
  std::uint16_t port_{};
};

}  // namespace lanaudio::control
