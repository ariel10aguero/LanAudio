#pragma once

#include "lanaudio/protocol/negotiation.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace lanaudio::protocol {

// Control frames are length-prefixed and hard-bounded. Nothing is allocated
// from a network-supplied length before the length has been validated.
inline constexpr std::size_t kControlFrameHeaderBytes = 3;
inline constexpr std::size_t kMaxControlBodyBytes = 2'048;
inline constexpr std::size_t kMaxControlFrameBytes =
    kControlFrameHeaderBytes + kMaxControlBodyBytes;
inline constexpr std::size_t kMaxTextFieldBytes = 128;
inline constexpr std::size_t kFingerprintBytes = 32;
inline constexpr std::size_t kNonceBytes = 32;

enum class ControlMessageType : std::uint8_t {
  hello = 1,
  capability_offer = 2,
  session_accept = 3,
  session_reject = 4,
  heartbeat = 5,
  goodbye = 6,
  pair_confirm = 7,
  pair_result = 8,
};

enum class RejectReason : std::uint8_t {
  unspecified = 0,
  unsupported_version = 1,
  untrusted_peer = 2,
  negotiation_failed = 3,
  busy = 4,
  policy = 5,
  timeout = 6,
};

struct HelloMessage {
  std::uint16_t protocol_version{kControlProtocolVersion};
  std::string device_name;
  std::string application_version;
  std::vector<std::uint8_t> fingerprint;
  std::vector<std::uint8_t> nonce;
};

struct CapabilityOfferMessage {
  DeviceCapabilities capabilities;
  std::uint16_t preferred_jitter_packets{4};
  std::uint32_t preferred_packet_microseconds{kPreferredPacketMicroseconds};
};

struct SessionAcceptMessage {
  SessionParameters parameters;
};

struct SessionRejectMessage {
  RejectReason reason{RejectReason::unspecified};
  std::string detail;
};

struct HeartbeatMessage {
  std::uint64_t counter{};
  std::uint64_t monotonic_micros{};
};

struct GoodbyeMessage {
  RejectReason reason{RejectReason::unspecified};
};

struct PairConfirmMessage {
  bool accepted{};
  std::vector<std::uint8_t> transcript_digest;
};

struct PairResultMessage {
  bool trusted{};
  std::string detail;
};

enum class CodecError {
  none,
  truncated,
  oversized,
  unknown_type,
  malformed_field,
  invalid_value,
};

[[nodiscard]] std::string describe(CodecError error);

// Encoders append a complete frame. They fail only when a field exceeds the
// documented bound, which indicates a local programming error.
[[nodiscard]] bool encode(const HelloMessage& message,
                          std::vector<std::byte>& frame);
[[nodiscard]] bool encode(const CapabilityOfferMessage& message,
                          std::vector<std::byte>& frame);
[[nodiscard]] bool encode(const SessionAcceptMessage& message,
                          std::vector<std::byte>& frame);
[[nodiscard]] bool encode(const SessionRejectMessage& message,
                          std::vector<std::byte>& frame);
[[nodiscard]] bool encode(const HeartbeatMessage& message,
                          std::vector<std::byte>& frame);
[[nodiscard]] bool encode(const GoodbyeMessage& message,
                          std::vector<std::byte>& frame);
[[nodiscard]] bool encode(const PairConfirmMessage& message,
                          std::vector<std::byte>& frame);
[[nodiscard]] bool encode(const PairResultMessage& message,
                          std::vector<std::byte>& frame);

// Reads the frame header only. Returns the total frame size when a complete
// frame is present, 0 when more bytes are required, and sets error on an
// unacceptable header.
[[nodiscard]] std::size_t peek_frame_size(std::span<const std::byte> buffer,
                                          CodecError& error) noexcept;

[[nodiscard]] CodecError frame_type(std::span<const std::byte> frame,
                                    ControlMessageType& type) noexcept;

[[nodiscard]] CodecError decode(std::span<const std::byte> frame,
                                HelloMessage& message);
[[nodiscard]] CodecError decode(std::span<const std::byte> frame,
                                CapabilityOfferMessage& message);
[[nodiscard]] CodecError decode(std::span<const std::byte> frame,
                                SessionAcceptMessage& message);
[[nodiscard]] CodecError decode(std::span<const std::byte> frame,
                                SessionRejectMessage& message);
[[nodiscard]] CodecError decode(std::span<const std::byte> frame,
                                HeartbeatMessage& message);
[[nodiscard]] CodecError decode(std::span<const std::byte> frame,
                                GoodbyeMessage& message);
[[nodiscard]] CodecError decode(std::span<const std::byte> frame,
                                PairConfirmMessage& message);
[[nodiscard]] CodecError decode(std::span<const std::byte> frame,
                                PairResultMessage& message);

}  // namespace lanaudio::protocol
