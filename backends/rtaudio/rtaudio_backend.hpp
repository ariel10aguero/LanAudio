#pragma once

#include "lanaudio/stream.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace lanaudio {

struct AudioDeviceInfo {
  std::uint32_t id{};
  std::string name;
  std::uint32_t input_channels{};
  std::uint32_t output_channels{};
  std::uint32_t current_sample_rate{};
  std::uint32_t preferred_sample_rate{};
  std::vector<std::uint32_t> sample_rates;
  bool default_input{};
  bool default_output{};
};

class RtAudioBackend {
 public:
  RtAudioBackend();
  ~RtAudioBackend();
  RtAudioBackend(const RtAudioBackend&) = delete;
  RtAudioBackend& operator=(const RtAudioBackend&) = delete;

  static std::vector<AudioDeviceInfo> devices(std::string& error);
  bool start_input(NetworkSender& sender, std::uint32_t device_id,
                   std::uint32_t first_channel, std::uint32_t sample_rate,
                   std::uint32_t channels,
                   std::uint32_t& buffer_frames, std::string& error);
  bool start_output(NetworkReceiver& receiver, std::uint32_t device_id,
                    std::uint32_t first_channel, std::uint32_t sample_rate,
                    std::uint32_t channels,
                    std::uint32_t& buffer_frames, std::string& error);

  // Callback variants used by the authenticated session. The audio thread
  // never waits on control-plane locks; the callbacks themselves must be
  // non-blocking.
  using InputHandler =
      std::function<void(std::span<const float> interleaved)>;
  using OutputHandler = std::function<void(std::span<float> interleaved)>;
  bool start_input(InputHandler handler, std::uint32_t device_id,
                   std::uint32_t first_channel, std::uint32_t sample_rate,
                   std::uint32_t channels, std::uint32_t& buffer_frames,
                   std::string& error);
  bool start_output(OutputHandler handler, std::uint32_t device_id,
                    std::uint32_t first_channel, std::uint32_t sample_rate,
                    std::uint32_t channels, std::uint32_t& buffer_frames,
                    std::string& error);
  void stop() noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace lanaudio
