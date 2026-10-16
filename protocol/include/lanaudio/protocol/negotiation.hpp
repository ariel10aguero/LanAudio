#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace lanaudio::protocol {

// Control protocol version implemented by this build. Version 1 was the
// unnegotiated 0.1 media prototype and is no longer accepted.
inline constexpr std::uint16_t kControlProtocolVersion = 2;
inline constexpr std::uint16_t kMinimumControlProtocolVersion = 2;

// Conservative application payload limit shared with the media protocol. The
// SRTP authentication tag and RTP header are accounted for separately.
inline constexpr std::uint32_t kMaxMediaPayloadBytes = 1'200;
inline constexpr std::uint32_t kRtpHeaderBytes = 12;
inline constexpr std::uint32_t kSrtpOverheadBytes = 16;

inline constexpr std::uint16_t kRequiredChannels = 2;
inline constexpr std::uint32_t kPreferredSampleRate = 48'000;
inline constexpr std::uint32_t kPreferredPacketMicroseconds = 5'000;

// Sample rates the media path is allowed to negotiate. Device-reported rates
// outside this list are ignored instead of being trusted blindly.
[[nodiscard]] std::span<const std::uint32_t> supported_sample_rates() noexcept;

// Packet durations the media path is allowed to negotiate, in microseconds.
[[nodiscard]] std::span<const std::uint32_t> supported_packet_durations()
    noexcept;

[[nodiscard]] bool is_supported_sample_rate(std::uint32_t rate) noexcept;
[[nodiscard]] bool is_supported_packet_duration(
    std::uint32_t microseconds) noexcept;

// Bounds on the duration a session may actually carry. Rates that do not
// divide evenly into a target duration land slightly off it.
inline constexpr std::uint32_t kMinPacketMicroseconds = 500;
inline constexpr std::uint32_t kMaxPacketMicroseconds = 25'000;

// Frames per packet for a rate/duration pair, rounded to the nearest frame.
[[nodiscard]] std::uint32_t frames_for_duration(
    std::uint32_t sample_rate, std::uint32_t microseconds) noexcept;

// The duration a given frame count actually occupies at a sample rate.
[[nodiscard]] std::uint32_t duration_for_frames(
    std::uint32_t frames, std::uint32_t sample_rate) noexcept;

[[nodiscard]] std::uint32_t payload_bytes_for(std::uint32_t frames,
                                              std::uint16_t channels) noexcept;

// Capabilities of one endpoint, derived from the selected audio device.
struct DeviceCapabilities {
  std::vector<std::uint32_t> sample_rates;
  std::vector<std::uint32_t> packet_durations;
  std::uint32_t preferred_sample_rate{kPreferredSampleRate};
  std::uint16_t channels{kRequiredChannels};

  [[nodiscard]] bool valid() const noexcept;
};

// Everything the media path needs once the control channel has agreed.
struct SessionParameters {
  std::uint32_t sample_rate{kPreferredSampleRate};
  std::uint16_t channels{kRequiredChannels};
  std::uint16_t frames_per_packet{};
  std::uint32_t packet_microseconds{};
  std::uint8_t payload_type{96};
  std::uint32_t ssrc{};
  std::uint16_t media_port{};
  std::uint16_t jitter_packets{4};
  std::uint64_t session_id{};

  [[nodiscard]] std::uint32_t payload_bytes() const noexcept;
  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(const SessionParameters&,
                         const SessionParameters&) noexcept = default;
};

enum class NegotiationError {
  none,
  invalid_capabilities,
  unsupported_channel_count,
  no_common_sample_rate,
  no_common_packet_duration,
  payload_exceeds_limit,
};

[[nodiscard]] std::string describe(NegotiationError error);

// Deterministic capability intersection. The receiver's preferred rate wins
// when both endpoints support it; otherwise the common rate closest to the
// project default is selected. The packet duration is then reduced as needed
// so that the payload plus RTP and SRTP overhead stays below the limit.
[[nodiscard]] NegotiationError negotiate_session(
    const DeviceCapabilities& receiver, const DeviceCapabilities& sender,
    SessionParameters& result,
    std::uint32_t preferred_packet_microseconds =
        kPreferredPacketMicroseconds) noexcept;

}  // namespace lanaudio::protocol
