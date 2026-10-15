#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lanaudio {

constexpr std::uint32_t kDefaultSampleRate = 48'000;
constexpr std::uint16_t kDefaultChannels = 2;
constexpr std::uint16_t kDefaultFramesPerPacket = 240;
constexpr std::uint8_t kDefaultPayloadType = 96;
constexpr std::size_t kRtpHeaderBytes = 12;
constexpr std::size_t kMaxPayloadBytes = 1'200;
constexpr std::size_t kMaxPacketBytes = kRtpHeaderBytes + kMaxPayloadBytes;

// Headroom for the SRTP authentication tag and any profile-specific trailer.
// Wire buffers are sized for the protected form so encryption never needs a
// second allocation on the media path.
constexpr std::size_t kMediaProtectionOverheadBytes = 16;
constexpr std::size_t kMaxProtectedPacketBytes =
    kMaxPacketBytes + kMediaProtectionOverheadBytes;

struct AudioFormat {
  std::uint32_t sample_rate{kDefaultSampleRate};
  std::uint16_t channels{kDefaultChannels};
  std::uint16_t frames_per_packet{kDefaultFramesPerPacket};

  [[nodiscard]] std::size_t samples_per_packet() const noexcept {
    return static_cast<std::size_t>(channels) * frames_per_packet;
  }
  [[nodiscard]] std::size_t payload_bytes() const noexcept {
    return samples_per_packet() * sizeof(std::int16_t);
  }
  [[nodiscard]] bool valid() const noexcept {
    return sample_rate >= 8'000 && sample_rate <= 192'000 &&
           channels >= 1 && channels <= 2 && frames_per_packet > 0 &&
           payload_bytes() <= kMaxPayloadBytes;
  }
};

struct RtpHeader {
  std::uint8_t payload_type{kDefaultPayloadType};
  bool marker{false};
  std::uint16_t sequence{};
  std::uint32_t timestamp{};
  std::uint32_t ssrc{};
};

struct RtpPacket {
  RtpHeader header;
  std::array<std::byte, kMaxPayloadBytes> payload{};
  std::uint16_t payload_size{};
};

enum class ParseError {
  none,
  too_short,
  wrong_version,
  unsupported_header,
  wrong_payload_type,
  payload_too_large
};

[[nodiscard]] std::size_t serialize_rtp(
    const RtpPacket& packet, std::span<std::byte> destination) noexcept;
[[nodiscard]] ParseError parse_rtp(
    std::span<const std::byte> bytes, std::uint8_t expected_payload_type,
    RtpPacket& packet) noexcept;

void float_to_l16be(std::span<const float> input,
                    std::span<std::byte> output) noexcept;
void l16be_to_float(std::span<const std::byte> input,
                    std::span<float> output) noexcept;

template <typename T, std::size_t Capacity>
class SpscRing {
 public:
  static_assert(Capacity >= 2);

  [[nodiscard]] bool try_push(const T& value) noexcept {
    const auto head = head_.load(std::memory_order_relaxed);
    const auto next = increment(head);
    if (next == tail_.load(std::memory_order_acquire)) return false;
    storage_[head] = value;
    head_.store(next, std::memory_order_release);
    return true;
  }

  [[nodiscard]] bool try_pop(T& value) noexcept {
    const auto tail = tail_.load(std::memory_order_relaxed);
    if (tail == head_.load(std::memory_order_acquire)) return false;
    value = storage_[tail];
    tail_.store(increment(tail), std::memory_order_release);
    return true;
  }

  [[nodiscard]] std::size_t size_approx() const noexcept {
    const auto head = head_.load(std::memory_order_acquire);
    const auto tail = tail_.load(std::memory_order_acquire);
    return head >= tail ? head - tail : Capacity - tail + head;
  }

 private:
  static constexpr std::size_t increment(std::size_t value) noexcept {
    return (value + 1) % Capacity;
  }
  std::array<T, Capacity> storage_{};
  alignas(64) std::atomic<std::size_t> head_{0};
  alignas(64) std::atomic<std::size_t> tail_{0};
};

struct StreamMetrics {
  std::atomic<std::uint64_t> packets_sent{};
  std::atomic<std::uint64_t> packets_received{};
  std::atomic<std::uint64_t> malformed_packets{};
  std::atomic<std::uint64_t> lost_packets{};
  std::atomic<std::uint64_t> reordered_packets{};
  std::atomic<std::uint64_t> late_packets{};
  std::atomic<std::uint64_t> underruns{};
  std::atomic<std::uint64_t> overruns{};
  std::atomic<std::uint32_t> jitter_buffer_packets{};
  std::atomic<std::int32_t> drift_ppm{};
  std::atomic<std::uint32_t> input_level_milli{};
  std::atomic<std::uint32_t> output_level_milli{};
  // Security and health telemetry. None of these expose key material.
  std::atomic<std::uint64_t> decrypt_failures{};
  std::atomic<std::uint64_t> replay_rejections{};
  std::atomic<std::uint64_t> foreign_source_rejections{};
  std::atomic<std::uint64_t> rate_limited_packets{};
  std::atomic<std::uint64_t> stream_resyncs{};
  std::atomic<std::uint64_t> media_callbacks{};
  std::atomic<std::uint64_t> last_media_micros{};
  std::atomic<bool> session_active{};
};

