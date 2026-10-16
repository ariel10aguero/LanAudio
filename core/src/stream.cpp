#include "lanaudio/stream.hpp"

#include "lanaudio/protocol/guard.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>

#ifdef __APPLE__
#include <pthread/qos.h>
#endif

namespace lanaudio {
namespace {

std::uint32_t level_milli(std::span<const float> samples) noexcept {
  float peak = 0.0F;
  for (const float sample : samples) {
    peak = std::max(peak, std::abs(sample));
  }
  return static_cast<std::uint32_t>(
      std::clamp(peak, 0.0F, 1.0F) * 1'000.0F);
}

// Media threads wake every few milliseconds. Without an interactive QoS class
// macOS coalesces their timers (and App Nap throttles them further), which
// turns a steady packet stream into bursts the receiver cannot absorb.
void promote_media_thread() noexcept {
#ifdef __APPLE__
  (void)pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

std::uint64_t monotonic_micros() noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

bool validate_session(const StreamConfig& config, std::string& error) {
  if (!config.format.valid()) {
    error = "invalid audio format";
    return false;
  }
  if (config.format.channels != 2) {
    error = "the media path supports stereo network audio only";
    return false;
  }
  if (config.require_protection && config.protector == nullptr) {
    error =
        "refusing to start unprotected media: no SRTP context was supplied by "
        "the authenticated control session";
    return false;
  }
  if (config.require_protection &&
      (config.ssrc == 0 || config.session_id == 0)) {
    error =
        "refusing to start media without a negotiated session identifier and "
        "SSRC";
    return false;
  }
  if (config.payload_type < 96 || config.payload_type > 127) {
    error = "negotiated RTP payload type is outside the dynamic range";
    return false;
  }
  return true;
}

}  // namespace

NetworkSender::NetworkSender(StreamConfig config) : config_(std::move(config)) {}
NetworkSender::~NetworkSender() { stop(); }

bool NetworkSender::start(std::string& error) {
  if (!validate_session(config_, error)) return false;
  if (!socket_.set_destination(config_.host, config_.port, error)) return false;
  if (running_.exchange(true)) return true;
  metrics_.session_active.store(true, std::memory_order_release);
  thread_ = std::thread([this] { run(); });
  return true;
}

void NetworkSender::stop() noexcept {
  running_.store(false, std::memory_order_release);
  if (thread_.joinable()) thread_.join();
  metrics_.session_active.store(false, std::memory_order_release);
}

void NetworkSender::push_audio(std::span<const float> interleaved) noexcept {
  metrics_.input_level_milli.store(level_milli(interleaved),
                                   std::memory_order_relaxed);
  metrics_.media_callbacks.fetch_add(1, std::memory_order_relaxed);
  while (!interleaved.empty()) {
    AudioBlock block;
    block.count = static_cast<std::uint16_t>(
        std::min(interleaved.size(), block.samples.size()));
    std::copy_n(interleaved.begin(), block.count, block.samples.begin());
    if (!queue_.try_push(block)) {
      metrics_.overruns.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    interleaved = interleaved.subspan(block.count);
  }
}

void NetworkSender::run() noexcept {
  promote_media_thread();
  Packetizer packetizer(config_.format, config_.ssrc);
  AudioBlock block;
  std::array<std::byte, kMaxProtectedPacketBytes> wire{};
  while (running_.load(std::memory_order_acquire)) {
    if (!queue_.try_pop(block)) {
      std::this_thread::sleep_for(std::chrono::microseconds(250));
      continue;
    }
    std::size_t offset = 0;
    while (offset < block.count) {
      RtpPacket packet;
      bool completed = false;
      offset += packetizer.push(
          std::span<const float>(block.samples.data() + offset,
                                 block.count - offset),
          packet, completed);
      if (!completed) continue;
      packet.header.payload_type = config_.payload_type;
      auto size = serialize_rtp(packet, wire);
      if (size == 0) {
        metrics_.malformed_packets.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      if (config_.protector != nullptr) {
        size = config_.protector->protect(wire, size);
        if (size == 0) {
          // Fail closed: never fall back to emitting the plaintext packet.
          metrics_.decrypt_failures.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
      }
      std::string error;
      if (socket_.send(std::span<const std::byte>(wire.data(), size), error)) {
        metrics_.packets_sent.fetch_add(1, std::memory_order_relaxed);
        metrics_.last_media_micros.store(monotonic_micros(),
                                         std::memory_order_relaxed);
      }
    }
  }
}

NetworkReceiver::NetworkReceiver(StreamConfig config)
    : config_(std::move(config)),
      jitter_(config_.jitter_packets),
      drift_(DriftController::Config{
          static_cast<double>(config_.jitter_packets), 100.0, 0.01, 0.02,
          250.0}) {}
NetworkReceiver::~NetworkReceiver() { stop(); }

bool NetworkReceiver::start(std::string& error) {
  if (!validate_session(config_, error)) return false;
  if (!socket_.bind(config_.port, error)) return false;
  if (running_.exchange(true)) return true;
  peer_ = config_.expected_peer;
  session_valid_.store(true, std::memory_order_release);
  metrics_.session_active.store(true, std::memory_order_release);
  thread_ = std::thread([this] { run(); });
  return true;
}

void NetworkReceiver::stop() noexcept {
  running_.store(false, std::memory_order_release);
  session_valid_.store(false, std::memory_order_release);
  if (thread_.joinable()) thread_.join();
  metrics_.session_active.store(false, std::memory_order_release);
}

void NetworkReceiver::expire_session() noexcept {
  // Stop accepting media immediately. render_audio observes this flag and
  // produces silence even if the device callback is still running.
  session_valid_.store(false, std::memory_order_release);
  metrics_.session_active.store(false, std::memory_order_release);
}

void NetworkReceiver::drain_queue() noexcept {
  AudioBlock discarded;
  while (queue_.try_pop(discarded)) {
  }
  current_ = AudioBlock{};
  current_offset_ = 0;
  resampler_primed_ = false;
  resample_phase_ = 0.0;
  fade_in_remaining_ = 0;
  frames_above_high_water_ = 0;
  playback_started_ = false;
}

void NetworkReceiver::run() noexcept {
  promote_media_thread();
  std::array<std::byte, kMaxProtectedPacketBytes> wire{};
  AudioBlock last_block{};
  bool have_audio = false;
  std::size_t consecutive_losses = 0;
  protocol::AbuseGuard guard;

  while (running_.load(std::memory_order_acquire)) {
    std::string error;
    Endpoint source;
    const auto count = socket_.receive_from(
        wire, std::chrono::milliseconds(20), source, error);
    if (count <= 0) continue;
    if (!session_valid_.load(std::memory_order_acquire)) continue;

    const auto now = std::chrono::steady_clock::now();
    if (!guard.accept(now)) {
      metrics_.rate_limited_packets.fetch_add(1, std::memory_order_relaxed);
      continue;
    }

    // Media is bound to the authenticated peer. The first accepted datagram
    // pins the address when the control channel did not supply one.
    if (config_.restrict_to_peer) {
      if (peer_.valid()) {
        if (!peer_.same_address(source)) {
          metrics_.foreign_source_rejections.fetch_add(
              1, std::memory_order_relaxed);
          if (guard.record_invalid(now)) {
            metrics_.rate_limited_packets.fetch_add(1,
                                                    std::memory_order_relaxed);
          }
          continue;
        }
      }
    }

    auto size = static_cast<std::size_t>(count);
    if (config_.protector != nullptr) {
      const auto plain = config_.protector->unprotect(wire, size);
      if (plain == 0) {
        if (config_.protector->last_failure_was_replay()) {
          metrics_.replay_rejections.fetch_add(1, std::memory_order_relaxed);
        } else {
          metrics_.decrypt_failures.fetch_add(1, std::memory_order_relaxed);
        }
        if (guard.record_invalid(now)) {
          metrics_.rate_limited_packets.fetch_add(1, std::memory_order_relaxed);
        }
        continue;
      }
      size = plain;
    }

    RtpPacket incoming;
    if (parse_rtp(std::span<const std::byte>(wire.data(), size),
                  config_.payload_type, incoming) != ParseError::none ||
        incoming.header.ssrc != config_.ssrc ||
        incoming.payload_size != config_.format.payload_bytes()) {
      metrics_.malformed_packets.fetch_add(1, std::memory_order_relaxed);
      if (guard.record_invalid(now)) {
        metrics_.rate_limited_packets.fetch_add(1, std::memory_order_relaxed);
      }
      continue;
    }

    guard.record_valid();
    if (config_.restrict_to_peer && !peer_.valid()) peer_ = source;
    metrics_.packets_received.fetch_add(1, std::memory_order_relaxed);
    metrics_.last_media_micros.store(monotonic_micros(),
                                     std::memory_order_relaxed);

    const auto result = jitter_.insert(incoming);
    if (result == JitterBuffer::InsertResult::too_late) {
      metrics_.late_packets.fetch_add(1, std::memory_order_relaxed);
    } else if (result == JitterBuffer::InsertResult::overrun) {
      metrics_.overruns.fetch_add(1, std::memory_order_relaxed);
    }
    metrics_.stream_resyncs.store(jitter_.resyncs(), std::memory_order_relaxed);
    metrics_.jitter_buffer_packets.store(
        static_cast<std::uint32_t>(jitter_.size()), std::memory_order_relaxed);

    RtpPacket ready;
    for (;;) {
      const auto pop_result = jitter_.pop(ready);
      if (pop_result == JitterBuffer::PopResult::waiting) break;
      if (pop_result == JitterBuffer::PopResult::missing) {
        metrics_.lost_packets.fetch_add(1, std::memory_order_relaxed);
        // Repeat the last good packet with a decaying gain. For sustained
        // tones this is far less audible than a 5 ms gap of silence, and
        // after kMaxConcealedPackets the output settles to silence.
        AudioBlock concealed;
        concealed.count =
            static_cast<std::uint16_t>(config_.format.samples_per_packet());
        concealed.samples.fill(0.0F);
        if (have_audio && consecutive_losses < kMaxConcealedPackets) {
          const auto frames = std::min<std::size_t>(
              config_.format.frames_per_packet, last_block.count / 2);
          const float start =
              1.0F - static_cast<float>(consecutive_losses) /
                         static_cast<float>(kMaxConcealedPackets);
          const float end =
              1.0F - static_cast<float>(consecutive_losses + 1) /
                         static_cast<float>(kMaxConcealedPackets);
          for (std::size_t frame = 0; frame < frames; ++frame) {
            const float gain =
                start + (end - start) * static_cast<float>(frame) /
                            static_cast<float>(frames);
            concealed.samples[frame * 2] = last_block.samples[frame * 2] * gain;
            concealed.samples[frame * 2 + 1] =
                last_block.samples[frame * 2 + 1] * gain;
          }
        }
        ++consecutive_losses;
        if (!queue_.try_push(concealed)) {
          // The playback queue is full; stop draining so the network thread
          // returns to reading sockets instead of spinning on a full queue.
          metrics_.overruns.fetch_add(1, std::memory_order_relaxed);
          break;
        }
        continue;
      }
      AudioBlock block;
      block.count = static_cast<std::uint16_t>(ready.payload_size / 2);
      l16be_to_float(
          std::span<const std::byte>(ready.payload.data(), ready.payload_size),
          std::span<float>(block.samples.data(), block.count));
      if (consecutive_losses != 0 && have_audio) {
        // Crossfade out of the concealment so the real packet does not
        // start with a step discontinuity.
        const float tail =
            consecutive_losses >= kMaxConcealedPackets
                ? 0.0F
                : 1.0F - static_cast<float>(consecutive_losses) /
                             static_cast<float>(kMaxConcealedPackets);
        const auto frames = std::min<std::size_t>(
            kConcealCrossfadeFrames,
            std::min<std::size_t>(block.count, last_block.count) / 2);
        for (std::size_t frame = 0; frame < frames; ++frame) {
          const float w = static_cast<float>(frame + 1) /
                          static_cast<float>(frames + 1);
          for (std::size_t ch = 0; ch < 2; ++ch) {
            const auto i = frame * 2 + ch;
            block.samples[i] =
                block.samples[i] * w + last_block.samples[i] * tail * (1.0F - w);
          }
        }
      }
      consecutive_losses = 0;
      if (block.count >= 2) {
        last_block = block;
        have_audio = true;
      }
      if (!queue_.try_push(block)) {
        metrics_.overruns.fetch_add(1, std::memory_order_relaxed);
        break;
      }
    }
  }
}

bool NetworkReceiver::next_source_frame(
    std::array<float, 2>& frame) noexcept {
  if (config_.format.channels != 2) return false;
  if (current_offset_ + 2 > current_.count) {
    if (!queue_.try_pop(current_)) return false;
    current_offset_ = 0;
  }
  frame[0] = current_.samples[current_offset_];
  frame[1] = current_.samples[current_offset_ + 1];
  current_offset_ += 2;
  return true;
}

void NetworkReceiver::render_audio(std::span<float> interleaved) noexcept {
  metrics_.media_callbacks.fetch_add(1, std::memory_order_relaxed);
  auto finalize_output = [this, interleaved]() noexcept {
    metrics_.output_level_milli.store(level_milli(interleaved),
                                      std::memory_order_relaxed);
    if (muted_.load(std::memory_order_relaxed)) {
      std::fill(interleaved.begin(), interleaved.end(), 0.0F);
    }
  };

  // Fail closed: an expired or unauthenticated session plays silence.
  if (!session_valid_.load(std::memory_order_acquire)) {
    std::fill(interleaved.begin(), interleaved.end(), 0.0F);
    if (current_.count != 0 || current_offset_ != 0) drain_queue();
    metrics_.output_level_milli.store(0, std::memory_order_relaxed);
    return;
  }
  if (config_.format.channels != 2) {
    std::fill(interleaved.begin(), interleaved.end(), 0.0F);
    metrics_.underruns.fetch_add(1, std::memory_order_relaxed);
    finalize_output();
    return;
  }
  // Playout cushion. The jitter buffer only enforces its startup depth once;
  // the playback queue is what actually absorbs network and device-callback
  // jitter, so the same depth is enforced here every time playback (re)starts.
  const auto target_blocks =
      std::max<std::size_t>(config_.jitter_packets, 1);
  const auto high_water_blocks =
      target_blocks + std::max<std::size_t>(target_blocks, 8);

  if (!resampler_primed_) {
    // Wait for a full cushion instead of resuming on the first packet.
    // Resuming at zero depth makes every later scheduling hiccup another
    // underrun, which is heard as a constant ticking.
    if (queue_.size_approx() < target_blocks) {
      std::fill(interleaved.begin(), interleaved.end(), 0.0F);
      finalize_output();
      return;
    }
  }

  // Bound latency, but only when the excess persists for a second. Wi-Fi
  // delivers audio in bursts after a stall, and that burst is exactly the
  // audio needed to ride out the next stall; trimming it immediately turned
  // every burst into an underrun.
  if (queue_.size_approx() > high_water_blocks) {
    frames_above_high_water_ += interleaved.size() / 2;
  } else {
    frames_above_high_water_ = 0;
  }
  // Audio that piled up before the output device first started is stale, so
  // that backlog is trimmed immediately.
  if (!playback_started_ && queue_.size_approx() > high_water_blocks) {
    frames_above_high_water_ = config_.format.sample_rate;
  }
  if (frames_above_high_water_ >= config_.format.sample_rate) {
    frames_above_high_water_ = 0;
    AudioBlock discarded;
    while (queue_.size_approx() > target_blocks && queue_.try_pop(discarded)) {
    }
    metrics_.overruns.fetch_add(1, std::memory_order_relaxed);
    drift_.reset();
  }

  if (!resampler_primed_) {
    if (!next_source_frame(frame_a_) || !next_source_frame(frame_b_)) {
      std::fill(interleaved.begin(), interleaved.end(), 0.0F);
      finalize_output();
      return;
    }
    resampler_primed_ = true;
    playback_started_ = true;
    // A fresh cushion invalidates the controller's integral state.
    drift_.reset();
  }

  // Include the partially consumed block so the controller sees a smooth
  // occupancy rather than a sawtooth that jumps by a whole packet.
  const auto samples_per_packet = config_.format.samples_per_packet();
  const auto current_remaining =
      current_.count > current_offset_ ? current_.count - current_offset_ : 0;
  const double occupancy =
      static_cast<double>(queue_.size_approx()) +
      static_cast<double>(current_remaining) /
          static_cast<double>(samples_per_packet);
  const double ratio = drift_.update(occupancy);
  metrics_.drift_ppm.store(
      static_cast<std::int32_t>(drift_.correction_ppm()),
      std::memory_order_relaxed);

  std::size_t output_offset = 0;
  while (output_offset + 1 < interleaved.size()) {
    const auto phase = static_cast<float>(resample_phase_);
    float gain = 1.0F;
    if (fade_in_remaining_ > 0) {
      gain = static_cast<float>(kResumeFadeFrames - fade_in_remaining_) /
             static_cast<float>(kResumeFadeFrames);
      --fade_in_remaining_;
    }
    interleaved[output_offset] =
        (frame_a_[0] + (frame_b_[0] - frame_a_[0]) * phase) * gain;
    interleaved[output_offset + 1] =
        (frame_a_[1] + (frame_b_[1] - frame_a_[1]) * phase) * gain;
    output_offset += 2;
    resample_phase_ += ratio;
    while (resample_phase_ >= 1.0) {
      frame_a_ = frame_b_;
      if (!next_source_frame(frame_b_)) {
        const auto frames_left = (interleaved.size() - output_offset) / 2;
        for (std::size_t i = 0; i < frames_left; ++i) {
          const float gain =
              i < 16 ? static_cast<float>(16 - i) / 16.0F : 0.0F;
          interleaved[output_offset + i * 2] = frame_a_[0] * gain;
          interleaved[output_offset + i * 2 + 1] = frame_a_[1] * gain;
        }
        if ((interleaved.size() & 1U) != 0) interleaved.back() = 0.0F;
        resampler_primed_ = false;
        resample_phase_ = 0.0;
        // The tail was faded to silence, so ramp back in on resume.
        fade_in_remaining_ = kResumeFadeFrames;
        metrics_.underruns.fetch_add(1, std::memory_order_relaxed);
        finalize_output();
        return;
      }
      resample_phase_ -= 1.0;
    }
  }
  if (output_offset < interleaved.size()) interleaved[output_offset] = 0.0F;
  finalize_output();
}

}  // namespace lanaudio
