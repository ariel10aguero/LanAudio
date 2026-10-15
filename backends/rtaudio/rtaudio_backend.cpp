#include "rtaudio_backend.hpp"

#ifdef LANAUDIO_HAS_RTAUDIO
#include <RtAudio.h>
#endif

#include <algorithm>
#include <array>
#include <utility>

namespace lanaudio {
namespace {

#ifdef LANAUDIO_HAS_RTAUDIO
bool supports_sample_rate(const RtAudio::DeviceInfo& info,
                          std::uint32_t sample_rate) {
  return info.sampleRates.empty() ||
         std::find(info.sampleRates.begin(), info.sampleRates.end(),
                   sample_rate) != info.sampleRates.end();
}
#endif

}  // namespace

struct RtAudioBackend::Impl {
#ifdef LANAUDIO_HAS_RTAUDIO
  RtAudio audio;
  NetworkSender* sender{};
  NetworkReceiver* receiver{};
  RtAudioBackend::InputHandler input_handler;
  RtAudioBackend::OutputHandler output_handler;
  unsigned int input_channels{2};
  std::array<float, 8'192> stereo_input{};

  void deliver_input(const float* samples, unsigned int frames) {
    if (samples == nullptr) return;
    if (input_channels >= 2) {
      const std::span<const float> interleaved(
          samples, static_cast<std::size_t>(frames) * 2);
      if (sender != nullptr) sender->push_audio(interleaved);
      if (input_handler) input_handler(interleaved);
      return;
    }
    std::size_t offset = 0;
    while (offset < frames) {
      const auto count = std::min<std::size_t>(frames - offset,
                                               stereo_input.size() / 2);
      for (std::size_t i = 0; i < count; ++i) {
        stereo_input[i * 2] = samples[offset + i];
        stereo_input[i * 2 + 1] = samples[offset + i];
      }
      const std::span<const float> interleaved(stereo_input.data(), count * 2);
      if (sender != nullptr) sender->push_audio(interleaved);
      if (input_handler) input_handler(interleaved);
      offset += count;
    }
  }

  static int input_callback(void*, void* input, unsigned int frames, double,
                            RtAudioStreamStatus, void* user) {
    auto& self = *static_cast<Impl*>(user);
    self.deliver_input(static_cast<const float*>(input), frames);
    return 0;
  }