// SRTP (or any future transform) applied on the network thread. The media
// callbacks never call this interface.
class MediaProtector {
 public:
  virtual ~MediaProtector() = default;

  // Transforms a serialized RTP packet in place. `buffer` has at least
  // kMediaProtectionOverheadBytes of spare capacity beyond `size`.
  // Returns the resulting size, or 0 when the packet must be dropped.
  [[nodiscard]] virtual std::size_t protect(std::span<std::byte> buffer,
                                            std::size_t size) noexcept = 0;
  [[nodiscard]] virtual std::size_t unprotect(std::span<std::byte> buffer,
                                              std::size_t size) noexcept = 0;

  // Distinguishes an authentication failure from a replay rejection so the
  // receiver can report them separately.
  [[nodiscard]] virtual bool last_failure_was_replay() const noexcept = 0;
  [[nodiscard]] virtual const char* profile_name() const noexcept = 0;
};

// Normalized peer address used to bind media to the authenticated peer.
struct Endpoint {
  std::array<std::uint8_t, 16> address{};
  std::uint16_t port{};

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] bool same_address(const Endpoint& other) const noexcept {
    return address == other.address;
  }
  [[nodiscard]] std::string to_string() const;
  friend bool operator==(const Endpoint&, const Endpoint&) noexcept = default;
};

class Packetizer {
 public:
  explicit Packetizer(AudioFormat format, std::uint32_t ssrc);
  [[nodiscard]] std::size_t push(std::span<const float> interleaved,
                                 RtpPacket& packet,
                                 bool& completed) noexcept;

 private:
  AudioFormat format_;
  RtpPacket staging_;
  std::array<float, kMaxPayloadBytes / sizeof(std::int16_t)> samples_{};
  std::size_t sample_count_{};
};

class JitterBuffer {
 public:
  static constexpr std::size_t kCapacity = 64;

  enum class InsertResult { inserted, duplicate, too_late, overrun, invalid };
  enum class PopResult { packet, missing, waiting };
  // Consecutive out-of-window packets tolerated before the buffer assumes the
  // peer restarted its sequence numbering and resynchronizes. Without this a
  // single runaway gap would wedge the receiver permanently.
  static constexpr std::uint16_t kResyncThreshold = 8;

  explicit JitterBuffer(std::uint16_t startup_packets = 2) noexcept;
  [[nodiscard]] InsertResult insert(const RtpPacket& packet) noexcept;
  [[nodiscard]] PopResult pop(RtpPacket& packet) noexcept;
  void reset() noexcept;
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] std::uint64_t resyncs() const noexcept { return resyncs_; }

 private:
  struct Slot {
    RtpPacket packet{};
    bool occupied{};
  };
  [[nodiscard]] bool has_buffered_successor() const noexcept;
  void resynchronize(std::uint16_t sequence) noexcept;

  std::array<Slot, kCapacity> slots_{};
  std::uint16_t next_sequence_{};
  std::uint16_t startup_packets_{};
  std::size_t size_{};
  std::uint16_t consecutive_out_of_window_{};
  std::uint64_t resyncs_{};
  bool started_{};
  bool sequence_initialized_{};
};

class DriftController {
 public:
  struct Config {
    double target_packets{2.0};
    double kp{100.0};
    double ki{0.01};
    double smoothing{0.02};
    double max_ppm{250.0};
  };

  DriftController() noexcept;
  explicit DriftController(Config config) noexcept;
  [[nodiscard]] double update(double occupancy_packets) noexcept;
  void reset() noexcept;
  [[nodiscard]] double correction_ppm() const noexcept { return correction_ppm_; }

 private:
  Config config_;
  double filtered_occupancy_{};
  double integral_{};
  double correction_ppm_{};
  bool initialized_{};
};

class UdpSocket {
 public:
  UdpSocket();
  ~UdpSocket();
  UdpSocket(const UdpSocket&) = delete;
  UdpSocket& operator=(const UdpSocket&) = delete;

  bool bind(std::uint16_t port, std::string& error);
  bool set_destination(std::string_view host, std::uint16_t port,
                       std::string& error);
  bool send(std::span<const std::byte> bytes, std::string& error);
  std::ptrdiff_t receive(std::span<std::byte> bytes,
                         std::chrono::milliseconds timeout,
                         std::string& error);
  // Same as receive, but also reports the normalized source address so the
  // caller can reject datagrams from anyone except the authenticated peer.
  std::ptrdiff_t receive_from(std::span<std::byte> bytes,
                              std::chrono::milliseconds timeout,
                              Endpoint& source, std::string& error);
  [[nodiscard]] Endpoint destination() const noexcept;
  [[nodiscard]] std::uint16_t local_port(std::string& error) const;

 private:
  std::intptr_t handle_{-1};
  std::array<std::byte, 128> destination_{};
  std::uint32_t destination_size_{};
};

// Resolves a host for comparison purposes without changing socket state.
[[nodiscard]] bool resolve_endpoint(std::string_view host, std::uint16_t port,
                                    Endpoint& endpoint, std::string& error);

}  // namespace lanaudio
