#pragma once

#include "lanaudio/core.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace lanaudio::srtp {

// AEAD_AES_128_GCM uses a 16-byte key plus a 12-byte salt per direction.
inline constexpr std::size_t kKeyWithSaltBytes = 28;
inline constexpr std::size_t kKeyMaterialBytes = kKeyWithSaltBytes * 2;
inline constexpr const char* kProfileName = "AEAD_AES_128_GCM";

enum class Direction { outbound, inbound };

// One-directional SRTP context. Sender and receiver contexts are separate and
// are destroyed, with their keys wiped, when the session ends.
class SrtpProtector final : public MediaProtector {
 public:
  ~SrtpProtector() override;
  SrtpProtector(const SrtpProtector&) = delete;
  SrtpProtector& operator=(const SrtpProtector&) = delete;

  // `key_material` must contain kKeyMaterialBytes derived from the TLS 1.3
  // exporter. The first half keys the client-to-server direction.
  [[nodiscard]] static std::unique_ptr<SrtpProtector> create(
      Direction direction, bool local_is_client, std::uint32_t ssrc,
      std::span<const std::uint8_t> key_material, std::string& error);

  [[nodiscard]] std::size_t protect(std::span<std::byte> buffer,
                                    std::size_t size) noexcept override;
  [[nodiscard]] std::size_t unprotect(std::span<std::byte> buffer,
                                      std::size_t size) noexcept override;
  [[nodiscard]] bool last_failure_was_replay() const noexcept override {
    return last_failure_was_replay_;
  }
  [[nodiscard]] const char* profile_name() const noexcept override {
    return kProfileName;
  }

 private:
  SrtpProtector(void* session, Direction direction,
                std::array<std::uint8_t, kKeyWithSaltBytes> key);

  void* session_{};
  Direction direction_{};
  std::array<std::uint8_t, kKeyWithSaltBytes> key_{};
  bool last_failure_was_replay_{};
};

// Initializes the libsrtp library once per process.
[[nodiscard]] bool initialize_library(std::string& error);

}  // namespace lanaudio::srtp
