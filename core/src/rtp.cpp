#include "lanaudio/core.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace lanaudio {
namespace {

std::uint16_t read_u16(const std::byte* p) noexcept {
  return (std::to_integer<std::uint16_t>(p[0]) << 8U) |
         std::to_integer<std::uint16_t>(p[1]);
}

std::uint32_t read_u32(const std::byte* p) noexcept {
  return (std::to_integer<std::uint32_t>(p[0]) << 24U) |
         (std::to_integer<std::uint32_t>(p[1]) << 16U) |
         (std::to_integer<std::uint32_t>(p[2]) << 8U) |
         std::to_integer<std::uint32_t>(p[3]);
}

void write_u16(std::byte* p, std::uint16_t value) noexcept {
  p[0] = std::byte(value >> 8U);
  p[1] = std::byte(value & 0xffU);
}

void write_u32(std::byte* p, std::uint32_t value) noexcept {
  p[0] = std::byte(value >> 24U);
  p[1] = std::byte((value >> 16U) & 0xffU);
  p[2] = std::byte((value >> 8U) & 0xffU);
  p[3] = std::byte(value & 0xffU);
}

}  // namespace

std::size_t serialize_rtp(const RtpPacket& packet,
                          std::span<std::byte> destination) noexcept {
  const auto required = kRtpHeaderBytes + packet.payload_size;
  if (packet.payload_size > kMaxPayloadBytes || destination.size() < required) {
    return 0;
  }
  destination[0] = std::byte{0x80};
  destination[1] = std::byte((packet.header.marker ? 0x80U : 0U) |
                             (packet.header.payload_type & 0x7fU));
  write_u16(destination.data() + 2, packet.header.sequence);
  write_u32(destination.data() + 4, packet.header.timestamp);
  write_u32(destination.data() + 8, packet.header.ssrc);
  std::memcpy(destination.data() + kRtpHeaderBytes, packet.payload.data(),
              packet.payload_size);
  return required;
}

ParseError parse_rtp(std::span<const std::byte> bytes,
                     std::uint8_t expected_payload_type,
                     RtpPacket& packet) noexcept {
  if (bytes.size() < kRtpHeaderBytes) return ParseError::too_short;
  const auto first = std::to_integer<unsigned>(bytes[0]);
  if ((first >> 6U) != 2U) return ParseError::wrong_version;
  if ((first & 0x3fU) != 0U) return ParseError::unsupported_header;
  const auto second = std::to_integer<unsigned>(bytes[1]);
  if ((second & 0x7fU) != expected_payload_type) {
    return ParseError::wrong_payload_type;
  }
  const auto payload_size = bytes.size() - kRtpHeaderBytes;
  if (payload_size > kMaxPayloadBytes) return ParseError::payload_too_large;

  packet.header.marker = (second & 0x80U) != 0;
  packet.header.payload_type = static_cast<std::uint8_t>(second & 0x7fU);
  packet.header.sequence = read_u16(bytes.data() + 2);
  packet.header.timestamp = read_u32(bytes.data() + 4);
  packet.header.ssrc = read_u32(bytes.data() + 8);
  packet.payload_size = static_cast<std::uint16_t>(payload_size);
  std::memcpy(packet.payload.data(), bytes.data() + kRtpHeaderBytes,
              payload_size);
  return ParseError::none;
}

void float_to_l16be(std::span<const float> input,
                    std::span<std::byte> output) noexcept {
  const auto count = std::min(input.size(), output.size() / 2);
  for (std::size_t i = 0; i < count; ++i) {
    const float bounded = std::clamp(input[i], -1.0F, 1.0F);
    const auto sample = static_cast<std::int16_t>(
        std::lrint(bounded * (bounded < 0.0F ? 32768.0F : 32767.0F)));
    write_u16(output.data() + i * 2, static_cast<std::uint16_t>(sample));
  }
}

void l16be_to_float(std::span<const std::byte> input,
                    std::span<float> output) noexcept {
  const auto count = std::min(output.size(), input.size() / 2);
  for (std::size_t i = 0; i < count; ++i) {
    const auto sample = static_cast<std::int16_t>(read_u16(input.data() + i * 2));
    output[i] = static_cast<float>(sample) / 32768.0F;
  }
}

Packetizer::Packetizer(AudioFormat format, std::uint32_t ssrc)
    : format_(format) {
  staging_.header.payload_type = kDefaultPayloadType;
  staging_.header.ssrc = ssrc;
  staging_.payload_size = static_cast<std::uint16_t>(format_.payload_bytes());
}

std::size_t Packetizer::push(std::span<const float> interleaved,
                             RtpPacket& packet, bool& completed) noexcept {
  completed = false;
  const auto needed = format_.samples_per_packet() - sample_count_;
  const auto consumed = std::min(needed, interleaved.size());
  std::copy_n(interleaved.begin(), consumed, samples_.begin() + sample_count_);
  sample_count_ += consumed;
  if (sample_count_ == format_.samples_per_packet()) {
    float_to_l16be(std::span<const float>(samples_.data(), sample_count_),
                   std::span<std::byte>(staging_.payload.data(),
                                        staging_.payload_size));
    packet = staging_;
    ++staging_.header.sequence;
    staging_.header.timestamp += format_.frames_per_packet;
    staging_.header.marker = false;
    sample_count_ = 0;
    completed = true;
  }
  return consumed;
}

}  // namespace lanaudio
