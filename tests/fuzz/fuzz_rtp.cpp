#include "lanaudio/core.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  if (data == nullptr) return 0;
  lanaudio::RtpPacket packet;
  (void)lanaudio::parse_rtp(std::as_bytes(std::span(data, size)), 96, packet);
  (void)lanaudio::parse_rtp(std::as_bytes(std::span(data, size)), 97, packet);
  return 0;
}
