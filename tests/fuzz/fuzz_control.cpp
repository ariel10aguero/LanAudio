#include "lanaudio/protocol/control_messages.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  if (data == nullptr || size == 0) return 0;
  const auto bytes = std::as_bytes(std::span(data, size));
  lanaudio::protocol::CodecError error = lanaudio::protocol::CodecError::none;
  (void)lanaudio::protocol::peek_frame_size(bytes, error);

  lanaudio::protocol::ControlMessageType type{};
  if (lanaudio::protocol::frame_type(bytes, type) !=
      lanaudio::protocol::CodecError::none) {
    return 0;
  }

  switch (type) {
    case lanaudio::protocol::ControlMessageType::hello: {
      lanaudio::protocol::HelloMessage message;
      (void)lanaudio::protocol::decode(bytes, message);
      break;
    }
    case lanaudio::protocol::ControlMessageType::capability_offer: {
      lanaudio::protocol::CapabilityOfferMessage message;
      (void)lanaudio::protocol::decode(bytes, message);
      break;
    }
    case lanaudio::protocol::ControlMessageType::session_accept: {
      lanaudio::protocol::SessionAcceptMessage message;
      (void)lanaudio::protocol::decode(bytes, message);
      break;
    }
    case lanaudio::protocol::ControlMessageType::session_reject: {
      lanaudio::protocol::SessionRejectMessage message;
      (void)lanaudio::protocol::decode(bytes, message);
      break;
    }
    case lanaudio::protocol::ControlMessageType::heartbeat: {
      lanaudio::protocol::HeartbeatMessage message;
      (void)lanaudio::protocol::decode(bytes, message);
      break;
    }
    case lanaudio::protocol::ControlMessageType::goodbye: {
      lanaudio::protocol::GoodbyeMessage message;
      (void)lanaudio::protocol::decode(bytes, message);
      break;
    }
    case lanaudio::protocol::ControlMessageType::pair_confirm: {
      lanaudio::protocol::PairConfirmMessage message;
      (void)lanaudio::protocol::decode(bytes, message);
      break;
    }
    case lanaudio::protocol::ControlMessageType::pair_result: {
      lanaudio::protocol::PairResultMessage message;
      (void)lanaudio::protocol::decode(bytes, message);
      break;
    }
  }
  return 0;
}
