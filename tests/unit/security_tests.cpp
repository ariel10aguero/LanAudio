#include "lanaudio/control/control_session.hpp"
#include "lanaudio/control/secure_session.hpp"
#include "lanaudio/protocol/control_messages.hpp"
#include "lanaudio/protocol/guard.hpp"
#include "lanaudio/protocol/negotiation.hpp"
#include "lanaudio/security/credential_store.hpp"
#include "lanaudio/security/identity.hpp"
#include "lanaudio/security/pairing.hpp"
#include "lanaudio/security/trust_store.hpp"
#include "lanaudio/srtp/srtp_protector.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

#define CHECK(expression)                                                     \
  do {                                                                        \
    if (!(expression)) {                                                      \
      std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: "          \
                << #expression << '\n';                                       \
      ++failures;                                                             \
    }                                                                         \
  } while (false)

namespace security = lanaudio::security;
namespace protocol = lanaudio::protocol;
namespace control = lanaudio::control;
namespace srtp = lanaudio::srtp;

// ---------------------------------------------------------------------------
// Negotiation
// ---------------------------------------------------------------------------

protocol::DeviceCapabilities capabilities(
    std::vector<std::uint32_t> rates, std::uint32_t preferred,
    std::vector<std::uint32_t> durations = {2'500, 5'000, 10'000, 20'000}) {
  protocol::DeviceCapabilities value;
  value.sample_rates = std::move(rates);
  value.packet_durations = std::move(durations);
  value.preferred_sample_rate = preferred;
  value.channels = protocol::kRequiredChannels;
  return value;
}

void test_negotiation_prefers_requested_packet_duration() {
  protocol::SessionParameters result;
  const auto both = capabilities({48'000}, 48'000);
  CHECK(protocol::negotiate_session(both, both, result, 2'500) ==
        protocol::NegotiationError::none);
  CHECK(result.packet_microseconds == 2'500);
}

void test_negotiation_defaults_to_five_milliseconds() {
  protocol::SessionParameters result;
  const auto both = capabilities({48'000}, 48'000);
  CHECK(protocol::negotiate_session(both, both, result) ==
        protocol::NegotiationError::none);
  CHECK(result.packet_microseconds == 5'000);
}

void test_negotiation_falls_back_when_preference_exceeds_mtu() {
  protocol::SessionParameters result;
  const auto both = capabilities(
      {192'000}, 192'000,
      {protocol::supported_packet_durations().begin(),
       protocol::supported_packet_durations().end()});
  CHECK(protocol::negotiate_session(both, both, result, 2'500) ==
        protocol::NegotiationError::none);
  CHECK(result.packet_microseconds == 1'250);
  CHECK(result.payload_bytes() + protocol::kSrtpOverheadBytes <=
        protocol::kMaxMediaPayloadBytes);
}

void test_negotiation_prefers_receiver_rate() {
  protocol::SessionParameters result;
  const auto receiver = capabilities({44'100, 48'000, 96'000}, 96'000);
  const auto sender = capabilities({44'100, 48'000, 96'000}, 48'000);
  CHECK(protocol::negotiate_session(receiver, sender, result) ==
        protocol::NegotiationError::none);
  CHECK(result.sample_rate == 96'000);
  CHECK(result.channels == 2);
  CHECK(result.frames_per_packet > 0);
  CHECK(result.payload_bytes() + protocol::kSrtpOverheadBytes <=
        protocol::kMaxMediaPayloadBytes);
}

void test_negotiation_falls_back_to_closest_common_rate() {
  protocol::SessionParameters result;
  const auto receiver = capabilities({44'100, 88'200}, 88'200);
  const auto sender = capabilities({44'100, 48'000}, 48'000);
  CHECK(protocol::negotiate_session(receiver, sender, result) ==
        protocol::NegotiationError::none);
  CHECK(result.sample_rate == 44'100);
}

// The agreed policy is to fail loudly rather than resample behind the user's
// back when no rate is shared.
void test_negotiation_fails_without_common_rate() {
  protocol::SessionParameters result;
  const auto receiver = capabilities({44'100}, 44'100);
  const auto sender = capabilities({48'000}, 48'000);
  CHECK(protocol::negotiate_session(receiver, sender, result) ==
        protocol::NegotiationError::no_common_sample_rate);
}

void test_negotiation_keeps_packets_under_the_mtu() {
  // Every supported rate must produce a packet that fits, which means high
  // rates automatically drop to shorter packets.
  for (const auto rate : protocol::supported_sample_rates()) {
    protocol::SessionParameters result;
    const auto both = capabilities({rate}, rate,
                                   {protocol::supported_packet_durations()
                                        .begin(),
                                    protocol::supported_packet_durations()
                                        .end()});
    CHECK(protocol::negotiate_session(both, both, result) ==
          protocol::NegotiationError::none);
    CHECK(result.sample_rate == rate);
    CHECK(result.payload_bytes() + protocol::kSrtpOverheadBytes <=
          protocol::kMaxMediaPayloadBytes);
    result.ssrc = 1;
    result.session_id = 1;
    result.media_port = 5'004;
    CHECK(result.valid());
  }

  // When only oversized durations are on offer, negotiation fails instead of
  // silently fragmenting or truncating audio.
  protocol::SessionParameters overflow;
  const auto too_long = capabilities({192'000}, 192'000, {10'000, 20'000});
  CHECK(protocol::negotiate_session(too_long, too_long, overflow) ==
        protocol::NegotiationError::payload_exceeds_limit);
}

void test_negotiation_rejects_unknown_rates() {
  protocol::SessionParameters result;
  const auto bogus = capabilities({7, 13, 999'999}, 7);
  CHECK(protocol::negotiate_session(bogus, bogus, result) !=
        protocol::NegotiationError::none);
}

// ---------------------------------------------------------------------------
// Control framing
// ---------------------------------------------------------------------------

void test_control_codec_round_trip() {
  protocol::HelloMessage hello;
  hello.device_name = "Studio Mac";
  hello.application_version = "0.2.0";
  hello.fingerprint.assign(protocol::kFingerprintBytes, 0xAB);
  hello.nonce.assign(protocol::kNonceBytes, 0x11);
  std::vector<std::byte> frame;
  CHECK(protocol::encode(hello, frame));

  protocol::CodecError error = protocol::CodecError::none;
  CHECK(protocol::peek_frame_size(frame, error) == frame.size());
  CHECK(error == protocol::CodecError::none);

  protocol::HelloMessage decoded;
  CHECK(protocol::decode(frame, decoded) == protocol::CodecError::none);
  CHECK(decoded.device_name == hello.device_name);
  CHECK(decoded.fingerprint == hello.fingerprint);
  CHECK(decoded.protocol_version == protocol::kControlProtocolVersion);
}

void test_capability_offer_codec_supports_new_and_legacy_layouts() {
  protocol::CapabilityOfferMessage offer;
  offer.capabilities = capabilities({48'000}, 48'000);
  offer.preferred_jitter_packets = 2;
  offer.preferred_packet_microseconds = 2'500;
  std::vector<std::byte> frame;
  CHECK(protocol::encode(offer, frame));

  protocol::CapabilityOfferMessage decoded;
  CHECK(protocol::decode(frame, decoded) == protocol::CodecError::none);
  CHECK(decoded.preferred_jitter_packets == 2);
  CHECK(decoded.preferred_packet_microseconds == 2'500);

  // Simulate a pre-field capability offer by removing the trailing u32 and
  // correcting the body length in the frame header.
  frame.resize(frame.size() - 4);
  const auto body_size = static_cast<std::uint16_t>(frame.size() - 3);
  frame[0] = static_cast<std::byte>(body_size >> 8U);
  frame[1] = static_cast<std::byte>(body_size & 0xffU);
  CHECK(protocol::decode(frame, decoded) == protocol::CodecError::none);
  CHECK(decoded.preferred_packet_microseconds == 5'000);
}

void test_control_codec_rejects_malformed_input() {
  protocol::CodecError error = protocol::CodecError::none;
  // A truncated header yields "need more bytes", not a parse.
  const std::array<std::byte, 2> partial{};
  CHECK(protocol::peek_frame_size(partial, error) == 0);
  CHECK(error == protocol::CodecError::none);

  // An oversized length must be refused before anything is allocated.
  std::array<std::byte, 3> oversized{std::byte{0xFF}, std::byte{0xFF},
                                     std::byte{1}};
  CHECK(protocol::peek_frame_size(oversized, error) == 0);
  CHECK(error == protocol::CodecError::oversized);

  // Random bodies must never decode into a usable message.
  protocol::HelloMessage hello;
  hello.device_name = "x";
  hello.fingerprint.assign(protocol::kFingerprintBytes, 0x01);
  hello.nonce.assign(protocol::kNonceBytes, 0x02);
  std::vector<std::byte> frame;
  CHECK(protocol::encode(hello, frame));
  for (std::size_t i = protocol::kControlFrameHeaderBytes; i < frame.size();
       ++i) {
    auto corrupted = frame;
    corrupted.resize(i);
    protocol::HelloMessage decoded;
    CHECK(protocol::decode(corrupted, decoded) != protocol::CodecError::none);
  }
}

// A peer must not be able to push an unsupported configuration through the
// accept frame.
void test_control_codec_validates_session_parameters() {
  protocol::SessionAcceptMessage accept;
  accept.parameters.sample_rate = 48'000;
  accept.parameters.channels = 2;
  accept.parameters.frames_per_packet = 240;
  accept.parameters.packet_microseconds = 5'000;
  accept.parameters.payload_type = 96;
  accept.parameters.ssrc = 0x1234;
  accept.parameters.media_port = 5'004;
  accept.parameters.session_id = 99;
  std::vector<std::byte> frame;
  CHECK(protocol::encode(accept, frame));
  protocol::SessionAcceptMessage decoded;
  CHECK(protocol::decode(frame, decoded) == protocol::CodecError::none);

  accept.parameters.payload_type = 3;  // outside the dynamic range
  std::vector<std::byte> bad;
  CHECK(protocol::encode(accept, bad));
  CHECK(protocol::decode(bad, decoded) != protocol::CodecError::none);
}

void test_abuse_guard_blocks_floods() {
  protocol::AbuseGuard guard;
  auto now = std::chrono::steady_clock::now();
  CHECK(guard.accept(now));
  for (int i = 0; i < 64; ++i) {
    (void)guard.record_invalid(now);
  }
  CHECK(guard.blocked(now));
  CHECK(!guard.accept(now));
  // The block is temporary, not permanent.
  CHECK(!guard.blocked(now + std::chrono::seconds(31)));
}

// ---------------------------------------------------------------------------
// Identity and trust
// ---------------------------------------------------------------------------

void test_identity_is_stable_and_private_key_is_not_leaked() {
  std::string error;
  auto identity = security::DeviceIdentity::generate("LanAudio Test", error);
  CHECK(identity != nullptr);
  if (!identity) return;

  const auto fingerprint = identity->fingerprint();
  CHECK(security::to_hex(fingerprint).size() == 64);
  CHECK(security::short_fingerprint(fingerprint).find('-') !=
        std::string::npos);

  const auto certificate = identity->certificate_pem();
  CHECK(certificate.find("BEGIN CERTIFICATE") != std::string::npos);
  CHECK(certificate.find("PRIVATE KEY") == std::string::npos);

  auto restored = security::DeviceIdentity::from_pem(
      identity->private_key_pem(), certificate, error);
  CHECK(restored != nullptr);
  if (restored) CHECK(restored->fingerprint() == fingerprint);

  security::Fingerprint parsed{};
  CHECK(security::parse_hex_fingerprint(security::to_hex(fingerprint),
                                        parsed));
  CHECK(parsed == fingerprint);
  CHECK(!security::parse_hex_fingerprint("not a fingerprint", parsed));
}

void test_identity_manager_persists_across_restarts() {
  auto store = security::make_memory_credential_store();
  std::string error;
  security::IdentityManager first(store, "Persisted Mac");
  auto* identity = first.identity(error);
  CHECK(identity != nullptr);
  if (!identity) return;
  const auto fingerprint = identity->fingerprint();

  security::IdentityManager second(store, "Persisted Mac");
  auto* reloaded = second.identity(error);
  CHECK(reloaded != nullptr);
  if (reloaded) CHECK(reloaded->fingerprint() == fingerprint);

  CHECK(second.reset(error));
  security::IdentityManager third(store, "Persisted Mac");
  auto* regenerated = third.identity(error);
  CHECK(regenerated != nullptr);
  if (regenerated) CHECK(regenerated->fingerprint() != fingerprint);
}

void test_trust_store_round_trip() {
  auto store = security::make_memory_credential_store();
  security::TrustStore trust(store);
  std::string error;
  CHECK(trust.load(error));
  CHECK(trust.peers().empty());

  security::Fingerprint first{};
  first.fill(0x11);
  security::Fingerprint second{};
  second.fill(0x22);
  CHECK(trust.add(first, "Living room\tMac\n", error));
  CHECK(trust.add(second, "Studio Mac", error));
  CHECK(trust.contains(first));
  CHECK(trust.peers().size() == 2);
  // Control characters must not survive into the stored record.
  const auto stored = trust.find(first);
  CHECK(stored.has_value());
  if (stored) CHECK(stored->name == "Living roomMac");

  security::TrustStore reloaded(store);
  CHECK(reloaded.load(error));
  CHECK(reloaded.contains(first) && reloaded.contains(second));

  CHECK(reloaded.rename(second, "Control Room", error));
  const auto renamed = reloaded.find(second);
  CHECK(renamed.has_value() && renamed->name == "Control Room");

  CHECK(reloaded.remove(first, error));
  CHECK(!reloaded.contains(first));
  CHECK(!reloaded.remove(first, error));

  CHECK(reloaded.reset(error));
  CHECK(reloaded.peers().empty());
  security::TrustStore after_reset(store);
  CHECK(after_reset.load(error));
  CHECK(after_reset.peers().empty());
}

void test_short_authentication_string_properties() {
  security::Fingerprint alice{};
  alice.fill(0x01);
  security::Fingerprint bob{};
  bob.fill(0x02);
  std::array<std::uint8_t, security::kPairingExporterBytes> exporter{};
  exporter.fill(0x7F);

  // Both peers derive the same transcript regardless of ordering.
  const auto forward = security::pairing_transcript(exporter, alice, bob);
  const auto reverse = security::pairing_transcript(exporter, bob, alice);
  CHECK(forward == reverse);

  const auto code = security::short_authentication_string(forward);
  CHECK(code.size() == 6);
  CHECK(std::all_of(code.begin(), code.end(),
                    [](char digit) { return digit >= '0' && digit <= '9'; }));

  // A different TLS session produces a different code.
  exporter.fill(0x7E);
  const auto other = security::pairing_transcript(exporter, alice, bob);
  CHECK(other != forward);
  CHECK(security::constant_time_equal(forward, reverse));
  CHECK(!security::constant_time_equal(forward, other));
}

// ---------------------------------------------------------------------------
// SRTP
// ---------------------------------------------------------------------------

void test_srtp_round_trip_and_replay_protection() {
  std::string error;
  std::array<std::uint8_t, srtp::kKeyMaterialBytes> material{};
  CHECK(security::random_bytes(material));
  const std::uint32_t ssrc = 0x1A2B3C4D;

  auto sender = srtp::SrtpProtector::create(srtp::Direction::outbound, true,
                                            ssrc, material, error);
  auto receiver = srtp::SrtpProtector::create(srtp::Direction::inbound, false,
                                              ssrc, material, error);
  CHECK(sender != nullptr);
  CHECK(receiver != nullptr);
  if (!sender || !receiver) return;
  CHECK(std::string(sender->profile_name()) == "AEAD_AES_128_GCM");

  lanaudio::RtpPacket packet;
  packet.header.payload_type = 96;
  packet.header.sequence = 1;
  packet.header.ssrc = ssrc;
  packet.payload_size = 64;
  std::array<std::byte, lanaudio::kMaxProtectedPacketBytes> wire{};
  const auto plain = lanaudio::serialize_rtp(packet, wire);
  CHECK(plain > 0);

  auto protected_copy = wire;
  const auto protected_size = sender->protect(protected_copy, plain);
  CHECK(protected_size > plain);
  // The payload must not survive in the clear.
  CHECK(!std::equal(wire.begin() + lanaudio::kRtpHeaderBytes,
                    wire.begin() + static_cast<std::ptrdiff_t>(plain),
                    protected_copy.begin() + lanaudio::kRtpHeaderBytes));

  auto received = protected_copy;
  const auto recovered = receiver->unprotect(received, protected_size);
  CHECK(recovered == plain);
  CHECK(std::equal(wire.begin(),
                   wire.begin() + static_cast<std::ptrdiff_t>(plain),
                   received.begin()));

  // Replaying the same packet must be rejected and reported as a replay.
  auto replay = protected_copy;
  CHECK(receiver->unprotect(replay, protected_size) == 0);
  CHECK(receiver->last_failure_was_replay());

  // A tampered tag is an authentication failure, not a replay.
  packet.header.sequence = 2;
  const auto second_plain = lanaudio::serialize_rtp(packet, wire);
  auto tampered = wire;
  const auto tampered_size = sender->protect(tampered, second_plain);
  tampered[tampered_size - 1] =
      tampered[tampered_size - 1] ^ std::byte{0xFF};
  CHECK(receiver->unprotect(tampered, tampered_size) == 0);
  CHECK(!receiver->last_failure_was_replay());
}

void test_srtp_requires_matching_keys_and_ssrc() {
  std::string error;
  std::array<std::uint8_t, srtp::kKeyMaterialBytes> material{};
  CHECK(security::random_bytes(material));
  CHECK(srtp::SrtpProtector::create(srtp::Direction::outbound, true, 0,
                                    material, error) == nullptr);

  std::array<std::uint8_t, 4> too_short{};
  CHECK(srtp::SrtpProtector::create(srtp::Direction::outbound, true, 7,
                                    too_short, error) == nullptr);

  // Both peers must take opposite halves of the exporter output, otherwise
  // the directions use different keys and nothing decrypts.
  auto sender = srtp::SrtpProtector::create(srtp::Direction::outbound, true, 7,
                                            material, error);
  auto wrong = srtp::SrtpProtector::create(srtp::Direction::inbound, true, 7,
                                           material, error);
  CHECK(sender != nullptr && wrong != nullptr);
  if (!sender || !wrong) return;
  lanaudio::RtpPacket packet;
  packet.header.payload_type = 96;
  packet.header.sequence = 9;
  packet.header.ssrc = 7;
  packet.payload_size = 32;
  std::array<std::byte, lanaudio::kMaxProtectedPacketBytes> wire{};
  const auto plain = lanaudio::serialize_rtp(packet, wire);
  const auto size = sender->protect(wire, plain);
  CHECK(size > 0);
  CHECK(wrong->unprotect(wire, size) == 0);
}

// ---------------------------------------------------------------------------
// End-to-end control channel
// ---------------------------------------------------------------------------

struct Endpoints {
  std::shared_ptr<control::SecurityContext> receiver;
  std::shared_ptr<control::SecurityContext> sender;
};

Endpoints make_endpoints() {
  std::string error;
  Endpoints endpoints;
  endpoints.receiver =
      control::SecurityContext::create_ephemeral("Receiver Mac", "test",
                                                 error);
  endpoints.sender =
      control::SecurityContext::create_ephemeral("Sender Mac", "test", error);
  CHECK(endpoints.receiver != nullptr);
  CHECK(endpoints.sender != nullptr);
  return endpoints;
}

bool wait_for(const std::function<bool()>& predicate,
              std::chrono::milliseconds limit) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return predicate();
}

void confirm_when_asked(control::SecureEndpoint& endpoint,
                        std::string& observed_code, bool accept) {
  if (wait_for([&endpoint] { return endpoint.pairing_gate().waiting(); },
               std::chrono::seconds(10))) {
    observed_code = endpoint.pairing_gate().pending_code();
    endpoint.pairing_gate().decide(accept);
  }
}

void test_secure_session_pairs_negotiates_and_streams() {
  auto endpoints = make_endpoints();
  if (!endpoints.receiver || !endpoints.sender) return;

  control::SecureReceiver::Options receiver_options;
  receiver_options.control_port = 45'105;
  receiver_options.media_port = 45'104;
  receiver_options.advertise = false;
  control::SecureReceiver receiver(endpoints.receiver, receiver_options);

  std::string error;
  CHECK(receiver.start(error));
  if (!receiver.running()) {
    std::cerr << "receiver did not start: " << error << '\n';
    return;
  }

  control::SecureSender::Options sender_options;
  sender_options.host = "127.0.0.1";
  sender_options.control_port = receiver_options.control_port;
  control::SecureSender sender(endpoints.sender, sender_options);
  CHECK(sender.start(error));

  std::string receiver_code;
  std::string sender_code;
  std::thread receiver_confirm(
      [&] { confirm_when_asked(receiver, receiver_code, true); });
  std::thread sender_confirm(
      [&] { confirm_when_asked(sender, sender_code, true); });
  receiver_confirm.join();
  sender_confirm.join();

  // Both devices must show the same six digits.
  CHECK(receiver_code.size() == 6);
  CHECK(receiver_code == sender_code);

  const bool streaming = wait_for(
      [&] {
        return sender.snapshot().state == control::SessionState::streaming &&
               receiver.snapshot().state == control::SessionState::streaming;
      },
      std::chrono::seconds(15));
  CHECK(streaming);
  if (!streaming) {
    std::cerr << "sender: " << sender.snapshot().message
              << "\nreceiver: " << receiver.snapshot().message << '\n';
    sender.stop();
    receiver.stop();
    return;
  }

  const auto parameters = sender.snapshot().parameters;
  CHECK(parameters.sample_rate == 48'000);
  CHECK(parameters.ssrc != 0);
  CHECK(parameters.session_id != 0);
  CHECK(parameters == receiver.snapshot().parameters);
  CHECK(sender.snapshot().media_profile == "AEAD_AES_128_GCM");
  CHECK(control::allows_media(sender.snapshot().state));

  // Both devices now trust each other and show the same fingerprint.
  CHECK(endpoints.sender->trust().contains(
      endpoints.receiver->identity().fingerprint()));
  CHECK(endpoints.receiver->trust().contains(
      endpoints.sender->identity().fingerprint()));

  // push_audio drops a block when it cannot take the media lock without
  // waiting, exactly as a real audio callback would, so keep feeding audio
  // like a device does instead of relying on a single block getting through.
  std::vector<float> audio(parameters.frames_per_packet * 2 * 6, 0.25F);
  const bool delivered = wait_for(
      [&] {
        const auto* metrics = receiver.metrics();
        if (metrics != nullptr && metrics->packets_received.load() >= 6) {
          return true;
        }
        sender.push_audio(audio);
        return false;
      },
      std::chrono::seconds(5));
  CHECK(delivered);
  if (!delivered) {
    if (const auto* sent = sender.metrics()) {
      std::cerr << "sender: sent=" << sent->packets_sent.load()
                << " send_fail=" << sent->send_failures.load()
                << " overruns=" << sent->overruns.load() << '\n';
    }
    if (const auto* received = receiver.metrics()) {
      std::cerr << "receiver: received=" << received->packets_received.load()
                << " rate_limited=" << received->rate_limited_packets.load()
                << " malformed=" << received->malformed_packets.load() << '\n';
    }
  }

  const auto* metrics = receiver.metrics();
  if (metrics != nullptr) {
    CHECK(metrics->decrypt_failures.load() == 0);
    CHECK(metrics->replay_rejections.load() == 0);
    CHECK(metrics->foreign_source_rejections.load() == 0);
  }

  std::vector<float> output(512, 0.0F);
  receiver.render_audio(output);
  CHECK(std::any_of(output.begin(), output.end(),
                    [](float sample) { return sample > 0.2F; }));

  // Losing the control channel must silence the media path.
  sender.stop();
  const bool failed_closed = wait_for(
      [&] { return !control::allows_media(receiver.snapshot().state); },
      std::chrono::seconds(10));
  CHECK(failed_closed);
  std::vector<float> after_loss(512, 0.5F);
  receiver.render_audio(after_loss);
  CHECK(std::all_of(after_loss.begin(), after_loss.end(),
                    [](float sample) { return sample == 0.0F; }));
  receiver.stop();
}

void test_secure_session_refuses_unpaired_peer() {
  auto endpoints = make_endpoints();
  if (!endpoints.receiver || !endpoints.sender) return;

  control::SecureReceiver::Options receiver_options;
  receiver_options.control_port = 45'107;
  receiver_options.media_port = 45'106;
  receiver_options.advertise = false;
  receiver_options.allow_pairing = false;  // trust-only mode
  control::SecureReceiver receiver(endpoints.receiver, receiver_options);

  std::string error;
  CHECK(receiver.start(error));

  control::SecureSender::Options sender_options;
  sender_options.host = "127.0.0.1";
  sender_options.control_port = receiver_options.control_port;
  sender_options.allow_pairing = false;
  control::SecureSender sender(endpoints.sender, sender_options);
  CHECK(sender.start(error));

  const bool refused = wait_for(
      [&] {
        const auto state = sender.snapshot().state;
        return state == control::SessionState::trust_revoked ||
               state == control::SessionState::connection_lost;
      },
      std::chrono::seconds(10));
  CHECK(refused);
  CHECK(!control::allows_media(sender.snapshot().state));
  CHECK(sender.metrics() == nullptr);
  sender.stop();
  receiver.stop();
}

void test_secure_session_aborts_when_code_is_declined() {
  auto endpoints = make_endpoints();
  if (!endpoints.receiver || !endpoints.sender) return;

  control::SecureReceiver::Options receiver_options;
  receiver_options.control_port = 45'109;
  receiver_options.media_port = 45'108;
  receiver_options.advertise = false;
  control::SecureReceiver receiver(endpoints.receiver, receiver_options);

  std::string error;
  CHECK(receiver.start(error));

  control::SecureSender::Options sender_options;
  sender_options.host = "127.0.0.1";
  sender_options.control_port = receiver_options.control_port;
  control::SecureSender sender(endpoints.sender, sender_options);
  CHECK(sender.start(error));

  std::string ignored_code;
  std::thread receiver_confirm(
      [&] { confirm_when_asked(receiver, ignored_code, false); });
  std::thread sender_confirm(
      [&] { confirm_when_asked(sender, ignored_code, true); });
  receiver_confirm.join();
  sender_confirm.join();

  const bool aborted = wait_for(
      [&] { return !sender.running(); }, std::chrono::seconds(10));
  CHECK(aborted);
  CHECK(!control::allows_media(sender.snapshot().state));
  // A declined code must leave both trust stores untouched.
  CHECK(!endpoints.sender->trust().contains(
      endpoints.receiver->identity().fingerprint()));
  CHECK(!endpoints.receiver->trust().contains(
      endpoints.sender->identity().fingerprint()));
  sender.stop();
  receiver.stop();
}

void test_stop_unblocks_pairing_wait() {
  auto endpoints = make_endpoints();
  if (!endpoints.receiver || !endpoints.sender) return;

  control::SecureReceiver::Options receiver_options;
  receiver_options.control_port = 45'111;
  receiver_options.media_port = 45'110;
  receiver_options.advertise = false;
  control::SecureReceiver receiver(endpoints.receiver, receiver_options);

  std::string error;
  CHECK(receiver.start(error));

  control::SecureSender::Options sender_options;
  sender_options.host = "127.0.0.1";
  sender_options.control_port = receiver_options.control_port;
  control::SecureSender sender(endpoints.sender, sender_options);
  CHECK(sender.start(error));

  CHECK(wait_for(
      [&] {
        return sender.pairing_gate().waiting() &&
               receiver.pairing_gate().waiting();
      },
      std::chrono::seconds(10)));

  const auto started = std::chrono::steady_clock::now();
  sender.stop();
  receiver.stop();
  CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
}

void test_one_sided_trust_pairs_again_on_both_devices() {
  auto endpoints = make_endpoints();
  if (!endpoints.receiver || !endpoints.sender) return;
  std::string error;
  // Only the receiver remembers the sender, as after Reset trust on the
  // sender. Both devices must show a code instead of diverging.
  CHECK(endpoints.receiver->trust().add(
      endpoints.sender->identity().fingerprint(), "Sender Mac", error));

  control::SecureReceiver::Options receiver_options;
  receiver_options.control_port = 45'113;
  receiver_options.media_port = 45'112;
  receiver_options.advertise = false;
  control::SecureReceiver receiver(endpoints.receiver, receiver_options);
  CHECK(receiver.start(error));

  control::SecureSender::Options sender_options;
  sender_options.host = "127.0.0.1";
  sender_options.control_port = receiver_options.control_port;
  control::SecureSender sender(endpoints.sender, sender_options);
  CHECK(sender.start(error));

  std::string receiver_code;
  std::string sender_code;
  std::thread receiver_confirm(
      [&] { confirm_when_asked(receiver, receiver_code, true); });
  std::thread sender_confirm(
      [&] { confirm_when_asked(sender, sender_code, true); });
  receiver_confirm.join();
  sender_confirm.join();
  CHECK(receiver_code.size() == 6);
  CHECK(receiver_code == sender_code);

  const bool streaming = wait_for(
      [&] {
        return sender.snapshot().state == control::SessionState::streaming &&
               receiver.snapshot().state == control::SessionState::streaming;
      },
      std::chrono::seconds(15));
  CHECK(streaming);
  CHECK(endpoints.sender->trust().contains(
      endpoints.receiver->identity().fingerprint()));
  sender.stop();
  receiver.stop();
}

void test_mutual_trust_skips_the_code() {
  auto endpoints = make_endpoints();
  if (!endpoints.receiver || !endpoints.sender) return;
  std::string error;
  CHECK(endpoints.receiver->trust().add(
      endpoints.sender->identity().fingerprint(), "Sender Mac", error));
  CHECK(endpoints.sender->trust().add(
      endpoints.receiver->identity().fingerprint(), "Receiver Mac", error));

  control::SecureReceiver::Options receiver_options;
  receiver_options.control_port = 45'115;
  receiver_options.media_port = 45'114;
  receiver_options.advertise = false;
  receiver_options.allow_pairing = false;
  control::SecureReceiver receiver(endpoints.receiver, receiver_options);
  CHECK(receiver.start(error));

  control::SecureSender::Options sender_options;
  sender_options.host = "127.0.0.1";
  sender_options.control_port = receiver_options.control_port;
  sender_options.allow_pairing = false;
  control::SecureSender sender(endpoints.sender, sender_options);
  CHECK(sender.start(error));

  const bool streaming = wait_for(
      [&] {
        return sender.snapshot().state == control::SessionState::streaming &&
               receiver.snapshot().state == control::SessionState::streaming;
      },
      std::chrono::seconds(15));
  CHECK(streaming);
  sender.stop();
  receiver.stop();
}

void test_pairing_gate_ignores_stale_clicks() {
  control::PairingGate gate;
  // A click with no code on screen must not approve the next pairing.
  gate.decide(true);
  CHECK(!gate.await_decision("123456", std::chrono::milliseconds(50)));

  // A click for an armed code is honoured even if it lands before the
  // worker starts waiting.
  gate.arm("654321");
  gate.decide(true);
  CHECK(gate.await_decision("654321", std::chrono::milliseconds(50)));

  // Re-arming discards a decision made for a previous code.
  gate.arm("111111");
  gate.decide(true);
  gate.arm("222222");
  CHECK(!gate.await_decision("222222", std::chrono::milliseconds(50)));

  gate.cancel();
  gate.arm("333333");
  gate.decide(true);
  CHECK(!gate.await_decision("333333", std::chrono::milliseconds(50)));
}

}  // namespace

int main() {
  test_negotiation_prefers_receiver_rate();
  test_negotiation_prefers_requested_packet_duration();
  test_negotiation_defaults_to_five_milliseconds();
  test_negotiation_falls_back_when_preference_exceeds_mtu();
  test_negotiation_falls_back_to_closest_common_rate();
  test_negotiation_fails_without_common_rate();
  test_negotiation_keeps_packets_under_the_mtu();
  test_negotiation_rejects_unknown_rates();
  test_control_codec_round_trip();
  test_capability_offer_codec_supports_new_and_legacy_layouts();
  test_control_codec_rejects_malformed_input();
  test_control_codec_validates_session_parameters();
  test_abuse_guard_blocks_floods();
  test_identity_is_stable_and_private_key_is_not_leaked();
  test_identity_manager_persists_across_restarts();
  test_trust_store_round_trip();
  test_short_authentication_string_properties();
  test_srtp_round_trip_and_replay_protection();
  test_srtp_requires_matching_keys_and_ssrc();
  test_secure_session_pairs_negotiates_and_streams();
  test_secure_session_refuses_unpaired_peer();
  test_secure_session_aborts_when_code_is_declined();
  test_stop_unblocks_pairing_wait();
  test_one_sided_trust_pairs_again_on_both_devices();
  test_mutual_trust_skips_the_code();
  test_pairing_gate_ignores_stale_clicks();
  if (failures == 0) std::cout << "all LanAudio security tests passed\n";
  return failures == 0 ? 0 : 1;
}
