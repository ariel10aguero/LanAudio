#pragma once

#include "lanaudio/control/tls_channel.hpp"
#include "lanaudio/protocol/control_messages.hpp"
#include "lanaudio/security/pairing.hpp"
#include "lanaudio/security/trust_store.hpp"
#include "lanaudio/srtp/srtp_protector.hpp"

#include <array>
#include <functional>
#include <string>

namespace lanaudio::control {

struct LocalInfo {
  std::string device_name{"LanAudio device"};
  std::string application_version{"0.0.0"};
  security::Fingerprint fingerprint{};
};

struct PeerInfo {
  security::Fingerprint fingerprint{};
  std::string device_name;
  std::string application_version;
  std::uint16_t protocol_version{};
};

enum class HandshakeStatus {
  ok,
  version_mismatch,
  identity_mismatch,
  untrusted,
  rejected,
  protocol_error,
  timeout,
  closed,
};

[[nodiscard]] std::string describe(HandshakeStatus status);

// Exchanges hello frames and binds the advertised fingerprint to the
// certificate that was actually used in the TLS handshake. Discovery metadata
// is never trusted on its own; this is the step that proves identity.
[[nodiscard]] HandshakeStatus exchange_hello(TlsChannel& channel,
                                             const LocalInfo& local,
                                             PeerInfo& peer,
                                             std::string& error);

// Collects the sample rates and packet durations this device can carry.
[[nodiscard]] protocol::DeviceCapabilities local_capabilities(
    std::uint32_t preferred_sample_rate = protocol::kPreferredSampleRate);

// Shown on both devices; the user must confirm the digits match. Returning
// false aborts pairing.
using ConfirmCodeCallback =
    std::function<bool(const std::string& code, const PeerInfo& peer)>;

struct PairingOutcome {
  bool trusted{};
  std::string code;
  std::string detail;
};

// Runs the six-digit short authentication string exchange over an already
// established TLS channel and, on mutual confirmation, pins the peer.
[[nodiscard]] PairingOutcome run_pairing(TlsChannel& channel,
                                         const security::DeviceIdentity& local,
                                         const PeerInfo& peer,
                                         const ConfirmCodeCallback& confirm,
                                         security::TrustStore& trust,
                                         std::string& error);

// Key material and agreed media parameters for one session. The destructor
// wipes the exported secret.
struct NegotiatedSession {
  NegotiatedSession() = default;
  ~NegotiatedSession();
  NegotiatedSession(const NegotiatedSession&) = delete;
  NegotiatedSession& operator=(const NegotiatedSession&) = delete;

  protocol::SessionParameters parameters{};
  std::array<std::uint8_t, srtp::kKeyMaterialBytes> key_material{};
  PeerInfo peer{};
  Endpoint peer_endpoint{};
  bool local_is_client{};
};

// Sender side: offers capabilities and adopts whatever the receiver accepts.
[[nodiscard]] HandshakeStatus negotiate_as_sender(
    TlsChannel& channel, const protocol::DeviceCapabilities& capabilities,
    NegotiatedSession& session, std::string& error,
    std::uint16_t preferred_jitter_packets = 4,
    std::uint32_t preferred_packet_microseconds =
        protocol::kPreferredPacketMicroseconds);

// Receiver side: intersects capabilities, assigns a random SSRC and session
// identifier, and answers with the agreed parameters.
[[nodiscard]] HandshakeStatus negotiate_as_receiver(
    TlsChannel& channel, const protocol::DeviceCapabilities& capabilities,
    std::uint16_t media_port, NegotiatedSession& session, std::string& error);

}  // namespace lanaudio::control
