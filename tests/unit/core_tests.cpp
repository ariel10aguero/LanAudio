#include "lanaudio/core.hpp"
#include "lanaudio/stream.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <span>
#include <thread>

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

lanaudio::RtpPacket packet(std::uint16_t sequence) {
  lanaudio::RtpPacket value;
  value.header.sequence = sequence;
  value.header.timestamp = static_cast<std::uint32_t>(sequence) * 240;
  value.header.ssrc = 42;
  value.payload_size = 4;
  value.payload[0] = std::byte{1};
  return value;
}

void test_rtp_round_trip() {
  auto source = packet(65'535);
  source.header.payload_type = 96;
  source.header.timestamp = 0xfffffff0U;
  std::array<std::byte, lanaudio::kMaxPacketBytes> wire{};
  const auto size = lanaudio::serialize_rtp(source, wire);
  CHECK(size == 16);
  CHECK(wire[0] == std::byte{0x80});
  CHECK(wire[1] == std::byte{0x60});
  CHECK(wire[2] == std::byte{0xff} && wire[3] == std::byte{0xff});
  CHECK(wire[4] == std::byte{0xff} && wire[5] == std::byte{0xff} &&
        wire[6] == std::byte{0xff} && wire[7] == std::byte{0xf0});
  CHECK(wire[8] == std::byte{0x00} && wire[9] == std::byte{0x00} &&
        wire[10] == std::byte{0x00} && wire[11] == std::byte{0x2a});
  lanaudio::RtpPacket decoded;
  CHECK(lanaudio::parse_rtp(
            std::span<const std::byte>(wire.data(), size), 96, decoded) ==
        lanaudio::ParseError::none);
  CHECK(decoded.header.sequence == source.header.sequence);
  CHECK(decoded.header.timestamp == source.header.timestamp);
  CHECK(decoded.header.ssrc == source.header.ssrc);
  CHECK(decoded.payload_size == source.payload_size);
}

void test_pcm_conversion() {
  const std::array<float, 5> input{-1.0F, -0.5F, 0.0F, 0.5F, 1.0F};
  std::array<std::byte, 10> encoded{};
  std::array<float, 5> output{};
  lanaudio::float_to_l16be(input, encoded);
  CHECK(encoded[0] == std::byte{0x80} && encoded[1] == std::byte{0x00});
  CHECK(encoded[8] == std::byte{0x7f} && encoded[9] == std::byte{0xff});
  lanaudio::l16be_to_float(encoded, output);
  for (std::size_t i = 0; i < input.size(); ++i) {
    CHECK(std::abs(input[i] - output[i]) < 0.0001F);
  }
}

void test_packetizer_callback_independence() {
  lanaudio::Packetizer packetizer(lanaudio::AudioFormat{}, 7);
  std::array<float, 128> callback{};
  std::size_t packets = 0;
  for (int callback_index = 0; callback_index < 15; ++callback_index) {
    std::size_t offset = 0;
    while (offset < callback.size()) {
      lanaudio::RtpPacket ready;
      bool completed = false;
      offset += packetizer.push(
          std::span<const float>(callback.data() + offset,
                                 callback.size() - offset),
          ready, completed);
      if (completed) {
        CHECK(ready.payload_size == 960);
        ++packets;
      }
    }
  }
  CHECK(packets == 4);
}

void test_jitter_reordering_and_wrap() {
  lanaudio::JitterBuffer buffer(2);
  CHECK(buffer.insert(packet(65'535)) ==
        lanaudio::JitterBuffer::InsertResult::inserted);
  CHECK(buffer.insert(packet(65'534)) ==
        lanaudio::JitterBuffer::InsertResult::inserted);
  CHECK(buffer.insert(packet(0)) ==
        lanaudio::JitterBuffer::InsertResult::inserted);
  lanaudio::RtpPacket result;
  CHECK(buffer.pop(result) == lanaudio::JitterBuffer::PopResult::packet &&
        result.header.sequence == 65'534);
  CHECK(buffer.pop(result) == lanaudio::JitterBuffer::PopResult::packet &&
        result.header.sequence == 65'535);
  CHECK(buffer.pop(result) == lanaudio::JitterBuffer::PopResult::packet &&
        result.header.sequence == 0);

  buffer.reset();
  CHECK(buffer.insert(packet(10)) ==
        lanaudio::JitterBuffer::InsertResult::inserted);
  CHECK(buffer.insert(packet(12)) ==
        lanaudio::JitterBuffer::InsertResult::inserted);
  CHECK(buffer.pop(result) == lanaudio::JitterBuffer::PopResult::packet &&
        result.header.sequence == 10);
  // 11 never arrived but 12 is buffered, so the gap is concealed at once.
  CHECK(buffer.pop(result) == lanaudio::JitterBuffer::PopResult::missing);
  CHECK(buffer.pop(result) == lanaudio::JitterBuffer::PopResult::packet &&
        result.header.sequence == 12);
  CHECK(buffer.pop(result) == lanaudio::JitterBuffer::PopResult::waiting);
  CHECK(buffer.insert(packet(13)) ==
        lanaudio::JitterBuffer::InsertResult::inserted);
  CHECK(buffer.pop(result) == lanaudio::JitterBuffer::PopResult::packet &&
        result.header.sequence == 13);
}

// Regression test for the dropout that silenced the stream after about one
// second: the read cursor used to advance on occupancy alone, ran ahead of
// the sender, and then rejected every arriving packet as too late.
void test_jitter_cursor_does_not_run_ahead() {
  lanaudio::JitterBuffer buffer(2);
  lanaudio::RtpPacket result;
  CHECK(buffer.insert(packet(100)) ==
        lanaudio::JitterBuffer::InsertResult::inserted);
  CHECK(buffer.insert(packet(101)) ==
        lanaudio::JitterBuffer::InsertResult::inserted);
  CHECK(buffer.pop(result) == lanaudio::JitterBuffer::PopResult::packet);
  CHECK(buffer.pop(result) == lanaudio::JitterBuffer::PopResult::packet);

  // A silent gap must not move the cursor, no matter how long it lasts.
  for (int i = 0; i < 1'000; ++i) {
    CHECK(buffer.pop(result) == lanaudio::JitterBuffer::PopResult::waiting);
  }
  CHECK(buffer.insert(packet(102)) ==
        lanaudio::JitterBuffer::InsertResult::inserted);
  CHECK(buffer.pop(result) == lanaudio::JitterBuffer::PopResult::packet &&
        result.header.sequence == 102);
  CHECK(buffer.resyncs() == 0);
}

void test_jitter_resynchronizes_after_sustained_offset() {
  lanaudio::JitterBuffer buffer(2);
  lanaudio::RtpPacket result;
  for (std::uint16_t sequence = 0; sequence < 4; ++sequence) {
    CHECK(buffer.insert(packet(sequence)) ==
          lanaudio::JitterBuffer::InsertResult::inserted);
  }
  while (buffer.pop(result) == lanaudio::JitterBuffer::PopResult::packet) {
  }

  // The peer restarts its sequence numbering far outside the window. The
  // buffer tolerates a few strays, then rebuilds around the live stream.
  const std::uint16_t restart = 40'000;
  for (std::uint16_t offset = 0;
       offset < lanaudio::JitterBuffer::kResyncThreshold; ++offset) {
    (void)buffer.insert(packet(static_cast<std::uint16_t>(restart + offset)));
  }
  CHECK(buffer.resyncs() == 1);
  CHECK(buffer.insert(packet(static_cast<std::uint16_t>(
            restart + lanaudio::JitterBuffer::kResyncThreshold))) ==
        lanaudio::JitterBuffer::InsertResult::inserted);
  CHECK(buffer.pop(result) == lanaudio::JitterBuffer::PopResult::packet);
}

void test_spsc_boundaries() {
  lanaudio::SpscRing<int, 4> queue;
  CHECK(queue.try_push(1));
  CHECK(queue.try_push(2));
  CHECK(queue.try_push(3));
  CHECK(!queue.try_push(4));
  int value = 0;
  CHECK(queue.try_pop(value) && value == 1);
  CHECK(queue.try_pop(value) && value == 2);
  CHECK(queue.try_pop(value) && value == 3);
  CHECK(!queue.try_pop(value));
}

void test_drift_controller() {
  for (const double device_drift : {-200.0, -100.0, -50.0, 50.0, 100.0,
                                    200.0}) {
    lanaudio::DriftController::Config config;
    config.target_packets = 4.0;
    lanaudio::DriftController controller(config);
    double occupancy = config.target_packets;
    for (int i = 0; i < 500'000; ++i) {
      const double ratio = controller.update(occupancy);
      const double correction = (ratio - 1.0) * 1.0e6;
      // Model one 64-frame callback. Occupancy is expressed in 240-frame
      // packets, so ppm rate error is scaled by 64 / 240 per callback.
      occupancy +=
          (device_drift - correction) * 64.0 / (1.0e6 * 240.0);
    }
    CHECK(std::abs(controller.correction_ppm() - device_drift) < 5.0);
    CHECK(std::abs(occupancy - config.target_packets) < 0.25);
  }
}

// Stand-in for SRTP so the core tests stay free of libsrtp while still
// exercising the protected media path and its failure accounting. It appends
// a keyed tag and rejects replayed sequence numbers.
class TestProtector final : public lanaudio::MediaProtector {
 public:
  static constexpr std::size_t kTagBytes = 8;

  [[nodiscard]] std::size_t protect(std::span<std::byte> buffer,
                                    std::size_t size) noexcept override {
    if (size == 0 || buffer.size() - size < kTagBytes) return 0;
    for (std::size_t i = 0; i < kTagBytes; ++i) {
      buffer[size + i] = std::byte{0xA5} ^ buffer[i];
    }
    return size + kTagBytes;
  }

  [[nodiscard]] std::size_t unprotect(std::span<std::byte> buffer,
                                      std::size_t size) noexcept override {
    replay_ = false;
    if (size <= kTagBytes) return 0;
    const auto plain = size - kTagBytes;
    for (std::size_t i = 0; i < kTagBytes; ++i) {
      if (buffer[plain + i] != (std::byte{0xA5} ^ buffer[i])) return 0;
    }
    const auto sequence =
        static_cast<std::uint16_t>((std::to_integer<unsigned>(buffer[2]) << 8) |
                                   std::to_integer<unsigned>(buffer[3]));
    if (seen_[sequence % seen_.size()] == sequence && accepted_ > 0) {
      replay_ = true;
      return 0;
    }
    seen_[sequence % seen_.size()] = sequence;
    ++accepted_;
    return plain;
  }

  [[nodiscard]] bool last_failure_was_replay() const noexcept override {
    return replay_;
  }
  [[nodiscard]] const char* profile_name() const noexcept override {
    return "TEST_TAG_ONLY";
  }

 private:
  std::array<std::uint16_t, 256> seen_{};
  std::uint64_t accepted_{};
  bool replay_{};
};

lanaudio::StreamConfig protected_config(std::uint16_t port,
                                        lanaudio::MediaProtector* protector) {
  lanaudio::StreamConfig config;
  // Do not contend with a real LanAudio receiver using the user-facing port.
  config.port = port;
  config.jitter_packets = 2;
  config.ssrc = 0x4C414E41U;
  config.session_id = 0x5345535349u;
  config.protector = protector;
  config.require_protection = true;
  config.restrict_to_peer = false;
  return config;
}

void test_media_requires_protection() {
  lanaudio::StreamConfig config = protected_config(45'006, nullptr);
  lanaudio::NetworkSender sender(config);
  lanaudio::NetworkReceiver receiver(config);
  std::string error;
  CHECK(!sender.start(error));
  CHECK(error.find("SRTP") != std::string::npos);
  error.clear();
  CHECK(!receiver.start(error));

  // A negotiated session identifier and SSRC are equally mandatory.
  TestProtector protector;
  lanaudio::StreamConfig unnegotiated = protected_config(45'006, &protector);
  unnegotiated.ssrc = 0;
  lanaudio::NetworkSender unnegotiated_sender(unnegotiated);
  error.clear();
  CHECK(!unnegotiated_sender.start(error));
}

void test_localhost_stream() {
  TestProtector outbound;
  TestProtector inbound;
  lanaudio::StreamConfig config = protected_config(45'004, nullptr);
  auto receiver_config = config;
  receiver_config.protector = &inbound;
  auto sender_config = config;
  sender_config.protector = &outbound;
  lanaudio::NetworkReceiver receiver(receiver_config);
  lanaudio::NetworkSender sender(sender_config);
  std::string error;
  CHECK(receiver.start(error));
  CHECK(sender.start(error));
  std::array<float, 480 * 6> audio;
  audio.fill(0.25F);
  sender.push_audio(audio);
  for (int i = 0; i < 50 && receiver.metrics().packets_received.load() < 6;
       ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  std::array<float, 480 * 4> output{};
  receiver.render_audio(output);
  CHECK(sender.metrics().packets_sent.load() == 6);
  CHECK(receiver.metrics().packets_received.load() == 6);
  CHECK(output[0] > 0.24F && output[0] < 0.26F);
  CHECK(sender.metrics().input_level_milli.load() >= 249);
  CHECK(receiver.metrics().output_level_milli.load() >= 249);
  receiver.set_muted(true);
  std::array<float, 480> muted_output{};
  receiver.render_audio(muted_output);
  CHECK(std::all_of(muted_output.begin(), muted_output.end(),
                    [](float sample) { return sample == 0.0F; }));
  CHECK(receiver.metrics().output_level_milli.load() >= 249);
  CHECK(receiver.metrics().decrypt_failures.load() == 0);
  CHECK(receiver.metrics().replay_rejections.load() == 0);

  // Fail closed: an expired session plays silence even while the device
  // callback keeps running.
  receiver.set_muted(false);
  receiver.expire_session();
  CHECK(!receiver.session_valid());
  std::array<float, 480> expired{};
  expired.fill(0.5F);
  receiver.render_audio(expired);
  CHECK(std::all_of(expired.begin(), expired.end(),
                    [](float sample) { return sample == 0.0F; }));
  sender.stop();
  receiver.stop();
}

}  // namespace

int main() {
  test_rtp_round_trip();
  test_pcm_conversion();
  test_packetizer_callback_independence();
  test_jitter_reordering_and_wrap();
  test_jitter_cursor_does_not_run_ahead();
  test_jitter_resynchronizes_after_sustained_offset();
  test_spsc_boundaries();
  test_drift_controller();
  test_media_requires_protection();
  test_localhost_stream();
  if (failures == 0) std::cout << "all LanAudio core tests passed\n";
  return failures == 0 ? 0 : 1;
}
