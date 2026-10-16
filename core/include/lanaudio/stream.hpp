#pragma once

#include "lanaudio/core.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <thread>

namespace lanaudio {

struct StreamConfig {
  AudioFormat format{};
  std::string host{"127.0.0.1"};
  std::uint16_t port{5004};
  // 0 means "not negotiated". A secure session always assigns a random SSRC.
  std::uint32_t ssrc{};
  std::uint16_t jitter_packets{4};
  std::uint8_t payload_type{kDefaultPayloadType};
  std::uint64_t session_id{};

  // Fail-closed switch. When true, the media path refuses to start without a
  // protector, so an authentication failure can never downgrade to plaintext.
  bool require_protection{true};
  MediaProtector* protector{nullptr};

  // When valid, the receiver only accepts datagrams from this address.
  Endpoint expected_peer{};
  bool restrict_to_peer{true};
};

class NetworkSender {
 public:
  explicit NetworkSender(StreamConfig config);
  ~NetworkSender();
  NetworkSender(const NetworkSender&) = delete;
  NetworkSender& operator=(const NetworkSender&) = delete;

  bool start(std::string& error);
  void stop() noexcept;
  void push_audio(std::span<const float> interleaved) noexcept;
  [[nodiscard]] const StreamMetrics& metrics() const noexcept { return metrics_; }

 private:
  struct AudioBlock {
    std::array<float, 512> samples{};
    std::uint16_t count{};
  };
  void run() noexcept;

  StreamConfig config_;
  UdpSocket socket_;
  SpscRing<AudioBlock, 128> queue_;
  StreamMetrics metrics_;
  std::thread thread_;
  std::atomic<bool> running_{false};
};

class NetworkReceiver {
 public:
  explicit NetworkReceiver(StreamConfig config);
  ~NetworkReceiver();
  NetworkReceiver(const NetworkReceiver&) = delete;
  NetworkReceiver& operator=(const NetworkReceiver&) = delete;

  bool start(std::string& error);
  void stop() noexcept;
  void render_audio(std::span<float> interleaved) noexcept;
  void set_muted(bool muted) noexcept {
    muted_.store(muted, std::memory_order_relaxed);
  }
  [[nodiscard]] bool muted() const noexcept {
    return muted_.load(std::memory_order_relaxed);
  }

  // Fail-closed teardown used when authentication is lost or the session
  // expires. Output is zero-filled immediately; buffered audio is discarded.
  void expire_session() noexcept;
  [[nodiscard]] bool session_valid() const noexcept {
    return session_valid_.load(std::memory_order_acquire);
  }
  [[nodiscard]] const StreamMetrics& metrics() const noexcept { return metrics_; }

 private:
  static constexpr std::size_t kResumeFadeFrames = 64;
  struct AudioBlock {
    std::array<float, kMaxPayloadBytes / sizeof(std::int16_t)> samples{};
    std::uint16_t count{};
  };
  void run() noexcept;
  bool next_source_frame(std::array<float, 2>& frame) noexcept;
  void drain_queue() noexcept;

  StreamConfig config_;
  UdpSocket socket_;
  JitterBuffer jitter_;
  SpscRing<AudioBlock, 128> queue_;
  StreamMetrics metrics_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> session_valid_{false};
  AudioBlock current_{};
  std::size_t current_offset_{};
  DriftController drift_;
  std::array<float, 2> frame_a_{};
  std::array<float, 2> frame_b_{};
  double resample_phase_{};
  bool resampler_primed_{};
  std::size_t fade_in_remaining_{};
  std::atomic<bool> muted_{false};
  Endpoint peer_{};
};

}  // namespace lanaudio