  static int output_callback(void* output, void*, unsigned int frames, double,
                             RtAudioStreamStatus, void* user) {
    auto& self = *static_cast<Impl*>(user);
    if (output == nullptr) return 0;
    const std::span<float> interleaved(
        static_cast<float*>(output), static_cast<std::size_t>(frames) * 2);
    if (self.receiver != nullptr) self.receiver->render_audio(interleaved);
    if (self.output_handler) self.output_handler(interleaved);
    return 0;
  }
#endif
};

RtAudioBackend::RtAudioBackend() : impl_(std::make_unique<Impl>()) {}
RtAudioBackend::~RtAudioBackend() { stop(); }

std::vector<AudioDeviceInfo> RtAudioBackend::devices(std::string& error) {
  std::vector<AudioDeviceInfo> result;
#ifdef LANAUDIO_HAS_RTAUDIO
  RtAudio audio;
  for (const auto id : audio.getDeviceIds()) {
    const auto info = audio.getDeviceInfo(id);
    AudioDeviceInfo device;
    device.id = id;
    device.name = info.name;
    device.input_channels = info.inputChannels;
    device.output_channels = info.outputChannels;
    device.current_sample_rate = info.currentSampleRate;
    device.preferred_sample_rate = info.preferredSampleRate;
    device.sample_rates.assign(info.sampleRates.begin(), info.sampleRates.end());
    device.default_input = info.isDefaultInput;
    device.default_output = info.isDefaultOutput;
    result.push_back(std::move(device));
  }
  if (result.empty()) error = "no probed audio devices";
#else
  error = "this build does not include RtAudio";
#endif
  return result;
}

bool RtAudioBackend::start_input(NetworkSender& sender, std::uint32_t device_id,
                                 std::uint32_t first_channel,
                                 std::uint32_t sample_rate,
                                 std::uint32_t channels,
                                 std::uint32_t& buffer_frames,
                                 std::string& error) {
#ifdef LANAUDIO_HAS_RTAUDIO
  if (channels != 2) {
    error = "the current real-time backend requires stereo";
    return false;
  }
  RtAudio::StreamParameters input;
  input.deviceId =
      device_id == 0 ? impl_->audio.getDefaultInputDevice() : device_id;
  const auto device_info = impl_->audio.getDeviceInfo(input.deviceId);
  if (device_info.inputChannels == 0) {
    error = "selected device has no input channels";
    return false;
  }
  if (first_channel >= device_info.inputChannels) {
    error = "selected input channel is outside the device's channel range";
    return false;
  }
  if (!supports_sample_rate(device_info, sample_rate)) {
    error = "selected audio input does not report support for " +
            std::to_string(sample_rate) + " Hz";
    return false;
  }
  impl_->input_channels =
      std::min(channels, device_info.inputChannels - first_channel);
  input.nChannels = impl_->input_channels;
  input.firstChannel = first_channel;
  RtAudio::StreamOptions options;
  options.flags = RTAUDIO_MINIMIZE_LATENCY;
  impl_->sender = &sender;
  if (impl_->audio.openStream(nullptr, &input, RTAUDIO_FLOAT32, sample_rate,
                              &buffer_frames, &Impl::input_callback,
                              impl_.get(), &options) != RTAUDIO_NO_ERROR) {
#ifdef __APPLE__
    error =
        "Unable to open the selected audio input. Grant LanAudio microphone "
        "access in System Settings > Privacy & Security > Microphone, then "
        "try again. Core Audio: " +
        impl_->audio.getErrorText();
#else
    error = "Unable to open the selected audio input: " +
            impl_->audio.getErrorText();
#endif
    return false;
  }
  if (impl_->audio.startStream() != RTAUDIO_NO_ERROR) {
    error = impl_->audio.getErrorText();
    return false;
  }
  return true;
#else
  (void)sender;
  (void)device_id;
  (void)first_channel;
  (void)sample_rate;
  (void)channels;
  (void)buffer_frames;
  error = "this build does not include RtAudio";
  return false;
#endif
}

bool RtAudioBackend::start_output(NetworkReceiver& receiver,
                                  std::uint32_t device_id,
                                  std::uint32_t first_channel,
                                  std::uint32_t sample_rate,
                                  std::uint32_t channels,
                                  std::uint32_t& buffer_frames,
                                  std::string& error) {
#ifdef LANAUDIO_HAS_RTAUDIO
  if (channels != 2) {
    error = "the current real-time backend requires stereo";
    return false;
  }
  RtAudio::StreamParameters output;
  output.deviceId =
      device_id == 0 ? impl_->audio.getDefaultOutputDevice() : device_id;
  const auto device_info = impl_->audio.getDeviceInfo(output.deviceId);
  if (first_channel + channels > device_info.outputChannels) {
    error = "selected output channel pair is outside the device's channel "
            "range";
    return false;
  }
  if (!supports_sample_rate(device_info, sample_rate)) {
    error = "selected audio output does not report support for " +
            std::to_string(sample_rate) + " Hz";
    return false;
  }
  output.nChannels = channels;
  output.firstChannel = first_channel;
  RtAudio::StreamOptions options;
  options.flags = RTAUDIO_MINIMIZE_LATENCY;
  impl_->receiver = &receiver;
  if (impl_->audio.openStream(&output, nullptr, RTAUDIO_FLOAT32, sample_rate,
                              &buffer_frames, &Impl::output_callback,
                              impl_.get(), &options) != RTAUDIO_NO_ERROR) {
    error = "Unable to open the selected audio output: " +
            impl_->audio.getErrorText();
    return false;
  }
  if (impl_->audio.startStream() != RTAUDIO_NO_ERROR) {
    error = impl_->audio.getErrorText();
    return false;
  }
  return true;
#else
  (void)receiver;
  (void)device_id;
  (void)first_channel;
  (void)sample_rate;
  (void)channels;
  (void)buffer_frames;
  error = "this build does not include RtAudio";
  return false;
#endif
}

bool RtAudioBackend::start_input(InputHandler handler, std::uint32_t device_id,
                                 std::uint32_t first_channel,
                                 std::uint32_t sample_rate,
                                 std::uint32_t channels,
                                 std::uint32_t& buffer_frames,
                                 std::string& error) {
#ifdef LANAUDIO_HAS_RTAUDIO
  if (channels != 2) {
    error = "the current real-time backend requires stereo";
    return false;
  }
  RtAudio::StreamParameters input;
  input.deviceId =
      device_id == 0 ? impl_->audio.getDefaultInputDevice() : device_id;
  const auto device_info = impl_->audio.getDeviceInfo(input.deviceId);
  if (device_info.inputChannels == 0) {
    error = "selected device has no input channels";
    return false;
  }
  if (first_channel >= device_info.inputChannels) {
    error = "selected input channel is outside the device's channel range";
    return false;
  }
  if (!supports_sample_rate(device_info, sample_rate)) {
    error = "selected audio input does not report support for " +
            std::to_string(sample_rate) + " Hz";
    return false;
  }
  impl_->input_channels =
      std::min(channels, device_info.inputChannels - first_channel);
  input.nChannels = impl_->input_channels;
  input.firstChannel = first_channel;
  RtAudio::StreamOptions options;
  options.flags = RTAUDIO_MINIMIZE_LATENCY;
  impl_->sender = nullptr;
  impl_->input_handler = std::move(handler);
  if (impl_->audio.openStream(nullptr, &input, RTAUDIO_FLOAT32, sample_rate,
                              &buffer_frames, &Impl::input_callback,
                              impl_.get(), &options) != RTAUDIO_NO_ERROR) {
#ifdef __APPLE__
    error =
        "Unable to open the selected audio input. Grant LanAudio microphone "
        "access in System Settings > Privacy & Security > Microphone, then "
        "try again. Core Audio: " +
        impl_->audio.getErrorText();
#else
    error = "Unable to open the selected audio input: " +
            impl_->audio.getErrorText();
#endif
    impl_->input_handler = {};
    return false;
  }
  if (impl_->audio.startStream() != RTAUDIO_NO_ERROR) {
    error = impl_->audio.getErrorText();
    impl_->input_handler = {};
    return false;
  }
  return true;
#else
  (void)handler;
  (void)device_id;
  (void)first_channel;
  (void)sample_rate;
  (void)channels;
  (void)buffer_frames;
  error = "this build does not include RtAudio";
  return false;
#endif
}

bool RtAudioBackend::start_output(OutputHandler handler,
                                  std::uint32_t device_id,
                                  std::uint32_t first_channel,
                                  std::uint32_t sample_rate,
                                  std::uint32_t channels,
                                  std::uint32_t& buffer_frames,
                                  std::string& error) {
#ifdef LANAUDIO_HAS_RTAUDIO
  impl_->output_handler = std::move(handler);
  NetworkReceiver unused_receiver({});
  // Opening through the existing overload reuses the Core Audio error path.
  // A dummy receiver is not started; the callback uses output_handler.
  (void)unused_receiver;
  RtAudio::StreamParameters output;
  output.deviceId =
      device_id == 0 ? impl_->audio.getDefaultOutputDevice() : device_id;
  const auto device_info = impl_->audio.getDeviceInfo(output.deviceId);
  if (channels != 2) {
    error = "the current real-time backend requires stereo";
    impl_->output_handler = {};
    return false;
  }
  if (first_channel + channels > device_info.outputChannels) {
    error = "selected output channel pair is outside the device's channel "
            "range";
    impl_->output_handler = {};
    return false;
  }
  if (!supports_sample_rate(device_info, sample_rate)) {
    error = "selected audio output does not report support for " +
            std::to_string(sample_rate) + " Hz";
    impl_->output_handler = {};
    return false;
  }
  output.nChannels = channels;
  output.firstChannel = first_channel;
  RtAudio::StreamOptions options;
  options.flags = RTAUDIO_MINIMIZE_LATENCY;
  impl_->receiver = nullptr;
  if (impl_->audio.openStream(&output, nullptr, RTAUDIO_FLOAT32, sample_rate,
                              &buffer_frames, &Impl::output_callback,
                              impl_.get(), &options) != RTAUDIO_NO_ERROR) {
    error = "Unable to open the selected audio output: " +
            impl_->audio.getErrorText();
    impl_->output_handler = {};
    return false;
  }
  if (impl_->audio.startStream() != RTAUDIO_NO_ERROR) {
    error = impl_->audio.getErrorText();
    impl_->output_handler = {};
    return false;
  }
  return true;
#else
  (void)handler;
  (void)device_id;
  (void)first_channel;
  (void)sample_rate;
  (void)channels;
  (void)buffer_frames;
  error = "this build does not include RtAudio";
  return false;
#endif
}

void RtAudioBackend::stop() noexcept {
#ifdef LANAUDIO_HAS_RTAUDIO
  if (impl_->audio.isStreamRunning()) impl_->audio.stopStream();
  if (impl_->audio.isStreamOpen()) impl_->audio.closeStream();
  impl_->sender = nullptr;
  impl_->receiver = nullptr;
  impl_->input_handler = {};
  impl_->output_handler = {};
#endif
}

}  // namespace lanaudio
