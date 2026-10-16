#include "lanaudio/control/control_session.hpp"

#include <algorithm>
#include <chrono>

namespace lanaudio::control {
namespace {

constexpr std::chrono::milliseconds kHandshakeTimeout{5'000};
constexpr std::chrono::milliseconds kConfirmTimeout{120'000};

bool send(TlsChannel& channel, const auto& message, std::string& error) {
  std::vector<std::byte> frame;
  if (!protocol::encode(message, frame)) {
    error = "unable to encode a control frame";
    return false;
  }
  return channel.write_frame(frame, error);
}

HandshakeStatus receive(TlsChannel& channel, std::vector<std::byte>& frame,
                        std::chrono::milliseconds timeout,
                        protocol::ControlMessageType& type,
                        std::string& error) {
  bool timed_out = false;
  if (!channel.read_frame(frame, timeout, timed_out, error)) {
    return timed_out ? HandshakeStatus::timeout : HandshakeStatus::closed;
  }
  const auto codec = protocol::frame_type(frame, type);
  if (codec != protocol::CodecError::none) {
    error = protocol::describe(codec);
    return HandshakeStatus::protocol_error;
  }
  return HandshakeStatus::ok;
}

// A reject frame carries the peer's reason; surface it instead of a generic
// protocol error so the user sees something actionable.
HandshakeStatus adopt_reject(std::span<const std::byte> frame,
                             std::string& error) {
  protocol::SessionRejectMessage reject;
  if (protocol::decode(frame, reject) == protocol::CodecError::none) {
    error = reject.detail.empty() ? "the peer rejected the session"
                                  : reject.detail;
    if (reject.reason == protocol::RejectReason::untrusted_peer) {
      return HandshakeStatus::untrusted;
    }
    if (reject.reason == protocol::RejectReason::unsupported_version) {
      return HandshakeStatus::version_mismatch;
    }
    return HandshakeStatus::rejected;
  }
  error = "the peer rejected the session";
  return HandshakeStatus::rejected;
}

bool reject(TlsChannel& channel, protocol::RejectReason reason,
            std::string detail) {
  protocol::SessionRejectMessage message;
  message.reason = reason;
  message.detail = std::move(detail);
  std::string ignored;
  return send(channel, message, ignored);
}

}  // namespace

std::string describe(HandshakeStatus status) {
  switch (status) {
    case HandshakeStatus::ok:
      return "ok";
    case HandshakeStatus::version_mismatch:
      return "the devices run incompatible protocol versions";
    case HandshakeStatus::identity_mismatch:
      return "the peer identity does not match its certificate";
    case HandshakeStatus::untrusted:
      return "the peer is not paired with this device";
    case HandshakeStatus::rejected:
      return "the peer rejected the session";
    case HandshakeStatus::protocol_error:
      return "the peer sent a malformed control frame";
    case HandshakeStatus::timeout:
      return "the control handshake timed out";
    case HandshakeStatus::closed:
      return "the control channel closed";
  }
  return "unknown control status";
}

HandshakeStatus exchange_hello(TlsChannel& channel, const LocalInfo& local,
                               PeerInfo& peer, std::string& error) {
  protocol::HelloMessage outgoing;
  outgoing.protocol_version = protocol::kControlProtocolVersion;
  outgoing.device_name = local.device_name;
  outgoing.application_version = local.application_version;
  // Nothing secret travels here: the fingerprint is a public key digest.
  outgoing.fingerprint.assign(local.fingerprint.begin(),
                              local.fingerprint.end());
  std::array<std::uint8_t, 32> nonce{};
  if (!security::random_bytes(nonce)) {
    error = "unable to generate a handshake nonce";
    return HandshakeStatus::protocol_error;
  }
  outgoing.nonce.assign(nonce.begin(), nonce.end());

  if (!send(channel, outgoing, error)) return HandshakeStatus::closed;

  std::vector<std::byte> frame;
  protocol::ControlMessageType type{};
  const auto status =
      receive(channel, frame, kHandshakeTimeout, type, error);
  if (status != HandshakeStatus::ok) return status;
  if (type == protocol::ControlMessageType::session_reject) {
    return adopt_reject(frame, error);
  }
  if (type != protocol::ControlMessageType::hello) {
    error = "expected a hello frame";
    return HandshakeStatus::protocol_error;
  }

  protocol::HelloMessage incoming;
  const auto codec = protocol::decode(frame, incoming);
  if (codec != protocol::CodecError::none) {
    error = protocol::describe(codec);
    return HandshakeStatus::protocol_error;
  }
  if (incoming.protocol_version < protocol::kMinimumControlProtocolVersion ||
      incoming.protocol_version > protocol::kControlProtocolVersion) {
    error = "peer speaks control protocol version " +
            std::to_string(incoming.protocol_version) + "; this build needs " +
            std::to_string(protocol::kControlProtocolVersion);
    (void)reject(channel, protocol::RejectReason::unsupported_version, error);
    return HandshakeStatus::version_mismatch;
  }

  // The certificate used in the handshake is authoritative. A hello that
  // claims a different identity is an impersonation attempt.
  const auto certificate_fingerprint = channel.peer_fingerprint();
  if (incoming.fingerprint.size() != certificate_fingerprint.size() ||
      !security::constant_time_equal(incoming.fingerprint,
                                     certificate_fingerprint)) {
    error = "the peer advertised an identity that differs from its TLS "
            "certificate";
    (void)reject(channel, protocol::RejectReason::policy, error);
    return HandshakeStatus::identity_mismatch;
  }

  peer.fingerprint = certificate_fingerprint;
  peer.device_name = incoming.device_name;
  peer.application_version = incoming.application_version;
  peer.protocol_version = incoming.protocol_version;
  return HandshakeStatus::ok;
}

protocol::DeviceCapabilities local_capabilities(
    std::uint32_t preferred_sample_rate) {
  protocol::DeviceCapabilities capabilities;
  const auto rates = protocol::supported_sample_rates();
  capabilities.sample_rates.assign(rates.begin(), rates.end());
  const auto durations = protocol::supported_packet_durations();
  capabilities.packet_durations.assign(durations.begin(), durations.end());
  capabilities.channels = protocol::kRequiredChannels;
  capabilities.preferred_sample_rate =
      protocol::is_supported_sample_rate(preferred_sample_rate)
          ? preferred_sample_rate
          : protocol::kPreferredSampleRate;
  return capabilities;
}

PairingOutcome run_pairing(TlsChannel& channel,
                           const security::DeviceIdentity& local,
                           const PeerInfo& peer,
                           const ConfirmCodeCallback& confirm,
                           security::TrustStore& trust, std::string& error) {
  PairingOutcome outcome;

  std::array<std::uint8_t, security::kPairingExporterBytes> exporter{};
  if (!channel.export_keying_material(security::kPairingExporterLabel,
                                      exporter, error)) {
    return outcome;
  }
  const auto transcript = security::pairing_transcript(
      exporter, local.fingerprint(), peer.fingerprint);
  security::secure_clear(exporter);
  outcome.code = security::short_authentication_string(transcript);

  // The digits only let a human compare two screens. Trust comes from the
  // transcript digest, which both sides verify byte for byte.
  const bool local_accepted = confirm && confirm(outcome.code, peer);

  protocol::PairConfirmMessage outgoing;
  outgoing.accepted = local_accepted;
  outgoing.transcript_digest.assign(transcript.begin(), transcript.end());
  if (!send(channel, outgoing, error)) return outcome;

  std::vector<std::byte> frame;
  protocol::ControlMessageType type{};
  const auto wait =
      local_accepted ? kConfirmTimeout : kHandshakeTimeout;
  const auto status = receive(channel, frame, wait, type, error);
  if (status != HandshakeStatus::ok) {
    if (error.empty()) error = describe(status);
    return outcome;
  }
  if (type != protocol::ControlMessageType::pair_confirm) {
    error = "expected a pairing confirmation frame";
    return outcome;
  }

  protocol::PairConfirmMessage incoming;
  const auto codec = protocol::decode(frame, incoming);
  if (codec != protocol::CodecError::none) {
    error = protocol::describe(codec);
    return outcome;
  }
  if (!security::constant_time_equal(incoming.transcript_digest, transcript)) {
    error = "the pairing transcripts do not match; pairing was aborted";
    return outcome;
  }
  if (!local_accepted) {
    error = "pairing was declined on this device";
    return outcome;
  }
  if (!incoming.accepted) {
    error = "pairing was declined on the other device";
    return outcome;
  }

  if (!trust.add(peer.fingerprint, peer.device_name, error)) return outcome;

  // Both sides acknowledge, and both sides consume the acknowledgement, so
  // the channel is left clean for session negotiation.
  protocol::PairResultMessage result;
  result.trusted = true;
  result.detail = "paired";
  if (!send(channel, result, error)) return outcome;

  const auto ack = receive(channel, frame, kConfirmTimeout, type, error);
  if (ack != HandshakeStatus::ok) {
    if (error.empty()) error = describe(ack);
    return outcome;
  }
  if (type != protocol::ControlMessageType::pair_result) {
    error = "expected a pairing result frame";
    return outcome;
  }
  protocol::PairResultMessage peer_result;
  if (protocol::decode(frame, peer_result) != protocol::CodecError::none ||
      !peer_result.trusted) {
    error = "the other device did not store the pairing";
    return outcome;
  }

  outcome.trusted = true;
  outcome.detail = "paired with " + peer.device_name;
  return outcome;
}

NegotiatedSession::~NegotiatedSession() { security::secure_clear(key_material); }

namespace {

bool derive_media_keys(TlsChannel& channel, NegotiatedSession& session,
                       std::string& error) {
  if (!channel.export_keying_material(security::kSrtpExporterLabel,
                                      session.key_material, error)) {
    return false;
  }
  session.local_is_client = channel.is_client();
  session.peer_endpoint = channel.peer_endpoint();
  return true;
}

}  // namespace

HandshakeStatus negotiate_as_sender(
    TlsChannel& channel, const protocol::DeviceCapabilities& capabilities,
    NegotiatedSession& session, std::string& error,
    std::uint16_t preferred_jitter_packets,
    std::uint32_t preferred_packet_microseconds) {
  if (!capabilities.valid()) {
    error = "the local audio device reported unusable capabilities";
    return HandshakeStatus::protocol_error;
  }
  protocol::CapabilityOfferMessage offer;
  offer.capabilities = capabilities;
  offer.preferred_jitter_packets =
      std::clamp<std::uint16_t>(preferred_jitter_packets, 2, 32);
  offer.preferred_packet_microseconds =
      protocol::is_supported_packet_duration(preferred_packet_microseconds)
          ? preferred_packet_microseconds
          : protocol::kPreferredPacketMicroseconds;
  if (!send(channel, offer, error)) return HandshakeStatus::closed;

  std::vector<std::byte> frame;
  protocol::ControlMessageType type{};
  const auto status = receive(channel, frame, kHandshakeTimeout, type, error);
  if (status != HandshakeStatus::ok) return status;
  if (type == protocol::ControlMessageType::session_reject) {
    return adopt_reject(frame, error);
  }
  if (type != protocol::ControlMessageType::session_accept) {
    error = "expected a session acceptance frame";
    return HandshakeStatus::protocol_error;
  }

  protocol::SessionAcceptMessage accept;
  const auto codec = protocol::decode(frame, accept);
  if (codec != protocol::CodecError::none) {
    error = protocol::describe(codec);
    return HandshakeStatus::protocol_error;
  }
  // The decoder already enforced SessionParameters::valid(); confirm the
  // accepted rate is one this device actually offered.
  if (std::find(capabilities.sample_rates.begin(),
                capabilities.sample_rates.end(),
                accept.parameters.sample_rate) ==
      capabilities.sample_rates.end()) {
    error = "the receiver accepted a sample rate this device cannot produce";
    return HandshakeStatus::protocol_error;
  }
  session.parameters = accept.parameters;
  if (!derive_media_keys(channel, session, error)) {
    return HandshakeStatus::protocol_error;
  }
  return HandshakeStatus::ok;
}

HandshakeStatus negotiate_as_receiver(
    TlsChannel& channel, const protocol::DeviceCapabilities& capabilities,
    std::uint16_t media_port, NegotiatedSession& session, std::string& error) {
  if (!capabilities.valid()) {
    error = "the local audio device reported unusable capabilities";
    return HandshakeStatus::protocol_error;
  }

  std::vector<std::byte> frame;
  protocol::ControlMessageType type{};
  const auto status = receive(channel, frame, kHandshakeTimeout, type, error);
  if (status != HandshakeStatus::ok) return status;
  if (type != protocol::ControlMessageType::capability_offer) {
    error = "expected a capability offer";
    (void)reject(channel, protocol::RejectReason::policy, error);
    return HandshakeStatus::protocol_error;
  }

  protocol::CapabilityOfferMessage offer;
  const auto codec = protocol::decode(frame, offer);
  if (codec != protocol::CodecError::none) {
    error = protocol::describe(codec);
    (void)reject(channel, protocol::RejectReason::policy, error);
    return HandshakeStatus::protocol_error;
  }

  protocol::SessionParameters parameters;
  const auto negotiation =
      protocol::negotiate_session(capabilities, offer.capabilities, parameters,
                                  offer.preferred_packet_microseconds);
  if (negotiation != protocol::NegotiationError::none) {
    error = protocol::describe(negotiation);
    (void)reject(channel, protocol::RejectReason::negotiation_failed, error);
    return HandshakeStatus::rejected;
  }

  parameters.media_port = media_port;
  parameters.ssrc = security::random_ssrc();
  parameters.session_id = security::random_session_id();
  parameters.jitter_packets =
      std::clamp<std::uint16_t>(offer.preferred_jitter_packets, 2, 32);
  if (!parameters.valid()) {
    error = "the negotiated parameters failed local validation";
    (void)reject(channel, protocol::RejectReason::negotiation_failed, error);
    return HandshakeStatus::protocol_error;
  }

  protocol::SessionAcceptMessage accept;
  accept.parameters = parameters;
  if (!send(channel, accept, error)) return HandshakeStatus::closed;

  session.parameters = parameters;
  if (!derive_media_keys(channel, session, error)) {
    return HandshakeStatus::protocol_error;
  }
  return HandshakeStatus::ok;
}

}  // namespace lanaudio::control
