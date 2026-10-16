#include "lanaudio/protocol/negotiation.hpp"

#include <algorithm>
#include <array>

namespace lanaudio::protocol {
namespace {

constexpr std::array<std::uint32_t, 8> kSampleRates{
    44'100, 48'000, 88'200, 96'000, 176'400, 192'000, 32'000, 16'000};

// Short durations exist so that high sample rates still fit inside one
// encrypted datagram; 192 kHz stereo needs 1.25 ms packets to stay under the
// payload limit.
constexpr std::array<std::uint32_t, 6> kPacketDurations{
    1'000, 1'250, 2'500, 5'000, 10'000, 20'000};

std::uint32_t distance_from_default(std::uint32_t rate) noexcept {
  return rate >= kPreferredSampleRate ? rate - kPreferredSampleRate
                                      : kPreferredSampleRate - rate;
}

std::vector<std::uint32_t> intersect(const std::vector<std::uint32_t>& first,
                                     const std::vector<std::uint32_t>& second,
                                     bool (*allowed)(std::uint32_t)) {
  std::vector<std::uint32_t> common;
  for (const auto value : first) {
    if (!allowed(value)) continue;
    if (std::find(second.begin(), second.end(), value) == second.end()) {
      continue;
    }
    if (std::find(common.begin(), common.end(), value) == common.end()) {
      common.push_back(value);
    }
  }
  return common;
}

}  // namespace

std::span<const std::uint32_t> supported_sample_rates() noexcept {
  return {kSampleRates.data(), kSampleRates.size()};
}

std::span<const std::uint32_t> supported_packet_durations() noexcept {
  return {kPacketDurations.data(), kPacketDurations.size()};
}

bool is_supported_sample_rate(std::uint32_t rate) noexcept {
  return std::find(kSampleRates.begin(), kSampleRates.end(), rate) !=
         kSampleRates.end();
}

bool is_supported_packet_duration(std::uint32_t microseconds) noexcept {
  return std::find(kPacketDurations.begin(), kPacketDurations.end(),
                   microseconds) != kPacketDurations.end();
}

std::uint32_t frames_for_duration(std::uint32_t sample_rate,
                                  std::uint32_t microseconds) noexcept {
  if (sample_rate == 0 || microseconds == 0) return 0;
  // Rates such as 44.1 kHz do not divide evenly into round durations, so the
  // frame count is rounded and the achieved duration is reported back.
  const std::uint64_t numerator =
      static_cast<std::uint64_t>(sample_rate) * microseconds + 500'000ULL;
  return static_cast<std::uint32_t>(numerator / 1'000'000ULL);
}

std::uint32_t duration_for_frames(std::uint32_t frames,
                                  std::uint32_t sample_rate) noexcept {
  if (sample_rate == 0 || frames == 0) return 0;
  const std::uint64_t numerator =
      static_cast<std::uint64_t>(frames) * 1'000'000ULL + sample_rate / 2;
  return static_cast<std::uint32_t>(numerator / sample_rate);
}

std::uint32_t payload_bytes_for(std::uint32_t frames,
                                std::uint16_t channels) noexcept {
  return frames * channels * 2U;
}

bool DeviceCapabilities::valid() const noexcept {
  if (sample_rates.empty() || packet_durations.empty()) return false;
  if (channels < kRequiredChannels) return false;
  const bool rates_known =
      std::any_of(sample_rates.begin(), sample_rates.end(),
                  [](std::uint32_t rate) { return is_supported_sample_rate(rate); });
  const bool durations_known =
      std::any_of(packet_durations.begin(), packet_durations.end(),
                  [](std::uint32_t value) {
                    return is_supported_packet_duration(value);
                  });
  return rates_known && durations_known;
}

std::uint32_t SessionParameters::payload_bytes() const noexcept {
  return payload_bytes_for(frames_per_packet, channels);
}

bool SessionParameters::valid() const noexcept {
  if (channels != kRequiredChannels) return false;
  if (!is_supported_sample_rate(sample_rate)) return false;
  if (frames_per_packet == 0) return false;
  if (packet_microseconds < kMinPacketMicroseconds ||
      packet_microseconds > kMaxPacketMicroseconds) {
    return false;
  }
  // The reported duration must be the one the frame count actually produces.
  if (duration_for_frames(frames_per_packet, sample_rate) !=
      packet_microseconds) {
    return false;
  }
  if (payload_type < 96 || payload_type > 127) return false;
  if (ssrc == 0 || session_id == 0 || media_port == 0) return false;
  if (jitter_packets < 2 || jitter_packets > 32) return false;
  return payload_bytes() + kSrtpOverheadBytes <= kMaxMediaPayloadBytes;
}

std::string describe(NegotiationError error) {
  switch (error) {
    case NegotiationError::none:
      return "negotiation succeeded";
    case NegotiationError::invalid_capabilities:
      return "the reported audio capabilities are incomplete or unusable";
    case NegotiationError::unsupported_channel_count:
      return "both devices must provide at least two channels";
    case NegotiationError::no_common_sample_rate:
      return "the two devices share no supported sample rate; select "
             "interfaces that both support a common rate";
    case NegotiationError::no_common_packet_duration:
      return "the two devices share no supported packet duration";
    case NegotiationError::payload_exceeds_limit:
      return "no common packet duration keeps the encrypted packet under the "
             "network limit";
  }
  return "unknown negotiation error";
}

NegotiationError negotiate_session(const DeviceCapabilities& receiver,
                                   const DeviceCapabilities& sender,
                                   SessionParameters& result,
                                   std::uint32_t preferred_packet_microseconds)
    noexcept {
  if (!receiver.valid() || !sender.valid()) {
    return NegotiationError::invalid_capabilities;
  }
  if (receiver.channels < kRequiredChannels ||
      sender.channels < kRequiredChannels) {
    return NegotiationError::unsupported_channel_count;
  }

  const auto rates = intersect(receiver.sample_rates, sender.sample_rates,
                               &is_supported_sample_rate);
  if (rates.empty()) return NegotiationError::no_common_sample_rate;

  std::uint32_t rate = rates.front();
  if (std::find(rates.begin(), rates.end(), receiver.preferred_sample_rate) !=
      rates.end()) {
    rate = receiver.preferred_sample_rate;
  } else {
    for (const auto candidate : rates) {
      const auto current = distance_from_default(rate);
      const auto proposed = distance_from_default(candidate);
      if (proposed < current || (proposed == current && candidate > rate)) {
        rate = candidate;
      }
    }
  }

  const auto durations = intersect(receiver.packet_durations,
                                   sender.packet_durations,
                                   &is_supported_packet_duration);
  if (durations.empty()) return NegotiationError::no_common_packet_duration;

  // Prefer the project default duration, then the longest duration that still
  // fits. Higher sample rates therefore fall back to shorter packets instead
  // of exceeding the packet limit.
  std::vector<std::uint32_t> ordered(durations);
  std::sort(ordered.begin(), ordered.end(), std::greater<std::uint32_t>());
  const auto preferred = std::find(ordered.begin(), ordered.end(),
                                   preferred_packet_microseconds);
  bool preferred_fits = false;
  if (preferred != ordered.end()) {
    const auto preferred_frames = frames_for_duration(rate, *preferred);
    const auto preferred_payload =
        payload_bytes_for(preferred_frames, kRequiredChannels);
    preferred_fits = preferred_frames != 0 &&
                     preferred_payload + kSrtpOverheadBytes <=
                         kMaxMediaPayloadBytes;
  }
  if (preferred_fits) {
    std::rotate(ordered.begin(), preferred, preferred + 1);
  } else {
    // An unavailable or oversized preference retains the established 5 ms
    // preference before falling through to the longest MTU-safe duration.
    const auto fallback = std::find(ordered.begin(), ordered.end(),
                                    kPreferredPacketMicroseconds);
    if (fallback != ordered.end()) {
      std::rotate(ordered.begin(), fallback, fallback + 1);
    }
  }

  for (const auto duration : ordered) {
    const auto frames = frames_for_duration(rate, duration);
    if (frames == 0) continue;
    const auto payload = payload_bytes_for(frames, kRequiredChannels);
    if (payload + kSrtpOverheadBytes > kMaxMediaPayloadBytes) continue;

    result.sample_rate = rate;
    result.channels = kRequiredChannels;
    result.frames_per_packet = static_cast<std::uint16_t>(frames);
    result.packet_microseconds = duration_for_frames(frames, rate);
    return NegotiationError::none;
  }
  return NegotiationError::payload_exceeds_limit;
}

}  // namespace lanaudio::protocol
