#include "lanaudio/protocol/control_messages.hpp"

#include <algorithm>
#include <cstring>

namespace lanaudio::protocol {
namespace {

class Writer {
 public:
  explicit Writer(ControlMessageType type) : type_(type) {}

  void u8(std::uint8_t value) { body_.push_back(static_cast<std::byte>(value)); }

  void u16(std::uint16_t value) {
    u8(static_cast<std::uint8_t>(value >> 8U));
    u8(static_cast<std::uint8_t>(value & 0xffU));
  }

  void u32(std::uint32_t value) {
    u16(static_cast<std::uint16_t>(value >> 16U));
    u16(static_cast<std::uint16_t>(value & 0xffffU));
  }

  void u64(std::uint64_t value) {
    u32(static_cast<std::uint32_t>(value >> 32U));
    u32(static_cast<std::uint32_t>(value & 0xffff'ffffULL));
  }

  void text(const std::string& value) {
    if (value.size() > kMaxTextFieldBytes) {
      failed_ = true;
      return;
    }
    u16(static_cast<std::uint16_t>(value.size()));
    for (const char character : value) {
      u8(static_cast<std::uint8_t>(character));
    }
  }

  void bytes(const std::vector<std::uint8_t>& value) {
    if (value.size() > kMaxTextFieldBytes) {
      failed_ = true;
      return;
    }
    u16(static_cast<std::uint16_t>(value.size()));
    for (const auto octet : value) u8(octet);
  }

  void u32_list(const std::vector<std::uint32_t>& values) {
    if (values.size() > 32) {
      failed_ = true;
      return;
    }
    u8(static_cast<std::uint8_t>(values.size()));
    for (const auto value : values) u32(value);
  }

  [[nodiscard]] bool finish(std::vector<std::byte>& frame) {
    if (failed_ || body_.size() > kMaxControlBodyBytes) return false;
    frame.clear();
    frame.reserve(kControlFrameHeaderBytes + body_.size());
    const auto size = static_cast<std::uint16_t>(body_.size());
    frame.push_back(static_cast<std::byte>(size >> 8U));
    frame.push_back(static_cast<std::byte>(size & 0xffU));
    frame.push_back(static_cast<std::byte>(type_));
    frame.insert(frame.end(), body_.begin(), body_.end());
    return true;
  }

 private:
  ControlMessageType type_;
  std::vector<std::byte> body_;
  bool failed_{};
};

class Reader {
 public:
  explicit Reader(std::span<const std::byte> body) : body_(body) {}

  std::uint8_t u8() {
    if (offset_ + 1 > body_.size()) {
      error_ = CodecError::truncated;
      return 0;
    }
    return std::to_integer<std::uint8_t>(body_[offset_++]);
  }

  std::uint16_t u16() {
    const auto high = u8();
    const auto low = u8();
    return static_cast<std::uint16_t>((high << 8U) | low);
  }

  std::uint32_t u32() {
    const auto high = u16();
    const auto low = u16();
    return (static_cast<std::uint32_t>(high) << 16U) | low;
  }

  std::uint64_t u64() {
    const auto high = u32();
    const auto low = u32();
    return (static_cast<std::uint64_t>(high) << 32U) | low;
  }

  std::string text() {
    const auto size = u16();
    if (error_ != CodecError::none) return {};
    if (size > kMaxTextFieldBytes) {
      error_ = CodecError::oversized;
      return {};
    }
    if (offset_ + size > body_.size()) {
      error_ = CodecError::truncated;
      return {};
    }
    std::string value;
    value.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
      const auto octet = std::to_integer<unsigned char>(body_[offset_ + i]);
      // Control characters are rejected so peer-supplied names cannot corrupt
      // terminal output or UI labels.
      if (octet < 0x20 || octet == 0x7f) {
        error_ = CodecError::malformed_field;
        return {};
      }
      value.push_back(static_cast<char>(octet));
    }
    offset_ += size;
    return value;
  }

  std::vector<std::uint8_t> bytes(std::size_t expected_size) {
    const auto size = u16();
    if (error_ != CodecError::none) return {};
    if (size > kMaxTextFieldBytes) {
      error_ = CodecError::oversized;
      return {};
    }
    if (expected_size != 0 && size != expected_size) {
      error_ = CodecError::malformed_field;
      return {};
    }
    if (offset_ + size > body_.size()) {
      error_ = CodecError::truncated;
      return {};
    }
    std::vector<std::uint8_t> value(size);
    for (std::size_t i = 0; i < size; ++i) {
      value[i] = std::to_integer<std::uint8_t>(body_[offset_ + i]);
    }
    offset_ += size;
    return value;
  }

  std::vector<std::uint32_t> u32_list() {
    const auto count = u8();
    if (error_ != CodecError::none) return {};
    if (count > 32) {
      error_ = CodecError::oversized;
      return {};
    }
    if (offset_ + static_cast<std::size_t>(count) * 4U > body_.size()) {
      error_ = CodecError::truncated;
      return {};
    }
    std::vector<std::uint32_t> values;
    values.reserve(count);
    for (std::uint8_t i = 0; i < count; ++i) values.push_back(u32());
    return values;
  }

  [[nodiscard]] CodecError finish() const noexcept {
    if (error_ != CodecError::none) return error_;
    return offset_ == body_.size() ? CodecError::none
                                   : CodecError::malformed_field;
  }

  [[nodiscard]] std::size_t remaining() const noexcept {
    return body_.size() - std::min(offset_, body_.size());
  }

  [[nodiscard]] CodecError error() const noexcept { return error_; }

 private:
  std::span<const std::byte> body_;
  std::size_t offset_{};
  CodecError error_{CodecError::none};
};

CodecError open_frame(std::span<const std::byte> frame,
                      ControlMessageType expected,
                      std::span<const std::byte>& body) noexcept {
  ControlMessageType actual{};
  const auto error = frame_type(frame, actual);
  if (error != CodecError::none) return error;
  if (actual != expected) return CodecError::unknown_type;
  body = frame.subspan(kControlFrameHeaderBytes);
  return CodecError::none;
}

}  // namespace

std::string describe(CodecError error) {
  switch (error) {
    case CodecError::none:
      return "valid control message";
    case CodecError::truncated:
      return "control message ended before all fields were present";
    case CodecError::oversized:
      return "control message field exceeds the permitted size";
    case CodecError::unknown_type:
      return "unexpected control message type";
    case CodecError::malformed_field:
      return "control message field is malformed";
    case CodecError::invalid_value:
      return "control message contains an unacceptable value";
  }
  return "unknown control codec error";
}

std::size_t peek_frame_size(std::span<const std::byte> buffer,
                            CodecError& error) noexcept {
  error = CodecError::none;
  if (buffer.size() < kControlFrameHeaderBytes) return 0;
  const auto size = static_cast<std::size_t>(
      (std::to_integer<unsigned>(buffer[0]) << 8U) |
      std::to_integer<unsigned>(buffer[1]));
  if (size > kMaxControlBodyBytes) {
    error = CodecError::oversized;
    return 0;
  }
  const auto total = kControlFrameHeaderBytes + size;
  return buffer.size() >= total ? total : 0;
}

CodecError frame_type(std::span<const std::byte> frame,
                      ControlMessageType& type) noexcept {
  if (frame.size() < kControlFrameHeaderBytes) return CodecError::truncated;
  const auto size = static_cast<std::size_t>(
      (std::to_integer<unsigned>(frame[0]) << 8U) |
      std::to_integer<unsigned>(frame[1]));
  if (size > kMaxControlBodyBytes) return CodecError::oversized;
  if (frame.size() != kControlFrameHeaderBytes + size) {
    return CodecError::truncated;
  }
  const auto raw = std::to_integer<std::uint8_t>(frame[2]);
  if (raw < static_cast<std::uint8_t>(ControlMessageType::hello) ||
      raw > static_cast<std::uint8_t>(ControlMessageType::pair_result)) {
    return CodecError::unknown_type;
  }
  type = static_cast<ControlMessageType>(raw);
  return CodecError::none;
}

bool encode(const HelloMessage& message, std::vector<std::byte>& frame) {
  Writer writer(ControlMessageType::hello);
  writer.u16(message.protocol_version);
  writer.text(message.device_name);
  writer.text(message.application_version);
  writer.bytes(message.fingerprint);
  writer.bytes(message.nonce);
  return writer.finish(frame);
}

CodecError decode(std::span<const std::byte> frame, HelloMessage& message) {
  std::span<const std::byte> body;
  if (const auto error = open_frame(frame, ControlMessageType::hello, body);
      error != CodecError::none) {
    return error;
  }
  Reader reader(body);
  message.protocol_version = reader.u16();
  message.device_name = reader.text();
  message.application_version = reader.text();
  message.fingerprint = reader.bytes(kFingerprintBytes);
  message.nonce = reader.bytes(kNonceBytes);
  const auto error = reader.finish();
  if (error != CodecError::none) return error;
  if (message.device_name.empty()) return CodecError::malformed_field;
  return CodecError::none;
}

bool encode(const CapabilityOfferMessage& message,
            std::vector<std::byte>& frame) {
  Writer writer(ControlMessageType::capability_offer);
  writer.u32_list(message.capabilities.sample_rates);
  writer.u32_list(message.capabilities.packet_durations);
  writer.u32(message.capabilities.preferred_sample_rate);
  writer.u16(message.capabilities.channels);
  writer.u16(message.preferred_jitter_packets);
  writer.u32(message.preferred_packet_microseconds);
  return writer.finish(frame);
}

CodecError decode(std::span<const std::byte> frame,
                  CapabilityOfferMessage& message) {
  std::span<const std::byte> body;
  if (const auto error =
          open_frame(frame, ControlMessageType::capability_offer, body);
      error != CodecError::none) {
    return error;
  }
  Reader reader(body);
  message.capabilities.sample_rates = reader.u32_list();
  message.capabilities.packet_durations = reader.u32_list();
  message.capabilities.preferred_sample_rate = reader.u32();
  message.capabilities.channels = reader.u16();
  message.preferred_jitter_packets = reader.u16();
  if (reader.remaining() != 0) {
    message.preferred_packet_microseconds = reader.u32();
    if (!is_supported_packet_duration(
            message.preferred_packet_microseconds)) {
      return CodecError::invalid_value;
    }
  } else {
    message.preferred_packet_microseconds = kPreferredPacketMicroseconds;
  }
  const auto error = reader.finish();
  if (error != CodecError::none) return error;
  if (message.preferred_jitter_packets < 2 ||
      message.preferred_jitter_packets > 32) {
    return CodecError::invalid_value;
  }
  return CodecError::none;
}

bool encode(const SessionAcceptMessage& message,
            std::vector<std::byte>& frame) {
  Writer writer(ControlMessageType::session_accept);
  const auto& parameters = message.parameters;
  writer.u32(parameters.sample_rate);
  writer.u16(parameters.channels);
  writer.u16(parameters.frames_per_packet);
  writer.u32(parameters.packet_microseconds);
  writer.u8(parameters.payload_type);
  writer.u32(parameters.ssrc);
  writer.u16(parameters.media_port);
  writer.u16(parameters.jitter_packets);
  writer.u64(parameters.session_id);
  return writer.finish(frame);
}

CodecError decode(std::span<const std::byte> frame,
                  SessionAcceptMessage& message) {
  std::span<const std::byte> body;
  if (const auto error =
          open_frame(frame, ControlMessageType::session_accept, body);
      error != CodecError::none) {
    return error;
  }
  Reader reader(body);
  auto& parameters = message.parameters;
  parameters.sample_rate = reader.u32();
  parameters.channels = reader.u16();
  parameters.frames_per_packet = reader.u16();
  parameters.packet_microseconds = reader.u32();
  parameters.payload_type = reader.u8();
  parameters.ssrc = reader.u32();
  parameters.media_port = reader.u16();
  parameters.jitter_packets = reader.u16();
  parameters.session_id = reader.u64();
  const auto error = reader.finish();
  if (error != CodecError::none) return error;
  // A peer cannot talk this endpoint into an unsupported media configuration.
  return parameters.valid() ? CodecError::none : CodecError::invalid_value;
}

bool encode(const SessionRejectMessage& message,
            std::vector<std::byte>& frame) {
  Writer writer(ControlMessageType::session_reject);
  writer.u8(static_cast<std::uint8_t>(message.reason));
  writer.text(message.detail);
  return writer.finish(frame);
}

CodecError decode(std::span<const std::byte> frame,
                  SessionRejectMessage& message) {
  std::span<const std::byte> body;
  if (const auto error =
          open_frame(frame, ControlMessageType::session_reject, body);
      error != CodecError::none) {
    return error;
  }
  Reader reader(body);
  const auto reason = reader.u8();
  message.detail = reader.text();
  const auto error = reader.finish();
  if (error != CodecError::none) return error;
  if (reason > static_cast<std::uint8_t>(RejectReason::timeout)) {
    return CodecError::invalid_value;
  }
  message.reason = static_cast<RejectReason>(reason);
  return CodecError::none;
}

bool encode(const HeartbeatMessage& message, std::vector<std::byte>& frame) {
  Writer writer(ControlMessageType::heartbeat);
  writer.u64(message.counter);
  writer.u64(message.monotonic_micros);
  return writer.finish(frame);
}

CodecError decode(std::span<const std::byte> frame, HeartbeatMessage& message) {
  std::span<const std::byte> body;
  if (const auto error = open_frame(frame, ControlMessageType::heartbeat, body);
      error != CodecError::none) {
    return error;
  }
  Reader reader(body);
  message.counter = reader.u64();
  message.monotonic_micros = reader.u64();
  return reader.finish();
}

bool encode(const GoodbyeMessage& message, std::vector<std::byte>& frame) {
  Writer writer(ControlMessageType::goodbye);
  writer.u8(static_cast<std::uint8_t>(message.reason));
  return writer.finish(frame);
}

CodecError decode(std::span<const std::byte> frame, GoodbyeMessage& message) {
  std::span<const std::byte> body;
  if (const auto error = open_frame(frame, ControlMessageType::goodbye, body);
      error != CodecError::none) {
    return error;
  }
  Reader reader(body);
  const auto reason = reader.u8();
  const auto error = reader.finish();
  if (error != CodecError::none) return error;
  if (reason > static_cast<std::uint8_t>(RejectReason::timeout)) {
    return CodecError::invalid_value;
  }
  message.reason = static_cast<RejectReason>(reason);
  return CodecError::none;
}

bool encode(const PairConfirmMessage& message, std::vector<std::byte>& frame) {
  Writer writer(ControlMessageType::pair_confirm);
  writer.u8(message.accepted ? 1U : 0U);
  writer.bytes(message.transcript_digest);
  return writer.finish(frame);
}

CodecError decode(std::span<const std::byte> frame,
                  PairConfirmMessage& message) {
  std::span<const std::byte> body;
  if (const auto error =
          open_frame(frame, ControlMessageType::pair_confirm, body);
      error != CodecError::none) {
    return error;
  }
  Reader reader(body);
  const auto accepted = reader.u8();
  message.transcript_digest = reader.bytes(kFingerprintBytes);
  const auto error = reader.finish();
  if (error != CodecError::none) return error;
  if (accepted > 1U) return CodecError::invalid_value;
  message.accepted = accepted == 1U;
  return CodecError::none;
}

bool encode(const PairResultMessage& message, std::vector<std::byte>& frame) {
  Writer writer(ControlMessageType::pair_result);
  writer.u8(message.trusted ? 1U : 0U);
  writer.text(message.detail);
  return writer.finish(frame);
}

CodecError decode(std::span<const std::byte> frame,
                  PairResultMessage& message) {
  std::span<const std::byte> body;
  if (const auto error =
          open_frame(frame, ControlMessageType::pair_result, body);
      error != CodecError::none) {
    return error;
  }
  Reader reader(body);
  const auto trusted = reader.u8();
  message.detail = reader.text();
  const auto error = reader.finish();
  if (error != CodecError::none) return error;
  if (trusted > 1U) return CodecError::invalid_value;
  message.trusted = trusted == 1U;
  return CodecError::none;
}

}  // namespace lanaudio::protocol
