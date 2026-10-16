#pragma once

#include <cstdint>

namespace lanaudio {

enum class LatencyMode {
  balanced,
  low_latency,
  reliable,
  // Wi-Fi links stall for 100 ms or more (power save, AWDL off-channel time),
  // so these trade latency for a cushion that rides out the stall.
  wifi,
  congested_wifi,
};

struct LatencyPreset {
  std::uint32_t packet_microseconds;
  std::uint16_t jitter_packets;
  std::uint32_t buffer_frames;
};

[[nodiscard]] constexpr LatencyPreset latency_preset(LatencyMode mode) noexcept {
  switch (mode) {
    case LatencyMode::low_latency:
      return {2'500, 2, 64};
    case LatencyMode::reliable:
      return {5'000, 8, 256};
    case LatencyMode::wifi:
      return {5'000, 16, 256};
    case LatencyMode::congested_wifi:
      return {5'000, 32, 256};
    case LatencyMode::balanced:
    default:
      return {5'000, 4, 128};
  }
}

}  // namespace lanaudio
