#pragma once

#include <cstdint>

namespace lanaudio {

enum class LatencyMode {
  balanced,
  low_latency,
  reliable,
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
    case LatencyMode::balanced:
    default:
      return {5'000, 4, 128};
  }
}

}  // namespace lanaudio
