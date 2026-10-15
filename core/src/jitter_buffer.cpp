#include "lanaudio/core.hpp"

#include <algorithm>

namespace lanaudio {
namespace {

std::int16_t sequence_delta(std::uint16_t value,
                            std::uint16_t reference) noexcept {
  return static_cast<std::int16_t>(value - reference);
}

}  // namespace

JitterBuffer::JitterBuffer(std::uint16_t startup_packets) noexcept
    : startup_packets_(
          std::clamp<std::uint16_t>(startup_packets, 1, kCapacity - 1)) {}

void JitterBuffer::resynchronize(std::uint16_t sequence) noexcept {
  for (auto& slot : slots_) slot.occupied = false;
  size_ = 0;
  started_ = false;
  next_sequence_ = sequence;
  sequence_initialized_ = true;
  consecutive_out_of_window_ = 0;
  ++resyncs_;
}

bool JitterBuffer::has_buffered_successor() const noexcept {
  for (const auto& slot : slots_) {
    if (!slot.occupied) continue;
    if (sequence_delta(slot.packet.header.sequence, next_sequence_) > 0) {
      return true;
    }
  }
  return false;
}

JitterBuffer::InsertResult JitterBuffer::insert(
    const RtpPacket& packet) noexcept {
  if (packet.payload_size == 0 || packet.payload_size > kMaxPayloadBytes) {
    return InsertResult::invalid;
  }
  const auto sequence = packet.header.sequence;
  if (!sequence_initialized_) {
    next_sequence_ = sequence;
    sequence_initialized_ = true;
  }

  auto delta = sequence_delta(sequence, next_sequence_);
  if (delta < 0) {
    if (!started_ && delta > -static_cast<std::int16_t>(kCapacity)) {
      next_sequence_ = sequence;
      delta = 0;
    } else if (++consecutive_out_of_window_ >= kResyncThreshold) {
      // The peer is consistently outside our window, so our read cursor is
      // stale rather than the peer being late. Rebuild around the live stream
      // instead of rejecting every remaining packet.
      resynchronize(sequence);
      delta = 0;
    } else {
      return InsertResult::too_late;
    }
  } else if (delta >= static_cast<std::int16_t>(kCapacity)) {
    if (++consecutive_out_of_window_ >= kResyncThreshold) {
      resynchronize(sequence);
      delta = 0;
    } else {
      return InsertResult::overrun;
    }
  }

  consecutive_out_of_window_ = 0;
  auto& slot = slots_[sequence % kCapacity];
  if (slot.occupied) {
    if (slot.packet.header.sequence == sequence) return InsertResult::duplicate;
    // The slot still holds an entry from an earlier window that the reader
    // skipped. Replacing it keeps `size_` honest; leaving it would make the
    // buffer look permanently occupied.
    slot.packet = packet;
    return InsertResult::overrun;
  }
  slot.packet = packet;
  slot.occupied = true;
  ++size_;
  if (!started_ && size_ >= startup_packets_) started_ = true;
  return InsertResult::inserted;
}

JitterBuffer::PopResult JitterBuffer::pop(RtpPacket& packet) noexcept {
  if (!started_ || !sequence_initialized_) return PopResult::waiting;
  auto& slot = slots_[next_sequence_ % kCapacity];
  if (!slot.occupied || slot.packet.header.sequence != next_sequence_) {
    // Only conceal a gap when a later packet is actually buffered. Advancing
    // on occupancy alone let the read cursor run away from the live stream
    // and made every subsequent packet arrive "too late".
    if (!has_buffered_successor()) return PopResult::waiting;
    ++next_sequence_;
    return PopResult::missing;
  }
  packet = slot.packet;
  slot.occupied = false;
  --size_;
  ++next_sequence_;
  return PopResult::packet;
}

void JitterBuffer::reset() noexcept {
  for (auto& slot : slots_) slot.occupied = false;
  next_sequence_ = 0;
  size_ = 0;
  started_ = false;
  sequence_initialized_ = false;
  consecutive_out_of_window_ = 0;
}

}  // namespace lanaudio
